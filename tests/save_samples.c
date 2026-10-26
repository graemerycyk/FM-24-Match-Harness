// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "save_samples.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>


enum { PATH_SIZE = 2048 };

static const char usualFolder[] =
	"Library/Application Support/Sports Interactive/Football Manager 2024";
static const char launcherPath[] = "Library/Application Support/fm24-harness/launch-fm24.sh";

// The saves, newest first; "Linked" is a link to "Career Two", whose time it shows.
const SaveSample saveSamples_saves[SAVE_SAMPLES_SAVES] = {
	{"LIV_MAIN_FM24", "cloud/games", "LIV_MAIN_FM24.fm", 300},
	{"Local Copy", "games", "Local Copy.fm", 200},
	{"Career Two", "cloud/games", "Career Two.fm", 100},
	{"Linked", "games", "Linked.fm", 100},
	{"LIV_MAIN_FM24", "games", "LIV_MAIN_FM24.fm", 50},
};

static bool makeDirectories(const char *path) {
	char partial[PATH_SIZE];
	for (size_t i = 1; path[i]; ++i) {
		if (path[i] == '/') {
			(void)snprintf(partial, sizeof partial, "%.*s", (int)i, path);
			(void)mkdir(partial, 0755);
		}
	}
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool writeText(const char *path, const char *text) {
	FILE *file = fopen(path, "wb");
	bool ok = file && fputs(text, file) >= 0;
	return file && fclose(file) == 0 && ok;
}

// A file in `folder` under `data`, last written `age` seconds after SAVE_SAMPLES_TIME.
static bool writeFile(const char *data, const char *folder, const char *name, unsigned age) {
	char path[PATH_SIZE];
	(void)snprintf(path, sizeof path, "%s/%s/%s", data, folder, name);
	struct timeval times[2] = {
		{.tv_sec = SAVE_SAMPLES_TIME + (time_t)age},
		{.tv_sec = SAVE_SAMPLES_TIME + (time_t)age},
	};
	return writeText(path, "fmf.") && utimes(path, times) == 0;
}

static bool makeData(const char *data) {
	char path[PATH_SIZE];
	(void)snprintf(path, sizeof path, "%s/cloud/games/Folder.fm", data);
	bool ok = makeDirectories(path);
	(void)snprintf(path, sizeof path, "%s/games", data);
	return ok && makeDirectories(path);
}

// The data folder's saves, and files that are not saves.
static bool writeSaves(const char *data) {
	bool ok = makeData(data);
	for (size_t i = 0; ok && i < SAVE_SAMPLES_SAVES; ++i) {
		const SaveSample *save = &saveSamples_saves[i];
		if (strcmp(save->name, "Linked") != 0) {
			ok = writeFile(data, save->folder, save->file, save->age);
		}
	}
	char path[PATH_SIZE];
	(void)snprintf(path, sizeof path, "%s/games/Linked.fm", data);
	ok = ok && symlink("../cloud/games/Career Two.fm", path) == 0;
	(void)snprintf(path, sizeof path, "%s/games/Broken.fm", data);
	ok = ok && symlink("Gone.fm", path) == 0;
	return ok && writeFile(data, "cloud/games", "config.xml", 400) &&
				 writeFile(data, "cloud/games", "UPPER.FM", 400) && writeFile(data, "games", ".fm", 400);
}

// Steam's settings: two accounts, the one that signed in last (42) with `option`, the other with
// one that would point elsewhere.
static bool writeSteam(const char *home, const char *option) {
	static const char config[] = "\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{"
															 "\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n"
															 "\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"2252570\"\n\t\t\t\t\t{"
															 "\n\t\t\t\t\t\t\"LaunchOptions\"\t\t"
															 "\"%s\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
	static const char logins[] =
		"\"users\"\n{\n\t\"76561197960265770\"\n\t{\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n"
		"\t\"76561197960265771\"\n\t{\n\t\t\"Timestamp\"\t\t\"300\"\n\t}\n}\n";
	char path[PATH_SIZE];
	char escaped[PATH_SIZE];
	char text[2 * PATH_SIZE];
	bool ok = true;
	for (int account = 42; ok && account <= 43; ++account) {
		const char *given = account == 42
													? option
													: "/bin/sh \"/x/launch-fm24.sh\" %command% --user_data_location=/Wrong";
		size_t length = 0;
		for (const char *c = given; *c && length + 2 < sizeof escaped; ++c) {
			if (*c == '"' || *c == '\\') {
				escaped[length++] = '\\';
			}
			escaped[length++] = *c;
		}
		escaped[length] = 0;
		(void)snprintf(
			path, sizeof path, "%s/Library/Application Support/Steam/userdata/%d/config", home, account
		);
		ok = makeDirectories(path);
		(void)snprintf(text, sizeof text, config, escaped);
		(void)snprintf(
			path,
			sizeof path,
			"%s/Library/Application Support/Steam/userdata/%d/config/localconfig.vdf",
			home,
			account
		);
		ok = ok && writeText(path, text);
	}
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/config", home);
	ok = ok && makeDirectories(path);
	(void)snprintf(
		path, sizeof path, "%s/Library/Application Support/Steam/config/loginusers.vdf", home
	);
	return ok && writeText(path, logins);
}

bool saveSamples_make(const char *home, int option) {
	char usual[PATH_SIZE];
	char moved[PATH_SIZE];
	char launch[3 * PATH_SIZE];
	(void)snprintf(usual, sizeof usual, "%s/%s", home, usualFolder);
	(void)snprintf(moved, sizeof moved, "%s/FM Data", home);
	switch (option) {
		case SAVE_SAMPLES_NO_OPTION: return writeSaves(usual);
		case SAVE_SAMPLES_LAUNCHER:
			(void)snprintf(launch, sizeof launch, "/bin/sh \"%s/%s\" %%command%%", home, launcherPath);
			return writeSaves(usual) && writeSteam(home, launch);
		case SAVE_SAMPLES_STEAM_FOLDER:
			// A stale copy in the usual folder is not looked at.
			(void)snprintf(
				launch,
				sizeof launch,
				"/bin/sh \"%s/%s\" %%command%% --user_data_location=\"%s\"",
				home,
				launcherPath,
				moved
			);
			return writeSaves(moved) && makeData(usual) &&
						 writeFile(usual, "cloud/games", "Stale.fm", 500) && writeSteam(home, launch);
		case SAVE_SAMPLES_OTHER_LAUNCHER:
			(void)snprintf(
				launch,
				sizeof launch,
				"/bin/sh \"/elsewhere/launch-fm24.sh\" %%command%% --user_data_location=\"%s\"",
				moved
			);
			return writeSaves(usual) && makeData(moved) &&
						 writeFile(moved, "cloud/games", "Elsewhere.fm", 500) && writeSteam(home, launch);
		case SAVE_SAMPLES_UNCLEAR:
			(void)snprintf(
				launch,
				sizeof launch,
				"/bin/sh \"%s/%s\" %%command%% --user_data_location \"%s\"",
				home,
				launcherPath,
				moved
			);
			return writeSaves(usual) && writeSaves(moved) && writeSteam(home, launch);
		default: return false;
	}
}

bool saveSamples_dataFolder(const char *home, int option, char *out, size_t size) {
	int written = option == SAVE_SAMPLES_UNCLEAR ? snprintf(out, size, "%s", "")
								: option == SAVE_SAMPLES_STEAM_FOLDER
									? snprintf(out, size, "%s/FM Data", home)
									: snprintf(out, size, "%s/%s", home, usualFolder);
	return written >= 0 && (size_t)written < size && option >= 0 &&
				 option < SAVE_SAMPLES_OPTION_COUNT;
}
