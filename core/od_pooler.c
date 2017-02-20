
/*
 * odissey.
 *
 * PostgreSQL connection pooler and request router.
*/

#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <machinarium.h>
#include <soprano.h>

#include "od_macro.h"
#include "od_list.h"
#include "od_pid.h"
#include "od_syslog.h"
#include "od_log.h"
#include "od_scheme.h"
#include "od_lex.h"
#include "od_config.h"
#include "od_server.h"
#include "od_server_pool.h"
#include "od_client.h"
#include "od_client_list.h"
#include "od_client_pool.h"
#include "od_route_id.h"
#include "od_route.h"
#include "od_route_pool.h"
#include "od.h"
#include "od_pooler.h"
#include "od_periodic.h"
#include "od_router.h"

static inline void
od_pooler(void *arg)
{
	od_pooler_t *pooler = arg;
	od_t *env = pooler->od;

	/* resolve listen address and port */
	char port[16];
	snprintf(port, sizeof(port), "%d", env->scheme.port);
	struct addrinfo *ai = NULL;
	int rc;
	rc = mm_getaddrinfo(pooler->server,
	                    env->scheme.host, port, NULL, &ai, 0);
	if (rc < 0) {
		od_error(&env->log, NULL, "failed to resolve %s:%d",
		         env->scheme.host,
		         env->scheme.port);
		return;
	}
	assert(ai != NULL);

	/* bind to listen address and port */
	rc = mm_bind(pooler->server, ai->ai_addr);
	freeaddrinfo(ai);
	if (rc < 0) {
		od_error(&env->log, NULL, "bind %s:%d failed",
		         env->scheme.host,
		         env->scheme.port);
		return;
	}

	/* starting periodic task scheduler fiber */
	rc = mm_create(pooler->env, od_periodic, pooler);
	if (rc < 0) {
		od_error(&env->log, NULL, "failed to create periodic fiber");
		return;
	}

	od_log(&env->log, NULL, "listening on %s:%d",
	       env->scheme.host, env->scheme.port);
	od_log(&env->log, NULL, "");

	/* accept loop */
	while (mm_is_online(pooler->env))
	{
		mm_io_t client_io;
		rc = mm_accept(pooler->server, env->scheme.backlog, &client_io);
		if (rc < 0) {
			od_error(&env->log, NULL, "accept failed");
			continue;
		}
		if (pooler->client_list.count >= env->scheme.client_max) {
			od_log(&pooler->od->log, client_io,
			       "C: pooler client_max reached (%d), closing connection",
			       env->scheme.client_max);
			mm_close(client_io);
			continue;
		}
		mm_io_nodelay(client_io, env->scheme.nodelay);
		if (env->scheme.keepalive > 0)
			mm_io_keepalive(client_io, 1, env->scheme.keepalive);
		od_client_t *client = od_clientalloc();
		if (client == NULL) {
			od_error(&env->log, NULL, "failed to allocate client object");
			mm_close(client_io);
			continue;
		}
		client->id = pooler->client_seq++;
		client->pooler = pooler;
		client->io = client_io;
		rc = mm_create(pooler->env, od_router, client);
		if (rc < 0) {
			od_error(&env->log, NULL, "failed to create client fiber");
			mm_close(client_io);
			od_clientfree(client);
			continue;
		}
		od_clientlist_add(&pooler->client_list, client);
	}
}

int od_pooler_init(od_pooler_t *pooler, od_t *od)
{
	pooler->env = mm_new();
	if (pooler->env == NULL)
		return -1;
	pooler->server = mm_io_new(pooler->env);
	if (pooler->server == NULL) {
		mm_free(pooler->env);
		return -1;
	}
	pooler->client_seq = 0;
	pooler->od = od;
	od_routepool_init(&pooler->route_pool);
	od_clientlist_init(&pooler->client_list);
	return 0;
}

int od_pooler_start(od_pooler_t *pooler)
{
	int rc;
	rc = mm_create(pooler->env, od_pooler, pooler);
	if (rc < 0) {
		od_error(&pooler->od->log, NULL,
		         "failed to create pooler fiber");
		return -1;
	}
	mm_start(pooler->env);
	return 0;
}
