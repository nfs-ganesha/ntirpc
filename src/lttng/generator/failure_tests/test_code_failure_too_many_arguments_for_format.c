#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "arg1: {}, arg2: {}", 1, 2, 3);

  return 0;
}