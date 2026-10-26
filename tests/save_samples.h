// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Made-up FM24 save folders and Steam settings under a home folder, for the macOS tests of finding
// saves: fm24ctl's `launch --save` (tests/macos_test.c) and the viewer's Launch FM24 sheet
// (src/ui/save_list.h, tests/viewer), which look through the same ones. The macOS viewer's Swift
// tests import this header (tests/viewer/module.modulemap), so it uses only standard headers.
#pragma once

#include <stdbool.h>
#include <stddef.h>


// What Steam's launch option for FM24 says about the game's data folder.
typedef enum SaveSamplesOption {
	SAVE_SAMPLES_NO_OPTION,      // no Steam settings: the usual data folder
	SAVE_SAMPLES_LAUNCHER,       // the harness's launcher, giving no data folder
	SAVE_SAMPLES_STEAM_FOLDER,   // the launcher with --user_data_location="<home>/FM Data"
	SAVE_SAMPLES_OTHER_LAUNCHER, // another launcher with that folder, which fm24ctl does not follow
	SAVE_SAMPLES_UNCLEAR,        // the launcher, setting the folder in a way fm24ctl does not follow
	SAVE_SAMPLES_OPTION_COUNT,
} SaveSamplesOption;

enum {
	SAVE_SAMPLES_TIME = 1700000000, // the oldest save's modification time
	SAVE_SAMPLES_SAVES = 5,         // the saves in the data folder
};

// The data folder's saves, newest first (by modification time, then cloud/games before games, then
// name): the name, the folder ("cloud/games" or "games"), the file name and the seconds after
// SAVE_SAMPLES_TIME at which it was last written.
typedef struct SaveSample {
	const char *name;
	const char *folder;
	const char *file;
	unsigned age;
} SaveSample;
extern const SaveSample saveSamples_saves[SAVE_SAMPLES_SAVES];

// Writes FM24's data folders under `home` (an empty folder) with the saves above and files that are
// not saves (another extension, ".FM", a folder, a broken link), in the data folder `option`
// leads to, and other saves in the usual one if that is another. Writes Steam's settings for
// `option` with two accounts, the one that signed in last having the option.
bool saveSamples_make(const char *home, int option);

// The data folder the saves are in for `option` ("" for SAVE_SAMPLES_UNCLEAR, which has none).
bool saveSamples_dataFolder(const char *home, int option, char *out, size_t size);
