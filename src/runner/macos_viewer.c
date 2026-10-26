// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macos_viewer.h"

#include "paths.h"
#include "platform.h"
#include "protocol.h"

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


extern char **environ;

enum { PATH_LENGTH = 4096 };

static int openWith(char *const *arguments);

int macosViewer_open(const wchar_t *fixture) {
	wchar_t absolute[HARNESS_PATH_LENGTH];
	char path[PATH_LENGTH];
	if (!paths_absolute(fixture, absolute, HARNESS_PATH_LENGTH) ||
			!paths_toUtf8(absolute, path, sizeof path)) {
		fprintf(stderr, "bad path: %ls\n", fixture);
		return 1;
	}
	struct stat status;
	if (stat(path, &status) != 0) {
		fprintf(stderr, "cannot open %s (%s)\n", path, platform_errorText(errno).text);
		return 1;
	}
	wchar_t wideDirectory[HARNESS_PATH_LENGTH];
	char directory[PATH_LENGTH];
	char app[PATH_LENGTH];
	bool found = paths_executableDirectory(wideDirectory, HARNESS_PATH_LENGTH) &&
							 paths_toUtf8(wideDirectory, directory, sizeof directory) &&
							 macosViewer_find(directory, app, sizeof app);
	if (found) {
		char *const arguments[] = {"/usr/bin/open", "-a", app, path, NULL};
		if (openWith(arguments) == 0) {
			return 0;
		}
		fprintf(stderr, "cannot open %s in %s\n", path, app);
		return 1;
	}
	char *const arguments[] = {"/usr/bin/open", "-b", MACOS_VIEWER_IDENTIFIER, path, NULL};
	if (openWith(arguments) == 0) {
		return 0;
	}
	fputs(
		"fm24ctl opens fixtures in " MACOS_VIEWER_APP ", which is neither beside it nor known to "
		"macOS: build it with scripts/build.sh, which puts it beside fm24ctl, or open the fixture "
		"with open -a <path to " MACOS_VIEWER_APP "> <fixture>\n",
		stderr
	);
	return 2;
}

bool macosViewer_find(const char *directory, char *app, size_t size) {
	static const char inside[] = "/Contents/MacOS";
	static const char bundle[] = ".app";
	size_t length = strlen(directory);
	size_t insideLength = sizeof inside - 1;
	if (length > insideLength && strcmp(directory + length - insideLength, inside) == 0) {
		size_t appLength = length - insideLength;
		if (appLength > sizeof bundle - 1 && appLength < size &&
				strncmp(directory + appLength - (sizeof bundle - 1), bundle, sizeof bundle - 1) == 0) {
			memcpy(app, directory, appLength);
			app[appLength] = 0;
			return true;
		}
	}
	char executable[PATH_LENGTH];
	int written = snprintf(app, size, "%s/" MACOS_VIEWER_APP, directory);
	int executableWritten =
		snprintf(executable, sizeof executable, "%s/Contents/MacOS/fm24viewer", app);
	return written > 0 && (size_t)written < size && executableWritten > 0 &&
				 (size_t)executableWritten < sizeof executable && access(executable, X_OK) == 0;
}

// Runs /usr/bin/open and returns its exit status (-1 if it cannot run).
static int openWith(char *const *arguments) {
	pid_t child = 0;
	int status = 0;
	int error = posix_spawn(&child, arguments[0], NULL, NULL, arguments, environ);
	if (error != 0) {
		fprintf(stderr, "cannot run %s (%s)\n", arguments[0], platform_errorText(error).text);
		return -1;
	}
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
