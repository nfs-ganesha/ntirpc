#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  volatile int a = 7;
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Func call: {}", a);

  return 0;
}
