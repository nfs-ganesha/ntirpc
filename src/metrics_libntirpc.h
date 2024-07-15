#ifndef METRICS_LIBNTIRPC_H
#define METRICS_LIBNTIRPC_H

#ifdef _HAVE_GSSAPI
#include <rpc/auth_gss.h>
#endif
#include <rpc/auth_stat.h>

typedef enum gss_svc_auth_step {
	VALIDATE_AUTH_DATA = 0,
	ACCEPT_SECURITY_CONTEXT,
	GET_NEXT_VERIFIER,
	SEND_CLIENT_REPLY,
	GSS_SVC_AUTH_STEPS_COUNT
} gss_svc_auth_step_t;

typedef enum svc_auth_op {
	SVC_AUTH_OP_WRAP = 0,
	SVC_AUTH_OP_UNWRAP,
	SVC_AUTH_OP_CHECKSUM,
	SVC_AUTH_OP_DESTROY,
	SVC_AUTH_OPS_COUNT
} svc_auth_op_t;

void metrics_libntirpc_update_tcp_connection_count(int connection_count);
void metrics_libntirpc_observe_svc_auth_request_latency(int sec_flavor,
	enum auth_stat, const struct timespec *latency);
#ifdef _HAVE_GSSAPI
void metrics_libntirpc_observe_gss_svc_auth_step_latency(gss_svc_auth_step_t,
	rpc_gss_svc_t, bool step_succeeded, const struct timespec *latency);
void metrics_libntirpc_observe_gss_svc_auth_op_latency(svc_auth_op_t,
	rpc_gss_svc_t, const struct timespec *latency);
#endif
void metrics_libntirpc_init(void);

#endif // METRICS_LIBNTIRPC_H
