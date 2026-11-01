// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macos_builds.h"

#include "edition.h"

#include <string.h>


#if FM_EDITION == 24
// Steam, 24.4.2 (build 2081827), signed 16 April 2025: the release of the supported Windows
// Steam build (PE timestamp 1744804480).
const MacosBuild macosBuilds[] = {
	{"arm64",
		{0xD7,
			0x10,
			0xF6,
			0xFA,
			0x6A,
			0xCA,
			0x32,
			0xBB,
			0x99,
			0xFE,
			0x45,
			0x1C,
			0xEE,
			0x8D,
			0xFA,
			0x3D},
		"24.4.2 (2081827)",
		2081827},
	{"x86_64",
		{0x4B,
			0xC0,
			0xB3,
			0x39,
			0xA4,
			0x6A,
			0x37,
			0x09,
			0x89,
			0x02,
			0x52,
			0x1E,
			0x60,
			0x94,
			0x9D,
			0x39},
		"24.4.2 (2081827)",
		2081827},
};
const size_t macosBuildCount = sizeof macosBuilds / sizeof macosBuilds[0];
#else
// Steam, 27.0.0.2486716, the free preview of October 2026: the slices of the engine plugin
// (game_plugin, plugin version 27.0.0+2486695). The app's CFBundleVersion is "0", so the build
// number is the last part of its CFBundleShortVersionString.
const MacosBuild macosBuilds[] = {
	{"arm64",
		{0x9E,
			0x8E,
			0x47,
			0xDE,
			0xF7,
			0x04,
			0x31,
			0x4C,
			0x91,
			0x7D,
			0xC3,
			0x2D,
			0x12,
			0x63,
			0x76,
			0x78},
		"27.0.0.2486716",
		2486716},
	{"x86_64",
		{0x22,
			0xCF,
			0xE5,
			0xC3,
			0xB5,
			0xCF,
			0x3C,
			0x00,
			0x80,
			0x43,
			0x1E,
			0xA1,
			0x90,
			0xA6,
			0x9B,
			0x89},
		"27.0.0.2486716",
		2486716},
};
const size_t macosBuildCount = sizeof macosBuilds / sizeof macosBuilds[0];
#endif

const MacosBuild *macosBuilds_find(const char *architecture, const uint8_t uuid[16]) {
	for (size_t i = 0; i < macosBuildCount; ++i) {
		if (strcmp(macosBuilds[i].architecture, architecture) == 0 &&
				memcmp(macosBuilds[i].uuid, uuid, sizeof macosBuilds[i].uuid) == 0) {
			return &macosBuilds[i];
		}
	}
	return NULL;
}
