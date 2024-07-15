#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  int a = 4;
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Func call: {}", a++);

  return 0;
}
