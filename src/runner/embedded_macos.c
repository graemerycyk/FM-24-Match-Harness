// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// embedded.h on macOS: fm24ctl carries the universal harness dylib in its __TEXT,__fm24_harness
// section (linked with -sectcreate), and installs it to
// ~/Library/Application Support/fm24-harness/HARNESS_DLL_NAME, where the game loads it at launch.

#include "embedded.h"
#include "files.h"
#include "paths.h"
#include "platform.h"
#include "protocol.h"

#include <errno.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static bool sameContents(const wchar_t *path, const void *bytes, size_t size);

bool embedded_directory(char *out, size_t size) {
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): fm24ctl has one thread
	if (!home || home[0] != '/') {
		fprintf(stderr, "cannot find the home folder\n");
		return false;
	}
	int written = snprintf(out, size, "%s/Library/Application Support/fm24-harness", home);
	if (written <= 0 || (size_t)written >= size) {
		fprintf(stderr, "the harness folder path is too long\n");
		return false;
	}
	// Each folder from ~/Library down, created if it is missing.
	for (char *slash = strchr(out + strlen(home) + 1, '/');; slash = strchr(slash + 1, '/')) {
		if (slash) {
			*slash = 0;
		}
		bool made = mkdir(out, 0755) == 0 || errno == EEXIST;
		int error = errno;
		if (slash) {
			*slash = '/';
		}
		if (!made) {
			fprintf(stderr, "cannot create %s (%s)\n", out, platform_errorText(error).text);
			return false;
		}
		if (!slash) {
			return true;
		}
	}
}

bool embedded_dllPath(wchar_t *out, size_t size) {
	char directory[1024];
	wchar_t wide[HARNESS_PATH_LENGTH];
	if (!embedded_directory(directory, sizeof directory) ||
			!platform_fromUtf8(directory, wide, HARNESS_PATH_LENGTH) ||
			!paths_join(out, size, wide, HARNESS_DLL_NAME)) {
		fprintf(stderr, "cannot resolve the harness dylib path\n");
		return false;
	}
	return true;
}

bool embedded_extract(const wchar_t *path) {
	unsigned long size = 0;
	const uint8_t *bytes = getsectiondata(&_mh_execute_header, "__TEXT", "__fm24_harness", &size);
	if (!bytes || !size) {
		fprintf(stderr, "this fm24ctl has no built-in harness dylib\n");
		return false;
	}
	return embedded_write(path, bytes, size);
}

bool embedded_write(const wchar_t *path, const void *bytes, size_t size) {
	if (sameContents(path, bytes, size)) {
		return true;
	}
	wchar_t temporary[HARNESS_PATH_LENGTH];
	int written =
		swprintf(temporary, HARNESS_PATH_LENGTH, L"%ls.%lu.tmp", path, platform_processId());
	char from[4096];
	char to[4096];
	if (written <= 0 || written >= HARNESS_PATH_LENGTH ||
			!platform_toUtf8(temporary, from, sizeof from) || !platform_toUtf8(path, to, sizeof to)) {
		fprintf(stderr, "the harness dylib path is too long\n");
		return false;
	}
	if (!files_write(temporary, bytes, size)) {
		fprintf(stderr, "cannot write %s\n", from);
		(void)unlink(from);
		return false;
	}
	// A new file, renamed over the old one: a game that has the old one loaded keeps its own copy.
	// Overwriting it in place would invalidate the code signature of its mapped pages.
	if (chmod(from, 0755) != 0 || rename(from, to) != 0) {
		fprintf(stderr, "cannot replace %s (%s)\n", to, platform_errorText(errno).text);
		(void)unlink(from);
		return false;
	}
	return true;
}

// True if the file at `path` holds exactly `bytes`.
static bool sameContents(const wchar_t *path, const void *bytes, size_t size) {
	ByteBuffer existing = {0};
	bool same = files_read(path, &existing) && existing.length == size &&
							memcmp(existing.data, bytes, size) == 0;
	buffer_free(&existing);
	return same;
}
