// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "match.h"

#include "game.h"
#include "memory.h"
#include "protocol.h"
#include "rng.h"
#include "stream.h"

#include <string.h>

#ifdef _WIN32
#include <xmmintrin.h>
#else
#include <fenv.h>
#endif


enum {
	SEED_HEADER_SIZE = 13,
	SEED_LABEL_GAP = 8,
	MAX_LABEL_LENGTH = 256,
	BUILD_MODE = 1, // MatchBuilder_Create's mode for the user's own match (build, serialise)
	RUN_TO_END = 1,
};

// Input streams by worker slot, created on first use and kept: the game owns their storage.
static DataBuffer *inputStreams[HARNESS_MAX_THREADS];

uint8_t *match_build(uint8_t *fixture, uint32_t seed) {
	rng_seedThread(seed);
	_Alignas(16) uint8_t scratch[16] = {0};
	((FixturePrepareFn)game_function(RVA_FIXTURE_PREPARE_TEAMS))(fixture, scratch);
#ifdef __APPLE__
	uint8_t *match = ((MatchAllocFn)game_function(RVA_ALLOC_FROM_POOL))(MATCH_SIZE);
#else
	uint8_t *match = ((MatchCreateFn)game_function(RVA_ALLOC_FROM_POOL))();
#endif
	if (!match) {
		return NULL;
	}
	((MatchFn)game_function(RVA_MATCH_CTOR))(match);
	*(uintptr_t *)match = (uintptr_t)game_address(RVA_GAME_MATCH_VTABLE);
	match[MATCH_BUILD_FLAG] = 0;
	InitArgs args = {.fixture = fixture, .seed = seed, .scratch = scratch, .mode = BUILD_MODE};
	uint8_t flags[8] = {0, BUILD_MODE, 0xFF};
	((MatchInitFn)game_function(RVA_INIT_FROM_FIXTURE))(match, &args, flags);
	return match;
}

void match_freeBuilt(uint8_t *match) {
	((MatchFn)game_function(RVA_GAME_MATCH_DTOR))(match);
	((MatchFn)game_function(RVA_GAME_MATCH_POOL_FREE))(match);
}

DataBuffer *match_inputStream(unsigned slot) {
	if (slot >= HARNESS_MAX_THREADS) {
		return NULL;
	}
	if (!inputStreams[slot]) {
		inputStreams[slot] = stream_create();
	}
	return inputStreams[slot];
}

uint8_t *match_deserialize(ByteBuffer *bytes) {
	return match_deserializeOn(0, bytes);
}

uint8_t *match_deserializeOn(unsigned slot, ByteBuffer *bytes) {
	DataBuffer *stream = match_inputStream(slot);
	if (!stream) {
		return NULL;
	}
	stream_attachInput(stream, bytes->data, bytes->length);
	uint8_t *match = ((MatchCreateFn)game_function(RVA_GAME_MATCH_CREATE))();
	bool ok =
		match && ((MatchStreamFn)game_function(RVA_GAME_MATCH_DESERIALIZE))(match, &stream->input, 0);
	stream_detachInput(stream);
	if (!ok && match) {
		match_delete(match);
	}
	return ok ? match : NULL;
}

void match_delete(uint8_t *match) {
#ifdef __APPLE__
	// The deleting destructor, which takes no flags under the Itanium ABI.
	MatchFn destroy = (*(MatchFn **)match)[MATCH_DELETING_DESTRUCTOR_SLOT];
	destroy(match);
#else
	// The scalar deleting destructor (vtable slot 0), as the game's session code uses.
	MatchDeleteFn destroy = **(MatchDeleteFn **)match;
	destroy(match, 1);
#endif
}

void match_handToAssistant(const uint8_t *match) {
	for (int side = 0; side < 2; ++side) {
		const uint8_t *team = *(uint8_t *const *)(match + (side ? MATCH_TEAM_AWAY : MATCH_TEAM_HOME));
		uint8_t *manager = *(uint8_t *const *)(team + MATCH_TEAM_MANAGER);
		uint32_t flags = memory_loadU32(manager + MANAGER_FLAGS);
		flags = (flags & MANAGER_AI_MANAGED) ? flags & ~(uint32_t)MANAGER_HUMAN_IN_CONTROL
																				 : (uint32_t)MANAGER_HUMAN_NO_PLAN;
		memcpy(manager + MANAGER_FLAGS, &flags, sizeof flags);
	}
}

void match_play(uint8_t *match) {
#ifdef _WIN32
	unsigned csr = _mm_getcsr();
#else
	fenv_t environment;
	(void)fegetenv(&environment);
#endif
	((MatchPrepareFn)game_function(RVA_GAME_MATCH_PREPARE))(match, 0);
	((MatchRunFn)game_function(RVA_GAME_MATCH_RUN))(match, RUN_TO_END, -1);
	// Run switches to round-to-nearest + flush-to-zero; restore ours
#ifdef _WIN32
	_mm_setcsr(csr);
#else
	(void)fesetenv(&environment);
#endif
}

MatchResult match_result(const uint8_t *match) {
	const uint8_t *home = *(uint8_t *const *)(match + MATCH_TEAM_HOME);
	const uint8_t *away = *(uint8_t *const *)(match + MATCH_TEAM_AWAY);
	return (MatchResult){
		.seed = memory_loadU32(match + MATCH_SEED),
		.homeGoals = home[MATCH_TEAM_GOALS],
		.awayGoals = away[MATCH_TEAM_GOALS],
		.clockTicks = memory_loadU32(match + MATCH_CLOCK),
		.finished = (memory_loadU32(match + MATCH_FLAGS) >> 2) & 1,
		.finalRngState = memory_loadU64(match + MATCH_RNG_STATE),
	};
}

bool match_seedOffset(const uint8_t *bytes, size_t size, size_t *offset) {
	if (size < 40 || bytes[0] != 0x09 || bytes[1] != 0x08 || bytes[6] != '!') {
		return false;
	}
	uint32_t labelLength = memory_loadU32(bytes + SEED_HEADER_SIZE);
	*offset = SEED_HEADER_SIZE + sizeof(uint32_t) + (size_t)labelLength + SEED_LABEL_GAP;
	return labelLength < MAX_LABEL_LENGTH && *offset + sizeof(uint32_t) <= size;
}
