#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  /* Format of type % is not supported. This will be recognized as 0 arguments,
   * even though we provide 1 */
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Trace with invalid format: %d", 1);

  return 0;
}
