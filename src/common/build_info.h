// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The commit the harness and fm24ctl were built from. cmake/build_info.cmake generates the
// definitions into the build tree on every build; both link the same copy through `common`.
#pragma once

// The commit's 40 lower-case hex digits, or "unknown" when the sources are not a Git checkout or
// Git was not found.
extern const char buildInfo_commit[];
// 1 if tracked files differed from the commit (untracked files do not count), 0 if not, -1 when the
// commit is unknown.
extern const int buildInfo_dirty;

// The slice this code was compiled for.
#if defined(__aarch64__) || defined(__arm64__)
#define BUILD_INFO_SLICE "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define BUILD_INFO_SLICE "x86_64"
#else
#define BUILD_INFO_SLICE "unknown"
#endif
