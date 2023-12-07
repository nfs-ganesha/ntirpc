#include "gmonitoring.h"
#include "metrics_libntirpc.h"
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <misc/timespec.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

/* Concurrent TCP Connections */
static gauge_metric_handle_t concurrent_tcp_metric;
static bool initialized = false;

/* For each security-flavor as an array index, assign a serial number to be
 * represented as the index in the metrics array.
 * The serial number `0` is not assigned, since un-mentioned security-flavors
 * will have the default `0` value in the array. Such entries will not be
 * represented in the metrics array.
 */
static const uint8_t sec_flavors_idx[] = {
	[AUTH_NONE] = 1, [AUTH_SYS] = 2, [RPCSEC_GSS] = 3
};

/* For each auth-stat as an array index, assign a serial number to be
 * represented as the index in the metrics array.
 * The serial number `0` is not assigned, since un-mentioned auth-stats
 * will have the default `0` value in the array. Such entries will not be
 * represented in the metrics array.
 */
static const uint8_t auth_stats_idx[] = {
	[AUTH_OK] = 1, [AUTH_BADCRED] = 2, [AUTH_REJECTEDCRED] = 3,
	[AUTH_BADVERF] = 4, [AUTH_REJECTEDVERF] = 5, [AUTH_TOOWEAK] = 6,
	[AUTH_INVALIDRESP] = 7, [AUTH_FAILED] = 8, [RPCSEC_GSS_CREDPROBLEM] = 9,
	[RPCSEC_GSS_CTXPROBLEM] = 10 };

/* Latency of authentication requests for each sec-flavor and auth-status */
static histogram_metric_handle_t
	svc_auth_request_latency[ARRAY_SIZE(sec_flavors_idx)][ARRAY_SIZE(auth_stats_idx)];


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
	default:
		__warnx(TIRPC_DEBUG_FLAG_ERROR,
			"%s: Unsupported security flavor value: %d",
			__func__, sec_flavor);
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
			"%s: Unsupported auth-stat value: %d",
			__func__, auth_status);
		abort();
	}
}

static void register_svc_auth_request_latency_metric(void) {
	int sf, as;

	for (sf = 0; sf < ARRAY_SIZE(sec_flavors_idx); sf++) {
		const int sf_idx = get_sec_flavor_idx_in_metrics_array(sf);

		/* Do not register un-mentioned values in sec-flavors array */
		if (sf_idx < 0)
			continue;

		const char *const sec_flavor = get_sec_flavor_string(sf);

		for (as = 0; as < ARRAY_SIZE(auth_stats_idx); as++) {
			const int as_idx = get_auth_stat_idx_in_metrics_array(as);

			/* Do not register un-mentioned values in auth-stats array */
			if (as_idx < 0)
				continue;

			const metric_label_t labels[] = {
				GMONITORING_INIT_LABEL("sec_flavor", sec_flavor),
				GMONITORING_INIT_LABEL("auth_status", get_auth_stat_string(as))
			};

			svc_auth_request_latency[sf_idx][as_idx] =
				gmonitoring_register_histogram_metric(
					"libntirpc__svc_auth_request_latency",
					GMONITORING_INIT_METADATA(
						"Distribution of time taken by an authentication request to complete",
						GMONITORING_UNIT_MICROSECOND),
					labels,
					ARRAY_SIZE(labels),
					gmonitoring_get_exp2_histogram_buckets_compact());
		}
	}
}

void metrics_libntirpc_observe_svc_auth_request_latency(int sec_flavor,
	enum auth_stat auth_status, const struct timespec *latency)
{
	const int sf_idx = get_sec_flavor_idx_in_metrics_array(sec_flavor);
	assert(sf_idx >= 0);
	const int as_idx = get_auth_stat_idx_in_metrics_array(auth_status);
	assert(as_idx >= 0);
	gmonitoring_observe_histogram_metric_value(
		svc_auth_request_latency[sf_idx][as_idx], timespec_us(latency));
}

void metrics_libntirpc_update_tcp_connection_count(int connection_count)
{
	gmonitoring_set_gauge_metric_value(concurrent_tcp_metric, connection_count);
}

void metrics_libntirpc_init(void){
	const metric_label_t empty_labels[] = {};
	assert(initialized == false);
	concurrent_tcp_metric = gmonitoring_register_gauge_metric(
			"libntirpc__tcp_connections_count",
			GMONITORING_INIT_METADATA("TCP connections count", GMONITORING_UNIT_NONE), empty_labels,
			ARRAY_SIZE(empty_labels));

	register_svc_auth_request_latency_metric();

	initialized = true;
}
