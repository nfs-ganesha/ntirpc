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

#ifndef __TRACE_GENERATOR_H__
#define __TRACE_GENERATOR_H__

#include <clang/AST/Expr.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/SourceManager.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ParsingError.h"
#include "StringLiteralTracepointArgument.h"
#include "TraceGeneratorSourceFileCallback.h"
#include "TracepointArgument.h"
#include "TracepointInfo.h"
#include "TracepointLocation.h"

namespace trace_generator {

using clang::ast_matchers::MatchFinder;
using std::filesystem::path;

class TraceGenerator : public MatchFinder::MatchCallback {
public:
  explicit TraceGenerator(const path &outputDirPath,
                          std::optional<std::string> provider,
                          const std::string &tracepointCallMatchId,
                          TraceGeneratorSourceFileCallback *sourceFileCallback)
      : outputDirPath_(outputDirPath), provider_(provider),
        tracepointCallMatchId_(tracepointCallMatchId),
        sourceFileCallback_(sourceFileCallback) {}

  virtual void run(const MatchFinder::MatchResult &result) override {
    auto call = result.Nodes.getNodeAs<clang::CallExpr>(tracepointCallMatchId_);
    if (!call) {
      return;
    }

    const auto &sourceManager = result.Context->getSourceManager();
    const auto location =
        getLocation(result.Context->getSourceManager(), call->getBeginLoc());

    if (!lttngGeneratorHeaderPath_.has_value()) {
      /* We get the location of the declaration of our target function. This the
       * path to lttng_generator.h. We only need to get it once. */
      lttngGeneratorHeaderPath_ =
          getLocation(sourceManager,
                      call->getCalleeDecl()->getCanonicalDecl()->getLocation())
              .getFilePath();
    }

    // Provider, event, log level, format
    static const int NUMBER_OF_REQUIRED_ARGS = 4;
    if (call->getNumArgs() < NUMBER_OF_REQUIRED_ARGS) {
      throw ParsingError("Call to tracepoint must have at least 4 arguments - "
                         "Provider, event, log level and format",
                         location);
    }

    const clang::StringLiteral *literalProv =
        tryParseStringLiteralArg(call->getArg(0));
    if (!literalProv) {
      throw ParsingError("1st argument must be a provider name", location);
    }
    const std::string provName = literalProv->getString().str();

    if (provider_.has_value() && provider_.value() != provName) {
      // We got to a trace with a different provider. Skip it.
      return;
    }

    const clang::StringLiteral *literalEvent =
        tryParseStringLiteralArg(call->getArg(1));
    if (!literalEvent) {
      throw ParsingError("2nd argument must be an event name", location);
    }
    std::string eventName = literalEvent->getString().str();

    if (lookupEvent(provName, eventName, location) == EXISTING_EVENT) {
      return;
    }

    const std::string logLevel = getLogLevelFromArg(call->getArg(2), location);

    const clang::StringLiteral *formatLiteral =
        tryParseStringLiteralArg(call->getArg(3));
    if (!formatLiteral) {
      throw ParsingError("4th argument must be a string literal format",
                         location);
    }

    auto callerFunction = getFunctionCallerName(
        result.Context, clang::DynTypedNode::create(*call), location);
    auto formatString = insertCallingFunctionToFormat(
        formatLiteral->getString().str(), callerFunction);

    auto formatArgType = getArgType(formatLiteral);
    auto formatArg = std::make_unique<StringLiteralTracepointArgument>(
        TracepointArgument(provName, eventName, "format", formatArgType),
        formatString);

    std::vector<std::unique_ptr<TracepointArgument>> arguments;
    arguments.push_back(std::move(formatArg));

    for (unsigned i = NUMBER_OF_REQUIRED_ARGS; i < call->getNumArgs(); i++) {
      const unsigned argNumber = i - NUMBER_OF_REQUIRED_ARGS + 1;
      auto arg = call->getArg(i);
      if (arg->HasSideEffects(*result.Context)) {
        throw ParsingError(
            "Argument " + std::to_string(argNumber) +
                " has side effects. We don't "
                "allow arguments with side effects because we found "
                "that old versions of LTTNG "
                "might call it more than once in the tracepoint "
                "macro expansion. Note that reading a volatile variable is "
                "also not allowed.",
            location);
      }

      const std::string defaultArgName = "arg_" + std::to_string(argNumber);
      arguments.push_back(
          getTracepointArgument(provName, eventName, arg, defaultArgName));
    }

    TracepointInfo info(provName, eventName, logLevel, arguments, location);

    auto &outputStream = getOutputFileStream(provName, location);
    outputStream << info.generateTracepointData(sourceManager,
                                                sourceFileCallback_);
  }

  void close() {
    for (auto &pair : providerToOutput_) {
      auto providerName = pair.first;
      auto &outputStream = pair.second;

      outputStream << getFileEnding(providerName);
      outputStream.close();

      std::cout << "Generated LTTNG traces in file: "
                << getOutputFilePath(providerName) << "\n";
    }

    providerToOutput_.clear();
  }

  ~TraceGenerator() { close(); }

private:
  const path outputDirPath_;
  const std::optional<std::string> provider_;
  const std::string tracepointCallMatchId_;
  std::unordered_map<std::string /* provider name */, std::ofstream>
      providerToOutput_;
  std::unordered_map<
      std::string /* provider name */,
      std::unordered_map<std::string /* event name*/, TracepointLocation>>
      eventMap_;
  std::optional<std::string> lttngGeneratorHeaderPath_;
  TraceGeneratorSourceFileCallback *sourceFileCallback_;

  path getOutputFilePath(const std::string &providerName) const {
    return outputDirPath_ / (providerName + ".h");
  }

  enum LookupEventResult { NEW_EVENT, EXISTING_EVENT };

  enum LookupEventResult lookupEvent(std::string providerName,
                                     std::string eventName,
                                     const TracepointLocation &location) {
    auto &eventsForProvider = eventMap_[providerName];

    const auto &locationIter = eventsForProvider.find(eventName);
    if (locationIter == eventsForProvider.end()) {
      // First time we see this event. Add it to the map
      (void)eventsForProvider.insert({eventName, location});
      return NEW_EVENT;
    }
    const auto &existingLocation = locationIter->second;

    /* TODO: Improve handling in case we use the same event in two different
     * locations. I think we can just add a bypass in case someone is creating
     * an event without location info, or provides the location info as a
     * dynamic variable.
     * UNIQUE_AUTO_TRACEPOINT mostly solves this issue, but doesn't
     * allow for a case where someone wants to have the same exact event name */
    if (existingLocation != location) {
      throw std::runtime_error(
          "We found the same event twice in two different "
          "locations. This is not allowed, because usually trace lines contain "
          "location info (like file name), which is encoded as a constant as "
          "part of the trace info for efficiency. We have a TODO to improve "
          "this, but for now, see if UNIQUE_AUTO_TRACEPOINT can "
          "help you resolve this issue. \nProvider: " +
          providerName + ", Event: " + eventName + ", location1: " +
          existingLocation.toString() + ", location2: " + location.toString());
    }

    return EXISTING_EVENT;
  }

  std::ofstream &getOutputFileStream(const std::string &providerName,
                                     const TracepointLocation &location) {
    auto streamIter = providerToOutput_.find(providerName);
    if (streamIter != providerToOutput_.end()) {
      return streamIter->second;
    }

    // Output file stream doesn't exist, we need to create it
    auto &outputStream = providerToOutput_[providerName];

    const std::string outputPath = getOutputFilePath(providerName).string();
    outputStream.open(outputPath, std::ios_base::out | std::ios_base::trunc);
    if (outputStream.fail()) {
      throw ParsingError("Failed to open file " + outputPath, location);
    }

    outputStream << getProviderFileBeginning(providerName);

    return outputStream;
  }

  static std::string getProviderFileDefine(std::string providerName) {
    return "__LTTNG_GENERATOR_" + toUpperCase(providerName) + "_H__";
  }

  std::string getProviderFileBeginning(std::string providerName) const {
    const std::string headerDefine = getProviderFileDefine(providerName);
    const std::string tracepointHeaderPath =
        sourceFileCallback_->getRelativeHeaderPath(
            getOutputFilePath(providerName));

    /* We add some constant expressions needed by LTTNG at the start of the
     * header file. This starts some #ifdefs what will be closed when we write
     * the end of the file (see getFileEnding())  */
    std::ostringstream data;
    data << "/* This is an autogenerated file, generated by lttng trace "
            "generator.\n"
            " * Do not edit it directly as it will be overriden the next time "
            "it is generated.\n"
            " * For more info see the generator README file. */\n\n"
         << "#ifndef LTTNG_PARSING\n\n"
         << "#undef TRACEPOINT_PROVIDER\n"
         << "#define TRACEPOINT_PROVIDER " << providerName << "\n\n"
         << "#undef TRACEPOINT_INCLUDE\n"
         << "#define TRACEPOINT_INCLUDE \"" << tracepointHeaderPath << "\"\n\n"
         << "#if !defined(" << headerDefine
         << ") || defined(TRACEPOINT_HEADER_MULTI_READ)\n"
         << "#define " << headerDefine << "\n\n"
         << "#include <lttng/tracepoint.h>\n"
         << "#include <stdint.h>\n";

    /* We include the lttng_generator.h for the file to be aware of the structs
     * it defines. */
    const std::string lttngGeneratorRelativeHeader =
        sourceFileCallback_->getRelativeHeaderPath(
            lttngGeneratorHeaderPath_.value());
    data << "#include \"" << lttngGeneratorRelativeHeader
         << "\" /* include:optional */\n\n";

    return data.str();
  }

  std::string getFileEnding(std::string providerName) {
    const std::string headerDefine = getProviderFileDefine(providerName);
    std::ostringstream data;

    data << "#endif // " << headerDefine << "\n\n"
         << "#include <lttng/tracepoint-event.h>\n\n"
         << "#endif // LTTNG_PARSING\n";

    return data.str();
  }

  static std::unique_ptr<TracepointArgument>
  getTracepointArgument(const std::string &provName,
                        const std::string &eventName, const clang::Expr *arg,
                        const std::string &defaultArgName) {
    clang::QualType argType = getArgType(arg);
    std::string argName = getArgName(arg, defaultArgName);

    const clang::StringLiteral *stringLiteral = tryParseStringLiteralArg(arg);
    if (stringLiteral) {
      return std::make_unique<StringLiteralTracepointArgument>(
          TracepointArgument(provName, eventName, argName, argType),
          stringLiteral->getString().str());
    }

    return std::make_unique<TracepointArgument>(provName, eventName, argName,
                                                argType);
  }

  /* Arguments might go through implicit cast before the reach a function call.
   * Implicit casting is where you provide a type that doesn't exactly fit the
   * function call, but the compiler knows it's good enough, so it casts it
   * without you asking.
   * In C++ it can happen if you have a non explicit constructor between the
   * types, and in C it can happen when you cast a smaller integer to a bigger
   * one (for example, uint16_t to uint32_t), or when you provide an enum
   * instead of an integer argument.
   *
   * In our case, we are specifically interested in the case of an enum.
   * If we don't remove the implicit casting, every enum will look like an
   * integer. */
  static const clang::Expr *removeImplicitCast(const clang::Expr *arg) {
    auto implicitCast = llvm::dyn_cast<clang::ImplicitCastExpr>(arg);
    if (implicitCast) {
      return removeImplicitCast(implicitCast->getSubExpr());
    }

    return arg;
  }

  static const clang::StringLiteral *
  tryParseStringLiteralArg(const clang::Expr *arg) {
    return llvm::dyn_cast<clang::StringLiteral>(removeImplicitCast(arg));
  }

  static std::string getLogLevelFromArg(const clang::Expr *arg,
                                        TracepointLocation location) {
    const auto *argNoCast = removeImplicitCast(arg);
    auto declRef = llvm::dyn_cast<clang::DeclRefExpr>(argNoCast);
    if (!declRef) {
      argNoCast->dump();
      throw ParsingError(
          "Invalid log level argument - should be log level enum value",
          location);
    }

    auto enumConstDecl =
        llvm::dyn_cast<clang::EnumConstantDecl>(declRef->getDecl());
    if (!enumConstDecl) {
      argNoCast->dump();
      throw ParsingError(
          "Invalid log level argument - should be log level enum value",
          location);
    }

    return enumConstDecl->getNameAsString();
  }

  /* This gets the underlying canonical type of an argument. Removing any
   * typedefs.  */
  static clang::QualType desugarType(clang::QualType type) {
    auto elaborated = llvm::dyn_cast<clang::ElaboratedType>(type);
    if (elaborated) {
      return desugarType(elaborated->desugar());
    }

    auto typeDef = llvm::dyn_cast<clang::TypedefType>(type);
    if (typeDef) {
      return desugarType(typeDef->desugar());
    }

    return type;
  }

  static clang::QualType getArgType(const clang::Expr *arg) {
    arg = removeImplicitCast(arg);

    auto declRef = llvm::dyn_cast<clang::DeclRefExpr>(arg);
    if (declRef) {
      auto enumDecl =
          llvm::dyn_cast<clang::EnumDecl>(declRef->getDecl()->getDeclContext());
      if (enumDecl) {
        return clang::QualType(enumDecl->getTypeForDecl(), 0);
      }
    }

    return desugarType(arg->getType());
  }

  static std::string getArgName(const clang::Expr *arg,
                                const std::string &defaultName) {
    arg = removeImplicitCast(arg);

    auto declRef = llvm::dyn_cast<clang::DeclRefExpr>(arg);
    if (declRef) {
      if (llvm::isa<clang::EnumConstantDecl>(declRef->getDecl())) {
        /* This is a constant enum, we don't want to use the name of the enum as
         * the name of the arg */
        return defaultName;
      }

      auto name = declRef->getNameInfo().getName().getAsString();
      if (name.empty()) {
        return defaultName;
      }

      /* We don't allow a leading underscore in variable name. This makes it
       * possible for us to parse arrays when formatting. Since an array
       * variable is converted by LTTNG to two variables, one for length that
       * starts with underscore and the other is the actual data. We need to
       * know that the arg name cannot start with underscore to know identify an
       * array for sure. */
      if (name[0] == '_') {
        return defaultName;
      }

      return name;
    }

    /* Some arguments don't have a "name", for example if it is a function call,
     * or just a literal value. In this case return the default name */
    return defaultName;
  }

  static std::string toUpperCase(std::string str) {
    transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
  }

  static std::string getFunctionCallerName(clang::ASTContext *context,
                                           clang::DynTypedNode node,
                                           const TracepointLocation &location) {
    auto parents = context->getParents(node);
    for (const auto parent : parents) {
      auto funcDecl = parent.get<clang::FunctionDecl>();
      if (funcDecl) {
        return funcDecl->getNameAsString();
      }

      return getFunctionCallerName(context, parent, location);
    }

    return std::string();
  }

  static TracepointLocation
  getLocation(const clang::SourceManager &sourceManager,
              const clang::SourceLocation &sourceLocation) {
    const auto filePath =
        sourceManager.getFilename(sourceManager.getExpansionLoc(sourceLocation))
            .str();
    const auto line = sourceManager.getExpansionLineNumber(sourceLocation);
    return TracepointLocation(filePath, line);
  }

  static std::string
  insertCallingFunctionToFormat(const std::string &format,
                                const std::string &funcName) {
    /* The calling function cannot be added to the format in compile time, since
     * the macro __func__ is not a string literal (see
     * https://shorturl.at/YCqin), so this must be added manually as a field
     * through the parsing tool.
     * For this reason, we add an option to add {fnc} to the format string and
     * we replace it here with the calling function name */

    static constexpr char FUNCTION_FORMAT[] = "{fnc}";

    std::string out = format;
    while (true) {
      auto index = out.find(FUNCTION_FORMAT);
      if (index == std::string::npos) {
        break;
      }

      out.replace(index, strlen(FUNCTION_FORMAT), funcName);
    }

    return out;
  }
};
} // namespace trace_generator

#endif // __TRACE_GENERATOR_H__