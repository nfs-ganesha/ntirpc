#ifndef _GTRACES_LIBNTIRPC_H_
#define _GTRACES_LIBNTIRPC_H_

#include "infra/traces/generate_trace_info_api.h"
#include "infra/traces/traces_api.h"

#define current_thread__traces_channel_get gtraces_libntrirpc_get_default_channel
#define current_thread__id_get gtraces_libntirpc_get_trace_line_header
#define traces__get_thread_state gtraces_libntirpc_get_ctx

typedef struct _gtraces_line_header {
    pthread_t t;
} gtraces_libntirpc_line_header;

void gtraces_libntirpc_init(void);
uint8_t gtraces_libntrirpc_get_default_channel(void);
Traces__Ctx *gtraces_libntirpc_get_ctx(void);
gtraces_libntirpc_line_header gtraces_libntirpc_get_trace_line_header(void);

#endif /* _GTRACES_LIBNTIRPC_H_ */
