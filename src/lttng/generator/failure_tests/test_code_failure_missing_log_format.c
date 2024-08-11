#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, 1);

  return 0;
}
