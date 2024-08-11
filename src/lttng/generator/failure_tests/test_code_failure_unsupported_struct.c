#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

struct testStruct {
  int a;
};

int test_func() {
  struct testStruct test = {.a = 3};
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "struct: {}", test);

  return 0;
}
