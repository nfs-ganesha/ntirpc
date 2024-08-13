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

#ifndef __TRACE_GENERATOR_SOURCE_FILE_CALLBACK_H__
#define __TRACE_GENERATOR_SOURCE_FILE_CALLBACK_H__

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <filesystem>

namespace trace_generator {

using std::filesystem::path;

class TraceGeneratorSourceFileCallback
    : public clang::tooling::SourceFileCallbacks {
public:
  TraceGeneratorSourceFileCallback(
      const std::optional<std::vector<std::string>> &optionalPath)
      : optionalPath_(optionalPath) {}

  bool handleBeginSource(clang::CompilerInstance &ci) override {
    currentCompilerInstance_ = &ci;
    return true;
  }

  std::string getRelativeHeaderPath(path headerFullPath) const {
    std::string headerFullPathStr =
        std::filesystem::canonical(headerFullPath).string();

    for (const auto &pathEntry : getPath()) {
      if (headerFullPathStr.substr(0, pathEntry.length()) == pathEntry &&
          headerFullPathStr.length() > pathEntry.length() + 1 &&
          headerFullPathStr[pathEntry.length() + 1] == '/') {
        /* We remove the relative path + 1 to also remove the following "/" */
        return headerFullPathStr.substr(pathEntry.length() + 1);
      }
    }

    // Failed to find relative header, return full header
    return headerFullPathStr;
  }

private:
  clang::CompilerInstance *currentCompilerInstance_ = nullptr;
  std::optional<std::vector<std::string>> optionalPath_;

  std::vector<std::string> getPath() const {
    if (optionalPath_.has_value()) {
      return optionalPath_.value();
    }

    std::vector<std::string> outPath;
    auto hso = currentCompilerInstance_->getInvocation().getHeaderSearchOpts();
    for (const auto &entry : hso.UserEntries) {
      outPath.push_back(entry.Path);
    }

    return outPath;
  }
};
} // namespace trace_generator

#endif // __TRACE_GENERATOR_SOURCE_FILE_CALLBACK_H__