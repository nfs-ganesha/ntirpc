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

#ifndef _STRING_LITERAL_TRACEPOINT_ARGUMENT_H__
#define _STRING_LITERAL_TRACEPOINT_ARGUMENT_H__

#include <algorithm>
#include <clang/Basic/SourceManager.h>
#include <ostream>
#include <string>

#include "TracepointArgument.h"

namespace trace_generator {

/* This class is used to represent arguments that are string literals.
 * We want to avoid the need to write the string to the binary logs every time
 * since that is very inefficient.
 *
 * In order to avoid that, we use a trick and define a new enum with a single
 * value, then we define the string that represents that enum value to be the
 * string literal. This way all the string information is kept as part of the
 * traces metadata, and all we need to write every tracepoint is the enum value.
 */
class StringLiteralTracepointArgument : virtual public TracepointArgument {
public:
  explicit StringLiteralTracepointArgument(
      const TracepointArgument &tracepointArgument, const std::string &value)
      : TracepointArgument(tracepointArgument), value_(value) {}

  virtual std::string getTpArgDefintion() const override {
    return "\t\tconst char*, unused_" + getArgName();
  }

  virtual std::string getTpFieldDefinition() const override {
    return "ctf_enum(" + getProvName() + ", " + getLttngEnumName() + ", int, " +
           getArgName() + ", " + getEnumValueName() + ")";
  }

  /* Generate TRACEPOINT_ENUM defs. For more details, see:
   * https://lttng.org/man/3/lttng-ust/#doc-tracepoint-enum */
  virtual std::string generateDefs(
      const clang::SourceManager &sourceManager,
      TraceGeneratorSourceFileCallback *sourceFileCallback) const override {
    std::ostringstream data;

    const std::string enumDefineName = getEnumDefineName();
    data << "#ifndef " << enumDefineName << "\n#define " << enumDefineName
         << "\n"
         << getEnumName() << " {\n\t" << getEnumValueName() << "\n};\n"
         << "#endif // " << enumDefineName << "\n\n";

    data << "TRACEPOINT_ENUM(\n\t" << getProvName() << ",\n\t"
         << getLttngEnumName() << ",\n\tTP_ENUM_VALUES(\n\t\tctf_enum_value(\""
         << value_ << "\", " << getEnumValueName() << ")\n\t)\n)\n\n";

    return data.str();
  }

  virtual bool operator==(const TracepointArgument &arg) const override {
    const auto *stringLiteral =
        dynamic_cast<const StringLiteralTracepointArgument *>(&arg);
    if (!stringLiteral) {
      return false;
    }

    return (TracepointArgument::operator==(arg) &&
            stringLiteral->value_ == this->value_);
  }

  virtual bool operator!=(const TracepointArgument &arg) const override {
    return !StringLiteralTracepointArgument::operator==(arg);
  }

  std::string getValue() const { return value_; }

private:
  std::string value_;

  std::string getEnumName() const {
    return "enum lttng_string_literal_" + getProvName() + "_" + getEventName() +
           "_" + getArgName();
  }

  std::string getLttngEnumName() const {
    std::string enumName = getEnumName();
    replace(enumName.begin(), enumName.end(), ' ', '_');
    return enumName;
  }

  std::string getEnumDefineName() const {
    return "__" + toUpperCase(getLttngEnumName()) + "_DEF__";
  }

  std::string getEnumValueName() const {
    return "LTTNG_STRING_LITERAL_" + toUpperCase(getProvName()) + "_" +
           toUpperCase(getEventName()) + "_" + toUpperCase(getArgName()) +
           "_VAL";
  }

  static std::string toUpperCase(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
  }
};
} // namespace trace_generator

#endif // _STRING_LITERAL_TRACEPOINT_ARGUMENT_H__