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

#ifndef __TRACEPOINT_INFO_H__
#define __TRACEPOINT_INFO_H__

#include <clang/Basic/SourceManager.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "ParsingError.h"
#include "StringLiteralTracepointArgument.h"
#include "TraceGeneratorSourceFileCallback.h"
#include "TracepointArgument.h"
#include "TracepointLocation.h"

namespace trace_generator {
class TracepointInfo {
public:
  explicit TracepointInfo(
      const std::string &provName, const std::string &eventName,
      std::string logLevel,
      const std::vector<std::unique_ptr<TracepointArgument>> &arguments,
      const TracepointLocation &location)
      : provName_(provName), eventName_(eventName), logLevel_(logLevel),
        arguments_(arguments), location_(location) {}

  std::string generateTracepointData(
      const clang::SourceManager &sourceManager,
      TraceGeneratorSourceFileCallback *sourceFileCallback) const {
    verifyTracepoint();

    if (getNumLttngArgs() > MAX_LTTNG_TRACE_ARGUMENTS) {
      throw ParsingError("LTTNG lib doesn't allow more than " +
                             std::to_string(MAX_LTTNG_TRACE_ARGUMENTS) +
                             " arguments per trace line, this trace has " +
                             std::to_string(getNumLttngArgs()) +
                             ". Note that variable array arguments count as 2, "
                             "for data and length",
                         location_);
    }

    std::ostringstream data;
    try {
      data << generateDefs(sourceManager, sourceFileCallback);
      data << generateTracepointEvent();
      data << generateTracepointLogLevel();
    } catch (const std::runtime_error &e) {
      throw ParsingError(e, location_);
    }

    return data.str();
  }

  bool isEqualWithoutLocation(const TracepointInfo &info) const {
    if (info.provName_ != this->provName_ ||
        info.eventName_ != this->eventName_) {
      return false;
    }

    if (info.arguments_.size() != this->arguments_.size()) {
      return false;
    }

    for (unsigned i = 0; i < info.arguments_.size(); i++) {
      if ((*info.arguments_[i].get()) != (*this->arguments_[i].get())) {
        return false;
      }
    }

    return true;
  }

  virtual bool operator==(const TracepointInfo &info) const {
    return info.location_ == this->location_ &&
           this->isEqualWithoutLocation(info);
  }

private:
  const std::string provName_;
  const std::string eventName_;
  const std::string logLevel_;
  const std::vector<std::unique_ptr<TracepointArgument>> &arguments_;
  const TracepointLocation location_;

  static const int MAX_LTTNG_TRACE_ARGUMENTS = 10;

  std::string
  generateDefs(const clang::SourceManager &sourceManager,
               TraceGeneratorSourceFileCallback *sourceFileCallback) const {
    std::ostringstream data;

    for (const auto &argument : arguments_) {
      data << argument->generateDefs(sourceManager, sourceFileCallback);
    }

    return data.str();
  }

  std::string generateTracepointEvent() const {
    std::ostringstream data;
    data << "TRACEPOINT_EVENT(\n"
         << "\t" << provName_ << ",\n"
         << "\t" << eventName_ << ",\n"
         << "\tTP_ARGS(\n";

    for (unsigned i = 0; i < arguments_.size(); i++) {
      data << arguments_.at(i)->getTpArgDefintion();

      if (i == arguments_.size() - 1) {
        data << "),\n";
      } else {
        data << ",\n";
      }
    }

    data << "\tTP_FIELDS(\n";
    for (unsigned i = 0; i < arguments_.size(); i++) {
      data << "\t\t" << arguments_.at(i)->getTpFieldDefinition() << "\n";
    }

    data << "\t)\n"
         << ")\n\n";

    return data.str();
  }

  std::string generateTracepointLogLevel() const {
    std::ostringstream data;
    data << "TRACEPOINT_LOGLEVEL(\n"
         << "\t" << provName_ << ",\n\t" << eventName_ << ",\n"
         << "\t" + logLevel_ + ")\n\n";

    return data.str();
  }

  unsigned getNumArgsInFormatString(std::string format) const {
    ParsingError invalidFormatError(
        "Invalid format: " + format +
            "\nWe currently only allow specifying parameters with "
            "{}. If you wanted to print {}, use {{}} to escape.",
        location_);

    bool inParenthesis = false;
    unsigned numArgs = 0;
    for (unsigned i = 0; i < format.size(); i++) {
      const char curr = format[i];
      const char next = (i == format.size() - 1) ? '\0' : format[i + 1];

      switch (curr) {
      case '{':
        if (next == '{') {
          // This is an escape char. Ignore this and the one after it
          i++;
          continue;
        }

        if (inParenthesis) {
          throw invalidFormatError;
        }

        inParenthesis = true;
        break;
      case '}':
        if (next == '}') {
          // This is an escape char. Ignore this and the one after it
          i++;
          continue;
        }

        if (!inParenthesis) {
          throw invalidFormatError;
        }

        numArgs++;
        inParenthesis = false;
        break;
      default:
        if (inParenthesis) {
          throw invalidFormatError;
        }
        break;
      }
    }

    if (inParenthesis) {
      // Parenthesis opened, but not closed.
      throw invalidFormatError;
    }

    return numArgs;
  }

  unsigned int getNumLttngArgs() const {
    unsigned int numArgs = 0;
    for (const auto &arg : arguments_) {
      numArgs += arg->getNumLttngArgs();
    }

    return numArgs;
  }

  /* Currently, for simplicity, we don't allow all the capabilities of python
   * format. We only allow to specify an argument in the format with {}.
   * Here we verify that the format is only given that way, and that the number
   * of arguments is appropriate */
  void verifyTracepoint() const {
    const auto *formatArg =
        dynamic_cast<const StringLiteralTracepointArgument *>(
            arguments_[0].get());
    if (!formatArg) {
      throw ParsingError("First argument is not a format? How is this possible "
                         "in this stage?!?! Format type is: " +
                             arguments_[0]->getArgType().getAsString(),
                         location_);
    }

    auto format = formatArg->getValue();
    unsigned int numArgs = getNumArgsInFormatString(format);
    // -1 for format argument
    unsigned int codeArguments = arguments_.size() - 1;
    if (numArgs != codeArguments) {
      throw ParsingError("Invalid format: " + format +
                             "\nWrong number of arguments. Format specifies " +
                             std::to_string(numArgs) +
                             " argument(s), but the code provides " +
                             std::to_string(codeArguments) + " argument(s)",
                         location_);
    }
  }
};
} // namespace trace_generator

#endif // __TRACEPOINT_INFO_H__