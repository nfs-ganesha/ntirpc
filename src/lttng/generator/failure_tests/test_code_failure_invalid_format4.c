#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  /* Invalid format only opening parenthesis */
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Trace with invalid format: {", 1);

  return 0;
}
