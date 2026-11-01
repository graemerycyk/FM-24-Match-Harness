// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The macOS builds of the game the harness supports, one per architecture slice of the universal
// image that holds the match engine (the executable, or the engine plugin: EDITION_ENGINE_PLUGIN),
// identified by the slice's LC_UUID (Mach-O has no link timestamp).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef struct MacosBuild {
	const char *architecture; // "arm64" or "x86_64"
	uint8_t uuid[16];
	const char *version;    // CFBundleShortVersionString, with any CFBundleVersion in brackets
	uint32_t bundleVersion; // the build number, written to configs as their game_build
} MacosBuild;

extern const MacosBuild macosBuilds[];
extern const size_t macosBuildCount;

// The supported build with this architecture and UUID, or NULL.
const MacosBuild *macosBuilds_find(const char *architecture, const uint8_t uuid[16]);
