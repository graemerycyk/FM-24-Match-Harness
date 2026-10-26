// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game_state.h"

#include "config_writer.h"
#include "database.h"
#include "game.h"
#include "memory.h"

#include <stdio.h>
#include <string.h>


enum {
	DAY_MASK = 0x1FF,
	TIME_SHIFT = 9,
	QUARTER_HOUR = 15,
	FIRST_SLOT_HOUR = 6,
};

#ifdef __APPLE__
static bool readFlag(const uint8_t *object, ptrdiff_t offset, bool *out);
static void readGame(GameState *state);
static void readSave(GameState *state);
static void readDate(GameState *state);
static bool readSaveName(const uint8_t *string, char *out, size_t size);

bool gameState_read(GameState *state, bool findTeam) {
	*state = (GameState){.processingKind = -1};
	const uint8_t *options = game_address(RVA_MACOS_GAME_OPTIONS);
	if (!readFlag(options, GAME_OPTIONS_IN_PROGRESS, &state->inProgress)) {
		return false;
	}
	uint8_t *startScreen = NULL;
	state->startScreenSeen =
		memory_readPointer(game_address(RVA_MACOS_START_SCREEN), 0, &startScreen);
	state->people = database_countPeople();
	readGame(state);
	if (findTeam && state->inProgress && state->startScreenSeen && !state->processing &&
			state->people) {
		state->humanTeam = database_findHumanTeam() != NULL;
	}
	readSave(state);
	readDate(state);
	return true;
}
#else
bool gameState_read(GameState *state, bool findTeam) {
	(void)findTeam;
	*state = (GameState){.processingKind = -1};
	return false;
}
#endif

GamePhase gameState_phase(const GameState *state) {
	if (state->processing) {
		return GAME_PHASE_BUSY;
	}
	if (!state->inProgress) {
		if (state->people) {
			return GAME_PHASE_LOADING;
		}
		return state->startScreenSeen ? GAME_PHASE_MENU : GAME_PHASE_STARTING;
	}
	if (!state->startScreenSeen) {
		return GAME_PHASE_LOADING;
	}
	return state->people && state->humanTeam ? GAME_PHASE_READY : GAME_PHASE_NO_TEAM;
}

const char *gameState_phaseName(GamePhase phase) {
	static const char *const names[] = {"starting", "menu", "loading", "busy", "ready", "no_team"};
	return (size_t)phase < sizeof names / sizeof names[0] ? names[phase] : "unknown";
}

void gameState_formatDate(uint16_t dayWord, uint16_t year, char *out, size_t size) {
	unsigned timeCode = (unsigned)dayWord >> TIME_SHIFT;
	unsigned minutes = timeCode ? FIRST_SLOT_HOUR * 60 + (timeCode - 1) * QUARTER_HOUR : 0;
	(void)snprintf(
		out,
		size,
		"%u day %u %02u:%02u",
		(unsigned)year,
		(unsigned)dayWord & DAY_MASK,
		(minutes / 60) % 24,
		minutes % 60
	);
}

#ifdef __APPLE__
static bool readFlag(const uint8_t *object, ptrdiff_t offset, bool *out) {
	uint8_t value = 0;
	bool ok = object && memory_read(object + offset, &value, sizeof value);
	*out = ok && value != 0;
	return ok;
}

// The GAME singleton's processing and auto-play state; it is not created until the game starts up.
static void readGame(GameState *state) {
	uint8_t *game = NULL;
	uint8_t *autoPlayFile = NULL;
	uint32_t kind = 0;
	if (!memory_readPointer(game_address(RVA_MACOS_GAME), 0, &game)) {
		return;
	}
	(void)readFlag(game, GAME_PROCESSING, &state->processing);
	if (memory_readU32(game, GAME_PROCESSING_KIND, &kind)) {
		state->processingKind = (int32_t)kind;
	}
	(void)readFlag(game, GAME_AUTO_PLAY, &state->autoPlay);
	state->autoPlayRead = memory_readPointer(game, GAME_AUTO_PLAY_FILE, &autoPlayFile);
}

static void readSave(GameState *state) {
	uint8_t *saveSystem = NULL;
	uint8_t *name = NULL;
	if (memory_readPointer(game_address(RVA_MACOS_SAVE_SYSTEM), 0, &saveSystem) &&
			memory_readPointer(saveSystem, SAVE_SYSTEM_NAME, &name)) {
		(void)readSaveName(name, state->save, sizeof state->save);
	}
}

// The current date is only the game's while a game is in progress.
static void readDate(GameState *state) {
	uint16_t dayWord = 0;
	uint16_t year = 0;
	if (state->inProgress && memory_readU16(game_address(RVA_CURRENT_DATE), 0, &dayWord) &&
			memory_readU16(game_address(RVA_CURRENT_DATE), 2, &year) && year) {
		gameState_formatDate(dayWord, year, state->date, sizeof state->date);
	}
}

// A game string object (a u32 byte length, then the bytes, as database.c reads names): its text
// without a trailing ".fm", if it is valid UTF-8 text that fits `size`. Otherwise "" and false.
static bool readSaveName(const uint8_t *string, char *out, size_t size) {
	static const char extension[] = ".fm";
	size_t extensionLength = sizeof extension - 1;
	uint32_t length = 0;
	bool ok = memory_readU32(string, 0, &length) && length > 0 && length < size &&
						memory_read(string + sizeof length, out, length) &&
						configWriter_isText((const uint8_t *)out, length);
	out[ok ? length : 0] = 0;
	if (ok && length > extensionLength && strcmp(out + length - extensionLength, extension) == 0) {
		out[length - extensionLength] = 0;
	}
	return ok;
}
#endif
