// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright 2024 Google LLC
 * contributeur : Shahar Hochma shaharhoch@google.com
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * ---------------------------------------
 */

#ifndef LTTNG_GENERATOR_H
#define LTTNG_GENERATOR_H

#include <lttng/tracepoint.h>

#define LTTNG_GENERATOR_CONCAT_IMPL(x, y) x##y
#define LTTNG_GENERATOR_MACRO_CONCAT(x, y) LTTNG_GENERATOR_CONCAT_IMPL(x, y)
#define LTTNG_GENERATOR_STRINGIFY(x) #x

/* __COMPILATION_UNIT_HASH__ is Injected in compilation by generate_cmake_rules
 * If you don't use  generate_cmake_rules don't use UNIQUE_AUTO_TRACEPOINT,
 * or inject __COMPILATION_UNIT_HASH__ in some other way */
#define GLOBAL_UNIQUE_COUNTER                                                  \
  LTTNG_GENERATOR_MACRO_CONCAT(__COMPILATION_UNIT_HASH__, __COUNTER__)

struct lttng_generator_variable_len_int_arr {
  const int *data;
  unsigned int len;
};

struct lttng_generator_variable_len_uint_arr {
  const unsigned int *data;
  unsigned int len;
};

struct lttng_generator_variable_len_byte_arr {
  const unsigned char *data;
  unsigned int len;
};

struct lttng_generator_variable_len_str {
  const char *data;
  unsigned int len;
};

struct lttng_generator_null_terminated_str {
  const char *str;
};

/* Define tracepoint wrapper macro. Normally this will just point to lttng
 * tracepoint. But when we parse the code for auto event generation it will
 * point to another function.
 * This is needed  because the code will not compile without the lttng
 * declarations, and we need the code to compile for auto generation parsing */

#ifdef LTTNG_PARSING
static void inline lttng_empty_tracepoint_for_parsing(const char *prov_name,
                                                      const char *event_name,
                                                      int log_level,
                                                      const char *format, ...) {
}

#define AUTO_TRACEPOINT(prov_name, event_name, log_level, format, ...)         \
  lttng_empty_tracepoint_for_parsing(LTTNG_GENERATOR_STRINGIFY(prov_name),     \
                                     LTTNG_GENERATOR_STRINGIFY(event_name),    \
                                     log_level, format, ##__VA_ARGS__)

#define TP_INT_ARR(_data, _len)                                                \
  ((struct lttng_generator_variable_len_int_arr){.data = (_data),              \
                                                 .len = (_len)})
#define TP_UINT_ARR(_data, _len)                                               \
  ((struct lttng_generator_variable_len_uint_arr){.data = (_data),             \
                                                  .len = (_len)})
#define TP_BYTE_ARR(_data, _len)                                               \
  ((struct lttng_generator_variable_len_byte_arr){.data = (_data),             \
                                                  .len = (_len)})
#define TP_VAR_STR_ARR(_data, _len)                                            \
  ((struct lttng_generator_variable_len_str){.data = (_data), .len = (_len)})

/* This is used for a null terminated string. For a variable length char
 * array use TP_VAR_STR_ARR() */
#define TP_STR(_str) ((struct lttng_generator_null_terminated_str){.str = _str})

#else // LTTNG_PARSING
#define AUTO_TRACEPOINT(prov_name, event_name, log_level, ...)                 \
  do {                                                                         \
    __attribute__((__unused__)) int _unused_log_level_tracepoint = log_level;  \
    tracepoint(prov_name, event_name, ##__VA_ARGS__);                          \
  } while (0)

#define TP_INT_ARR(_data, _len) (_data), (_len)
#define TP_UINT_ARR(_data, _len) (_data), (_len)
#define TP_BYTE_ARR(_data, _len) (_data), (_len)
#define TP_VAR_STR_ARR(_data, _len) (_data), (_len)

/* This is used for a null terminated string. For a variable length char
 * array use TP_VAR_STR_ARR() */
#define TP_STR(_str) (_str)

#endif // LTTNG_PARSING

/* Use UNIQUE_AUTO_TRACEPOINT when the tracepoint might be called for more than
 * a single location. This will generate a unique event name for each
 * location.
 * The only downside of this is that the resulting event name will have an added
 * hash at the end - event_name_<hash>*/
#define UNIQUE_AUTO_TRACEPOINT(prov_name, event_name, format, ...)             \
  AUTO_TRACEPOINT(                                                             \
      prov_name,                                                               \
      LTTNG_GENERATOR_MACRO_CONCAT(                                            \
          LTTNG_GENERATOR_MACRO_CONCAT(event_name, _), GLOBAL_UNIQUE_COUNTER), \
      format, ##__VA_ARGS__)

#endif // LTTNG_GENERATOR_H
