// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for the game's engine plugin (macos_stand_in_plugin.c), which the stand-in game loads
// and unloads as the game does its engine, and what the stand-in harness's tables say about it
// (macos_stand_in_harness.c): the functions a run calls are the stand-in's, every other function
// the harness checks starts with the engine's bytes, and the game's state objects are in its
// state, which is that of a game at its main menu until a save is loaded.
#pragma once

#include "game.h"

#include <stdint.h>


// The first bytes of the stand-in's "code": the signature of every function the harness checks.
#define STAND_IN_ENGINE_BYTES                                                                      \
	{0x46, 0x4D, 0x53, 0x74, 0x61, 0x6E, 0x64, 0x2D, 0x69, 0x6E, 0x20, 0x65, 0x6E, 0x67, 0x69, 0x6E, \
		0x65, 0x2C, 0x20, 0x33, 0x32, 0x20, 0x62, 0x79, 0x74, 0x65, 0x73, 0x20, 0x6C, 0x6F, 0x6E, 0x67}

typedef struct StandInState {
	uint8_t options[256]; // the game options object: no game in progress, until a save loads
	// [p]: the GAME object, with the auto-play file if it read one. The stand-in harness's tables
	// give it as the start screen too, as the game's builds' mappings do.
	const void *game;
	const void *saveSystem;  // [p], as is the next: none
	const void *humanMatches;
	const void *peopleList;   // [p]: none, until a save loads
	const void *matchContext; // [p]: the engine's serialisation context
	uint16_t date[2];         // the in-game date, once a save loads
	const void *vtables[2];   // what the vptrs of fixtures and teams would be
} StandInState;

// A human manager's vtable is standInPersonVtable[STAND_IN_PERSON_VPTR], with the slots the
// harness reads around it: the offset of the person's fields before it, and the getter after.
enum {
	STAND_IN_PERSON_VPTR = -PERSON_FIELDS_OFFSET / (int)sizeof(void *),
	STAND_IN_PERSON_SLOTS =
		STAND_IN_PERSON_VPTR + PERSON_FIELDS_GETTER_SLOT / (int)sizeof(void *) + 1,
};
