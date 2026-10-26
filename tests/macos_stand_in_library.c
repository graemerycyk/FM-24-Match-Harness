// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for the harness dylib and Steam's overlay in the launcher test (macos_test.c). Each
// copy that dyld inserts adds its own path to FM24_TEST_LOADED, which macos_stand_in_game.c
// records.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((constructor)) static void recordLoad(void) {
	Dl_info image;
	if (!dladdr((const void *)recordLoad, &image) || !image.dli_fname) {
		return;
	}
	const char *before = getenv("FM24_TEST_LOADED");
	char loaded[4096];
	int written = snprintf(
		loaded, sizeof loaded, "%s%s%s", before ? before : "", before ? ":" : "", image.dli_fname
	);
	if (written > 0 && (size_t)written < sizeof loaded) {
		(void)setenv("FM24_TEST_LOADED", loaded, 1);
	}
}
