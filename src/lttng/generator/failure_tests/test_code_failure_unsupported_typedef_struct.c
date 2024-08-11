#include "lttng_generator.h"

#ifndef LTTNG_PARSING
#include <auto/prov.h>
#endif // LTTNG_PARSING

typedef struct _testStruct {
  int a;
} testStruct;

int test_func() {
  testStruct test = {.a = 3};
  AUTO_TRACEPOINT(prov, event, TRACE_INFO, "Typedef struct: {}", test);

  return 0;
}
