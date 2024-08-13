#include <float.h>
#include <limits.h>
#include <stdint.h>

#include "lttng_generator.h"
#include "test_code.h"

#ifndef LTTNG_PARSING
#include <auto/prov1.h>
#include <auto/prov2.h>
#include <auto/prov3.h>
#endif // LTTNG_PARSING

#define CONCAT(a, b) a##b

int global_var = 1;

#define MACRO_WITH_TRACEPOINT                                                  \
  UNIQUE_AUTO_TRACEPOINT(prov1, generated_event, TRACE_NOTICE,                 \
                         "{fnc} | Test event {}", 1)

enum testEnum { TEST_VAL1, TEST_VAL2 };

struct testStruct {
  int num;
};

typedef int IntTypedef;

static int funcWithTracepoint() {
  int num = 10;
  AUTO_TRACEPOINT(prov3, event_in_func, TRACE_INFO,
                  "{fnc} | This is a tracepoint inside another function. {}",
                  num);
  return num;
}

int main(void) {
  AUTO_TRACEPOINT(prov1, event_no_arg, TRACE_ERR,
                  "This is a tracepoint with no arguments | {fnc}");
  int int_var = 5;
  char char_var = 'a';
  char char_arr[] = "const size char array";
  char *char_ptr = (char *)0x333;
  char **double_ptr = (char **)0x444;
  AUTO_TRACEPOINT(
      prov2, event1, TRACE_CRIT,
      "int literal: {}, string literal: {}, int var: {}, char "
      "var: {}, char_array: {}, char pointer: {}, double pointer: {}",
      1, "Test string literal", int_var, char_var, char_arr, char_ptr,
      double_ptr);

  float f = FLT_MIN;
  double d = DBL_MAX;
  long l = LONG_MAX;
  unsigned long ul = ULLONG_MAX;
  long long ll = LLONG_MAX;
  unsigned long long ull = ULLONG_MAX;
  short s = SHRT_MIN;
  unsigned short us = USHRT_MAX;
  AUTO_TRACEPOINT(
      prov2, event2, TRACE_CRIT,
      "{fnc} | float: {}, double: {}, long: {}, ulong: {}, long long: {}, "
      "unsigned long log: {}, short: {}, ushort: {}",
      f, d, l, ul, ll, ull, s, us);

  struct testStruct strct = {.num = 8};
  struct testStruct *strct_ptr = (struct testStruct *)0x444;
  AUTO_TRACEPOINT(prov1, event1, TRACE_WARNING,
                  "enum: {}, int: {}, struct pointer: {}, result: {}",
                  TEST_VAL1, strct.num, strct_ptr, strct.num + 1);

  int arr[] = {20, -21, 22, -23};
  AUTO_TRACEPOINT(prov2, event_arrays, TRACE_ALERT,
                  "variable len string: {}, variable len int array: {}, "
                  "terminated string: {}",
                  TP_VAR_STR_ARR("var_len_str", 11),
                  TP_INT_ARR(arr, sizeof(arr) / sizeof(arr[0])),
                  TP_STR("null terminated str"));

  unsigned int uarr[] = {30, 31, 32, 33};
  unsigned char byte_arr[] = {0x1, 0x2, 0x3};
  AUTO_TRACEPOINT(prov2, event_arrays2, TRACE_EMERG,
                  "variable len uint array: {}, "
                  "byte array: {}",
                  TP_UINT_ARR(uarr, sizeof(uarr) / sizeof(uarr[0])),
                  TP_BYTE_ARR(byte_arr, sizeof(byte_arr)));

  /* Call macro twice to see that it works */
  MACRO_WITH_TRACEPOINT;
  MACRO_WITH_TRACEPOINT;

  (void)funcWithTracepoint();
  inlineFuncWithTrace();

  enum testEnum e = TEST_VAL1;
  /* Unfortunately, I don't see a way to recognize the type of const enum value,
   * and it will be represented as int */
  AUTO_TRACEPOINT(prov1, event_enum, TRACE_DEBUG, "enum1: {}, enum2: {}", e,
                  TEST_VAL2);

  enum headerEnum header_enum = HEADER_ENUM_VAL1;
  AUTO_TRACEPOINT(prov1, event_header_enum, TRACE_NOTICE,
                  "enum1: {}, enum2: {}", header_enum, HEADER_ENUM_VAL2);

  AUTO_TRACEPOINT(prov1, special_string, TRACE_NOTICE,
                  "format with quotes: \\\"\\\"");

  /* Note that the format also counts as one argument */
  AUTO_TRACEPOINT(prov1, max_args, TRACE_NOTICE,
                  "Tracepoint with max args: {} {} {} {} {} {} {} {} {}", 1, 2,
                  3, 4, 5, 6, 7, 8, 9);

  AUTO_TRACEPOINT(prov1, curly_brackets, TRACE_NOTICE,
                  "Tracepoint with curly brackets: {{x}} {{{}}}, {}", 1, 2);

  AUTO_TRACEPOINT(prov1, max_uint64, TRACE_NOTICE, "Max uint64: {}",
                  UINT64_MAX);

  AUTO_TRACEPOINT(prov1, unicode, TRACE_NOTICE, "Unicode char: 😋");

  IntTypedef td = 5;
  AUTO_TRACEPOINT(prov1, type_def, TRACE_NOTICE, "Typedef: {}", td);

  const int cnst = 1;
  static char stat = 6;
  static const int sc = 7;
  AUTO_TRACEPOINT(prov1, qualifiers, TRACE_NOTICE,
                  "const: {}, static: {}, static const: {}", cnst, stat, sc);

  const int ca[] = {1, 2, 3};
  int arr1[] = {4, 5, 6};
  static int sa[] = {4, 5, 6};
  static const int sca[] = {8, 9, 10};
  AUTO_TRACEPOINT(
      prov1, array_qualifiers, TRACE_NOTICE,
      "const arr: {}, array: {}, static array: {}, static const array: {}", ca,
      arr1, sa, sca);

  int CONCAT(var_, 1) = 3;
  AUTO_TRACEPOINT(prov1, preprocessor, TRACE_NOTICE, "Preprocessor: {}",
                  CONCAT(var_, 1));

  AUTO_TRACEPOINT(prov1, fnc_test, TRACE_NOTICE,
                  "fnc several times: {fnc} {fnc} {fnc}");

  int arr3[] = {1, 2, 3};
  int empty_arr[] = {};
  AUTO_TRACEPOINT(prov1, empty, TRACE_NOTICE,
                  "empty arr: {}, empty var len arr: {}, empty str: {}, empty "
                  "val len str: {}",
                  empty_arr, TP_INT_ARR(arr3, 0), TP_STR(""),
                  TP_VAR_STR_ARR("aaa", 0));

  /* Trace generator doesn't allow leading underscore in variable name. This is
   * because lttng uses underscore to mark an array length variable. We test
   * this here */
  int _a = 3;
  int __a = 4;
  int ___a = 5;
  AUTO_TRACEPOINT(prov1, underscore_vars, TRACE_NOTICE,
                  "Underscore vars: {} {} {}", _a, __a, ___a);

  test_code2_func();

  return 0;
}
