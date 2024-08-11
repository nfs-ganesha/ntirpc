// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Shachar Hochma shaharhoch@google.com
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

#ifndef __PARSING_ERROR_H__
#define __PARSING_ERROR_H__

#include <stdexcept>

#include "TracepointLocation.h"

namespace trace_generator {

class ParsingError : virtual public std::runtime_error {
public:
  explicit ParsingError(const std::string &message,
                        const TracepointLocation &location)
      : runtime_error(getExceptionMessage(message, location)) {}

  explicit ParsingError(const runtime_error &error,
                        const TracepointLocation &location)
      : runtime_error(getExceptionMessage(error.what(), location)) {}

private:
  static std::string getExceptionMessage(const std::string &message,
                                         const TracepointLocation &location) {
    return std::string("Failed to parse line ") +
           std::string(location.getFilePath()) + ":" +
           std::to_string(location.getLine()) + "\n\tError: " + message + "\n";
  }
};

}; // namespace trace_generator

#endif // __PARSING_ERROR_H__