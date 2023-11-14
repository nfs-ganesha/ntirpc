#include "gmonitoring.h"
#include "metrics_libntirpc.h"
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

/* Concurrent TCP Connections */
static gauge_metric_handle_t concurrent_tcp_metric;
static bool initialized = false;

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
	initialized = true;
}
