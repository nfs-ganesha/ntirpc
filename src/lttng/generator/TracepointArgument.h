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

#ifndef __TRACEPOINT_ARGUMENT_H__
#define __TRACEPOINT_ARGUMENT_H__

#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>
#include <ostream>
#include <string>

#include "TraceGeneratorSourceFileCallback.h"

namespace trace_generator {
class TracepointArgument {
public:
  explicit TracepointArgument(const std::string &provName,
                              const std::string &eventName,
                              const std::string &argName,
                              clang::QualType argType)
      : provName_(provName), eventName_(eventName), argName_(argName),
        argType_(argType) {}

  virtual ~TracepointArgument() {}

  std::string getArgName() const { return argName_; }

  clang::QualType getArgType() const { return argType_; }

  std::string getProvName() const { return provName_; };

  std::string getEventName() const { return eventName_; };

  virtual std::string getTpArgDefintion() const {
    /* We currently allow tracing of several special structs to enable tracing
     * of variable length arrays and strings.
     *
     * This must be first, so it doesn't get recognized as any other type
     */

    auto typeString = argType_.getAsString();
    if (typeString == "struct lttng_generator_variable_len_int_arr") {
      return "\t\tconst int *, " + argName_ + "_data,\n\t\tunsigned int, " +
             argName_ + "_len";
    }

    if (typeString == "struct lttng_generator_variable_len_uint_arr") {
      return "\t\tconst unsigned int *, " + argName_ +
             "_data,\n\t\tunsigned int, " + argName_ + "_len";
    }

    if (typeString == "struct lttng_generator_variable_len_byte_arr") {
      return "\t\tconst unsigned char *, " + argName_ +
             "_data,\n\t\tunsigned int , " + argName_ + "_len";
    }

    if (typeString == "struct lttng_generator_variable_len_str") {
      return "\t\tconst char *, " + argName_ + "_data,\n\t\tunsigned int, " +
             argName_ + "_len";
    }

    if (typeString == "struct lttng_generator_null_terminated_str") {
      return "\t\tconst char *, " + argName_;
    }

    auto constArrayType = llvm::dyn_cast<clang::ConstantArrayType>(argType_);
    if (constArrayType) {
      auto elementType = constArrayType->getElementType();
      auto builtinElementType = llvm::dyn_cast<clang::BuiltinType>(elementType);
      if (builtinElementType) {
        return "\t\t" + elementType.getAsString() + " *, " + argName_;
      }
    }

    if (argType_->isPointerType()) {
      return "\t\tconst void *, " + argName_;
    }

    auto enumType = llvm::dyn_cast<clang::EnumType>(argType_);
    if (enumType) {
      return std::string("\t\t") +
             (enumType->isSignedIntegerType() ? "int" : "unsigned int") + ", " +
             argName_;
    }

    return "\t\t" + argType_.getAsString() + ", " + argName_;
  }

  virtual std::string getTpFieldDefinition() const {

    /* We currently allow tracing of several special structs to enable tracing
     * of variable length arrays and strings.
     */
    auto typeString = argType_.getAsString();
    if (typeString == "struct lttng_generator_variable_len_int_arr") {
      return std::string("ctf_sequence(") + "int, " + argName_ + ", " +
             argName_ + "_data, unsigned int, " + argName_ + "_len" + ")";
    }

    if (typeString == "struct lttng_generator_variable_len_uint_arr") {
      return std::string("ctf_sequence(") + "unsigned int, " + argName_ + ", " +
             argName_ + "_data, unsigned int, " + argName_ + "_len" + ")";
    }

    if (typeString == "struct lttng_generator_variable_len_byte_arr") {
      return std::string("ctf_sequence_hex(") + "unsigned char, " + argName_ +
             ", " + argName_ + "_data, unsigned int, " + argName_ + "_len" +
             ")";
    }

    if (typeString == "struct lttng_generator_variable_len_str") {
      return std::string("ctf_sequence_text(") + "char, " + argName_ + ", " +
             argName_ + "_data, unsigned int, " + argName_ + "_len" + ")";
    }

    if (typeString == "struct lttng_generator_null_terminated_str") {
      return std::string("ctf_string(") + argName_ + ", " + argName_ + ")";
    }

    if (argType_->isPointerType()) {
      return std::string("ctf_integer_hex(") + "intptr_t" + ", " + argName_ +
             ", " + argName_ + ")";
    }

    auto enumType = llvm::dyn_cast<clang::EnumType>(argType_);
    if (enumType) {
      return std::string("ctf_enum(") + provName_ + ", " + getLttngEnumName() +
             ", " + (enumType->isSignedIntegerType() ? "int" : "unsigned int") +
             ", " + argName_ + ", " + argName_ + ")";
    }

    auto builtin = llvm::dyn_cast<clang::BuiltinType>(argType_);
    if (builtin) {
      if (argType_.getUnqualifiedType().getAsString() == "char") {
        return std::string("ctf_integer_hex(") +
               argType_.getUnqualifiedType().getAsString() + ", " + argName_ +
               ", " + argName_ + ")";
      }

      if (builtin->isInteger()) {
        return std::string("ctf_integer(") +
               argType_.getUnqualifiedType().getAsString() + ", " + argName_ +
               ", " + argName_ + ")";
      }

      if (builtin->isFloatingPoint()) {
        return std::string("ctf_float(") +
               argType_.getUnqualifiedType().getAsString() + ", " + argName_ +
               ", " + argName_ + ")";
      }
    }

    auto constArrayType = llvm::dyn_cast<clang::ConstantArrayType>(argType_);
    if (constArrayType) {
      auto elementType = constArrayType->getElementType();
      auto builtinElementType = llvm::dyn_cast<clang::BuiltinType>(elementType);
      if (builtinElementType) {
        if (builtinElementType->isCharType()) {
          return std::string("ctf_array_text(char, ") + argName_ + ", " +
                 argName_ + ", " +
                 std::to_string(constArrayType->getSize().getLimitedValue()) +
                 ")";
        }

        if (builtinElementType->isInteger()) {
          return std::string("ctf_array(") +
                 elementType.getUnqualifiedType().getAsString() + ", " +
                 argName_ + ", " + argName_ + ", " +
                 std::to_string(constArrayType->getSize().getLimitedValue()) +
                 ")";
        }
      }
    }

    argType_.dump();
    throw std::runtime_error("Argument type \"" + argType_.getAsString() +
                             "\" for arg \"" + argName_ + "\" not supported");
  }

  virtual std::string
  generateDefs(const clang::SourceManager &sourceManager,
               TraceGeneratorSourceFileCallback *sourceFileCallback) const {
    if (!argType_->isEnumeralType()) {
      return "";
    }

    return generateEnumDefs(sourceManager, sourceFileCallback);
  }

  virtual bool operator==(const TracepointArgument &arg) const {
    return (arg.argName_ == this->argName_ && arg.argType_ == this->argType_ &&
            arg.provName_ == this->provName_ &&
            arg.eventName_ == this->eventName_);
  }

  virtual bool operator!=(const TracepointArgument &arg) const {
    return (arg.argName_ != this->argName_ || arg.argType_ != this->argType_ ||
            arg.provName_ != this->provName_ ||
            arg.eventName_ != this->eventName_);
  }

  virtual unsigned int getNumLttngArgs() {
    auto typeString = argType_.getAsString();

    if (typeString == "struct lttng_generator_variable_len_int_arr" ||
        typeString == "struct lttng_generator_variable_len_uint_arr" ||
        typeString == "struct lttng_generator_variable_len_byte_arr" ||
        typeString == "struct lttng_generator_variable_len_str") {
      // Variable length structs are converted to 2 arguments - data and length
      return 2;
    }

    return 1;
  }

private:
  const std::string provName_;
  const std::string eventName_;
  const std::string argName_;
  const clang::QualType argType_;

  const clang::EnumType *getEnumType() const {
    auto enumType = llvm::dyn_cast<clang::EnumType>(argType_);
    if (!enumType) {
      throw std::runtime_error("Trying to get enum type for unsuitable type " +
                               argType_.getAsString());
    }

    return enumType;
  }

  std::string getLttngEnumName() const {
    std::string enumName = getEnumType()->getTypeClassName();
    replace(enumName.begin(), enumName.end(), ' ', '_');
    return enumName + "_" + provName_ + "_" + eventName_ + "_" + argName_;
  }

  /* Generate TRACEPOINT_ENUM defs. For more details, see:
   * https://lttng.org/man/3/lttng-ust/#doc-tracepoint-enum */
  std::string generateEnumDefs(
      const clang::SourceManager &sourceManager,
      const TraceGeneratorSourceFileCallback *sourceFileCallback) const {
    std::ostringstream data;

    auto defineFileLoc =
        getEnumType()->getDecl()->getDefinition()->getLocation();
    const auto defineFileFullPath =
        sourceManager.getFilename(sourceManager.getExpansionLoc(defineFileLoc))
            .str();
    const auto defineFileRelativePath =
        sourceFileCallback->getRelativeHeaderPath(defineFileFullPath);

    data << "TRACEPOINT_ENUM(\n\t" << getProvName() << ",\n\t"
         << getLttngEnumName() << ",\n\tTP_ENUM_VALUES(\n";

    for (auto enumerator :
         getEnumType()->getDecl()->getDefinition()->enumerators()) {
      data << "\t\tctf_enum_value(\"" << enumerator->getNameAsString() << "\", "
           << std::to_string(enumerator->getInitVal().getLimitedValue())
           << ")\n";
    }

    data << "\t)\n)\n\n";
    return data.str();
  }

  static bool endsWith(const std::string &str, const std::string &suffix) {
    if (str.length() < suffix.length()) {
      return false;
    }

    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
  }

  static bool isHeader(const std::string &file) {
    return endsWith(file, ".h") || endsWith(file, ".hpp");
  }
};
} // namespace trace_generator

#endif // __TRACEPOINT_ARGUMENT_H__