#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

static int foo() { return 3; }

int test_func() {
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Func call: {}", foo());

  return 0;
}
