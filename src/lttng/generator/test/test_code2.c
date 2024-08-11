#include "lttng_generator.h"
#include "test_code.h"

#ifndef LTTNG_PARSING
#include <auto/prov1.h>
#endif // LTTNG_PARSING

extern int global_var;

void test_code2_func() {
  AUTO_TRACEPOINT(prov1, event_test2, TRACE_INFO,
                  "{fnc} | This is an event in the same provider but other "
                  "compilation unit");

  AUTO_TRACEPOINT(prov1, global_var, TRACE_INFO, "Global var is: {}",
                  global_var);
}