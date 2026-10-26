// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macos_builds.h"

#include <string.h>


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

const MacosBuild *macosBuilds_find(const char *architecture, const uint8_t uuid[16]) {
	for (size_t i = 0; i < macosBuildCount; ++i) {
		if (strcmp(macosBuilds[i].architecture, architecture) == 0 &&
				memcmp(macosBuilds[i].uuid, uuid, sizeof macosBuilds[i].uuid) == 0) {
			return &macosBuilds[i];
		}
	}
	return NULL;
}
