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

#ifndef __TRACEPOINT_LOCATION_H__
#define __TRACEPOINT_LOCATION_H__

#include <clang/Basic/FileEntry.h>
#include <clang/Basic/SourceLocation.h>
#include <string>

namespace trace_generator {
class TracepointLocation {
public:
  explicit TracepointLocation(const clang::FullSourceLoc &location)
      : filePath_(location.getFileEntry()->getName()),
        line_(location.getLineNumber()) {}

  explicit TracepointLocation(const std::string &filePath, unsigned int line)
      : filePath_(filePath), line_(line) {}

  TracepointLocation(const TracepointLocation &location)
      : TracepointLocation(location.filePath_, location.line_) {}

  const std::string &getFilePath() const { return filePath_; }

  unsigned int getLine() const { return line_; }

  std::string toString() const {
    return getFilePath() + ":" + std::to_string(getLine());
  }

  bool operator==(const TracepointLocation &location) const {
    return (this->getLine() == location.getLine() &&
            this->getFilePath() == location.getFilePath());
  }

  bool operator!=(const TracepointLocation &location) const {
    return !TracepointLocation::operator==(location);
  }

private:
  const std::string filePath_;
  const unsigned int line_;
};
} // namespace trace_generator

#endif // __TRACEPOINT_LOCATION_H__