// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The harness as the plugin test (macos_test.c) has the stand-in game load it: the harness's own
// sources, built with tables and a supported build for the stand-in engine plugin
// (macos_stand_in_plugin.h) in place of the game's, serving the socket FMNN_TEST_SOCKET names in
// place of the user's.

#include "channel.h"
#include "edition.h"
#include "game.h"
#include "game_macos.h"
#include "macos_builds.h"
#include "macos_stand_in_offsets.h"
#include "macos_stand_in_plugin.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static bool standInSocketPath(char *out, size_t size);

#define channel_socketPath standInSocketPath
#include "../src/dll/main_macos.c" // NOLINT(bugprone-suspicious-include): serve a test socket.
#undef channel_socketPath

#if defined(__aarch64__)
#define STAND_IN_ARCHITECTURE "arm64"
#else
#define STAND_IN_ARCHITECTURE "x86_64"
#endif

#define STAND_IN_STATE_FIELD(field) (STAND_IN_STATE + (uint32_t)offsetof(StandInState, field))

// Every requirement of game_macos.c, sorted by its Windows RVA: the stand-in engine's functions
// for those a run calls, the engine bytes for the others, the data in the state.
static const MacosAddress addresses[] = {
	{RVA_DATA_BUFFER_CTOR, STAND_IN_DATABUFFERCTOR},
	{RVA_GAME_MATCH_CREATE, STAND_IN_MATCHCREATE},
	{RVA_MATCH_CTOR, STAND_IN_ENGINE},
	{RVA_GAME_MATCH_DTOR, STAND_IN_MATCHDTOR},
	{RVA_GAME_MATCH_PREPARE, STAND_IN_MATCHPREPARE},
	{RVA_GAME_MATCH_RUN, STAND_IN_MATCHRUN},
	{RVA_GAME_MATCH_SERIALIZE, STAND_IN_ENGINE},
	{RVA_GAME_MATCH_DESERIALIZE, STAND_IN_MATCHDESERIALIZE},
	{RVA_FIXTURE_PREPARE_TEAMS, STAND_IN_ENGINE},
	{RVA_GAME_MATCH_POOL_FREE, STAND_IN_MATCHPOOLFREE},
	{RVA_INIT_FROM_FIXTURE, STAND_IN_ENGINE},
	{RVA_GAME_MATCH_VTABLE, STAND_IN_GAMEMATCHVTABLE},
	{RVA_FIXTURE_VTABLE, STAND_IN_STATE_FIELD(vtables[0])},
	{RVA_HUMAN_NON_PLAYER_VTABLE,
		STAND_IN_PERSONVTABLE + (uint32_t)(STAND_IN_PERSON_VPTR * sizeof(void *))},
	{RVA_TEAM_VTABLE, STAND_IN_STATE_FIELD(vtables[1])},
	{RVA_CURRENT_DATE, STAND_IN_STATE_FIELD(date)},
	{RVA_HUMAN_MATCHES_HOLDER, STAND_IN_STATE_FIELD(humanMatches)},
	{RVA_PEOPLE_LIST, STAND_IN_STATE_FIELD(peopleList)},
	{RVA_MACOS_THREAD_RNG, STAND_IN_THREADRNG},
	{RVA_MACOS_PERSON_FORENAME, STAND_IN_ENGINE},
	{RVA_MACOS_GAME, STAND_IN_STATE_FIELD(game)},
	{RVA_MACOS_SAVE_SYSTEM, STAND_IN_STATE_FIELD(saveSystem)},
	{RVA_MACOS_GAME_OPTIONS, STAND_IN_STATE_FIELD(options)},
	{RVA_MACOS_START_SCREEN, STAND_IN_STATE_FIELD(game)},
	{RVA_MACOS_MATCH_FACTORY, STAND_IN_MATCHFACTORY},
	{RVA_MACOS_MATCH_CONTEXT, STAND_IN_STATE_FIELD(matchContext)},
};

// The engine bytes, checked as the game's code is; the stand-in engine's functions are the
// compiler's, so their signatures are empty.
static const MacosSignature signatures[] = {
	{STAND_IN_ENGINE, MACOS_SIGNATURE_MAX, STAND_IN_ENGINE_BYTES},
	{STAND_IN_DATABUFFERCTOR, 0, {0}},
	{STAND_IN_THREADRNG, 0, {0}},
	{STAND_IN_MATCHFACTORY, 0, {0}},
	{STAND_IN_MATCHCREATE, 0, {0}},
	{STAND_IN_MATCHDESERIALIZE, 0, {0}},
	{STAND_IN_MATCHPREPARE, 0, {0}},
	{STAND_IN_MATCHRUN, 0, {0}},
	{STAND_IN_MATCHDTOR, 0, {0}},
	{STAND_IN_MATCHPOOLFREE, 0, {0}},
};

static const MacosTables tables = {
	.architecture = STAND_IN_ARCHITECTURE,
	.addresses = addresses,
	.addressCount = sizeof addresses / sizeof addresses[0],
	.signatures = signatures,
	.signatureCount = sizeof signatures / sizeof signatures[0],
};

const MacosBuildTables macosBuildTables[] = {{STAND_IN_UUID, &tables, 0x004C}};
const size_t macosBuildTableCount = 1;

const MacosBuild macosBuilds[] = {{STAND_IN_ARCHITECTURE, STAND_IN_UUID, "stand-in", 1}};
const size_t macosBuildCount = 1;

const MacosBuild *macosBuilds_find(const char *architecture, const uint8_t uuid[16]) {
	return strcmp(architecture, macosBuilds[0].architecture) == 0 &&
						 memcmp(uuid, macosBuilds[0].uuid, sizeof macosBuilds[0].uuid) == 0
					 ? &macosBuilds[0]
					 : NULL;
}

static bool standInSocketPath(char *out, size_t size) {
	const char *path = getenv(EDITION_ENV_PREFIX "TEST_SOCKET");
	int written = path ? snprintf(out, size, "%s", path) : -1;
	return written > 0 && (size_t)written < size;
}
