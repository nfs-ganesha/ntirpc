#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

int test_func() {
  /* This trace is not allowed, because LTTNG only allows 10 arguments at most
   * Note that the format also counts as one argument.
   * This has 4 variable arrays, so 8 arguments, then the format and 2 other
   * regular arguments. */
  AUTO_TRACEPOINT(prov, event, TRACE_INFO,
                  "Trace with too many arguments {} {} {} {} {} {}",
                  TP_INT_ARR(NULL, 0), TP_UINT_ARR(NULL, 0),
                  TP_BYTE_ARR(NULL, 0), TP_VAR_STR_ARR(NULL, 0), 1, 2);

  return 0;
}
