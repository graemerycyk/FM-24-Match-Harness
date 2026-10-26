// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The macOS builds of Football Manager 2024 the harness supports, one per architecture slice of
// the universal executable, identified by the slice's LC_UUID (Mach-O has no link timestamp).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef struct MacosBuild {
	const char *architecture; // "arm64" or "x86_64"
	uint8_t uuid[16];
	const char *version;    // CFBundleShortVersionString (CFBundleVersion)
	uint32_t bundleVersion; // CFBundleVersion, written to configs as their game_build
} MacosBuild;

extern const MacosBuild macosBuilds[];
extern const size_t macosBuildCount;

// The supported build with this architecture and UUID, or NULL.
const MacosBuild *macosBuilds_find(const char *architecture, const uint8_t uuid[16]);
