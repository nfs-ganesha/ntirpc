#ifndef __TEST_CODE_H__
#define __TEST_CODE_H__

#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov1.h>
#include <auto/prov2.h>
#endif // LTTNG_PARSING

enum headerEnum { HEADER_ENUM_VAL1, HEADER_ENUM_VAL2 };

static inline void inlineFuncWithTrace() {
  AUTO_TRACEPOINT(prov1, inline_event, TRACE_INFO, "{fnc} | inline function");
}

#define MACRO_IN_HEADER_WITH_TRACEPOINT                                        \
  UNIQUE_AUTO_TRACEPOINT(prov2, event_in_header, TRACE_NOTICE,                 \
                         "{fnc} | event in header {}", 1)

void test_code2_func();

#endif // __TEST_CODE_H__