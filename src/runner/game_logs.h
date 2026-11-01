// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// FM24's file-list error logs on macOS, "file llist errors_<date>_<time>.txt" in the Logs folder of
// its data folder: each match the harness builds adds about 0.2 MB to the current one, FM24 starts
// a new one in each session that builds a match, and it never deletes them. `batch run` trims them
// between chunks (trim_game_logs, see docs/CLI.md). game_logs_macos.c does the work; on Windows
// nothing does.
#pragma once

#include <stdbool.h>
#include <stdint.h>


enum { GAME_LOGS_PATH_LENGTH = 4096, GAME_LOGS_TEXT_LENGTH = 128 };

// What can fail, each kind of which batch run warns of once.
typedef enum GameLogsProblem {
	GAME_LOGS_FOLDER, // FM24's data folder is not known
	GAME_LOGS_READ,   // the Logs folder cannot be read
	GAME_LOGS_EMPTY,  // the newest log cannot be emptied
	GAME_LOGS_DELETE, // an older log cannot be deleted
	GAME_LOGS_PROBLEMS,
} GameLogsProblem;

// What a trim did, and why each kind of failure happened (its first).
typedef struct GameLogsTrim {
	char folder[GAME_LOGS_PATH_LENGTH]; // the Logs folder; "" if not known
	unsigned emptied;                   // 0 or 1
	unsigned deleted;
	uint64_t freedBytes; // the disk space the emptied and deleted logs took
	char failed[GAME_LOGS_PROBLEMS][GAME_LOGS_TEXT_LENGTH]; // "" for none
} GameLogsTrim;

// True if `name` is a log's: "file llist errors_", anything, then ".txt".
bool gameLogs_isLogName(const char *name);

// Trims the logs in `folder` (absolute, UTF-8): of the regular files directly in it with a log's
// name, never a link, a folder or anything else, it empties the newest by modification time in
// place, which FM24 may still have open and write on, and deletes the others. A missing folder has
// nothing to trim. Records in `out` what it did and why anything failed.
void gameLogs_trim(const char *folder, GameLogsTrim *out);
