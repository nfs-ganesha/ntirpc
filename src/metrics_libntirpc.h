/*
 * Copyright (c) 2025, Google, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef METRICS_LIBNTIRPC_H
#define METRICS_LIBNTIRPC_H

#ifdef _HAVE_GSSAPI
#include <rpc/auth_gss.h>
#endif
#include <rpc/auth_stat.h>
#include <time.h>

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
void metrics_libntirpc_observe_svc_auth_request_latency(
	int sec_flavor, enum auth_stat, const struct timespec *latency);
void register_gss_threads_metrics(void);
void metrics_libntirpc_update_threads_info_gauge(uint32_t under_utilization_thrds, uint32_t schedulable_thrds_count);

#ifdef _HAVE_GSSAPI
void metrics_libntirpc_observe_gss_svc_auth_step_latency(
	gss_svc_auth_step_t, rpc_gss_svc_t, bool step_succeeded,
	const struct timespec *latency);
void metrics_libntirpc_observe_gss_svc_auth_op_latency(
	svc_auth_op_t, rpc_gss_svc_t, const struct timespec *latency);
#endif
void metrics_libntirpc_init(void);

static inline int metrics_libntirpc_clock_gettime(struct timespec *tp)
{
#ifdef USE_MONITORING
	return clock_gettime(CLOCK_MONOTONIC, tp);
#else
	tp->tv_sec = 0;
	tp->tv_nsec = 0;
	return 0;
#endif
}

#endif // METRICS_LIBNTIRPC_H
