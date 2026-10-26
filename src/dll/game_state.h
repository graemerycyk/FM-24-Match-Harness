// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// What the game is doing, read from its memory without changing anything: whether a game is loaded
// and idle, which save it is and the in-game date. The `status` job reports it, other jobs wait
// while the game is busy (job_execute.c), and exports record the save and the date. Only the macOS
// build's objects have been located so far; on Windows gameState_read finds nothing.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	GAME_STATE_DATE_LENGTH = 32,  // gameState_formatDate's longest text and its NUL
	GAME_STATE_SAVE_LENGTH = 256, // a save name of up to 255 UTF-8 bytes and its NUL
};

typedef enum GamePhase {
	GAME_PHASE_STARTING, // the start screen's first entry has not ended yet
	GAME_PHASE_MENU,     // no game is loaded
	GAME_PHASE_LOADING,  // a load or a new game is under way (see gameState_phase)
	GAME_PHASE_BUSY,     // the game is processing: loading, saving and the like
	GAME_PHASE_READY,    // a game is in progress, idle, with the human manager's team
	GAME_PHASE_NO_TEAM,  // a game is in progress, but no human manager's team was found
} GamePhase;

typedef struct GameState {
	bool inProgress;        // a game has been loaded or created, and not left since
	bool processing;        // the game is processing something
	int32_t processingKind; // what it processes, in the game's own numbering; -1 when it is not
	size_t people;          // people in the game's database: 0 with no game loaded
	bool humanTeam;         // the human manager's team was found (looked for only with `findTeam`)
	bool startScreenSeen;   // the start screen's first entry, with any auto-play load, has ended
	bool autoPlay;          // the game's auto-play flag, on until it finds no auto-play file
	bool autoPlayRead;      // the game has read and parsed an auto-play file
	char date[GAME_STATE_DATE_LENGTH]; // the in-game date while a game is in progress, else ""
	char save[GAME_STATE_SAVE_LENGTH]; // the loaded save's name without ".fm", or "" if unknown
} GameState;

// Reads the game's state; false (and a cleared state) if it cannot be read, as on Windows. Looking
// for the human manager's team is a pass over the game's people, so it is done only with `findTeam`
// and only while a game is in progress and idle.
bool gameState_read(GameState *state, bool findTeam);

// What the state means. Only a state read with `findTeam` can be GAME_PHASE_READY. A load is under
// way while a database is loaded with no game in progress, and while a game in progress waits for
// the start screen's first entry to end: an auto-play load sets "in progress" before it is done.
GamePhase gameState_phase(const GameState *state);

// The phase as the status job names it: "starting", "menu", "loading", "busy", "ready", "no_team".
const char *gameState_phaseName(GamePhase phase);

// A game date (a u16 day of the year | time code << 9, then a u16 year) as the harness writes
// dates: "2023 day 186 19:30". Time codes count quarter hours from 06:00, plus one; 0 is 00:00.
void gameState_formatDate(uint16_t dayWord, uint16_t year, char *out, size_t size);
