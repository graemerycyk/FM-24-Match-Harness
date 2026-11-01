// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "match.h"

#include "game.h"
#include "memory.h"
#include "protocol.h"
#include "rng.h"
#include "stream.h"

#include <string.h>

#if EDITION_ENGINE_PLUGIN
#include "log.h"
#endif

#ifdef _WIN32
#include <xmmintrin.h>
#else
#include <fenv.h>
#endif


#if EDITION_ENGINE_PLUGIN
// The header of a serialised match: the game's version (4 bytes), the format version (u16), a zero,
// a tag and a u32; then the label (a u32 length and its bytes), a u32, and the seed.
enum {
	SEED_HEADER_SIZE = 12,
	HEADER_FORMAT_VERSION = 4,
	HEADER_ZERO = 6,
	HEADER_TAG = 7,
	HEADER_TAG_VALUE = 0x02,
	SEED_LABEL_GAP = 4,
};
#else
enum {
	SEED_HEADER_SIZE = 13,
	SEED_LABEL_GAP = 8,
};
#endif

enum {
	MAX_LABEL_LENGTH = 256,
	BUILD_MODE = 1, // MatchBuilder_Create's mode for the user's own match (build, serialise)
	RUN_TO_END = 1,
};

// Input streams by worker slot, created on first use and kept: the game owns their storage.
static DataBuffer *inputStreams[HARNESS_MAX_THREADS];

#if EDITION_ENGINE_PLUGIN
static bool isGameMatch(const uint8_t *match, const char *made);

// As the engine plugin's match builder does: it prepares the teams, makes the match with the
// GAME_MATCH factory (which also sets the vtable and clears the build flag), then initialises it
// from the fixture with the seed and mode in its InitArgs and a 16-byte flags block.
uint8_t *match_build(uint8_t *fixture, uint32_t seed) {
	rng_seedThread(seed);
	_Alignas(16) uint8_t scratch[16] = {0};
	((FixturePrepareFn)game_function(RVA_FIXTURE_PREPARE_TEAMS))(fixture, scratch);
	uint8_t *match = ((MatchCreateFn)game_function(RVA_MACOS_MATCH_FACTORY))();
	if (!match || !isGameMatch(match, "built")) {
		return NULL; // what else the factory made is left alone
	}
	InitArgs args = {.fixture = fixture, .scratch = scratch, .seed = seed, .mode = BUILD_MODE};
	uint8_t flags[MATCH_BUILD_FLAGS_SIZE] = {0, BUILD_MODE, 0xFF};
	((MatchInitFn)game_function(RVA_INIT_FROM_FIXTURE))(match, &args, flags);
	return match;
}
#else
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
#endif

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
#if EDITION_ENGINE_PLUGIN
	// The engine creates a GAME_MATCH through the factory it registers for the thread, or by default;
	// without one it would make a plain match, which the harness does not load into.
	DataBuffer *stream = match_inputStream(slot);
	void *context = stream ? match_context() : NULL;
	if (!context) {
		return NULL;
	}
	stream_attachInput(stream, bytes->data, bytes->length);
	uint8_t *match = ((MatchCreateFn)game_function(RVA_GAME_MATCH_CREATE))();
	bool ok =
		match && isGameMatch(match, "created") &&
		((MatchStreamFn)game_function(RVA_GAME_MATCH_DESERIALIZE))(match, &stream->input, context, 0);
#else
	DataBuffer *stream = match_inputStream(slot);
	if (!stream) {
		return NULL;
	}
	stream_attachInput(stream, bytes->data, bytes->length);
	uint8_t *match = ((MatchCreateFn)game_function(RVA_GAME_MATCH_CREATE))();
	bool ok =
		match && ((MatchStreamFn)game_function(RVA_GAME_MATCH_DESERIALIZE))(match, &stream->input, 0);
#endif
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
#if EDITION_ENGINE_PLUGIN
	uint16_t version = game_formatVersion();
	if (size < SEED_HEADER_SIZE + sizeof(uint32_t) || version == 0 ||
			memory_loadU16(bytes + HEADER_FORMAT_VERSION) != version || bytes[HEADER_ZERO] != 0 ||
			bytes[HEADER_TAG] != HEADER_TAG_VALUE) {
		return false;
	}
#else
	if (size < 40 || bytes[0] != 0x09 || bytes[1] != 0x08 || bytes[6] != '!') {
		return false;
	}
#endif
	uint32_t labelLength = memory_loadU32(bytes + SEED_HEADER_SIZE);
	*offset = SEED_HEADER_SIZE + sizeof(uint32_t) + (size_t)labelLength + SEED_LABEL_GAP;
	return labelLength < MAX_LABEL_LENGTH && *offset + sizeof(uint32_t) <= size;
}

#if EDITION_ENGINE_PLUGIN
void *match_context(void) {
	void *const *global = game_address(RVA_MACOS_MATCH_CONTEXT);
	void *context = global ? *global : NULL;
	if (!context) {
		log_message("refusing: the engine has not set up its match serialisation context");
	}
	return context;
}

// Whether the engine `made` a GAME_MATCH: a match that carries its vtable. Logs it if not.
static bool isGameMatch(const uint8_t *match, const char *made) {
	const void *vtable = *(const void *const *)match;
	const void *expected = game_address(RVA_GAME_MATCH_VTABLE);
	if (vtable == expected) {
		return true;
	}
	log_message(
		"refusing: the engine %s a match with the vtable %p, not GAME_MATCH's (%p)",
		made,
		vtable,
		expected
	);
	return false;
}
#endif
