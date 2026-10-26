// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Football Manager 2024 saves for `launch --save`: finding one by name in the game's data folder,
// the auto-play file that has the game load it, and the harness's status line, which says which
// save is loaded (see session.h). Text only: the caller lists the folders.
#pragma once

#include <stdbool.h>
#include <stddef.h>


enum {
	SAVES_NAME_LENGTH = 256,     // a save's name as the harness reports it, and its NUL
	SAVES_DATE_LENGTH = 32,      // an in-game date as the status line writes it, and its NUL
	SAVES_SIMILAR_LENGTH = 1024, // the names of the saves a search lists instead
};

// The folders FM24 keeps saves in, under its data folder, in the order a name is looked up.
typedef enum SavesFolder {
	SAVES_CLOUD, // cloud/games: Steam Cloud saves, where the game saves by default
	SAVES_LOCAL, // games
	SAVES_FOLDER_COUNT,
} SavesFolder;

typedef enum SavesResult {
	SAVES_FOUND,     // in exactly one folder
	SAVES_AMBIGUOUS, // in both
	SAVES_MISSING,   // in neither
} SavesResult;

// A search for the save file named `name` plus ".fm", exactly: other saves whose names start with
// it, ignoring case, are only listed.
typedef struct SaveSearch {
	const char *name;
	bool found[SAVES_FOLDER_COUNT];
	char similar[SAVES_SIMILAR_LENGTH]; // "A, B (v02)"
	size_t similarCount;                // how many there are, listed or not
	size_t similarListed;
} SaveSearch;

typedef enum StatusPhase {
	STATUS_STARTING, // the game's start screen has not finished its first setup
	STATUS_MENU,     // no game is loaded
	STATUS_LOADING,  // a game is being loaded or created
	STATUS_BUSY,     // the game is processing
	STATUS_READY,    // a game is loaded and idle, with the human manager's team
	STATUS_NO_TEAM,  // a game is loaded and idle, but the human manager's team was not found
} StatusPhase;

// The status job's answer (see docs/CLI.md).
typedef struct GameStatus {
	StatusPhase phase;
	bool inProgress;
	bool processing;
	int kind;
	unsigned long long people;
	bool humanTeam;
	bool menuSeen;
	bool autoPlay;
	bool autoPlayRead;
	char date[SAVES_DATE_LENGTH]; // "" if the harness left it out
	char save[SAVES_NAME_LENGTH]; // "" if the harness left it out
} GameStatus;

// The folder's path under the data folder: "cloud/games" or "games".
const char *saves_folderName(SavesFolder folder);

// True if `--save` names a save rather than a file: no '/' and no ".fm" ending.
bool saves_isName(const char *text);

// The save name a .fm file stands for, as the harness reports it: its file name without a trailing
// ".fm". False if `path` names no file or it does not fit.
bool saves_nameOfPath(const char *path, char *out, size_t size);

// True if the save the harness reports is `name`. Only the last part of a reported path counts.
bool saves_isSave(const char *reported, const char *name);

void saves_startSearch(SaveSearch *search, const char *name);
// Adds a regular file named `fileName` in `folder` to the search.
void saves_note(SaveSearch *search, SavesFolder folder, const char *fileName);
// Where the search found the save; `folder` is set for SAVES_FOUND.
SavesResult saves_result(const SaveSearch *search, SavesFolder *folder);

// `data`/`folder`/`name`.fm.
bool saves_path(const char *data, SavesFolder folder, const char *name, char *out, size_t size);

// The auto-play file that has FM24 load the save at `path`, and nothing else:
// {"LOAD_GAME": "<path>"}, with '"' and '\' escaped. False if the path has control characters or
// the text does not fit.
bool saves_autoplayJson(const char *path, char *out, size_t size);

// Parses the status job's result line: "ok: <phase>", every flag and count in order, then an
// optional date and save. False (with `out` cleared) for anything else.
bool saves_parseStatus(const char *line, GameStatus *out);

// The phase as the status line names it.
const char *saves_phaseName(StatusPhase phase);
