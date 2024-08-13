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

#include <algorithm>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "generate_lttng.h"

using namespace clang::tooling;

static std::vector<std::string> splitPath(std::string path) {
  std::vector<std::string> output;

  std::istringstream stream(path);
  std::string pathEntry;
  while (std::getline(stream, pathEntry, ':')) {
    output.push_back(std::filesystem::canonical(pathEntry).string());
  }

  return output;
}

static std::vector<std::string>
getCompileSourcePath(const CompilationDatabase &compilationDatabase,
                     const std::vector<std::string> &sourcePathList) {
  std::vector<std::string> sourcesToCompile;

  const auto sourcesInDb = compilationDatabase.getAllFiles();
  for (const auto &source : sourcePathList) {
    if (std::find(sourcesInDb.begin(), sourcesInDb.end(),
                  std::filesystem::canonical(source)) != sourcesInDb.end()) {
      sourcesToCompile.push_back(source);
    }
  }

  return sourcesToCompile;
}

int main(int argc, const char **argv) {
  llvm::cl::OptionCategory optionCategory(
      "Generate LTTNG Traces", "This tool automatically generates all the "
                               "boilerplate code for LTTNG tracepoints");
  llvm::cl::extrahelp commonHelp(CommonOptionsParser::HelpMessage);

  llvm::cl::opt<std::string> outputDir(
      "output_dir", llvm::cl::NormalFormatting, llvm::cl::Required,
      llvm::cl::desc("Output directory for generated headers"));

  llvm::cl::opt<std::string> compileCommandsPath(
      "compile_commands_dir", llvm::cl::NormalFormatting, llvm::cl::Optional,
      llvm::cl::desc("Path to the directory containing compile_commands.json"));

  llvm::cl::opt<std::string> provider(
      "provider", llvm::cl::NormalFormatting, llvm::cl::Optional,
      llvm::cl::init(""),
      llvm::cl::desc("The provider for which to generate traces"));

  llvm::cl::opt<std::string> includePath(
      "include_path", llvm::cl::NormalFormatting, llvm::cl::Optional,
      llvm::cl::init(""),
      llvm::cl::desc(
          "Include path to to generate headers relative to. If not "
          "given, we use the include path given in compile_commands. This is a "
          "string with a colon delimiter, for example: <path1>:<path2>"));

  auto expectedParser = CommonOptionsParser::create(argc, argv, optionCategory);
  if (!expectedParser) {
    llvm::errs() << expectedParser.takeError();
    return 1;
  }
  auto optionalProvider = provider.getValue() == ""
                              ? std::optional<std::string>()
                              : std::optional<std::string>(provider.getValue());

  auto optionalCompilePath = includePath.getValue() == ""
                                 ? std::optional<std::vector<std::string>>()
                                 : std::optional<std::vector<std::string>>(
                                       splitPath(includePath.getValue()));

  const CompilationDatabase *compilationDatabase;
  std::unique_ptr<CompilationDatabase> compilationDbFromDir;
  if (compileCommandsPath.getValue() != "") {
    std::string err;
    compilationDbFromDir = CompilationDatabase::loadFromDirectory(
        compileCommandsPath.getValue(), err);
    if (!compilationDbFromDir) {
      std::cerr << "Failed to get compilation database from "
                << compileCommandsPath.getValue() << ".\n"
                << "Err: " << err << "\n";
      exit(1);
    }

    compilationDatabase = compilationDbFromDir.get();
  } else {
    compilationDatabase = &expectedParser->getCompilations();
  }

  ClangTool tool(*compilationDatabase,
                 getCompileSourcePath(*compilationDatabase,
                                      expectedParser->getSourcePathList()));
  generate_lttng(tool, std::filesystem::absolute(outputDir.getValue()),
                 optionalProvider, optionalCompilePath);

  return 0;
}