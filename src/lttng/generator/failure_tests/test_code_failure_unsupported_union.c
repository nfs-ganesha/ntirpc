#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

union testUnion {
  int a;
  char b;
};

int test_func() {
  union testUnion test = {.a = 3};
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "union: {}", test);

  return 0;
}
