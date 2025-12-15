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

#include "monitoring.h"
#include "metrics_libntirpc.h"
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <misc/timespec.h>
#include <rpc/auth.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(a[0]))

/* Generic enum to represent commonly used operation status */
typedef enum generic_op_status {
	GENERIC_SUCCESS_STATUS = 0,
	GENERIC_FAILURE_STATUS,
	GENERIC_OP_STATUS_COUNT,
} generic_op_status_t;

static inline const char *
get_generic_op_status_name(generic_op_status_t op_status)
{
	switch (op_status) {
	case GENERIC_SUCCESS_STATUS:
		return "success";
	case GENERIC_FAILURE_STATUS:
		return "failure";
	default:
		abort();
	}
}

/* Concurrent TCP Connections */
static gauge_metric_handle_t concurrent_tcp_metric;

/* Thread utilization metrics */
static gauge_metric_handle_t threads_scheduled;
static gauge_metric_handle_t threads_schedulable;

static bool initialized = false;

/* For each security-flavor as an array index, assign a serial number to be
 * represented as the index in the metrics array.
 * The serial number `0` is not assigned, since un-mentioned security-flavors
 * will have the default `0` value in the array. Such entries will not be
 * represented in the metrics array.
 */
static const uint8_t sec_flavors_idx[] = { [AUTH_NONE] = 1, [AUTH_SYS] = 2,
					[RPCSEC_GSS] = 3 , [AUTH_TLS] = 4};

/* For each auth-stat as an array index, assign a serial number to be
 * represented as the index in the metrics array.
 * The serial number `0` is not assigned, since un-mentioned auth-stats
 * will have the default `0` value in the array. Such entries will not be
 * represented in the metrics array.
 */
static const uint8_t auth_stats_idx[] = { [AUTH_OK] = 1,
					  [AUTH_BADCRED] = 2,
					  [AUTH_REJECTEDCRED] = 3,
					  [AUTH_BADVERF] = 4,
					  [AUTH_REJECTEDVERF] = 5,
					  [AUTH_TOOWEAK] = 6,
					  [AUTH_INVALIDRESP] = 7,
					  [AUTH_FAILED] = 8,
					  [RPCSEC_GSS_CREDPROBLEM] = 9,
					  [RPCSEC_GSS_CTXPROBLEM] = 10 };

/* Latency of authentication requests for each sec-flavor and auth-status */
static histogram_metric_handle_t svc_auth_request_latency[ARRAY_SIZE(
	sec_flavors_idx)][ARRAY_SIZE(auth_stats_idx)];

#ifdef _HAVE_GSSAPI
/* For each RPCSEC_GSS-service as an array index, assign a serial number to be
 * represented as the index in the metrics array.
 * The serial number `0` is not assigned, since un-mentioned RPCSEC_GSS
 * services will have the default `0` value in the array. Such entries will
 * not be represented in the metrics array.
 */
static const uint8_t gss_svcs_idx[] = { [RPCSEC_GSS_SVC_NONE] = 1,
					[RPCSEC_GSS_SVC_INTEGRITY] = 2,
					[RPCSEC_GSS_SVC_PRIVACY] = 3 };

/* Latency of gss svc-auth steps */
static histogram_metric_handle_t gss_svc_auth_steps_latency[ARRAY_SIZE(
	gss_svcs_idx)][GSS_SVC_AUTH_STEPS_COUNT][GENERIC_OP_STATUS_COUNT];

/* Latency of gss svc-auth ops */
static histogram_metric_handle_t
	gss_svc_auth_ops_latency[ARRAY_SIZE(gss_svcs_idx)][SVC_AUTH_OPS_COUNT];

static int get_gss_svc_idx_in_metrics_array(rpc_gss_svc_t svc)
{
	assert(svc < ARRAY_SIZE(gss_svcs_idx));
	const uint8_t gss_svc_idx = gss_svcs_idx[svc];
	/* The metric indexes represented by the gss-svcs array start from 1 */
	return (gss_svc_idx - 1);
}

#endif
static int get_sec_flavor_idx_in_metrics_array(int sec_flavor)
{
	assert(sec_flavor < ARRAY_SIZE(sec_flavors_idx));
	const uint8_t sec_flavor_idx = sec_flavors_idx[sec_flavor];
	/* The metric indexes represented by the sec-flavors array start from 1 */
	return (sec_flavor_idx - 1);
}

static int get_auth_stat_idx_in_metrics_array(enum auth_stat auth_status)
{
	assert(auth_status < ARRAY_SIZE(auth_stats_idx));
	const uint8_t auth_stat_idx = auth_stats_idx[auth_status];
	/* The metric indexes represented by the auth-stats array start from 1 */
	return (auth_stat_idx - 1);
}

/* Get string corresponding to security flavor */
static const char *get_sec_flavor_string(int sec_flavor)
{
	switch (sec_flavor) {
	case AUTH_NONE:
		return "NONE";
	case AUTH_SYS:
		return "SYS";
	case RPCSEC_GSS:
		return "RPCSEC_GSS";
	case AUTH_TLS:
		return "AUTH_TLS";
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported security flavor value: %d", __func__,
			sec_flavor);
		abort();
	}
}

/* Get string corresponding to auth-status */
static const char *get_auth_stat_string(enum auth_stat auth_status)
{
	switch (auth_status) {
	case AUTH_OK:
		return "AUTH_OK";
	case AUTH_BADCRED:
		return "AUTH_BADCRED";
	case AUTH_REJECTEDCRED:
		return "AUTH_REJECTEDCRED";
	case AUTH_BADVERF:
		return "AUTH_BADVERF";
	case AUTH_REJECTEDVERF:
		return "AUTH_REJECTEDVERF";
	case AUTH_TOOWEAK:
		return "AUTH_TOOWEAK";
	case AUTH_INVALIDRESP:
		return "AUTH_INVALIDRESP";
	case AUTH_FAILED:
		return "AUTH_FAILED";
	case RPCSEC_GSS_CREDPROBLEM:
		return "RPCSEC_GSS_CREDPROBLEM";
	case RPCSEC_GSS_CTXPROBLEM:
		return "RPCSEC_GSS_CTXPROBLEM";
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported auth-stat value: %d", __func__,
			auth_status);
		abort();
	}
}

/* Get string corresponding to gss svc-auth step */
static const char *get_gss_svc_auth_step_string(gss_svc_auth_step_t step)
{
	switch (step) {
	case VALIDATE_AUTH_DATA:
		return "VALIDATE_AUTH_DATA";
	case ACCEPT_SECURITY_CONTEXT:
		return "ACCEPT_SECURITY_CONTEXT";
	case GET_NEXT_VERIFIER:
		return "GET_NEXT_VERIFIER";
	case SEND_CLIENT_REPLY:
		return "SEND_CLIENT_REPLY";
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported gss svc-auth step value: %d", __func__,
			step);
		abort();
	}
}

#ifdef _HAVE_GSSAPI
/* Get string corresponding to RPCSEC_GSS service */
static const char *get_gss_svc_string(rpc_gss_svc_t svc)
{
	switch (svc) {
	case RPCSEC_GSS_SVC_NONE:
		return "RPCSEC_GSS_SVC_NONE";
	case RPCSEC_GSS_SVC_INTEGRITY:
		return "RPCSEC_GSS_SVC_INTEGRITY";
	case RPCSEC_GSS_SVC_PRIVACY:
		return "RPCSEC_GSS_SVC_PRIVACY";
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported RPCSEC_GSS service value: %d",
			__func__, svc);
		abort();
	}
}
#endif

/* Get string corresponding to svc-auth op */
static const char *get_svc_auth_op_string(svc_auth_op_t op)
{
	switch (op) {
	case SVC_AUTH_OP_WRAP:
		return "WRAP";
	case SVC_AUTH_OP_UNWRAP:
		return "UNWRAP";
	case SVC_AUTH_OP_CHECKSUM:
		return "CHECKSUM";
	case SVC_AUTH_OP_DESTROY:
		return "DESTROY";
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported svc-auth op value: %d", __func__, op);
		abort();
	}
}

void register_gss_threads_metrics(void)
{
	const metric_label_t empty_labels[] = {};
	threads_scheduled = monitoring__register_gauge(
		"Threads_currently_scheduled",
		METRIC_METADATA("no.of threads in usage", METRIC_UNIT_NONE),
		empty_labels, ARRAY_SIZE(empty_labels));
	threads_schedulable = monitoring__register_gauge(
		"threads_schedulable",
		METRIC_METADATA("no.of threads schedulable", METRIC_UNIT_NONE),
		empty_labels, ARRAY_SIZE(empty_labels));
}

static void register_svc_auth_request_latency_metric(void)
{
	int sf, as;

	for (sf = 0; sf < ARRAY_SIZE(sec_flavors_idx); sf++) {
		const int sf_idx = get_sec_flavor_idx_in_metrics_array(sf);

		/* Do not register un-mentioned values in sec-flavors array */
		if (sf_idx < 0)
			continue;

		const char *const sec_flavor = get_sec_flavor_string(sf);

		for (as = 0; as < ARRAY_SIZE(auth_stats_idx); as++) {
			const int as_idx =
				get_auth_stat_idx_in_metrics_array(as);

			/* Do not register un-mentioned values in auth-stats array */
			if (as_idx < 0)
				continue;

			const metric_label_t labels[] = {
				METRIC_LABEL("sec_flavor", sec_flavor),
				METRIC_LABEL("auth_status",
					     get_auth_stat_string(as))
			};

			svc_auth_request_latency[sf_idx][as_idx] =
				monitoring__register_histogram(
					"libntirpc__svc_auth_request_latency",
					METRIC_METADATA(
						"Distribution of time taken by an authentication request to complete",
						METRIC_UNIT_MICROSECOND),
					labels, ARRAY_SIZE(labels),
					monitoring__buckets_exp2_compact());
		}
	}
}

static void register_gss_svc_auth_steps_latency_metric(void)
{
#ifdef _HAVE_GSSAPI
	int svc, as, status;

	for (svc = 0; svc < ARRAY_SIZE(gss_svcs_idx); svc++) {
		const int svc_idx = get_gss_svc_idx_in_metrics_array(svc);

		/* Do not register un-mentioned values in gss-svcs array */
		if (svc_idx < 0)
			continue;
		const char *const svc_str = get_gss_svc_string(svc);

		for (as = 0; as < GSS_SVC_AUTH_STEPS_COUNT; as++) {
			const char *const auth_step_str =
				get_gss_svc_auth_step_string(as);

			for (status = 0; status < GENERIC_OP_STATUS_COUNT;
			     status++) {
				const metric_label_t labels[] = {
					METRIC_LABEL("svc", svc_str),
					METRIC_LABEL("auth_step",
						     auth_step_str),
					METRIC_LABEL("step_status",
						     get_generic_op_status_name(
							     status))
				};
				gss_svc_auth_steps_latency[svc_idx][as][status] =
					monitoring__register_histogram(
						"libntirpc__gss_svc_auth_steps_latency",
						METRIC_METADATA(
							"Distribution of time taken by gss service-auth steps to complete",
							METRIC_UNIT_MICROSECOND),
						labels, ARRAY_SIZE(labels),
						monitoring__buckets_exp2_compact());
			}
		}
	}
#endif
}

static void register_gss_svc_auth_ops_latency_metric(void)
{
#ifdef _HAVE_GSSAPI
	int svc, i;

	for (svc = 0; svc < ARRAY_SIZE(gss_svcs_idx); svc++) {
		const int svc_idx = get_gss_svc_idx_in_metrics_array(svc);

		/* Do not register un-mentioned values in gss-svcs array */
		if (svc_idx < 0)
			continue;
		const char *const svc_str = get_gss_svc_string(svc);

		for (i = 0; i < SVC_AUTH_OPS_COUNT; i++) {
			const metric_label_t labels[] = {
				METRIC_LABEL("svc", svc_str),
				METRIC_LABEL("op", get_svc_auth_op_string(i))
			};
			gss_svc_auth_ops_latency[svc_idx][i] =
				monitoring__register_histogram(
					"libntirpc__gss_svc_auth_ops_latency",
					METRIC_METADATA(
						"Distribution of time taken by gss service-auth ops to complete",
						METRIC_UNIT_MICROSECOND),
					labels, ARRAY_SIZE(labels),
					monitoring__buckets_exp2_compact());
		}
	}
#endif
}

void metrics_libntirpc_update_threads_info_gauge(
	uint32_t under_utilization_thrds, uint32_t schedulable_thrds_count)
{
	monitoring__gauge_set(threads_scheduled, under_utilization_thrds);
	monitoring__gauge_set(threads_schedulable, schedulable_thrds_count);
}

void metrics_libntirpc_observe_svc_auth_request_latency(
	int sec_flavor, enum auth_stat auth_status,
	const struct timespec *latency)
{
	const int sf_idx = get_sec_flavor_idx_in_metrics_array(sec_flavor);
	assert(sf_idx >= 0);
	const int as_idx = get_auth_stat_idx_in_metrics_array(auth_status);
	assert(as_idx >= 0);
	monitoring__histogram_observe(svc_auth_request_latency[sf_idx][as_idx],
				      timespec_us(latency));
}

#ifdef _HAVE_GSSAPI

void metrics_libntirpc_observe_gss_svc_auth_step_latency(
	gss_svc_auth_step_t step, rpc_gss_svc_t svc, bool step_succeeded,
	const struct timespec *latency)
{
	const int svc_idx = get_gss_svc_idx_in_metrics_array(svc);
	assert(svc_idx >= 0);
	const generic_op_status_t step_status = step_succeeded ?
							GENERIC_SUCCESS_STATUS :
							GENERIC_FAILURE_STATUS;
	monitoring__histogram_observe(
		gss_svc_auth_steps_latency[svc_idx][step][step_status],
		timespec_us(latency));
}

void metrics_libntirpc_observe_gss_svc_auth_op_latency(
	svc_auth_op_t op, rpc_gss_svc_t svc, const struct timespec *latency)
{
	const int svc_idx = get_gss_svc_idx_in_metrics_array(svc);
	assert(svc_idx >= 0);
	monitoring__histogram_observe(gss_svc_auth_ops_latency[svc_idx][op],
				      timespec_us(latency));
}
#endif

void metrics_libntirpc_update_tcp_connection_count(int connection_count)
{
	monitoring__gauge_set(concurrent_tcp_metric, connection_count);
}

void metrics_libntirpc_init(void)
{
	if (initialized)
		return;
	const metric_label_t empty_labels[] = {};
	concurrent_tcp_metric = monitoring__register_gauge(
		"libntirpc__tcp_connections_count",
		METRIC_METADATA("TCP connections count", METRIC_UNIT_NONE),
		empty_labels, ARRAY_SIZE(empty_labels));
	register_gss_threads_metrics();
	register_svc_auth_request_latency_metric();
	register_gss_svc_auth_steps_latency_metric();
	register_gss_svc_auth_ops_latency_metric();
	initialized = true;
}
