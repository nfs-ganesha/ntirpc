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

#include "generate_lttng.h"
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace clang::tooling;
using std::filesystem::path;

inline static const std::string COMBINED_HEADER_FILE_DEFAULT_NAME =
    "generated_lttng.h";

static void printHelpAndExit(int argc, const char **argv) {
  std::cerr << argv[0]
            << "<dir containing compile_commands.json> <output-dir> \n";
  exit(1);
}

static void parseArgs(int argc, const char **argv, path &srcDir,
                      path &outputDir) {
  if (argc != 3) {
    printHelpAndExit(argc, argv);
  }

  srcDir.assign(std::filesystem::absolute(argv[1]));
  if (!std::filesystem::is_directory(srcDir)) {
    std::cerr << argv[1]
              << "Is not a valid source directory (does it exist?)\n";
    printHelpAndExit(argc, argv);
  }

  outputDir.assign(std::filesystem::absolute(argv[2]));
  if (!std::filesystem::is_directory(outputDir)) {
    std::cerr << argv[1]
              << "Is not a valid output directory (does it exist?)\n";
    printHelpAndExit(argc, argv);
  }
}

int main(int argc, const char **argv) {
  path srcDir, outputDir;
  parseArgs(argc, argv, srcDir, outputDir);

  std::string error;
  auto db = CompilationDatabase::loadFromDirectory(srcDir.string(), error);
  if (!db) {
    throw std::runtime_error("Failed to get compilation database in dir " +
                             srcDir.string() + ". Error: " + error);
  }

  ClangTool tool(*db.get(), db->getAllFiles());
  generate_lttng(tool, outputDir, std::optional<std::string>(),
                 std::optional<std::vector<std::string>>());

  return 0;
}