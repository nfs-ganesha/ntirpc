#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  /* This trace is not allowed, because LTTNG only allows 10 arguments at most
   * Note that the format also counts as one argument */
  AUTO_TRACEPOINT(prov, event, TRACE_INFO,
                  "Trace with too many arguments {} {} {} {} {} {} {} {} {} {}",
                  1, 2, 3, 4, 5, 6, 7, 8, 9, 10);

  return 0;
}
