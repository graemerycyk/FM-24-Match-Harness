// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// inject.h on macOS: the game cannot be injected while it runs (see macos_game.h), so this makes
// sure the current harness is installed and explains how to start the game with it: through
// Steam's launch option, which is the tested route.

#include "channel.h"
#include "inject.h"
#include "macos_game.h"
#include "protocol.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>


enum { PATH_LENGTH = 4096 };

static bool otherVersion(char *name, size_t size);

bool inject_harness(void) {
	char launcher[PATH_LENGTH];
	char executable[PATH_LENGTH];
	if (!macosGame_installHarness(false, launcher, sizeof launcher)) {
		return false;
	}
	unsigned long pid = (unsigned long)macosGame_findRunning(executable, sizeof executable);
	char other[256];
	if (!pid) {
		fprintf(stderr, "Football Manager 2024 is not running.\n");
	} else if (otherVersion(other, sizeof other)) {
		fprintf(
			stderr,
			"Football Manager 2024 (pid %lu) has another harness version loaded (%s); this fm24ctl uses "
			"%s.\n"
			"Quit the game and start it again to load this version.\n",
			pid,
			other,
			HARNESS_PROTOCOL_TEXT
		);
	} else {
		fprintf(
			stderr,
			"Football Manager 2024 (pid %lu) is running without the harness: on macOS it loads when the "
			"game starts.\nQuit the game and start it again with the harness.\n",
			pid
		);
	}
	if (macosGame_launchOptionSet(launcher)) {
		fprintf(
			stderr,
			"Start it from Steam, whose launch option loads the harness, or with `fm24ctl launch`; then "
			"load your save and leave the game idle.\n"
		);
	} else {
		// Without the option, `launch` would open the game directly, which has not been verified.
		fprintf(
			stderr,
			"Set Steam's launch option for it once, then start it from Steam (`fm24ctl install` says "
			"how, and when Steam has saved the option):\n  /bin/sh \"%s\" %%command%%\n",
			launcher
		);
	}
	return false;
}

unsigned long inject_findGame(void) {
	char executable[PATH_LENGTH];
	return (unsigned long)macosGame_findRunning(executable, sizeof executable);
}

// The socket name of another harness version a running game serves in the temporary directory,
// if there is one. Sockets left by games that crashed or were force-quit do not count.
static bool otherVersion(char *name, size_t size) {
	char path[PATH_LENGTH];
	if (!channel_socketPath(path, sizeof path)) {
		return false;
	}
	char *slash = strrchr(path, '/');
	if (!slash) {
		return false;
	}
	*slash = 0;
	DIR *directory = opendir(path);
	if (!directory) {
		return false;
	}
	bool found = false;
	static const char prefix[] = "fm24-harness-";
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(directory); entry && !found; entry = readdir(directory)) {
		size_t length = strlen(entry->d_name);
		char socket[PATH_LENGTH];
		int written = snprintf(socket, sizeof socket, "%s/%s", path, entry->d_name);
		if (strncmp(entry->d_name, prefix, sizeof prefix - 1) == 0 && length > 5 &&
				strcmp(entry->d_name + length - 5, ".sock") == 0 &&
				strcmp(entry->d_name, HARNESS_SOCKET_NAME) != 0 && length < size && written > 0 &&
				(size_t)written < sizeof socket && channel_served(socket)) {
			strcpy(name, entry->d_name);
			found = true;
		}
	}
	closedir(directory);
	return found;
}
