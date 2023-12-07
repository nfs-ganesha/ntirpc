#ifndef METRICS_LIBNTIRPC_H
#define METRICS_LIBNTIRPC_H

#include <rpc/auth.h>

void metrics_libntirpc_update_tcp_connection_count(int connection_count);
void metrics_libntirpc_observe_svc_auth_request_latency(int sec_flavor,
	enum auth_stat, const struct timespec *latency);
void metrics_libntirpc_init(void);

#endif // METRICS_LIBNTIRPC_H
