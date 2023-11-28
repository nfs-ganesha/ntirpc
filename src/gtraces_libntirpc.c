#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#include "gsh_intrinsic.h"
#include "gtraces_libntirpc_internal.h"

#define GTRACES_LIBNTIRPC_FILE_NAME "gtraces_libntirpc"
#define GTRACES_LIBNTIRPC_RUNNING_SEQ_ID 1
#define GTRACES_LIBNTIRPC_DEFAULT_CHANNEL 0
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))

#define GTRACES_LIBNTIRPC_CHANNEL_SIZES {	\
	10 /* LOGS*/	\
	}

static Traces__Ctx *gtraces_libntirpc_ctx;
static char *gtraces_libntirpc_file = TRACES__DEFAULT_PATH GTRACES_LIBNTIRPC_FILE_NAME;

uint8_t gtraces_libntrirpc_get_default_channel(void) {
	return GTRACES_LIBNTIRPC_DEFAULT_CHANNEL;
}

Traces__Ctx *gtraces_libntirpc_get_ctx(void) {
	return gtraces_libntirpc_ctx;
}

void gtraces_print_trace_info(void) {
	generate_trace_info__generate_py_range(__start_trace_info, __stop_trace_info);
}

CurrentThread__ID_T_Expanded gtraces_libntirpc_get_trace_line_header(void) {
	return current_thread__id_expanded_init(
		0 /* node_id unavailable */,
		TID__GANESHA_CORE,
		0 /* is_verbose unavailable */,
		0 /* op_id unavailable */,
		pthread_self(),
		TID__GANESHA_CORE,
		QOS_WORKLOAD_TYPE__USER_IO
	);
}

void gtraces_libntirpc_set_dir(const char* dir_path) {
	__attribute__ ((__unused__)) static bool path_set = false;
	int len = 0;

	// Not expecting this function to be called more than once
	assert(true != path_set);

	len = strlen(dir_path) + strlen("/") + strlen(GTRACES_LIBNTIRPC_FILE_NAME) + 1;
	gtraces_libntirpc_file = (char *)malloc(len);
	snprintf(gtraces_libntirpc_file, len, "%s/%s", dir_path, GTRACES_LIBNTIRPC_FILE_NAME);

	path_set = true;
}

void gtraces_libntirpc_init(void) {
	Traces__InitParams_V traces_init_params;
	uint32_t trace_channel_sizes[] = GTRACES_LIBNTIRPC_CHANNEL_SIZES;

	assert(GTRACES_LIBNTIRPC_DEFAULT_CHANNEL < ARRAY_SIZE(trace_channel_sizes));
	traces__set_init_params(&traces_init_params,
		gtraces_libntirpc_file,
		ARRAY_SIZE(trace_channel_sizes),
		trace_channel_sizes,
		GTRACES_LIBNTIRPC_RUNNING_SEQ_ID);
	gtraces_libntirpc_ctx = traces__init_mt_global(&traces_init_params);
}
