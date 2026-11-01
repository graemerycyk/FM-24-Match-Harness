// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for the game in the launcher test (macos_test.c). It records how it was started in
// the file FM24_TEST_OUTPUT names: its process ID, whether it runs under Rosetta, the libraries
// inserted into it (from macos_stand_in_library.c, in load order), DYLD_INSERT_LIBRARIES and its
// arguments from argv[0], each followed by '|'.
//
// With FM24_TEST_BUNDLES set, it then loads and unloads bundles as its standard input says, as a
// game that keeps its engine in a plugin loads and unloads it (the plugin test, which inserts the
// stand-in harness). Each line is a command, answered with a line on standard output: `load
// <path>` dlopens a bundle ("loaded"), `unload` dlcloses the last one loaded ("unloaded"), `play
// <save>` has the last one loaded load a save as if by hand ("playing"; see
// macos_stand_in_plugin.c) and `exit`, like the end of the input, ends it. With FM24_TEST_LOAD
// set, it first loads the bundle that names, as the game loads its engine once started, and says
// so as `load` does.

#include "edition.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

enum { BUNDLES_MAX = 8 };

static int record(const char *output, int argc, char **argv);
static int loadBundles(void);

int main(int argc, char **argv) {
	const char *output = getenv(EDITION_ENV_PREFIX "TEST_OUTPUT");
	bool bundles = getenv(EDITION_ENV_PREFIX "TEST_BUNDLES") != NULL;
	if (!output && !bundles) {
		return 1;
	}
	if (output && record(output, argc, argv) != 0) {
		return 1;
	}
	return bundles ? loadBundles() : 0;
}

static int record(const char *output, int argc, char **argv) {
	FILE *file = fopen(output, "wb");
	if (!file) {
		return 1;
	}
	int translated = 0;
	size_t size = sizeof translated;
	if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0) {
		translated = 0; // An Intel Mac has no Rosetta.
	}
	const char *loaded = getenv(EDITION_ENV_PREFIX "TEST_LOADED");
	const char *inserted = getenv("DYLD_INSERT_LIBRARIES");
	int ok = fprintf(
		file, "%d|%d|%s|%s|", (int)getpid(), translated, loaded ? loaded : "", inserted ? inserted : ""
	);
	for (int i = 0; i < argc && ok >= 0; ++i) {
		ok = fprintf(file, "%s|", argv[i]);
	}
	return fclose(file) == 0 && ok >= 0 ? 0 : 1;
}

static int loadBundles(void) {
	void *handles[BUNDLES_MAX];
	size_t count = 0;
	char line[4096];
	const char *first = getenv(EDITION_ENV_PREFIX "TEST_LOAD");
	bool pending = first && snprintf(line, sizeof line, "load %s", first) < (int)sizeof line;
	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	while (pending || fgets(line, sizeof line, stdin)) {
		pending = false;
		line[strcspn(line, "\n")] = 0;
		if (strncmp(line, "load ", 5) == 0 && count < BUNDLES_MAX) {
			void *handle = dlopen(line + 5, RTLD_NOW | RTLD_LOCAL);
			if (handle) {
				handles[count++] = handle;
				puts("loaded");
			} else {
				printf("cannot load: %s\n", dlerror());
			}
		} else if (strncmp(line, "play ", 5) == 0 && count > 0) {
			void (*play)(const char *) =
				(void (*)(const char *))dlsym(handles[count - 1], "standInLoadSave");
			if (play) {
				play(line + 5);
			}
			puts(play ? "playing" : "cannot play");
		} else if (strcmp(line, "unload") == 0 && count > 0) {
			puts(dlclose(handles[--count]) == 0 ? "unloaded" : "cannot unload");
		} else if (strcmp(line, "exit") == 0) {
			break;
		} else {
			printf("cannot follow: %s\n", line);
		}
	}
	return 0;
}
