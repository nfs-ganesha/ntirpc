#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  /* We only allow specifying variable format with {}*/
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "arg1: {0}, arg2: {1}", 1, 2);

  return 0;
}