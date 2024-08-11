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

#ifndef __GENERATE_LTTNG_H__
#define __GENERATE_LTTNG_H__

#include <clang/Tooling/Tooling.h>
#include <filesystem>

void generate_lttng(
    clang::tooling::ClangTool &tool, const std::filesystem::path &outputDir,
    std::optional<std::string> provider,
    const std::optional<std::vector<std::string>> &optionalPath);

#endif // __GENERATE_LTTNG_H__