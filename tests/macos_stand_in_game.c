// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for the game in the launcher test (macos_test.c). It records how it was started in
// the file FM24_TEST_OUTPUT names: its process ID, whether it runs under Rosetta, the libraries
// inserted into it (from macos_stand_in_library.c, in load order), DYLD_INSERT_LIBRARIES and its
// arguments from argv[0], each followed by '|'.

#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
	const char *output = getenv("FM24_TEST_OUTPUT");
	FILE *file = output ? fopen(output, "wb") : NULL;
	if (!file) {
		return 1;
	}
	int translated = 0;
	size_t size = sizeof translated;
	if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0) {
		translated = 0; // An Intel Mac has no Rosetta.
	}
	const char *loaded = getenv("FM24_TEST_LOADED");
	const char *inserted = getenv("DYLD_INSERT_LIBRARIES");
	int ok = fprintf(
		file, "%d|%d|%s|%s|", (int)getpid(), translated, loaded ? loaded : "", inserted ? inserted : ""
	);
	for (int i = 0; i < argc && ok >= 0; ++i) {
		ok = fprintf(file, "%s|", argv[i]);
	}
	return fclose(file) == 0 && ok >= 0 ? 0 : 1;
}
