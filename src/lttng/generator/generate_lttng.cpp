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

#include "TraceGenerator.h"
#include "TraceGeneratorSourceFileCallback.h"
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace trace_generator;
using std::filesystem::path;

const std::string
    LTTNG_TRACEPOINT_FUNC_NAME("lttng_empty_tracepoint_for_parsing");

const std::string TRACEPOINT_CALL_MATCH_ID = "tracepointCall";
const std::vector<std::string> BUILD_ARGS_TO_ADD = {"-DLTTNG_PARSING",
                                                    "-Wno-everything"};

static ArgumentsAdjuster getRemoveWarningArgumentAdjuster() {
  return [](const CommandLineArguments &args, llvm::StringRef /*unused*/) {
    CommandLineArguments adjustedArgs;

    for (auto &arg : args) {
      if (arg.substr(0, 2) != "-W") {
        adjustedArgs.push_back(arg);
      }
    }

    return adjustedArgs;
  };
}

void generate_lttng(
    ClangTool &tool, const path &outputDir, std::optional<std::string> provider,
    const std::optional<std::vector<std::string>> &optionalPath) {
  tool.appendArgumentsAdjuster(getClangSyntaxOnlyAdjuster());
  tool.appendArgumentsAdjuster(getClangStripOutputAdjuster());

  /* We disable all warnings. This is because sometimes the compilation
   * flags were generated for another compiler (like GCC), which has different
   * warning flags than clang. Or the project compiles normally with warnings,
   * and they are ignored.
   * In any case, warning should be checked in actual compile time and not in
   * parsing.*/
  tool.appendArgumentsAdjuster(getRemoveWarningArgumentAdjuster());
  tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
      BUILD_ARGS_TO_ADD, ArgumentInsertPosition::BEGIN));

  MatchFinder finder;
  TraceGeneratorSourceFileCallback sourceFileCallback(optionalPath);
  TraceGenerator lineTraceGenerator(
      outputDir, provider, TRACEPOINT_CALL_MATCH_ID, &sourceFileCallback);

  StatementMatcher functionCallMatcher =
      callExpr(callee(functionDecl(hasName(LTTNG_TRACEPOINT_FUNC_NAME))))
          .bind(TRACEPOINT_CALL_MATCH_ID);
  finder.addMatcher(functionCallMatcher, &lineTraceGenerator);

  std::cout << "Running lttng traces generation\n\n";
  try {
    int res =
        tool.run(newFrontendActionFactory(&finder, &sourceFileCallback).get());
    lineTraceGenerator.close();
    if (res != 0) {
      std::cout.flush();
      throw std::runtime_error(
          "\nSome errors have occurred during clang compilation.\n");
    }
  } catch (std::runtime_error &e) {
    std::cerr << "Traces generation failed!\n\n";
    std::cout.flush();
    std::cerr.flush();
    throw e;
  }

  std::cout << "\nTracing generation done!\n";
}