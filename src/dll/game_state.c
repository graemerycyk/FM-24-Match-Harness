// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game_state.h"

#include "config_writer.h"
#include "database.h"
#include "edition.h"
#include "game.h"
#include "memory.h"
#include "platform.h"

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
#ifndef GAME_UNLOCATED_SAVE_NAME
static bool readSaveName(const uint8_t *string, char *out, size_t size);
#endif
#if EDITION_ENGINE_PLUGIN
static GameSettle samples; // the job thread's samples of the game
static uint64_t sampleTimeMs(void);
#endif

bool gameState_read(GameState *state, bool findTeam) {
	*state = (GameState){.processingKind = -1};
#if EDITION_ENGINE_PLUGIN
	if (game_engineState() != GAME_ENGINE_ACCEPTED) {
		// The game has not loaded its engine yet, or has unloaded it: it is starting, and a load
		// must settle afresh once the engine is back.
		samples = (GameSettle){0};
		return true;
	}
#endif
	const uint8_t *options = game_address(RVA_MACOS_GAME_OPTIONS);
	if (!readFlag(options, GAME_OPTIONS_IN_PROGRESS, &state->inProgress)) {
		return false;
	}
	uint8_t *startScreen = NULL;
	state->startScreenSeen =
		memory_readPointer(game_address(RVA_MACOS_START_SCREEN), 0, &startScreen);
	state->people = database_countPeople();
#if EDITION_ENGINE_PLUGIN
	// The plugin's builds map the start screen to GAME, which the engine makes before it reads the
	// auto-play file: a game in progress has loaded only once it has settled.
	bool settled = gameState_settled(&samples, state->inProgress, state->people, sampleTimeMs());
	state->startScreenSeen = state->startScreenSeen && (!state->inProgress || settled);
#endif
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

#if EDITION_ENGINE_PLUGIN
bool gameState_settled(GameSettle *settle, bool inProgress, size_t people, uint64_t nowMs) {
	if (!settle->sampled || settle->inProgress != inProgress || settle->people != people ||
			nowMs - settle->lastMs > GAME_STATE_GAP_MS) {
		*settle =
			(GameSettle){.sampled = true, .inProgress = inProgress, .people = people, .sinceMs = nowMs};
	}
	settle->lastMs = nowMs;
	return inProgress && people && nowMs - settle->sinceMs >= GAME_STATE_SETTLE_MS;
}

void gameState_sample(void) {
	GameState state;
	(void)gameState_read(&state, false);
}

// A sample's time, in milliseconds of platform_ticks' monotonic clock.
static uint64_t sampleTimeMs(void) {
	return platform_ticks() / (platform_tickFrequency() / 1000);
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
// Where its processing state has not been located (GAME_UNLOCATED_PROCESSING), it is never read as
// processing.
static void readGame(GameState *state) {
	uint8_t *game = NULL;
	uint8_t *autoPlayFile = NULL;
#ifndef GAME_UNLOCATED_PROCESSING
	uint32_t kind = 0;
#endif
	if (!memory_readPointer(game_address(RVA_MACOS_GAME), 0, &game)) {
		return;
	}
#ifndef GAME_UNLOCATED_PROCESSING
	(void)readFlag(game, GAME_PROCESSING, &state->processing);
	if (memory_readU32(game, GAME_PROCESSING_KIND, &kind)) {
		state->processingKind = (int32_t)kind;
	}
#endif
	(void)readFlag(game, GAME_AUTO_PLAY, &state->autoPlay);
	state->autoPlayRead = memory_readPointer(game, GAME_AUTO_PLAY_FILE, &autoPlayFile);
}

// The loaded save's name, where it has been located (GAME_UNLOCATED_SAVE_NAME); else it is "".
static void readSave(GameState *state) {
#ifdef GAME_UNLOCATED_SAVE_NAME
	(void)state;
#else
	uint8_t *saveSystem = NULL;
	uint8_t *name = NULL;
	if (memory_readPointer(game_address(RVA_MACOS_SAVE_SYSTEM), 0, &saveSystem) &&
			memory_readPointer(saveSystem, SAVE_SYSTEM_NAME, &name)) {
		(void)readSaveName(name, state->save, sizeof state->save);
	}
#endif
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

#ifndef GAME_UNLOCATED_SAVE_NAME
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
#endif
