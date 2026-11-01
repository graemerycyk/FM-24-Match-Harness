// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// match.c against fakes of the game's functions and objects: what it hands the engine to build,
// load and play a match, where it finds the seed in serialised matches, and what it reads of a
// played one, in the edition's layout.

#include "edition.h"
#include "test.h"
#include "tests.h"

#include <fenv.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// match.c's calls into the rest of the harness and the game, renamed to the fakes below, and its
// own functions renamed so that they do not meet dll_test.c's fakes of them.
#define log_message matchTest_log
#define game_function matchTest_function
#define game_address matchTest_address
#define game_formatVersion matchTest_formatVersion
#define stream_create matchTest_streamCreate
#define stream_attachInput matchTest_attachInput
#define stream_detachInput matchTest_detachInput
#define rng_seedThread matchTest_seedThread
#define match_build matchTest_build
#define match_freeBuilt matchTest_freeBuilt
#define match_inputStream matchTest_inputStream
#define match_deserialize matchTest_deserialize
#define match_deserializeOn matchTest_deserializeOn
#define match_delete matchTest_delete
#define match_handToAssistant matchTest_handToAssistant
#define match_play matchTest_play
#define match_result matchTest_result
#define match_seedOffset matchTest_seedOffset
#define match_context matchTest_context
#include "../src/dll/match.c" // NOLINT(bugprone-suspicious-include): call the engine's fakes.
#undef log_message
#undef game_function
#undef game_address
#undef game_formatVersion
#undef stream_create
#undef stream_attachInput
#undef stream_detachInput
#undef rng_seedThread


enum { MAX_CALLS = 16, LOGGED_LENGTH = 4096, FORMAT_VERSION = 0x004C };

void matchTest_log(const char *format, ...);
uint16_t matchTest_formatVersion(void);

// What the fake engine was asked to do, in order, with its arguments.
typedef struct EngineCall {
	uint32_t rva;
	const void *arguments[4];
	unsigned flags;
} EngineCall;

static EngineCall calls[MAX_CALLS];
static size_t callCount;
static char logged[LOGGED_LENGTH];
static uint16_t formatVersion; // what game_formatVersion() gives
static uint32_t seededWith;
static bool seeded;

// The game's objects: the match the factory, create or the pool allocator returns, its vtable, or
// another class's, and the serialisation context's global.
static _Alignas(16) uint8_t engineMatch[MATCH_SIZE];
static void *gameMatchVtable[4];
static void *otherVtable[4];
static void **madeVtable;     // what the factory or create leave in the match's vptr
static void *contextGlobal;   // the engine's serialisation context, or NULL before it is set up
static bool deserialiseFails; // the next deserialise fails
static uint8_t initArgs[sizeof(InitArgs)];
#if EDITION_ENGINE_PLUGIN
static uint8_t initFlags[MATCH_BUILD_FLAGS_SIZE];
#else
static uint8_t initFlags[8];
static size_t allocatedSize; // what the pool allocator was asked for
#endif
static uint8_t attached[64]; // the input bytes deserialise found attached to the stream
static size_t attachedSize;

static SharedBuffer sharedBuffer;
static DataBuffer dataBuffer;
static uint8_t inputStream[32];

static void record(uint32_t rva, const void *a, const void *b, const void *c, unsigned flags) {
	if (callCount < MAX_CALLS) {
		calls[callCount++] = (EngineCall){rva, {a, b, c}, flags};
	}
}

void matchTest_log(const char *format, ...) {
	size_t used = strlen(logged);
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(logged + used, sizeof logged - used, format, arguments);
	va_end(arguments);
	used = strlen(logged);
	if (used + 1 < sizeof logged) {
		logged[used] = '\n';
		logged[used + 1] = 0;
	}
}

uint16_t matchTest_formatVersion(void) {
	return formatVersion;
}

void matchTest_seedThread(uint32_t seed) {
	seeded = true;
	seededWith = seed;
}

DataBuffer *matchTest_streamCreate(void) {
	dataBuffer = (DataBuffer){.shared = &sharedBuffer, .input = inputStream};
	return &dataBuffer;
}

void matchTest_attachInput(DataBuffer *buffer, uint8_t *bytes, size_t size) {
	buffer->shared->begin = bytes;
	buffer->shared->end = bytes + size;
}

void matchTest_detachInput(DataBuffer *buffer) {
	buffer->shared->begin = NULL;
	buffer->shared->end = NULL;
}

static uint8_t *makeMatch(void) {
	memset(engineMatch, 0xAA, sizeof engineMatch);
	memcpy(engineMatch, &madeVtable, sizeof madeVtable);
	return engineMatch;
}

// NOLINTBEGIN(readability-non-const-parameter): fakes keep the game's signatures.
static uint8_t *fakeMake(void) {
	record(RVA_GAME_MATCH_CREATE, NULL, NULL, NULL, 0);
	return makeMatch();
}

#if EDITION_ENGINE_PLUGIN
static uint8_t *fakeFactory(void) {
	record(RVA_MACOS_MATCH_FACTORY, NULL, NULL, NULL, 0);
	return makeMatch();
}

static char fakeDeserialise(uint8_t *match, void **stream, void *context, unsigned flags) {
	record(RVA_GAME_MATCH_DESERIALIZE, match, stream, context, flags);
#else
static uint8_t *fakeAllocate(size_t size) {
	record(RVA_ALLOC_FROM_POOL, NULL, NULL, NULL, 0);
	allocatedSize = size;
	return makeMatch();
}

static void fakeConstruct(uint8_t *match) {
	record(RVA_MATCH_CTOR, match, NULL, NULL, 0);
	memset(match, 0, MATCH_SIZE);
}

static char fakeDeserialise(uint8_t *match, void **stream, unsigned flags) {
	record(RVA_GAME_MATCH_DESERIALIZE, match, stream, NULL, flags);
#endif
	SharedBuffer *shared = dataBuffer.shared;
	attachedSize = shared->begin ? (size_t)(shared->end - shared->begin) : 0;
	if (attachedSize <= sizeof attached) {
		memcpy(attached, shared->begin, attachedSize);
	}
	return (char)!deserialiseFails;
}

static void fakePrepareTeams(uint8_t *fixture, void *scratch) {
	record(RVA_FIXTURE_PREPARE_TEAMS, fixture, scratch, NULL, 0);
}

static void fakeInit(uint8_t *match, InitArgs *args, uint8_t *flags) {
	record(RVA_INIT_FROM_FIXTURE, match, args, flags, 0);
	memcpy(initArgs, args, sizeof initArgs);
	memcpy(initFlags, flags, sizeof initFlags);
}

static void fakeDelete(uint8_t *match) {
	record(RVA_GAME_MATCH_DTOR, match, *(void **)match, NULL, 0);
}

static void fakePrepare(uint8_t *match, char unknown) {
	record(RVA_GAME_MATCH_PREPARE, match, NULL, NULL, (unsigned)unknown);
}

static int fakeRun(uint8_t *match, unsigned mode, int untilTick) {
	record(RVA_GAME_MATCH_RUN, match, NULL, NULL, mode);
	(void)fesetround(FE_TOWARDZERO); // the engine changes the floating-point environment
	return untilTick;
}
// NOLINTEND(readability-non-const-parameter)

#define FUNCTION(fake)                                                                             \
	do {                                                                                             \
		VoidFn function = NULL;                                                                        \
		memcpy(&function, &(fake), sizeof function);                                                   \
		return function;                                                                               \
	} while (0)

VoidFn matchTest_function(uint32_t rva) {
	static MatchCreateFn const make = fakeMake;
	static MatchStreamFn const deserialise = fakeDeserialise;
	static FixturePrepareFn const prepareTeams = fakePrepareTeams;
	static MatchInitFn const init = fakeInit;
	static MatchPrepareFn const prepare = fakePrepare;
	static MatchRunFn const run = fakeRun;
	switch (rva) {
		case RVA_GAME_MATCH_CREATE: FUNCTION(make);
		case RVA_GAME_MATCH_DESERIALIZE: FUNCTION(deserialise);
		case RVA_FIXTURE_PREPARE_TEAMS: FUNCTION(prepareTeams);
		case RVA_INIT_FROM_FIXTURE: FUNCTION(init);
		case RVA_GAME_MATCH_PREPARE: FUNCTION(prepare);
		case RVA_GAME_MATCH_RUN: FUNCTION(run);
#if EDITION_ENGINE_PLUGIN
		case RVA_MACOS_MATCH_FACTORY: {
			static MatchCreateFn const factory = fakeFactory;
			FUNCTION(factory);
		}
#else
		case RVA_ALLOC_FROM_POOL: {
			static MatchAllocFn const allocate = fakeAllocate;
			FUNCTION(allocate);
		}
		case RVA_MATCH_CTOR: {
			static MatchFn const construct = fakeConstruct;
			FUNCTION(construct);
		}
#endif
		default: ASSERT_INT_EQ((int)rva, 0); return NULL;
	}
}

void *matchTest_address(uint32_t rva) {
	switch (rva) {
		case RVA_GAME_MATCH_VTABLE: return gameMatchVtable;
#if EDITION_ENGINE_PLUGIN
		case RVA_MACOS_MATCH_CONTEXT: return &contextGlobal;
#endif
		default: ASSERT_INT_EQ((int)rva, 0); return NULL;
	}
}

static void reset(void) {
	callCount = 0;
	logged[0] = 0;
	formatVersion = FORMAT_VERSION;
	seeded = false;
	deserialiseFails = false;
	madeVtable = gameMatchVtable;
	MatchFn deleting = fakeDelete;
	memcpy(&gameMatchVtable[MATCH_DELETING_DESTRUCTOR_SLOT], &deleting, sizeof deleting);
	memcpy(&otherVtable[MATCH_DELETING_DESTRUCTOR_SLOT], &deleting, sizeof deleting);
	static int context;
	contextGlobal = &context;
}

static bool called(size_t index, uint32_t rva) {
	return index < callCount && calls[index].rva == rva;
}

// A serialised match's header with a label of `labelLength` bytes and the seed after it, as the
// edition writes it; returns its size.
static size_t writeHeader(uint8_t *out, uint32_t labelLength, uint32_t seed) {
#if EDITION_ENGINE_PLUGIN
	const uint8_t start[] = {27, 0, 0, 1, FORMAT_VERSION & 0xFF, FORMAT_VERSION >> 8, 0, 0x02};
	size_t size = sizeof start + sizeof(uint32_t); // the game version and format; a tag and a u32
	memcpy(out, start, sizeof start);
	memset(out + sizeof start, 0x11, sizeof(uint32_t));
#else
	const uint8_t start[] = {0x09, 0x08, 0, 0, 0, 0, '!', 0, 0, 0, 0, 0, 0};
	size_t size = sizeof start;
	memcpy(out, start, sizeof start);
#endif
	memcpy(out + size, &labelLength, sizeof labelLength);
	size += sizeof labelLength;
	memset(out + size, 'L', labelLength);
	size += labelLength + SEED_LABEL_GAP;
	memcpy(out + size, &seed, sizeof seed);
	return size + sizeof seed;
}

TEST(finds_the_seed_after_the_editions_header) {
	reset();
	uint8_t bytes[400] = {0};
	size_t offset = 0;
	// After the header, the label and the gap: FM24 13 + 4 + 8 bytes, FM27 12 + 4 + 4.
	size_t size = writeHeader(bytes, 5, 0x12345678);
	size_t padded = size < 40 ? 40 : size;
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, padded, &offset), true);
	ASSERT_INT_EQ((int)offset, (int)(size - sizeof(uint32_t)));
	ASSERT_INT_EQ((int)memory_loadU32(bytes + offset), 0x12345678);
#if EDITION_ENGINE_PLUGIN
	ASSERT_INT_EQ((int)offset, 25);
	// The seed may end the bytes; one byte fewer and it is cut off.
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, size, &offset), true);
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, size - 1, &offset), false);
	// An empty label.
	size = writeHeader(bytes, 0, 7);
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, size, &offset) && offset == 20, true);
	// Another format version than the running build's, or no build to say, is not read.
	writeHeader(bytes, 5, 7);
	bytes[HEADER_FORMAT_VERSION] = FORMAT_VERSION - 1;
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
	writeHeader(bytes, 5, 7);
	formatVersion = 0;
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
	formatVersion = FORMAT_VERSION;
	// Nor a header whose zero byte or tag is wrong.
	bytes[HEADER_ZERO] = 1;
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
	writeHeader(bytes, 5, 7);
	bytes[HEADER_TAG] = 0x03;
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
	// FM24's header is not FM27's.
	const uint8_t fm24[40] = {0x09, 0x08, 0, 0, 0, 0, '!'};
	ASSERT_BOOL_EQ(matchTest_seedOffset(fm24, sizeof fm24, &offset), false);
	// Too short to hold the label's length.
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, SEED_HEADER_SIZE + 3, &offset), false);
#else
	ASSERT_INT_EQ((int)offset, 30);
	bytes[6] = '?';
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
#endif
	// An over-long label is not a match's.
	size = writeHeader(bytes, MAX_LABEL_LENGTH - 1, 7);
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, size, &offset), true);
	size = writeHeader(bytes, MAX_LABEL_LENGTH, 7);
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, size, &offset), false);
	uint32_t huge = UINT32_MAX;
	memcpy(bytes + SEED_HEADER_SIZE, &huge, sizeof huge);
	ASSERT_BOOL_EQ(matchTest_seedOffset(bytes, sizeof bytes, &offset), false);
}

TEST(builds_a_match_as_the_engines_builder_does) {
	reset();
	uint8_t fixture[16];
	uint8_t *match = matchTest_build(fixture, 0xC0FFEE);
	ASSERT_BOOL_EQ(match == engineMatch, true);
	ASSERT_BOOL_EQ(seeded && seededWith == 0xC0FFEE, true);
	// Prepare the teams into a scratch block, make the match, then initialise it from the fixture.
	ASSERT_BOOL_EQ(called(0, RVA_FIXTURE_PREPARE_TEAMS), true);
	const void *scratch = calls[0].arguments[1];
	ASSERT_BOOL_EQ(calls[0].arguments[0] == fixture, true);
#if EDITION_ENGINE_PLUGIN
	ASSERT_BOOL_EQ(called(1, RVA_MACOS_MATCH_FACTORY), true);
	ASSERT_BOOL_EQ(called(2, RVA_INIT_FROM_FIXTURE) && callCount == 3, true);
	// InitArgs: the fixture, the scratch block, zeros, the seed at +0x20 and the mode at +0x24.
	uint8_t expected[sizeof(InitArgs)] = {0};
	memcpy(expected, &(void *){fixture}, sizeof(void *));
	memcpy(expected + 8, &scratch, sizeof scratch);
	memcpy(expected + 0x20, &(uint32_t){0xC0FFEE}, sizeof(uint32_t));
	expected[0x24] = 1;
	ASSERT_INT_EQ(memcmp(initArgs, expected, sizeof expected), 0);
	// The flags block: 0, the mode and 0xFF, then zeros, 16 bytes in all.
	const uint8_t flags[MATCH_BUILD_FLAGS_SIZE] = {0, 1, 0xFF};
	ASSERT_INT_EQ(memcmp(initFlags, flags, sizeof flags), 0);
#else
	ASSERT_BOOL_EQ(called(1, RVA_ALLOC_FROM_POOL) && allocatedSize == MATCH_SIZE, true);
	ASSERT_BOOL_EQ(called(2, RVA_MATCH_CTOR) && called(3, RVA_INIT_FROM_FIXTURE), true);
	ASSERT_BOOL_EQ(*(void **)match == (void *)gameMatchVtable && match[MATCH_BUILD_FLAG] == 0, true);
	InitArgs args;
	memcpy(&args, initArgs, sizeof args);
	ASSERT_BOOL_EQ(args.fixture == fixture && args.scratch == scratch, true);
	ASSERT_BOOL_EQ(args.seed == 0xC0FFEE && args.mode == 1, true);
	const uint8_t flags[8] = {0, 1, 0xFF};
	ASSERT_INT_EQ(memcmp(initFlags, flags, sizeof flags), 0);
#endif
	ASSERT_BOOL_EQ(calls[callCount - 1].arguments[0] == match, true);
}

TEST(loads_matches_into_a_game_match) {
	reset();
	uint8_t bytes[] = {1, 2, 3, 4, 5};
	ByteBuffer input = {.data = bytes, .length = sizeof bytes};
	uint8_t *match = matchTest_deserializeOn(2, &input);
	ASSERT_BOOL_EQ(match == engineMatch, true);
	ASSERT_BOOL_EQ(called(0, RVA_GAME_MATCH_CREATE) && called(1, RVA_GAME_MATCH_DESERIALIZE), true);
	// Deserialise reads the slot's input stream, which holds the bytes for the call.
	ASSERT_BOOL_EQ(calls[1].arguments[0] == match, true);
	ASSERT_BOOL_EQ(calls[1].arguments[1] == (void *)&dataBuffer.input, true);
	ASSERT_BOOL_EQ(attachedSize == sizeof bytes && memcmp(attached, bytes, sizeof bytes) == 0, true);
	ASSERT_BOOL_EQ(sharedBuffer.begin == NULL, true);
	ASSERT_INT_EQ((int)calls[1].flags, 0);
#if EDITION_ENGINE_PLUGIN
	// ... with the engine's serialisation context.
	ASSERT_BOOL_EQ(calls[1].arguments[2] == contextGlobal, true);
#endif
	// A match deserialise cannot load is deleted.
	reset();
	deserialiseFails = true;
	ASSERT_BOOL_EQ(matchTest_deserializeOn(2, &input) == NULL, true);
	ASSERT_BOOL_EQ(called(2, RVA_GAME_MATCH_DTOR) && callCount == 3, true);
}

#if EDITION_ENGINE_PLUGIN
TEST(refuses_matches_the_engine_made_without_game_matchs_vtable) {
	uint8_t bytes[] = {1, 2, 3};
	ByteBuffer input = {.data = bytes, .length = sizeof bytes};
	// With no GAME_MATCH factory, create makes a plain match: deleted through its own vtable, and
	// never loaded into.
	reset();
	madeVtable = otherVtable;
	ASSERT_BOOL_EQ(matchTest_deserializeOn(1, &input) == NULL, true);
	ASSERT_BOOL_EQ(called(0, RVA_GAME_MATCH_CREATE) && called(1, RVA_GAME_MATCH_DTOR), true);
	ASSERT_BOOL_EQ(callCount == 2 && calls[1].arguments[1] == (void *)otherVtable, true);
	ASSERT_BOOL_EQ(
		strstr(logged, "refusing: the engine created a match with the vtable") != NULL, true
	);
	// A factory that made something else is refused before the match is built, and left alone.
	reset();
	madeVtable = otherVtable;
	uint8_t fixture[16];
	ASSERT_BOOL_EQ(matchTest_build(fixture, 1) == NULL, true);
	ASSERT_BOOL_EQ(called(1, RVA_MACOS_MATCH_FACTORY) && callCount == 2, true);
	ASSERT_BOOL_EQ(
		strstr(logged, "refusing: the engine built a match with the vtable") != NULL, true
	);
	// Before the engine sets its serialisation context up, nothing is loaded.
	reset();
	contextGlobal = NULL;
	ASSERT_BOOL_EQ(matchTest_deserializeOn(1, &input) == NULL && callCount == 0, true);
	ASSERT_BOOL_EQ(strstr(logged, "has not set up its match serialisation context") != NULL, true);
	ASSERT_BOOL_EQ(matchTest_context() == NULL, true);
}
#endif

// A played match's teams, with their managers, in the edition's layout.
typedef struct PlayedMatch {
	_Alignas(void *) uint8_t teams[2][MATCH_TEAM_MANAGER + sizeof(void *) + MATCH_TEAM_GOALS];
	_Alignas(uint32_t) uint8_t managers[2][MANAGER_FLAGS + sizeof(uint32_t)];
} PlayedMatch;

static void
setUpPlayed(PlayedMatch *played, uint8_t *match, uint32_t homeFlags, uint32_t awayFlags) {
	memset(match, 0, MATCH_SIZE);
	memset(played, 0, sizeof *played);
	for (int side = 0; side < 2; ++side) {
		uint8_t *team = played->teams[side];
		uint8_t *manager = played->managers[side];
		memcpy(match + (side ? MATCH_TEAM_AWAY : MATCH_TEAM_HOME), &team, sizeof team);
		memcpy(team + MATCH_TEAM_MANAGER, &manager, sizeof manager);
		memcpy(manager + MANAGER_FLAGS, side ? &awayFlags : &homeFlags, sizeof(uint32_t));
	}
}

TEST(hands_both_teams_to_the_assistant) {
	reset();
	PlayedMatch played;
	// A human in control loses it, and the plan of whoever built the input with it; an AI-managed
	// team keeps its flags but control.
	setUpPlayed(&played, engineMatch, MANAGER_HUMAN_IN_CONTROL | 0x100, MANAGER_AI_MANAGED | 0x1);
	matchTest_handToAssistant(engineMatch);
	ASSERT_INT_EQ((int)memory_loadU32(played.managers[0] + MANAGER_FLAGS), MANAGER_HUMAN_NO_PLAN);
	ASSERT_INT_EQ((int)memory_loadU32(played.managers[1] + MANAGER_FLAGS), MANAGER_AI_MANAGED);
}

TEST(plays_and_reads_the_result_in_the_editions_layout) {
	reset();
	PlayedMatch played;
	setUpPlayed(&played, engineMatch, 0, 0);
	played.teams[0][MATCH_TEAM_GOALS] = 3;
	played.teams[1][MATCH_TEAM_GOALS] = 1;
	memcpy(engineMatch + MATCH_SEED, &(uint32_t){99}, sizeof(uint32_t));
	memcpy(engineMatch + MATCH_CLOCK, &(uint32_t){22000}, sizeof(uint32_t));
	memcpy(engineMatch + MATCH_FLAGS, &(uint32_t){4}, sizeof(uint32_t));
	memcpy(engineMatch + MATCH_RNG_STATE, &(uint64_t){0x0123456789ABCDEFULL}, sizeof(uint64_t));
	int rounding = fegetround();
	matchTest_play(engineMatch);
	// Prepared, then run to the end; the engine's floating-point environment is undone.
	ASSERT_BOOL_EQ(called(0, RVA_GAME_MATCH_PREPARE) && called(1, RVA_GAME_MATCH_RUN), true);
	ASSERT_INT_EQ((int)calls[1].flags, 1);
	ASSERT_INT_EQ(fegetround(), rounding);
	MatchResult result = matchTest_result(engineMatch);
	ASSERT_BOOL_EQ(result.seed == 99 && result.homeGoals == 3 && result.awayGoals == 1, true);
	ASSERT_BOOL_EQ(result.clockTicks == 22000 && result.finished, true);
	ASSERT_BOOL_EQ(result.finalRngState == 0x0123456789ABCDEFULL, true);
}

void registerMatchTests(void) {
	test_run(
		"match finds the seed after the edition's header", test_finds_the_seed_after_the_editions_header
	);
	test_run(
		"match builds a match as the engine's builder does",
		test_builds_a_match_as_the_engines_builder_does
	);
	test_run("match loads matches into a GAME_MATCH", test_loads_matches_into_a_game_match);
#if EDITION_ENGINE_PLUGIN
	test_run(
		"match refuses matches the engine made without GAME_MATCH's vtable",
		test_refuses_matches_the_engine_made_without_game_matchs_vtable
	);
#endif
	test_run("match hands both teams to the assistant", test_hands_both_teams_to_the_assistant);
	test_run(
		"match plays and reads the result in the edition's layout",
		test_plays_and_reads_the_result_in_the_editions_layout
	);
}
