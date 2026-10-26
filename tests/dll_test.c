// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "test.h"
#include "tests.h"

#include <stdlib.h>


static int traceAllocationsUntilFailure = -1;
static void *traceRealloc(void *pointer, size_t size);

#define realloc traceRealloc
#include "../src/dll/trace.c" // NOLINT(bugprone-suspicious-include): inject trace allocation failures.
#undef realloc

#include "../src/dll/config_writer.c" // NOLINT(bugprone-suspicious-include): exercise UTF-8 recognition.
#include "../src/dll/labels.c" // NOLINT(bugprone-suspicious-include): exercise label walk state.
#include "../src/dll/match_stats.c" // NOLINT(bugprone-suspicious-include): use synthetic engine objects.

static int lockBalance; // seed builder locks initialised less those destroyed
static void countLockInit(PlatformLock *lock);
static void countLockDestroy(PlatformLock *lock);

#define platform_initLock countLockInit
#define platform_destroyLock countLockDestroy
#include "../src/dll/seed_build.c" // NOLINT(bugprone-suspicious-include): exercise record alignment.
#undef platform_initLock
#undef platform_destroyLock

#include "../src/dll/game_state.c"  // NOLINT(bugprone-suspicious-include): read a synthetic game.
#include "../src/dll/job_inspect.c" // NOLINT(bugprone-suspicious-include): decode synthetic matches.
#include "../src/dll/job_status.c"  // NOLINT(bugprone-suspicious-include): report its state.

// macos_test.c serves jobs with a job_execute of its own.
void dllTest_execute(char *text);
#define job_execute dllTest_execute
#include "../src/dll/job_execute.c" // NOLINT(bugprone-suspicious-include): refuse jobs while busy.
#undef job_execute

#include "../src/dll/job_build.c" // NOLINT(bugprone-suspicious-include): export a synthetic game.
#include "../src/dll/output.c"    // NOLINT(bugprone-suspicious-include): write its config.


static ByteBuffer engineOutput;
static SharedBuffer engineShared;
static DataBuffer engineStream;
static void *engineVtable[STREAM_VTABLE_PREFIX + STREAM_VTABLE_SLOTS];
static void **engineOutputVtable;
static uint32_t builtSeed;
static bool usedTrace;
static bool hasFixture;
static unsigned serialisations;
static unsigned tracedSerialisations;
static unsigned doubleWriteAt; // the serialisation (counting from 1) that writes its seed twice
static uint8_t *humanMatchesHolder;
static const uint8_t *expectedBuildTeams[2];
static bool failBuild;
static const uint32_t *fakeFrames; // what game_translateFrames() returns while set
static size_t fakeFrameCount;
static bool seedFound;                     // match_seedOffset() finds a seed at offset 0
static bool fakeMatch;                     // serialisations also write fakeMatchWrites
static uint32_t observerSerialisations;    // bit n: the nth serialisation lists an observer
static bool fakePlayers;                   // serialisations also write two home players, A then B
static uint32_t reorderSerialisations;     // bit n: the nth serialisation writes B before A
static uint32_t thirdPlayerSerialisations; // bit n: the nth serialisation adds player C
static size_t fakePeople;                  // database_countPeople()
static uint8_t *fakeHumanTeam;             // database_findHumanTeam()
static unsigned teamSearches;              // calls of database_findHumanTeam()
static JobCommand dispatched;              // the last job the jobs_* stubs below ran
#ifdef __APPLE__
// The game's objects that game_state.c reads (game_address() returns them).
static uint8_t fakeGameOptions[GAME_OPTIONS_IN_PROGRESS + 1];
static uint8_t fakeGame[GAME_PROCESSING_KIND + sizeof(int32_t)];
static uint8_t fakeSaveSystem[SAVE_SYSTEM_NAME + sizeof(void *)];
static uint8_t fakeSaveName[sizeof(uint32_t) + GAME_STATE_SAVE_LENGTH + 8]; // a game string object
static uint8_t *fakeSingletons[3]; // the GAME and SAVE_SYSTEM pointers, the start screen's
static uint16_t fakeDate[2];
#endif

// A match block: each write's translated stack (its call site, then the chain from the inside
// out, ending in GAME_MATCH__serialize) and byte. The observer count's byte is '0' or '1'.
typedef struct FakeWrite {
	uint32_t frames[4];
	size_t frameCount;
	char byte;
	bool observer; // written only by a serialisation that lists an observer
	bool seeded;   // the byte plus the seed modulo 3
} FakeWrite;

static const FakeWrite fakeMatchWrites[] = {
	{{0x1A7C140, SITE_PART_MATCH}, 2, 'm', false, false},
	{{SITE_OBSERVER_COUNT, SITE_PART_MATCH}, 2, '0', false, false},
	{{0x43748C6, 0x8455B3DC, SITE_OBSERVER_ENTRY, SITE_PART_MATCH}, 4, 'e', true, false},
	{{SITE_TEXT, SITE_OBSERVER_TEAM, SITE_PART_MATCH}, 3, 't', true, false},
	{{0x1A7C222, SITE_PART_MATCH}, 2, 'a', false, true},
};

static void countLockInit(PlatformLock *lock) {
	platform_initLock(lock);
	++lockBalance;
}

static void countLockDestroy(PlatformLock *lock) {
	platform_destroyLock(lock);
	--lockBalance;
}

static void *traceRealloc(void *pointer, size_t size) {
	if (traceAllocationsUntilFailure == 0) {
		return NULL;
	}
	if (traceAllocationsUntilFailure > 0) {
		--traceAllocationsUntilFailure;
	}
	return realloc(pointer, size);
}

static StreamWriteResult engineWrite(void *self, const void *bytes, StreamWriteLength length) {
	(void)self;
	if (length < 0 || !buffer_append(&engineOutput, bytes, (size_t)length)) {
		return (StreamWriteResult)-1;
	}
	return (StreamWriteResult)length;
}

static bool writeFake(
	StreamWriteFn write,
	void *handle,
	const uint32_t *frames,
	size_t frameCount,
	const void *bytes,
	size_t length
) {
	fakeFrames = frames;
	fakeFrameCount = frameCount;
	bool ok = write(handle, bytes, (StreamWriteLength)length) == (StreamWriteResult)length;
	fakeFrameCount = 0;
	return ok;
}

// A home player as MATCH_PLAYER__serialize writes it: its first write, its unique ID, a value and
// `settings` familiarity settings (which give it a layout of its own).
static bool
writeFakePlayer(StreamWriteFn write, void *handle, uint32_t uid, char value, unsigned settings) {
	static const uint32_t first[] = {SITE_PLAYER_FIRST, SITE_TEAM_PLAYER, SITE_PART_HOME};
	static const uint32_t id[] = {SITE_PLAYER_UID, 0x1A7FDEE, SITE_TEAM_PLAYER, SITE_PART_HOME};
	static const uint32_t data[] = {0x1A80913, 0x1B3C914, SITE_TEAM_PLAYER, SITE_PART_HOME};
	static const uint32_t setting[] = {
		SITE_FAMILIARITY_SETTING, 0x1B3C914, SITE_TEAM_PLAYER, SITE_PART_HOME};
	bool ok = writeFake(write, handle, first, 3, "p", 1) &&
						writeFake(write, handle, id, 4, &uid, sizeof uid) &&
						writeFake(write, handle, data, 4, &value, 1);
	for (unsigned i = 0; ok && i < settings; ++i) {
		ok = writeFake(write, handle, setting, 4, "f", 1);
	}
	return ok;
}

typedef struct FakePlayerWrite {
	uint32_t uid;
	char value;
	unsigned settings;
} FakePlayerWrite;

// Player A (value 'a' plus the seed modulo 3, one setting) and player B, in the order the nth
// serialisation's bit in reorderSerialisations picks, then C if its bit asks for one.
static bool writeFakePlayers(StreamWriteFn write, void *handle, unsigned serialisation) {
	bool reorder = serialisation < 32 && (reorderSerialisations >> serialisation & 1u);
	bool third = serialisation < 32 && (thirdPlayerSerialisations >> serialisation & 1u);
	char a = (char)('a' + builtSeed % 3);
	const FakePlayerWrite order[] = {{'A', a, 1}, {'B', 'b', 0}};
	bool ok = true;
	for (size_t k = 0; ok && k < 2; ++k) {
		const FakePlayerWrite *player = &order[reorder ? 1 - k : k];
		ok = writeFakePlayer(write, handle, player->uid, player->value, player->settings);
	}
	return ok && (!third || writeFakePlayer(write, handle, 'C', 'c', 2));
}

// NOLINTBEGIN(readability-non-const-parameter): test doubles retain the game's signatures.
static char engineSerialise(uint8_t *match, void **handle, unsigned flags) {
	(void)match;
	(void)flags;
	void **vtable = *(void ***)*handle;
	usedTrace = vtable != &engineVtable[STREAM_VTABLE_PREFIX];
	++serialisations;
	tracedSerialisations += usedTrace;
	StreamWriteFn write = NULL;
	memcpy(&write, &vtable[STREAM_WRITE_SLOT], sizeof write);
	bool ok = write(*handle, &builtSeed, sizeof builtSeed) == sizeof builtSeed;
	if (ok && serialisations == doubleWriteAt) {
		ok = write(*handle, &builtSeed, sizeof builtSeed) == sizeof builtSeed;
	}
	bool observer = serialisations < 32 && (observerSerialisations >> serialisations & 1u);
	for (size_t i = 0; ok && fakeMatch && i < sizeof fakeMatchWrites / sizeof fakeMatchWrites[0];
		++i) {
		const FakeWrite *fake = &fakeMatchWrites[i];
		if (fake->observer && !observer) {
			continue;
		}
		char byte = fake->byte;
		if (fake->frames[0] == SITE_OBSERVER_COUNT) {
			byte = (char)('0' + observer);
		} else if (fake->seeded) {
			byte = (char)(fake->byte + builtSeed % 3);
		}
		fakeFrames = fake->frames;
		fakeFrameCount = fake->frameCount;
		ok = write(*handle, &byte, 1) == 1;
		fakeFrameCount = 0;
	}
	if (ok && fakePlayers) {
		ok = writeFakePlayers(write, *handle, serialisations);
	}
	return (char)ok;
}

VoidFn game_function(uint32_t rva) {
	ASSERT_INT_EQ((int)rva, RVA_GAME_MATCH_SERIALIZE);
	MatchStreamFn serialise = engineSerialise;
	VoidFn function = NULL;
	memcpy(&function, &serialise, sizeof function);
	return function;
}

VoidFn game_functionAt(const void *address) {
	VoidFn function = NULL;
	memcpy(&function, &address, sizeof function);
	return function;
}

void *game_address(uint32_t rva) {
	switch (rva) {
		case RVA_HUMAN_MATCHES_HOLDER: return &humanMatchesHolder;
#ifdef __APPLE__
		case RVA_MACOS_GAME_OPTIONS: return fakeGameOptions;
		case RVA_MACOS_GAME: return &fakeSingletons[0];
		case RVA_MACOS_SAVE_SYSTEM: return &fakeSingletons[1];
		case RVA_MACOS_START_SCREEN: return &fakeSingletons[2];
		case RVA_CURRENT_DATE: return fakeDate;
#endif
		default: return (void *)(uintptr_t)rva; // NOLINT(performance-no-int-to-ptr): synthetic.
	}
}

uint32_t game_rva(const void *address) {
	uintptr_t value = (uintptr_t)address;
	return value <= UINT32_MAX ? (uint32_t)value : 0;
}

size_t game_translateFrames(void *const *frames, size_t count, uint32_t *out, size_t capacity) {
	if (fakeFrameCount) {
		count = fakeFrameCount;
	}
	size_t written = count < capacity ? count : capacity;
	for (size_t i = 0; i < written; ++i) {
		out[i] = fakeFrameCount ? fakeFrames[i] : game_rva(frames[i]);
	}
	return written;
}

uint32_t game_timestamp(void) {
	return GAME_PE_TIMESTAMP_STEAM;
}

DataBuffer *stream_create(void) {
	StreamWriteFn write = engineWrite;
	memcpy(&engineVtable[STREAM_VTABLE_PREFIX + STREAM_WRITE_SLOT], &write, sizeof write);
	engineOutputVtable = &engineVtable[STREAM_VTABLE_PREFIX];
	engineStream.shared = &engineShared;
	engineStream.output = &engineOutputVtable;
	return &engineStream;
}

void stream_resetOutput(DataBuffer *buffer) {
	(void)buffer;
	buffer_clear(&engineOutput);
}

const uint8_t *stream_outputData(const DataBuffer *buffer) {
	(void)buffer;
	return engineOutput.data;
}

size_t stream_outputSize(const DataBuffer *buffer) {
	(void)buffer;
	return engineOutput.length;
}

uint8_t *match_build(uint8_t *fixture, uint32_t seed) {
	for (unsigned side = 0; side < 2; ++side) {
		if (expectedBuildTeams[side]) {
			uint8_t *team = NULL;
			ASSERT_BOOL_EQ(memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &team), true);
			ASSERT_BOOL_EQ(team == expectedBuildTeams[side], true);
		}
	}
	builtSeed = seed;
	return failBuild ? NULL : (uint8_t *)&builtSeed;
}

void match_freeBuilt(uint8_t *match) {
	(void)match;
}

uint8_t *match_deserialize(ByteBuffer *bytes) {
	ASSERT_BOOL_EQ(bytes->length >= sizeof builtSeed, true);
	if (bytes->length < sizeof builtSeed) {
		return NULL;
	}
	memcpy(&builtSeed, bytes->data, sizeof builtSeed);
	return (uint8_t *)&builtSeed;
}

void match_delete(uint8_t *match) {
	(void)match;
}

bool match_seedOffset(const uint8_t *bytes, size_t size, size_t *offset) {
	(void)bytes;
	(void)size;
	*offset = 0;
	return seedFound;
}

bool fixture_findNext(NextFixture *next, const char **error) {
	if (!hasFixture) {
		*error = "no test fixture";
		return false;
	}
	*next = (NextFixture){.fixture = (uint8_t *)&builtSeed};
	return true;
}

bool fixture_isAttended(const uint8_t *fixture) {
	(void)fixture;
	return true;
}

size_t database_countPeople(void) {
	return fakePeople;
}

uint8_t *database_findHumanTeam(void) {
	++teamSearches;
	return fakeHumanTeam;
}

static void runStub(const Job *job) {
	dispatched = job->command;
	job_writeResult("ok: stub");
}

void jobs_run(const Job *job) {
	runStub(job);
}

void jobs_replaceTeam(const Job *job) {
	runStub(job);
}

void jobs_listTeams(const Job *job) {
	runStub(job);
}

void jobs_listPlayers(const Job *job) {
	runStub(job);
}

void jobs_getPlayer(const Job *job) {
	runStub(job);
}

bool database_loadPeople(PeopleIndex *index) {
	*index = (PeopleIndex){0};
	return true;
}

void database_freePeople(PeopleIndex *index) {
	(void)index;
}

uint8_t *database_findPerson(const PeopleIndex *index, uint32_t uniqueId) {
	(void)index;
	(void)uniqueId;
	return NULL;
}

bool database_personName(const uint8_t *person, char *out, size_t size) {
	(void)person;
	(void)snprintf(out, size, "player");
	return true;
}

void database_clubShortName(const uint8_t *team, char *out, size_t size) {
	(void)team;
	(void)snprintf(out, size, "team");
}
// NOLINTEND(readability-non-const-parameter)

TEST(interns_numeric_paths) {
	Serialised serialised = {0};
	active = &serialised;
	memset(pathBuckets, 0, sizeof pathBuckets);
	uint32_t first[] = {SITE_PART_HOME, SITE_TEAM_PLAYER};
	uint32_t collision[] = {SITE_PART_HOME, SITE_TEAM_PLAYER + PATH_BUCKETS};
	ASSERT_BOOL_EQ(
		(hashChain(first, 2) & (PATH_BUCKETS - 1)) == (hashChain(collision, 2) & (PATH_BUCKETS - 1)),
		true
	);
	ASSERT_INT_EQ((int)internPath(first, 2), 0);
	ASSERT_INT_EQ((int)internPath(collision, 2), 1);
	for (unsigned i = 0; i < 1000; ++i) {
		ASSERT_INT_EQ((int)internPath(first, 2), 0);
	}
	ASSERT_BOOL_EQ(serialised.pathCount == 2, true);
	if (serialised.pathCount == 2) {
		ASSERT_BOOL_EQ(strcmp(serialised.paths[0], "1ac77d5>1b6f456") == 0, true);
	}
	void *frames[] = {
		game_address(SITE_PLAYER_UID),
		game_address(SITE_TEAM_PLAYER),
		game_address(SITE_PART_HOME),
		NULL,
	};
	uint32_t rvas[4];
	ASSERT_INT_EQ((int)game_translateFrames(frames, 4, rvas, 4), 4);
	ASSERT_INT_EQ((int)pathOf(rvas, 4), 0);
	ASSERT_INT_EQ((int)internPath(first, 0), 2);
	trace_free(&serialised);
	memset(pathBuckets, 0, sizeof pathBuckets);
	for (uint32_t i = 0; i < PATH_BUCKETS; ++i) {
		ASSERT_BOOL_EQ(internPath(&i, 1) != UINT32_MAX, true);
	}
	uint32_t extra = PATH_BUCKETS;
	ASSERT_BOOL_EQ(internPath(&extra, 1) == UINT32_MAX, true);
	trace_free(&serialised);
	active = NULL;
}

TEST(rejects_incomplete_traces) {
	uint8_t bytes[3] = {0};
	TraceRecord records[] = {{.length = 1}, {.offset = 1, .length = 2}, {.offset = 3}};
	Serialised serialised = {
		.bytes = {.data = bytes, .length = sizeof bytes},
		.records = records,
		.recordCount = 2,
		.pathCount = 1,
	};
	ASSERT_BOOL_EQ(recordsTileBytes(&serialised), true);
	records[1].length = 3;
	ASSERT_BOOL_EQ(recordsTileBytes(&serialised), false);
	records[1].length = 2;
	records[1].path = 1;
	ASSERT_BOOL_EQ(recordsTileBytes(&serialised), false);
	records[1].path = 0;
	serialised.recordCount = 3;
	ASSERT_BOOL_EQ(recordsTileBytes(&serialised), false);
}

TEST(reports_trace_allocation_failures) {
	for (int failure = 0; failure < 2; ++failure) {
		Serialised serialised = {0};
		traceAllocationsUntilFailure = failure;
		ASSERT_BOOL_EQ(trace_serialize((uint8_t *)&builtSeed, &serialised, true), false);
		ASSERT_BOOL_EQ(active == NULL && engineOutputVtable == &engineVtable[STREAM_VTABLE_PREFIX], true);
		trace_free(&serialised);
	}
	traceAllocationsUntilFailure = -1;
	Serialised serialised = {0};
	ASSERT_BOOL_EQ(trace_serialize((uint8_t *)&builtSeed, &serialised, true), true);
	trace_free(&serialised);
}

TEST(only_traces_setup_round_trips) {
	uint32_t seed = 99;
	ByteBuffer input = {.data = (uint8_t *)&seed, .length = sizeof seed};
	SeedBuilder builder;
	char error[256] = "";
	serialisations = 0;
	tracedSerialisations = 0;
	hasFixture = true;
	bool ok = seedBuilder_open(&builder, &input, seed, NULL, error, sizeof error);
	hasFixture = false;
	ASSERT_BOOL_EQ(ok, true);
	ASSERT_BOOL_EQ(serialisations == 3 && tracedSerialisations == 2, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 0, true);
		seedBuilder_close(&builder);
	}
	// An input that differs from the build of its seed carries that difference as an edit.
	hasFixture = true;
	ok = seedBuilder_open(&builder, &input, seed - 1, NULL, error, sizeof error);
	hasFixture = false;
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 1, true);
		seedBuilder_close(&builder);
	}
}

TEST(rejects_inputs_with_another_layout) {
	uint32_t seed = 99;
	ByteBuffer input = {.data = (uint8_t *)&seed, .length = sizeof seed};
	SeedBuilder builder;
	char error[256] = "";
	serialisations = 0;
	doubleWriteAt = 3; // the input's round trip: one more write than the game's build
	hasFixture = true;
	bool ok = seedBuilder_open(&builder, &input, seed, NULL, error, sizeof error);
	hasFixture = false;
	doubleWriteAt = 0;
	ASSERT_BOOL_EQ(ok, false);
	ASSERT_BOOL_EQ(strstr(error, "or its layout was edited") != NULL, true);
	ASSERT_BOOL_EQ(builder.edited == NULL && builder.input.recordCount == 0, true);
	ASSERT_INT_EQ(lockBalance, 0); // a builder that failed to open has no lock to destroy
}

TEST(opens_and_rebuilds_inputs_whose_observers_differ) {
	uint32_t seed = 99;
	ByteBuffer input = {.data = (uint8_t *)&seed, .length = sizeof seed};
	SeedBuilder builder;
	char error[256] = "";
	fakeMatch = true;
	hasFixture = true;
	// The input's round trip (the third serialisation) lists an observer that the build of its seed
	// (round-tripped in the second) doesn't: that is no edit.
	serialisations = 0;
	observerSerialisations = 1U << 3;
	bool ok = seedBuilder_open(&builder, &input, seed, NULL, error, sizeof error);
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 0, true);
		seedBuilder_close(&builder);
	}
	// The other way round, with edits before and after the observers (the seed, and the write that
	// depends on it). A rebuild (the fourth) that lists an observer keeps it, and the edits.
	serialisations = 0;
	observerSerialisations = 1U << 2 | 1U << 4;
	ok = seedBuilder_open(&builder, &input, seed - 1, NULL, error, sizeof error);
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 2, true);
		ByteBuffer out = {0};
		ASSERT_BOOL_EQ(seedBuilder_build(&builder, 7, &out), true);
		uint8_t expected[sizeof seed + 5];
		memcpy(expected, &seed, sizeof seed);
		memcpy(expected + sizeof seed, "m1eta", 5); // the build's own last write is b
		ASSERT_BOOL_EQ(
			out.length == sizeof expected && memcmp(out.data, expected, sizeof expected) == 0, true
		);
		buffer_free(&out);
		seedBuilder_close(&builder);
	}
	hasFixture = false;
	fakeMatch = false;
	observerSerialisations = 0;
	ASSERT_INT_EQ(lockBalance, 0);
}

TEST(drops_only_replaced_team_edits) {
	TraceRecord records[] = {{.path = 0}, {.path = 1}, {.path = 1}};
	char *paths[] = {"1ac77d5", "1ac77e8"};
	bool edited[3];
	SeedBuilder builder = {
		.input = {.records = records, .recordCount = 3, .paths = paths, .pathCount = 2},
		.edited = edited,
	};
	size_t dropped = 0;
	for (unsigned side = 0; side < 2; ++side) {
		edited[0] = edited[1] = edited[2] = true;
		builder.editCount = 3;
		ASSERT_BOOL_EQ(seedBuilder_dropTeamEdits(&builder, side, &dropped), true);
		ASSERT_BOOL_EQ(dropped == (side ? 2u : 1u), true);
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == (side ? 1u : 2u), true);
		ASSERT_BOOL_EQ(
			edited[0] == (side != 0) && edited[1] == (side == 0) && edited[2] == (side == 0), true
		);
	}
	records[0].path = 2;
	ASSERT_BOOL_EQ(seedBuilder_dropTeamEdits(&builder, 1, &dropped), false);
	ASSERT_BOOL_EQ(dropped == 0 && seedBuilder_editCount(&builder) == 1, true);
}

static void storePointer(uint8_t *base, size_t offset, const void *value) {
	memcpy(base + offset, &value, sizeof value);
}

TEST(swaps_either_or_both_teams_and_restores_game_state) {
	uint8_t fixture[FIXTURE_AWAY + sizeof(void *)] = {0};
	uint8_t copies[8][sizeof fixture];
	uint8_t teams[2][TEAM_IN_GAME_DATA + sizeof(void *)] = {0};
	uint8_t nextMatch[2][NEXT_MATCH_OPPONENT + sizeof(void *)] = {0};
	uint8_t replacements[2] = {0};
	uint8_t holder[HUMAN_MATCHES + 2 * sizeof(void *)] = {0};
	uint8_t *copyPointers[8];
	uint8_t **entries[8];
	for (unsigned side = 0; side < 2; ++side) {
		storePointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, teams[side]);
		storePointer(teams[side], TEAM_IN_GAME_DATA, nextMatch[side]);
		storePointer(nextMatch[side], NEXT_MATCH_OPPONENT, teams[1 - side]);
	}
	for (unsigned i = 0; i < 8; ++i) {
		memcpy(copies[i], fixture, sizeof fixture);
		copyPointers[i] = copies[i];
		entries[i] = &copyPointers[i];
	}
	storePointer(holder, HUMAN_MATCHES, entries);
	storePointer(holder, HUMAN_MATCHES + sizeof(void *), entries + 1);
	humanMatchesHolder = holder;
	const uint8_t *overrides[][2] = {{&replacements[0], NULL},
		{NULL, &replacements[1]},
		{&replacements[0], &replacements[1]},
		{teams[1], &replacements[1]},
		{&replacements[0], teams[0]},
		{teams[1], teams[0]}};
	const uint8_t *opponents[][2] = {{teams[1], &replacements[0]},
		{&replacements[1], teams[0]},
		{&replacements[1], &replacements[0]},
		{&replacements[1], &replacements[1]},
		{&replacements[0], &replacements[0]},
		{teams[1], teams[0]}};
	for (unsigned scenario = 0; scenario < sizeof overrides / sizeof overrides[0]; ++scenario) {
		SeedBuilder builder = {.next = {.fixture = fixture}};
		for (unsigned side = 0; side < 2; ++side) {
			builder.teams[side] = overrides[scenario][side];
			expectedBuildTeams[side] = builder.teams[side] ? builder.teams[side] : teams[side];
		}
		Swap swaps[MAX_SWAPS];
		size_t count = 0;
		ASSERT_BOOL_EQ(swapTeams(&builder, swaps, &count), true);
		for (unsigned side = 0; side < 2; ++side) {
			uint8_t *value = NULL;
			ASSERT_BOOL_EQ(memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true);
			ASSERT_BOOL_EQ(value == expectedBuildTeams[side], true);
			ASSERT_BOOL_EQ(
				memory_readPointer(copies[0], side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true
			);
			ASSERT_BOOL_EQ(value == expectedBuildTeams[side], true);
			ASSERT_BOOL_EQ(memory_readPointer(nextMatch[side], NEXT_MATCH_OPPONENT, &value), true);
			ASSERT_BOOL_EQ(value == opponents[scenario][side], true);
		}
		restoreSwaps(swaps, count);
		for (unsigned failure = 0; failure < 2; ++failure) {
			Serialised serialised = {0};
			failBuild = failure != 0;
			ASSERT_BOOL_EQ(buildSerialised(&builder, 99, &serialised, false), !failBuild);
			trace_free(&serialised);
			for (unsigned side = 0; side < 2; ++side) {
				uint8_t *value = NULL;
				ASSERT_BOOL_EQ(
					memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true
				);
				ASSERT_BOOL_EQ(value == teams[side], true);
				ASSERT_BOOL_EQ(
					memory_readPointer(copies[0], side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true
				);
				ASSERT_BOOL_EQ(value == teams[side], true);
				ASSERT_BOOL_EQ(memory_readPointer(nextMatch[side], NEXT_MATCH_OPPONENT, &value), true);
				ASSERT_BOOL_EQ(value == teams[1 - side], true);
			}
		}
	}
	SeedBuilder builder = {
		.next = {.fixture = fixture}, .teams = {&replacements[0], &replacements[1]}};
	storePointer(holder, HUMAN_MATCHES + sizeof(void *), entries + 8);
	Swap swaps[MAX_SWAPS];
	size_t count = 0;
	ASSERT_BOOL_EQ(swapTeams(&builder, swaps, &count), false);
	for (unsigned i = 0; i < 8; ++i) {
		ASSERT_INT_EQ(memcmp(copies[i], fixture, sizeof fixture), 0);
	}
	for (unsigned side = 0; side < 2; ++side) {
		uint8_t *value = NULL;
		ASSERT_BOOL_EQ(memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true);
		ASSERT_BOOL_EQ(value == teams[side], true);
	}
	// The same team may play on both sides.
	builder.teams[0] = teams[1];
	builder.teams[1] = NULL;
	ASSERT_BOOL_EQ(swapTeams(&builder, swaps, &count), true);
	for (unsigned side = 0; side < 2; ++side) {
		uint8_t *value = NULL;
		ASSERT_BOOL_EQ(memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true);
		ASSERT_BOOL_EQ(value == teams[1], true);
	}
	restoreSwaps(swaps, count);
	for (unsigned side = 0; side < 2; ++side) {
		uint8_t *value = NULL;
		ASSERT_BOOL_EQ(memory_readPointer(fixture, side ? FIXTURE_AWAY : FIXTURE_HOME, &value), true);
		ASSERT_BOOL_EQ(value == teams[side], true);
		ASSERT_BOOL_EQ(memory_readPointer(nextMatch[side], NEXT_MATCH_OPPONENT, &value), true);
		ASSERT_BOOL_EQ(value == teams[1 - side], true);
	}
	for (unsigned i = 0; i < 8; ++i) {
		ASSERT_INT_EQ(memcmp(copies[i], fixture, sizeof fixture), 0);
	}
	expectedBuildTeams[0] = expectedBuildTeams[1] = NULL;
	humanMatchesHolder = NULL;
	failBuild = false;
}

static void appendTestRecord(Serialised *serialised, const char *bytes, const char *path) {
	uint32_t index = trace_findPath(serialised, path);
	TraceRecord record = {
		.offset = (uint32_t)serialised->bytes.length,
		.length = (uint32_t)strlen(bytes),
		.caller = strchr(path, '>') ? 0 : SITE_PART_HOME,
		.path = index,
	};
	ASSERT_BOOL_EQ(index != UINT32_MAX, true);
	ASSERT_BOOL_EQ(buffer_append(&serialised->bytes, bytes, record.length), true);
	ASSERT_BOOL_EQ(trace_appendRecord(serialised, record), true);
}

TEST(copies_either_team_block_over_the_other) {
	Serialised built = {0};
	appendTestRecord(&built, "m", "1ac76fb");
	appendTestRecord(&built, "HH", "1ac77d5>10");
	appendTestRecord(&built, "aaa", "1ac77e8>10");
	appendTestRecord(&built, "x", "1ac77fe");
	ASSERT_BOOL_EQ(mirrorSide(&built, 1), true);
	ASSERT_BOOL_EQ(built.bytes.length == 8 && built.recordCount == 4, true);
	if (built.bytes.length == 8 && built.recordCount == 4) {
		ASSERT_BOOL_EQ(memcmp(built.bytes.data, "maaaaaax", 8) == 0, true);
		ASSERT_BOOL_EQ(recordsTileBytes(&built), true);
		ASSERT_BOOL_EQ(built.records[1].path == 1 && built.records[2].path == 2, true);
	}
	trace_free(&built);
	appendTestRecord(&built, "m", "1ac76fb");
	appendTestRecord(&built, "HH", "1ac77d5>10");
	appendTestRecord(&built, "h", "1ac77d5");
	appendTestRecord(&built, "aaa", "1ac77e8>10");
	appendTestRecord(&built, "x", "1ac77fe");
	ASSERT_BOOL_EQ(mirrorSide(&built, 0), true);
	ASSERT_BOOL_EQ(built.bytes.length == 8 && built.recordCount == 6, true);
	if (built.bytes.length == 8 && built.recordCount == 6) {
		ASSERT_BOOL_EQ(memcmp(built.bytes.data, "mHHhHHhx", 8) == 0, true);
		ASSERT_BOOL_EQ(recordsTileBytes(&built), true);
		ASSERT_INT_EQ((int)built.records[3].path, 3);
		ASSERT_BOOL_EQ(strcmp(built.paths[built.records[4].path], "1ac77e8") == 0, true);
		ASSERT_BOOL_EQ(built.records[4].caller == SITE_PART_AWAY, true);
		ASSERT_BOOL_EQ(built.records[5].path == 4 && built.records[5].offset == 7, true);
		Side side = {.serialised = &built};
		size_t away[2] = {0};
		ASSERT_BOOL_EQ(parseChains(&built, &side.chains), true);
		ASSERT_BOOL_EQ(findPart(&side, SITE_PART_AWAY, away) && away[0] == 3 && away[1] == 5, true);
		freeChains(&side.chains);
	}
	trace_free(&built);
	// Without an away block nothing is copied.
	appendTestRecord(&built, "m", "1ac76fb");
	appendTestRecord(&built, "HH", "1ac77d5>10");
	ASSERT_BOOL_EQ(mirrorSide(&built, 0), false);
	ASSERT_BOOL_EQ(built.bytes.length == 3 && built.recordCount == 2, true);
	trace_free(&built);
}

TEST(builds_a_team_playing_itself_against_the_fixtures_opponent) {
	uint8_t fixture[FIXTURE_AWAY + sizeof(void *)] = {0};
	uint8_t teams[3] = {0}; // the fixture's home and away teams, and another
	storePointer(fixture, FIXTURE_HOME, &teams[0]);
	storePointer(fixture, FIXTURE_AWAY, &teams[1]);
	const uint8_t *overrides[][2] = {
		{&teams[1], NULL}, {NULL, &teams[0]}, {&teams[2], &teams[2]}, {&teams[2], NULL}, {NULL, NULL}};
	const unsigned sides[] = {1, 0, 0, 2, 2};
	const uint8_t *builds[][2] = {
		{NULL, &teams[1]}, {&teams[0], NULL}, {&teams[2], NULL}, {&teams[2], NULL}, {NULL, NULL}};
	for (unsigned i = 0; i < sizeof sides / sizeof sides[0]; ++i) {
		SeedBuilder builder = {
			.next = {.fixture = fixture}, .teams = {overrides[i][0], overrides[i][1]}};
		SeedBuilder build;
		ASSERT_INT_EQ((int)planBuild(&builder, &build), (int)sides[i]);
		ASSERT_BOOL_EQ(build.teams[0] == builds[i][0] && build.teams[1] == builds[i][1], true);
		ASSERT_BOOL_EQ(build.next.fixture == fixture, true);
	}
}

#ifdef __APPLE__
// What setFakeGame() makes the game's objects say.
typedef struct FakeGame {
	bool created; // the GAME singleton exists
	bool inProgress;
	bool processing;
	int32_t kind;
	size_t people;
	bool humanTeam;
	bool startScreen;
	bool autoPlay;
	bool autoPlayRead;
	uint16_t dayWord;
	uint16_t year;
	const char *save; // the save system's name, or NULL for none
} FakeGame;

static const FakeGame readyGame = {
	.created = true,
	.inProgress = true,
	.kind = -1,
	.people = 3,
	.humanTeam = true,
	.startScreen = true,
	.autoPlay = true,
	.autoPlayRead = true,
	.dayWord = 123 | 37 << 9, // 15:00
	.year = 2024,
	.save = "LIV_MAIN_FM24.fm",
};

static void setFakeGame(const FakeGame *fake) {
	memset(fakeGameOptions, 0, sizeof fakeGameOptions);
	memset(fakeGame, 0, sizeof fakeGame);
	memset(fakeSaveSystem, 0, sizeof fakeSaveSystem);
	fakeGameOptions[GAME_OPTIONS_IN_PROGRESS] = fake->inProgress;
	fakeGame[GAME_PROCESSING] = fake->processing;
	memcpy(fakeGame + GAME_PROCESSING_KIND, &fake->kind, sizeof fake->kind);
	fakeGame[GAME_AUTO_PLAY] = fake->autoPlay;
	storePointer(fakeGame, GAME_AUTO_PLAY_FILE, fake->autoPlayRead ? fakeSaveName : NULL);
	if (fake->save) {
		uint32_t length = (uint32_t)strlen(fake->save);
		memcpy(fakeSaveName, &length, sizeof length);
		memcpy(fakeSaveName + sizeof length, fake->save, length);
		storePointer(fakeSaveSystem, SAVE_SYSTEM_NAME, fakeSaveName);
	}
	fakeSingletons[0] = fake->created ? fakeGame : NULL;
	fakeSingletons[1] = fakeSaveSystem;
	fakeSingletons[2] = fake->startScreen ? fakeGame : NULL;
	fakeDate[0] = fake->dayWord;
	fakeDate[1] = fake->year;
	fakePeople = fake->people;
	fakeHumanTeam = fake->humanTeam ? fakeGame : NULL;
}

// A game string object in `out`: a u32 byte length (`length`), then the bytes of `text`.
static uint8_t *gameString(uint8_t *out, uint32_t length, const char *text) {
	memcpy(out, &length, sizeof length);
	memcpy(out + sizeof length, text, strlen(text));
	return out;
}
#endif

// writeHeader's text for a config with a home team override and this save and game date.
static bool headerText(const char *save, const char *gameDate, char *text, size_t size) {
	uint8_t bytes[6] = {0};
	Serialised serialised = {.bytes = {.data = bytes, .length = sizeof bytes}};
	uint32_t teams[2] = {678, 0};
	FILE *file = tmpfile();
	if (!file) {
		return false;
	}
	writeHeader(file, &serialised, "test", save, gameDate, teams, false);
	bool ok = fflush(file) == 0 && fseek(file, 0, SEEK_SET) == 0;
	size_t length = ok ? fread(text, 1, size - 1, file) : 0;
	text[length] = 0;
	return fclose(file) == 0 && length > 0;
}

TEST(writes_both_team_overrides) {
	uint8_t bytes[6] = {0};
	Serialised serialised = {.bytes = {.data = bytes, .length = sizeof bytes}};
	FILE *file = tmpfile();
	ASSERT_BOOL_EQ(file != NULL, true);
	if (!file) {
		return;
	}
	uint32_t teams[2] = {678, 677};
	writeHeader(file, &serialised, "test", NULL, NULL, teams, false);
	ASSERT_INT_EQ(fflush(file), 0);
	ASSERT_INT_EQ(fseek(file, 0, SEEK_SET), 0);
	char text[512] = {0};
	ASSERT_BOOL_EQ(fread(text, 1, sizeof text - 1, file) > 0, true);
	ASSERT_BOOL_EQ(strstr(text, "\"home_team\": 678") != NULL, true);
	ASSERT_BOOL_EQ(strstr(text, "\"away_team\": 677") != NULL, true);
	ASSERT_INT_EQ(fclose(file), 0);
}

TEST(writes_the_save_and_game_date_after_the_source) {
	static const struct {
		const char *save;
		const char *gameDate;
		const char *expected;
	} cases[] = {
		{"LIV \"MAIN\" \\ FM24",
			"2024 day 123 15:00",
			"\"source\": \"test\",\n  \"save\": \"LIV \\\"MAIN\\\" \\\\ FM24\",\n"
			"  \"game_date\": \"2024 day 123 15:00\",\n  \"home_team\": 678,\n"},
		{NULL,
			"2024 day 1 00:00",
			"\"source\": \"test\",\n  \"game_date\": \"2024 day 1 00:00\",\n  \"home_team\": 678,\n"},
		{"", NULL, "\"source\": \"test\",\n  \"home_team\": 678,\n"},
		{"\xC3\x98ystein", "", "\"source\": \"test\",\n  \"save\": \"\xC3\x98ystein\",\n  \"home"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		char text[512];
		ASSERT_BOOL_EQ(headerText(cases[i].save, cases[i].gameDate, text, sizeof text), true);
		ASSERT_BOOL_EQ(strstr(text, cases[i].expected) != NULL, true);
		ASSERT_BOOL_EQ(strstr(text, "\"save\"") != NULL, cases[i].save && cases[i].save[0]);
		ASSERT_BOOL_EQ(
			strstr(text, "\"game_date\"") != NULL, cases[i].gameDate && cases[i].gameDate[0]
		);
	}
}

TEST(names_what_the_game_is_doing_and_formats_its_dates) {
	static const struct {
		GameState state;
		const char *phase;
	} cases[] = {
		{{.processingKind = -1}, "starting"},
		{{.startScreenSeen = true}, "menu"},
		{{.startScreenSeen = true, .people = 5}, "loading"},
		{{.people = 5}, "loading"},
		{{.processing = true}, "busy"},
		{{.inProgress = true, .processing = true, .people = 5, .humanTeam = true}, "busy"},
		{{.inProgress = true, .startScreenSeen = true, .people = 5, .humanTeam = true}, "ready"},
		{{.inProgress = true, .startScreenSeen = true, .people = 5}, "no_team"},
		{{.inProgress = true, .startScreenSeen = true, .humanTeam = true}, "no_team"},
		// The start screen's auto-play load is in progress before its last steps have run.
		{{.inProgress = true, .people = 5, .humanTeam = true}, "loading"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		ASSERT_INT_EQ(strcmp(gameState_phaseName(gameState_phase(&cases[i].state)), cases[i].phase), 0);
	}
	char date[GAME_STATE_DATE_LENGTH];
	gameState_formatDate(186 | 55 << 9, 2023, date, sizeof date);
	ASSERT_INT_EQ(strcmp(date, "2023 day 186 19:30"), 0);
	gameState_formatDate(1, 2024, date, sizeof date);
	ASSERT_INT_EQ(strcmp(date, "2024 day 1 00:00"), 0);
	gameState_formatDate(UINT16_MAX, UINT16_MAX, date, sizeof date);
	ASSERT_INT_EQ(strcmp(date, "65535 day 511 13:30"), 0);
}

#ifdef __APPLE__
TEST(reads_save_names_only_as_short_utf8_text) {
	uint8_t string[sizeof(uint32_t) + GAME_STATE_SAVE_LENGTH + 8] = {0};
	char name[GAME_STATE_SAVE_LENGTH];
	static const struct {
		const char *text;
		const char *name;
	} valid[] = {
		{"LIV_MAIN_FM24.fm", "LIV_MAIN_FM24"},
		{"LIV_MAIN_FM24 (v02)", "LIV_MAIN_FM24 (v02)"},
		{"\xC3\x98ystein.fm.fm", "\xC3\x98ystein.fm"},
		{"save.FM", "save.FM"},
		{".fm", ".fm"},
	};
	for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
		uint32_t length = (uint32_t)strlen(valid[i].text);
		ASSERT_BOOL_EQ(
			readSaveName(gameString(string, length, valid[i].text), name, sizeof name), true
		);
		ASSERT_INT_EQ(strcmp(name, valid[i].name), 0);
	}
	char longest[GAME_STATE_SAVE_LENGTH + 1];
	memset(longest, 'a', GAME_STATE_SAVE_LENGTH);
	longest[GAME_STATE_SAVE_LENGTH] = 0;
	ASSERT_BOOL_EQ(
		readSaveName(gameString(string, GAME_STATE_SAVE_LENGTH - 1, longest), name, sizeof name), true
	);
	ASSERT_BOOL_EQ(strlen(name) == GAME_STATE_SAVE_LENGTH - 1, true);
	// Too long or empty, not UTF-8 text, or not there at all.
	const struct {
		uint32_t length;
		const char *text;
	} invalid[] = {
		{GAME_STATE_SAVE_LENGTH, longest},
		{UINT32_MAX, "save"},
		{0, "save"},
		{2, "\xC3("},
		{4, "\xC0\xAF.fm"},
		{8, "save\n.fm"},
		{4, "\x7Fsav"},
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		memset(name, 'x', sizeof name);
		ASSERT_BOOL_EQ(
			readSaveName(gameString(string, invalid[i].length, invalid[i].text), name, sizeof name), false
		);
		ASSERT_INT_EQ(name[0], 0);
	}
	ASSERT_BOOL_EQ(readSaveName(NULL, name, sizeof name), false);
	// NOLINTNEXTLINE(performance-no-int-to-ptr): an address nothing is mapped at.
	ASSERT_BOOL_EQ(readSaveName((const uint8_t *)(uintptr_t)16, name, sizeof name), false);
}

TEST(reads_the_game_state_from_its_objects) {
	GameState state;
	teamSearches = 0;
	setFakeGame(&readyGame);
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_BOOL_EQ(state.inProgress && !state.processing && state.processingKind == -1, true);
	ASSERT_BOOL_EQ(state.people == 3 && state.humanTeam && state.startScreenSeen, true);
	ASSERT_BOOL_EQ(state.autoPlay && state.autoPlayRead, true);
	ASSERT_INT_EQ(strcmp(state.date, "2024 day 123 15:00"), 0);
	ASSERT_INT_EQ(strcmp(state.save, "LIV_MAIN_FM24"), 0);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_READY);
	ASSERT_INT_EQ((int)teamSearches, 1);
	// The human manager is looked for only when asked.
	ASSERT_BOOL_EQ(gameState_read(&state, false), true);
	ASSERT_BOOL_EQ(state.humanTeam, false);
	ASSERT_INT_EQ((int)teamSearches, 1);
	// Before the game creates its objects, and at the start screen: no date, and no search.
	setFakeGame(&(FakeGame){.kind = 5});
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_STARTING);
	ASSERT_BOOL_EQ(state.processingKind == -1 && !state.autoPlay && !state.autoPlayRead, true);
	ASSERT_BOOL_EQ(state.date[0] == 0 && state.save[0] == 0, true);
	setFakeGame(&(FakeGame){.created = true, .kind = -1, .startScreen = true, .save = "old.fm"});
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_MENU);
	ASSERT_INT_EQ(strcmp(state.save, "old"), 0);
	// Loading (the database is there, the game not yet in progress), then processing.
	FakeGame game = readyGame;
	game.inProgress = false;
	setFakeGame(&game);
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_LOADING);
	ASSERT_BOOL_EQ(state.date[0] == 0, true);
	game = readyGame;
	game.processing = true;
	game.kind = 7;
	setFakeGame(&game);
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_BUSY);
	ASSERT_INT_EQ(state.processingKind, 7);
	ASSERT_INT_EQ((int)teamSearches, 1);
	// In progress before the start screen's first entry has ended, as during its auto-play load:
	// still loading, and the people are not searched.
	game = readyGame;
	game.startScreen = false;
	setFakeGame(&game);
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_LOADING);
	ASSERT_BOOL_EQ(state.inProgress && !state.startScreenSeen && !state.humanTeam, true);
	ASSERT_INT_EQ((int)teamSearches, 1);
	// In progress without the human's team; a save name that is not text and a date with no year
	// are left out.
	game = readyGame;
	game.humanTeam = false;
	game.save = "LIV\tMAIN";
	game.year = 0;
	setFakeGame(&game);
	ASSERT_BOOL_EQ(gameState_read(&state, true), true);
	ASSERT_INT_EQ((int)gameState_phase(&state), GAME_PHASE_NO_TEAM);
	ASSERT_BOOL_EQ(state.date[0] == 0 && state.save[0] == 0, true);
	setFakeGame(&(FakeGame){0});
}
#endif

// `name` in the temporary directory, made unique to this process.
static bool temporaryPath(const wchar_t *name, wchar_t *out, size_t size) {
#ifdef _WIN32
	wchar_t directory[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, directory);
	if (!length || length >= MAX_PATH) {
		return false;
	}
#else
	const wchar_t *directory = L"/tmp/";
#endif
	int written = swprintf(out, size, L"%lsfm24-%lu-%ls", directory, platform_processId(), name);
	return written > 0 && (size_t)written < size;
}

TEST(decodes_binary_fixtures_as_exports) {
	wchar_t input[HARNESS_PATH_LENGTH];
	wchar_t output[HARNESS_PATH_LENGTH];
	bool ok = temporaryPath(L"decode.bin", input, HARNESS_PATH_LENGTH) &&
						temporaryPath(L"decode.json", output, HARNESS_PATH_LENGTH) &&
						files_write(input, "seed", 4);
	ASSERT_BOOL_EQ(ok, true);
	if (!ok) {
		return;
	}
	fakeMatch = true;
	seedFound = true;
#ifdef __APPLE__
	setFakeGame(&readyGame); // a binary fixture's origin is unknown whatever the game has loaded
#endif
	for (int full = 0; full < 2; ++full) {
		Job job = {.command = JOB_INSPECT, .full = full != 0};
		wcscpy(job.input, input);
		wcscpy(job.output, output);
		jobs_inspect(&job);
		ASSERT_INT_EQ(strcmp(job_takeResult(), "ok: decoded fixture without playing it"), 0);
		ByteBuffer json = {0};
		ASSERT_BOOL_EQ(files_read(output, &json), true);
		const char *text = buffer_string(&json);
		ASSERT_BOOL_EQ(strstr(text, "\"format\": \"fm24-match-config/1\"") != NULL, true);
		ASSERT_BOOL_EQ(strstr(text, "\"bytes\": ") != NULL, full != 0);
		ASSERT_BOOL_EQ(strstr(text, "\"paths\": [") != NULL, full != 0);
		ASSERT_BOOL_EQ(strstr(text, "\"site\": ") != NULL, full != 0);
		ASSERT_BOOL_EQ(strstr(text, "\"role\": \"seed\"") != NULL, true);
		ASSERT_BOOL_EQ(strstr(text, "\"save\"") == NULL && strstr(text, "\"game_date\"") == NULL, true);
		buffer_free(&json);
	}
#ifdef __APPLE__
	setFakeGame(&(FakeGame){0});
#endif
	seedFound = false;
	Job job = {.command = JOB_INSPECT};
	wcscpy(job.input, input);
	wcscpy(job.output, output);
	jobs_inspect(&job);
	ASSERT_INT_EQ(strncmp(job_takeResult(), "error: ", 7), 0);
	fakeMatch = false;
	(void)platform_removeFile(input);
	(void)platform_removeFile(output);
}

TEST(refuses_jobs_but_status_while_the_game_is_busy) {
	wchar_t output[HARNESS_PATH_LENGTH];
	char path[HARNESS_PATH_LENGTH * 3];
	bool ok = temporaryPath(L"busy.json", output, HARNESS_PATH_LENGTH) &&
						platform_toUtf8(output, path, sizeof path);
	ASSERT_BOOL_EQ(ok, true);
	if (!ok) {
		return;
	}
	char build[sizeof path + 64];
	(void)snprintf(build, sizeof build, "command=build_next\noutput=%s\nseed=42\n", path);
	static const char run[] = "command=run\ninput=/in.bin\noutput=/out.csv\nseeds=1\n";
	char text[sizeof build];
#ifdef __APPLE__
	FakeGame loading = readyGame;
	loading.inProgress = false;
	FakeGame processing = readyGame;
	processing.processing = true;
	processing.kind = 7;
	FakeGame autoPlayLoad = readyGame; // in progress before the start screen's first entry has ended
	autoPlayLoad.startScreen = false;
	static const char *const reasons[] = {"loading", "processing kind 7", "loading"};
	const FakeGame *busy[] = {&loading, &processing, &autoPlayLoad};
	for (size_t i = 0; i < sizeof busy / sizeof busy[0]; ++i) {
		const char *jobs[] = {build, run};
		for (size_t k = 0; k < 2; ++k) {
			setFakeGame(busy[i]);
			dispatched = JOB_NONE;
			(void)snprintf(text, sizeof text, "%s", jobs[k]);
			dllTest_execute(text);
			char expected[128];
			(void)snprintf(
				expected, sizeof expected, "error: FM24 is busy (%s); wait until it is idle", reasons[i]
			);
			ASSERT_INT_EQ(strcmp(job_takeResult(), expected), 0);
			ASSERT_INT_EQ((int)dispatched, JOB_NONE);
		}
	}
	(void)snprintf(text, sizeof text, "command=status\n");
	dllTest_execute(text);
	ASSERT_INT_EQ(
		strcmp(
			job_takeResult(),
			"ok: loading in_progress=1 processing=0 kind=-1 people=3 human_team=0 menu_seen=0 autoplay=1 "
			"autoplay_read=1 date=2024 day 123 15:00 save=LIV_MAIN_FM24"
		),
		0
	);
	setFakeGame(&processing);
	(void)snprintf(text, sizeof text, "command=status\n");
	dllTest_execute(text);
	ASSERT_INT_EQ(
		strcmp(
			job_takeResult(),
			"ok: busy in_progress=1 processing=1 kind=7 people=3 human_team=0 menu_seen=1 autoplay=1 "
			"autoplay_read=1 date=2024 day 123 15:00 save=LIV_MAIN_FM24"
		),
		0
	);
	FakeGame idle = readyGame;
	idle.save = "LIV MAIN (v02).fm";
	setFakeGame(&idle);
	(void)snprintf(text, sizeof text, "command=status\n");
	dllTest_execute(text);
	ASSERT_INT_EQ(
		strcmp(
			job_takeResult(),
			"ok: ready in_progress=1 processing=0 kind=-1 people=3 human_team=1 menu_seen=1 autoplay=1 "
			"autoplay_read=1 date=2024 day 123 15:00 save=LIV MAIN (v02)"
		),
		0
	);
	setFakeGame(&readyGame);
#else
	(void)snprintf(text, sizeof text, "command=status\n");
	dllTest_execute(text);
	ASSERT_INT_EQ(strcmp(job_takeResult(), "error: status is not available on Windows yet"), 0);
#endif
	// An idle game (or Windows, where the harness cannot tell) runs the jobs. The export records the
	// save and the in-game date where they are known.
	dispatched = JOB_NONE;
	(void)snprintf(text, sizeof text, "%s", run);
	dllTest_execute(text);
	ASSERT_INT_EQ(strcmp(job_takeResult(), "ok: stub"), 0);
	ASSERT_INT_EQ((int)dispatched, JOB_RUN);
	hasFixture = true;
	fakeMatch = true;
	seedFound = true;
	(void)snprintf(text, sizeof text, "%s", build);
	dllTest_execute(text);
	ASSERT_INT_EQ(strncmp(job_takeResult(), "ok: ", 4), 0);
	ByteBuffer json = {0};
	ASSERT_BOOL_EQ(files_read(output, &json), true);
#ifdef __APPLE__
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&json),
			"\",\n  \"save\": \"LIV_MAIN_FM24\",\n  \"game_date\": \"2024 day 123 15:00\",\n"
		) != NULL,
		true
	);
	setFakeGame(&(FakeGame){0});
#else
	ASSERT_BOOL_EQ(strstr(buffer_string(&json), "\"save\"") == NULL, true);
#endif
	buffer_free(&json);
	hasFixture = false;
	fakeMatch = false;
	seedFound = false;
	(void)platform_removeFile(output);
}

TEST(skips_tracing_unedited_builds) {
	SeedBuilder builder = {0};
	platform_initLock(&builder.lock);
	ByteBuffer out = {0};
	usedTrace = true;
	ASSERT_BOOL_EQ(seedBuilder_build(&builder, 12345, &out), true);
	ASSERT_BOOL_EQ(usedTrace, false);
	ASSERT_BOOL_EQ(out.length == sizeof(uint32_t), true);
	if (out.length == sizeof(uint32_t)) {
		ASSERT_BOOL_EQ(memory_loadU32(out.data) == 12345, true);
	}
	ASSERT_BOOL_EQ(seedBuilder_build(&builder, UINT32_MAX, &out), true);
	ASSERT_BOOL_EQ(usedTrace, false);
	if (out.length == sizeof(uint32_t)) {
		ASSERT_BOOL_EQ(memory_loadU32(out.data) == UINT32_MAX, true);
	}
	platform_destroyLock(&builder.lock);
	buffer_free(&out);
}

TEST(merges_resized_edits_with_the_same_layout) {
	uint8_t inputBytes[] = {'i', 'E', 'D', 'I', 'T', 'z'};
	uint8_t builtBytes[] = {'a', 'b', 'c', 'd'};
	TraceRecord inputRecords[] = {
		{.site = 1, .length = 1},
		{.site = 2, .offset = 1, .length = 4},
		{.site = 3, .offset = 5, .length = 1},
	};
	TraceRecord builtRecords[] = {
		{.site = 1, .length = 1},
		{.site = 2, .offset = 1, .length = 2},
		{.site = 3, .offset = 3, .length = 1},
	};
	bool edited[] = {false, true, false};
	SeedBuilder builder = {
		.input = {.bytes = {.data = inputBytes, .length = sizeof inputBytes},
			.records = inputRecords,
			.recordCount = 3},
		.edited = edited,
		.editCount = 1,
	};
	Serialised built = {.bytes = {.data = builtBytes, .length = sizeof builtBytes},
		.records = builtRecords,
		.recordCount = 3};
	ByteBuffer out = {0};
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	ASSERT_BOOL_EQ(out.length == 6, true);
	if (out.length == 6) {
		ASSERT_BOOL_EQ(memcmp(out.data, "aEDITd", 6) == 0, true);
	}
	buffer_free(&out);
}

TEST(rejects_unplaceable_edits) {
	uint8_t inputBytes[] = {10, 20, 30};
	uint8_t builtBytes[] = {11, 40, 50, 31};
	TraceRecord inputRecords[] = {
		{.site = 1, .length = 1},
		{.site = 2, .offset = 1, .length = 1},
		{.site = 3, .offset = 2, .length = 1},
	};
	TraceRecord builtRecords[] = {
		{.site = 1, .length = 1},
		{.site = 4, .offset = 1, .length = 1},
		{.site = 5, .offset = 2, .length = 1},
		{.site = 3, .offset = 3, .length = 1},
	};
	char *paths[] = {""};
	bool edited[] = {false, true, false};
	SeedBuilder builder = {
		.input = {.bytes = {.data = inputBytes, .length = sizeof inputBytes},
			.records = inputRecords,
			.recordCount = 3,
			.paths = paths,
			.pathCount = 1},
		.edited = edited,
		.editCount = 1,
	};
	Serialised built = {.bytes = {.data = builtBytes, .length = sizeof builtBytes},
		.records = builtRecords,
		.recordCount = 4,
		.paths = paths,
		.pathCount = 1};
	ByteBuffer out = {0};
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), false);
	edited[1] = false;
	edited[2] = true;
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	const uint8_t expected[] = {11, 40, 50, 30};
	ASSERT_BOOL_EQ(out.length == sizeof expected, true);
	if (out.length == sizeof expected) {
		ASSERT_BOOL_EQ(memcmp(out.data, expected, sizeof expected) == 0, true);
	}
	buffer_free(&out);
}

static void
appendSiteRecord(Serialised *serialised, const char *bytes, const char *path, uint32_t site) {
	appendTestRecord(serialised, bytes, path);
	if (serialised->recordCount) {
		serialised->records[serialised->recordCount - 1].site = site;
	}
}

// A match block with `observers` observers (each entry's bytes `entry`) between two other writes,
// then a fixture field holding `fixture`.
static void appendObserverMatch(
	Serialised *serialised,
	unsigned observers,
	const char *entry,
	const char *before,
	const char *after,
	const char *fixture
) {
	appendSiteRecord(serialised, before, "1ac76fb", 0x1A7C140);
	const char count[] = {(char)('0' + observers), '\0'};
	appendSiteRecord(serialised, count, "1ac76fb", SITE_OBSERVER_COUNT);
	for (unsigned i = 0; i < observers; ++i) {
		appendSiteRecord(serialised, entry, "1ac76fb>1a7c1b8>8455b3dc", 0x43748C6);
		appendSiteRecord(serialised, "p", "1ac76fb>1a7c1b8", 0x8455B48C);
		appendSiteRecord(serialised, "Brazil", "1ac76fb>1a7c1f1", SITE_TEXT);
	}
	appendSiteRecord(serialised, after, "1ac76fb", 0x1A7C222);
	appendSiteRecord(serialised, fixture, "1ac77b3", 0x19BED03);
}

TEST(finds_observers_by_call_chain) {
	Serialised serialised = {0};
	appendObserverMatch(&serialised, 1, "e", "m", "n", "f");
	appendSiteRecord(&serialised, "t", "1ac77d5>1a7c1b8", 0x43748C6);
	appendSiteRecord(&serialised, "w", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	Side side = {.serialised = &serialised};
	ASSERT_BOOL_EQ(parseChains(&serialised, &side.chains), true);
	const bool expected[] = {false, true, true, true, true, false, false, false, false};
	ASSERT_BOOL_EQ(serialised.recordCount == sizeof expected / sizeof expected[0], true);
	for (size_t i = 0; i < serialised.recordCount && i < sizeof expected / sizeof expected[0]; ++i) {
		ASSERT_BOOL_EQ(isObserver(&side, i), expected[i]);
	}
	ASSERT_BOOL_EQ(countObservers(&side) == 4 && skipObservers(&side, 1, 9) == 5, true);
	freeChains(&side.chains);
	trace_free(&serialised);
}

TEST(compares_inputs_apart_from_their_observers) {
	const struct {
		unsigned observers[2];
		const char *entries[2];
		const char *fixture;
		bool edited;
	} cases[] = {
		{{0, 1}, {"e", "e"}, "f", false},
		{{1, 0}, {"e", "e"}, "f", false},
		{{1, 2}, {"e", "e"}, "F", true},
		{{1, 1}, {"e", "E"}, "f", false},
		{{0, 0}, {"e", "e"}, "F", true},
	};
	for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
		Serialised reference = {0};
		Serialised input = {0};
		appendObserverMatch(&reference, cases[c].observers[0], cases[c].entries[0], "m", "n", "f");
		appendObserverMatch(
			&input, cases[c].observers[1], cases[c].entries[1], "m", "n", cases[c].fixture
		);
		Side sides[2] = {{.serialised = &reference}, {.serialised = &input}};
		ASSERT_BOOL_EQ(
			parseChains(&reference, &sides[0].chains) && parseChains(&input, &sides[1].chains), true
		);
		bool edited[16] = {0};
		SeedPlayers players[2] = {0};
		char why[64] = "";
		ASSERT_BOOL_EQ(input.recordCount <= 16, true);
		ASSERT_BOOL_EQ(findEdits(&sides[0], &sides[1], edited, players, why, sizeof why), true);
		size_t edits = 0;
		for (size_t i = 0; i < input.recordCount && i < 16; ++i) {
			edits += edited[i];
		}
		ASSERT_BOOL_EQ(edits == (cases[c].edited ? 1u : 0u), true);
		ASSERT_BOOL_EQ(edited[input.recordCount - 1] == cases[c].edited, true);
		freeChains(&sides[0].chains);
		freeChains(&sides[1].chains);
		trace_free(&reference);
		trace_free(&input);
	}
	// Any other difference in the writes is not this fixture's.
	for (unsigned change = 0; change < 3; ++change) {
		Serialised reference = {0};
		Serialised input = {0};
		appendObserverMatch(&reference, 0, "e", "m", "n", "f");
		appendObserverMatch(&input, 1, "e", "m", "n", "f");
		if (change == 0) {
			appendSiteRecord(&input, "h", "1ac77d5>1b6f456", SITE_PLAYER_FIRST);
		} else if (change == 1) {
			appendSiteRecord(&input, "m", "1ac76fb", 0x1A7C238);
		} else {
			input.records[input.recordCount - 1].site = 0x19BED04;
		}
		Side sides[2] = {{.serialised = &reference}, {.serialised = &input}};
		ASSERT_BOOL_EQ(
			parseChains(&reference, &sides[0].chains) && parseChains(&input, &sides[1].chains), true
		);
		bool edited[16] = {0};
		SeedPlayers players[2] = {0};
		char why[64] = "";
		ASSERT_BOOL_EQ(findEdits(&sides[0], &sides[1], edited, players, why, sizeof why), false);
		ASSERT_BOOL_EQ(players[0].list == NULL && players[1].list == NULL, true);
		freeChains(&sides[0].chains);
		freeChains(&sides[1].chains);
		trace_free(&reference);
		trace_free(&input);
	}
}

TEST(merges_edits_around_other_observers) {
	// The input lists an observer and one weather change; the build no observer and two weather
	// changes. The edits before, between and after them are kept, and the build's observers.
	Serialised input = {0};
	appendSiteRecord(&input, "M", "1ac76fb", 0x1A7C140);
	appendSiteRecord(&input, "1", "1ac76fb", SITE_OBSERVER_COUNT);
	appendSiteRecord(&input, "o", "1ac76fb>1a7c1b8>8455b3dc", 0x43748C6);
	appendSiteRecord(&input, "Brazil", "1ac76fb>1a7c1f1", SITE_TEXT);
	appendSiteRecord(&input, "E", "1ac76fb", 0x1A7C222);
	appendSiteRecord(&input, "w", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&input, "n", "1ac76fb", 0x1A7C238);
	appendSiteRecord(&input, "F", "1ac77b3", 0x19BED03);
	Serialised built = {0};
	appendSiteRecord(&built, "m", "1ac76fb", 0x1A7C140);
	appendSiteRecord(&built, "0", "1ac76fb", SITE_OBSERVER_COUNT);
	appendSiteRecord(&built, "e", "1ac76fb", 0x1A7C222);
	appendSiteRecord(&built, "x", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&built, "y", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&built, "b", "1ac76fb", 0x1A7C238);
	appendSiteRecord(&built, "f", "1ac77b3", 0x19BED03);
	bool edited[] = {true, false, false, false, true, false, false, true};
	SeedBuilder builder = {.input = input, .edited = edited, .editCount = 3};
	ByteBuffer out = {0};
	ASSERT_BOOL_EQ(input.recordCount == sizeof edited / sizeof edited[0], true);
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	const char expected[] = "M0ExybF";
	ASSERT_BOOL_EQ(out.length == sizeof expected - 1, true);
	if (out.length == sizeof expected - 1) {
		ASSERT_BOOL_EQ(memcmp(out.data, expected, sizeof expected - 1) == 0, true);
	}
	// Where the layouts match, records pair one to one and the build's observers are kept too.
	trace_free(&built);
	appendSiteRecord(&built, "m", "1ac76fb", 0x1A7C140);
	appendSiteRecord(&built, "2", "1ac76fb", SITE_OBSERVER_COUNT);
	appendSiteRecord(&built, "q", "1ac76fb>1a7c1b8>8455b3dc", 0x43748C6);
	appendSiteRecord(&built, "Spain", "1ac76fb>1a7c1f1", SITE_TEXT);
	appendSiteRecord(&built, "e", "1ac76fb", 0x1A7C222);
	appendSiteRecord(&built, "x", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&built, "b", "1ac76fb", 0x1A7C238);
	appendSiteRecord(&built, "f", "1ac77b3", 0x19BED03);
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	const char same[] = "M2qSpainExbF";
	ASSERT_BOOL_EQ(out.length == sizeof same - 1, true);
	if (out.length == sizeof same - 1) {
		ASSERT_BOOL_EQ(memcmp(out.data, same, sizeof same - 1) == 0, true);
	}
	// The other way round: the build lists an observer and the input none.
	Serialised plain = {0};
	appendSiteRecord(&plain, "M", "1ac76fb", 0x1A7C140);
	appendSiteRecord(&plain, "0", "1ac76fb", SITE_OBSERVER_COUNT);
	appendSiteRecord(&plain, "E", "1ac76fb", 0x1A7C222);
	appendSiteRecord(&plain, "w", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&plain, "n", "1ac76fb", 0x1A7C238);
	appendSiteRecord(&plain, "F", "1ac77b3", 0x19BED03);
	bool plainEdited[] = {true, false, true, false, false, true};
	builder = (SeedBuilder){.input = plain, .edited = plainEdited, .editCount = 3};
	trace_free(&built);
	appendSiteRecord(&built, "m", "1ac76fb", 0x1A7C140);
	appendSiteRecord(&built, "1", "1ac76fb", SITE_OBSERVER_COUNT);
	appendSiteRecord(&built, "q", "1ac76fb>1a7c1b8>8455b3dc", 0x43748C6);
	appendSiteRecord(&built, "Spain", "1ac76fb>1a7c1f1", SITE_TEXT);
	appendSiteRecord(&built, "e", "1ac76fb", 0x1A7C222);
	appendSiteRecord(&built, "x", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&built, "y", "1ac76fb>1a7bf46", SITE_WEATHER_CHANGE_TIME);
	appendSiteRecord(&built, "b", "1ac76fb", 0x1A7C238);
	appendSiteRecord(&built, "f", "1ac77b3", 0x19BED03);
	ASSERT_BOOL_EQ(plain.recordCount == sizeof plainEdited / sizeof plainEdited[0], true);
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	const char observed[] = "M1qSpainExybF";
	ASSERT_BOOL_EQ(out.length == sizeof observed - 1, true);
	if (out.length == sizeof observed - 1) {
		ASSERT_BOOL_EQ(memcmp(out.data, observed, sizeof observed - 1) == 0, true);
	}
	buffer_free(&out);
	trace_free(&built);
	trace_free(&plain);
	trace_free(&input);
}

// A home player of a test team: its unique ID, a value, and how many familiarity settings it
// writes (which gives it a layout of its own), each holding `setting`.
typedef struct FakePlayer {
	uint32_t uid;
	char value;
	unsigned settings;
	char setting;
} FakePlayer;

static void appendBytes(
	Serialised *serialised,
	const void *bytes,
	uint32_t length,
	const char *path,
	uint32_t site
) {
	uint32_t index = trace_findPath(serialised, path);
	TraceRecord record = {
		.offset = (uint32_t)serialised->bytes.length, .length = length, .site = site, .path = index};
	ASSERT_BOOL_EQ(index != UINT32_MAX, true);
	ASSERT_BOOL_EQ(buffer_append(&serialised->bytes, bytes, length), true);
	ASSERT_BOOL_EQ(trace_appendRecord(serialised, record), true);
}

// A player of the team whose block `part` (1ac77d5: home, 1ac77e8: away) writes.
static void appendPlayer(Serialised *serialised, const char *part, const FakePlayer *player) {
	char path[64];
	(void)snprintf(path, sizeof path, "%s>1b6f456", part);
	appendSiteRecord(serialised, "p", path, SITE_PLAYER_FIRST);
	(void)snprintf(path, sizeof path, "%s>1b6f456>1a7fdee", part);
	appendBytes(serialised, &player->uid, sizeof player->uid, path, SITE_PLAYER_UID);
	(void)snprintf(path, sizeof path, "%s>1b6f456>1b3c914", part);
	appendBytes(serialised, &player->value, 1, path, 0x1A80913);
	(void)snprintf(path, sizeof path, "%s>1b6f456>1b3c914>1a80e9b", part);
	for (unsigned i = 0; i < player->settings; ++i) {
		appendBytes(serialised, &player->setting, 1, path, SITE_FAMILIARITY_SETTING);
	}
}

// A team's player count, its players and its next write.
static void
appendTeam(Serialised *serialised, const char *part, const FakePlayer *players, size_t count) {
	appendSiteRecord(serialised, "n", part, SITE_TEAM_PLAYER_COUNT);
	for (size_t k = 0; k < count; ++k) {
		appendPlayer(serialised, part, &players[k]);
	}
	appendSiteRecord(serialised, "x", part, SITE_TEAM_HAS_EXTRA);
}

// Compares the builder's input with `reference`, the game's build of its seed, as
// seedBuilder_open() does: `builder` gets the input's edits and players.
static bool openOn(SeedBuilder *builder, Serialised *reference, char *why, size_t whySize) {
	Side sides[2] = {{.serialised = reference}, {.serialised = &builder->input}};
	builder->edited = calloc(builder->input.recordCount, sizeof *builder->edited);
	bool ok = builder->edited && parseChains(reference, &sides[0].chains) &&
						parseChains(&builder->input, &sides[1].chains) &&
						findEdits(&sides[0], &sides[1], builder->edited, builder->players, why, whySize);
	for (size_t i = 0; ok && i < builder->input.recordCount; ++i) {
		builder->editCount += builder->edited[i];
	}
	freeChains(&sides[0].chains);
	freeChains(&sides[1].chains);
	if (builder->edited) {
		countLockInit(&builder->lock); // seedBuilder_close() destroys it
	}
	return ok;
}

// openOn() with a home team of `input` players and a game's build of `game` players.
static bool openTeams(
	SeedBuilder *builder,
	const FakePlayer *game,
	size_t gameCount,
	const FakePlayer *input,
	size_t inputCount,
	char *why,
	size_t whySize
) {
	Serialised reference = {0};
	*builder = (SeedBuilder){0};
	appendTeam(&reference, "1ac77d5", game, gameCount);
	appendTeam(&builder->input, "1ac77d5", input, inputCount);
	bool ok = openOn(builder, &reference, why, whySize);
	trace_free(&reference);
	return ok;
}

// Whether a team's players (0: home, 1: away) pair with the people named by `persons`' letters.
static bool pairedWith(const SeedBuilder *builder, unsigned team, const char *persons) {
	const SeedPlayers *players = &builder->players[team];
	bool ok = players->count == strlen(persons);
	for (size_t k = 0; ok && k < players->count; ++k) {
		ok = players->list[k].person == (uint32_t)persons[k];
	}
	return ok;
}

// Whether the builder merges its edits into `build` as `want`. Frees both.
static bool mergesAs(const SeedBuilder *builder, Serialised *build, Serialised *want) {
	ByteBuffer out = {0};
	bool ok = mergeEdits(builder, build, 7, &out) && out.length == want->bytes.length &&
						memcmp(out.data, want->bytes.data, out.length) == 0;
	buffer_free(&out);
	trace_free(build);
	trace_free(want);
	return ok;
}

// mergesAs() with home teams of `built` and `expected` players.
static bool mergesInto(
	const SeedBuilder *builder,
	const FakePlayer *built,
	size_t count,
	const FakePlayer *expected,
	size_t expectedCount
) {
	Serialised build = {0};
	Serialised want = {0};
	appendTeam(&build, "1ac77d5", built, count);
	appendTeam(&want, "1ac77d5", expected, expectedCount);
	return mergesAs(builder, &build, &want);
}

TEST(splits_players_by_their_first_write) {
	Serialised serialised = {0};
	appendSiteRecord(&serialised, "n", "1ac77d5", SITE_TEAM_PLAYER_COUNT);
	appendPlayer(&serialised, "1ac77d5", &(FakePlayer){'A', 'a', 0, 'f'});
	appendPlayer(&serialised, "1ac77d5", &(FakePlayer){'B', 'b', 1, 'f'});
	// Neither a first write deeper in the player's calls nor another unique ID makes another player.
	appendSiteRecord(&serialised, "p", "1ac77d5>1b6f456>1b3c914", SITE_PLAYER_FIRST);
	appendBytes(&serialised, "ZZZZ", 4, "1ac77d5>1b6f456>1a7fdee", SITE_PLAYER_UID);
	appendPlayer(&serialised, "1ac77d5", &(FakePlayer){'A', 'c', 2, 'f'});
	appendSiteRecord(&serialised, "x", "1ac77d5", SITE_TEAM_HAS_EXTRA);
	Side side = {.serialised = &serialised};
	ASSERT_BOOL_EQ(parseChains(&serialised, &side.chains), true);
	SeedPlayer *players = NULL;
	size_t count = 0;
	ASSERT_BOOL_EQ(splitPlayers(&side, 1, &players, &count), true);
	ASSERT_BOOL_EQ(count == 3, true);
	if (count == 3) {
		const size_t starts[] = {1, 4, 10};
		const size_t ends[] = {4, 10, 15};
		const uint32_t persons[] = {'A', 'B', 'A'};
		for (size_t k = 0; k < 3; ++k) {
			ASSERT_BOOL_EQ(players[k].start == starts[k] && players[k].end == ends[k], true);
			ASSERT_BOOL_EQ(players[k].uidRecord == starts[k] + 1, true);
			ASSERT_BOOL_EQ(players[k].person == persons[k], true);
			ASSERT_BOOL_EQ(players[k].occurrence == (k == 2 ? 1u : 0u), true);
		}
	}
	free(players);
	// The team's players begin with a first write and are not split up by other records.
	ASSERT_BOOL_EQ(splitPlayers(&side, 0, &players, &count), false);
	ASSERT_BOOL_EQ(splitPlayers(&side, 2, &players, &count), false);
	ASSERT_BOOL_EQ(players == NULL && count == 0, true);
	freeChains(&side.chains);
	appendPlayer(&serialised, "1ac77d5", &(FakePlayer){'C', 'c', 0, 'f'});
	ASSERT_BOOL_EQ(parseChains(&serialised, &side.chains), true);
	ASSERT_BOOL_EQ(splitPlayers(&side, 1, &players, &count), false);
	freeChains(&side.chains);
	trace_free(&serialised);
}

TEST(opens_inputs_whose_players_are_in_another_order) {
	const FakePlayer game[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}, {'C', 'c', 2, 'f'}};
	const FakePlayer input[] = {{'A', 'a', 0, 'f'}, {'C', 'c', 2, 'f'}, {'B', 'b', 1, 'f'}};
	SeedBuilder builder;
	char why[128] = "";
	ASSERT_BOOL_EQ(openTeams(&builder, game, 3, input, 3, why, sizeof why), true);
	ASSERT_BOOL_EQ(builder.editCount == 0 && pairedWith(&builder, 0, "ACB"), true);
	ASSERT_BOOL_EQ(builder.players[0].moved == 2 && builder.players[1].count == 0, true);
	seedBuilder_close(&builder);
	const FakePlayer edited[] = {{'A', 'a', 0, 'f'}, {'C', 'E', 2, 'f'}, {'B', 'b', 1, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 3, edited, 3, why, sizeof why), true);
	ASSERT_BOOL_EQ(builder.editCount == 1 && pairedWith(&builder, 0, "ACB"), true);
	if (builder.players[0].count == 3) {
		ASSERT_BOOL_EQ(builder.edited[builder.players[0].list[1].start + 2], true); // C's value
	}
	seedBuilder_close(&builder);
	ASSERT_INT_EQ(lockBalance, 0);
}

TEST(pairs_players_with_the_same_layout_by_their_records) {
	const FakePlayer game[] = {{'G', 'x', 3, 'g'}, {'P', 'y', 3, 'p'}};
	const struct {
		FakePlayer input[2];
		const char *persons;
		size_t edits;
		size_t moved;
	} cases[] = {
		{{{'P', 'y', 3, 'p'}, {'G', 'x', 3, 'g'}}, "PG", 0, 2},
		// P injected over G, with G's familiarity, beside P himself (--allow-duplicate)...
		{{{'P', 'y', 3, 'g'}, {'P', 'y', 3, 'p'}}, "GP", 2, 0},
		// ...and in the other order.
		{{{'P', 'y', 3, 'p'}, {'P', 'y', 3, 'g'}}, "PG", 2, 2},
		// Only the unique IDs exchanged.
		{{{'P', 'x', 3, 'g'}, {'G', 'y', 3, 'p'}}, "GP", 2, 0},
		// Two others injected over P and G, listed in that order.
		{{{'I', 'i', 3, 'p'}, {'J', 'j', 3, 'g'}}, "PG", 4, 2},
	};
	for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
		SeedBuilder builder;
		char why[128] = "";
		ASSERT_BOOL_EQ(openTeams(&builder, game, 2, cases[c].input, 2, why, sizeof why), true);
		ASSERT_BOOL_EQ(pairedWith(&builder, 0, cases[c].persons), true);
		ASSERT_BOOL_EQ(builder.editCount == cases[c].edits, true);
		ASSERT_BOOL_EQ(builder.players[0].moved == cases[c].moved, true);
		seedBuilder_close(&builder);
	}
}

TEST(rejects_inputs_with_other_players) {
	const struct {
		FakePlayer game[3];
		size_t gameCount;
		FakePlayer input[3];
		size_t inputCount;
		const char *why;
	} cases[] = {
		{{{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}},
			2,
			{{'A', 'a', 0, 'f'}, {'Z', 'z', 3, 'f'}},
			2,
			"home.player[1] (unique ID 90) matches no player of the game's build"},
		{{{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}, {'C', 'c', 2, 'f'}},
			3,
			{{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}},
			2,
			"the home team has 2 players, the game's build 3"},
		// As many players with each layout, or none.
		{{{'A', 'a', 1, 'f'}, {'C', 'c', 2, 'f'}},
			2,
			{{'A', 'a', 1, 'f'}, {'B', 'b', 1, 'f'}},
			2,
			"home.player[1] (unique ID 66)"},
	};
	for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
		SeedBuilder builder;
		char why[128] = "";
		ASSERT_BOOL_EQ(
			openTeams(
				&builder,
				cases[c].game,
				cases[c].gameCount,
				cases[c].input,
				cases[c].inputCount,
				why,
				sizeof why
			),
			false
		);
		ASSERT_BOOL_EQ(strstr(why, cases[c].why) != NULL, true);
		ASSERT_BOOL_EQ(builder.players[0].list == NULL, true);
		seedBuilder_close(&builder);
	}
}

TEST(merges_edits_onto_the_same_person) {
	SeedBuilder builder;
	char why[128] = "";
	// The build lists the players in another order, with another layout...
	const FakePlayer game[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer input[] = {{'A', 'E', 0, 'f'}, {'B', 'b', 1, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, input, 2, why, sizeof why), true);
	const FakePlayer built[] = {{'B', 'q', 1, 'f'}, {'A', 'r', 0, 'f'}};
	const FakePlayer merged[] = {{'B', 'q', 1, 'f'}, {'A', 'E', 0, 'f'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, built, 2, merged, 2), true);
	// ...or with the input's layout, where the records would otherwise pair one to one.
	seedBuilder_close(&builder);
	const FakePlayer same[] = {{'A', 'a', 1, 'g'}, {'B', 'b', 1, 'h'}};
	const FakePlayer sameInput[] = {{'A', 'E', 1, 'g'}, {'B', 'b', 1, 'h'}};
	ASSERT_BOOL_EQ(openTeams(&builder, same, 2, sameInput, 2, why, sizeof why), true);
	const FakePlayer sameBuilt[] = {{'B', 'q', 1, 'h'}, {'A', 'r', 1, 'g'}};
	const FakePlayer sameMerged[] = {{'B', 'q', 1, 'h'}, {'A', 'E', 1, 'g'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, sameBuilt, 2, sameMerged, 2), true);
	// In the same order, the edit stays in its place.
	const FakePlayer inOrder[] = {{'A', 'r', 1, 'g'}, {'B', 'q', 1, 'h'}};
	const FakePlayer inOrderMerged[] = {{'A', 'E', 1, 'g'}, {'B', 'q', 1, 'h'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, inOrder, 2, inOrderMerged, 2), true);
	seedBuilder_close(&builder);
	// An injected player follows the player it replaced.
	const FakePlayer kept[] = {{'A', 'a', 3, 'g'}, {'B', 'b', 3, 'h'}};
	const FakePlayer injected[] = {{'X', 'X', 3, 'g'}, {'B', 'b', 3, 'h'}};
	ASSERT_BOOL_EQ(openTeams(&builder, kept, 2, injected, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(pairedWith(&builder, 0, "AB"), true);
	const FakePlayer keptBuilt[] = {{'B', 'q', 3, 'h'}, {'A', 'r', 3, 'g'}};
	const FakePlayer keptMerged[] = {{'B', 'q', 3, 'h'}, {'X', 'X', 3, 'g'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, keptBuilt, 2, keptMerged, 2), true);
	seedBuilder_close(&builder);
	ASSERT_INT_EQ(lockBalance, 0);
}

TEST(fails_seeds_whose_build_leaves_out_an_edited_player) {
	SeedBuilder builder;
	char why[128] = "";
	const FakePlayer game[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer editedA[] = {{'A', 'E', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer built[] = {{'B', 'q', 1, 'f'}, {'C', 'c', 2, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, editedA, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(mergesInto(&builder, built, 2, built, 2), false);
	seedBuilder_close(&builder);
	// A player without edits may be left out.
	const FakePlayer editedB[] = {{'A', 'a', 0, 'f'}, {'B', 'E', 1, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, editedB, 2, why, sizeof why), true);
	const FakePlayer merged[] = {{'B', 'E', 1, 'f'}, {'C', 'c', 2, 'f'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, built, 2, merged, 2), true);
	seedBuilder_close(&builder);
}

TEST(pairs_players_when_the_teams_parts_differ) {
	SeedBuilder builder;
	char why[128] = "";
	const FakePlayer game[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer input[] = {{'A', 'E', 0, 'f'}, {'B', 'b', 1, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, input, 2, why, sizeof why), true);
	// The build's team has a part the input's lacks, so its parts are not aligned one by one.
	const FakePlayer players[] = {{'B', 'q', 1, 'f'}, {'A', 'r', 0, 'f'}};
	const FakePlayer merged[] = {{'B', 'q', 1, 'f'}, {'A', 'E', 0, 'f'}};
	Serialised built = {0};
	Serialised want = {0};
	appendSiteRecord(&built, "s", "1ac77d5>1b6f362", SITE_BACKROOM_FIRST);
	appendTeam(&built, "1ac77d5", players, 2);
	appendSiteRecord(&want, "s", "1ac77d5>1b6f362", SITE_BACKROOM_FIRST);
	appendTeam(&want, "1ac77d5", merged, 2);
	ByteBuffer out = {0};
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	ASSERT_BOOL_EQ(
		out.length == want.bytes.length && memcmp(out.data, want.bytes.data, out.length) == 0, true
	);
	trace_free(&built);
	trace_free(&want);
	seedBuilder_close(&builder);
	// Here the part follows the players and ends like B, so the two teams' ends pair it with B's last
	// record. B's edit goes to B alone.
	const FakePlayer editedB[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'E'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, editedB, 2, why, sizeof why), true);
	const FakePlayer mergedB[] = {{'B', 'q', 1, 'E'}, {'A', 'r', 0, 'f'}};
	appendSiteRecord(&built, "n", "1ac77d5", SITE_TEAM_PLAYER_COUNT);
	appendSiteRecord(&want, "n", "1ac77d5", SITE_TEAM_PLAYER_COUNT);
	for (size_t k = 0; k < 2; ++k) {
		appendPlayer(&built, "1ac77d5", &players[k]);
		appendPlayer(&want, "1ac77d5", &mergedB[k]);
	}
	appendSiteRecord(&built, "t", "1ac77d5>1b6f4b7", SITE_FAMILIARITY_SETTING);
	appendSiteRecord(&built, "x", "1ac77d5", SITE_TEAM_HAS_EXTRA);
	appendSiteRecord(&want, "t", "1ac77d5>1b6f4b7", SITE_FAMILIARITY_SETTING);
	appendSiteRecord(&want, "x", "1ac77d5", SITE_TEAM_HAS_EXTRA);
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), true);
	ASSERT_BOOL_EQ(
		out.length == want.bytes.length && memcmp(out.data, want.bytes.data, out.length) == 0, true
	);
	trace_free(&built);
	trace_free(&want);
	seedBuilder_close(&builder);
	// The other way round: the input's part follows its players, and the build lacks it but has a
	// player C the input lacks, whose last record the ends pair with the part's. The part's edit
	// does not go to C, so the seed fails.
	builder = (SeedBuilder){0};
	Serialised reference = {0};
	const FakePlayer gameA[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 0, 'f'}};
	const FakePlayer withPart[] = {{'A', 'E', 0, 'f'}, {'B', 'b', 0, 'f'}};
	appendSiteRecord(&reference, "n", "1ac77d5", SITE_TEAM_PLAYER_COUNT);
	appendSiteRecord(&builder.input, "n", "1ac77d5", SITE_TEAM_PLAYER_COUNT);
	for (size_t k = 0; k < 2; ++k) {
		appendPlayer(&reference, "1ac77d5", &gameA[k]);
		appendPlayer(&builder.input, "1ac77d5", &withPart[k]);
	}
	appendSiteRecord(&reference, "t", "1ac77d5>1b6f4b7", SITE_FAMILIARITY_SETTING);
	appendSiteRecord(&builder.input, "T", "1ac77d5>1b6f4b7", SITE_FAMILIARITY_SETTING);
	appendSiteRecord(&reference, "x", "1ac77d5", SITE_TEAM_HAS_EXTRA);
	appendSiteRecord(&builder.input, "x", "1ac77d5", SITE_TEAM_HAS_EXTRA);
	ASSERT_BOOL_EQ(openOn(&builder, &reference, why, sizeof why), true);
	trace_free(&reference);
	const FakePlayer extra[] = {{'B', 'q', 0, 'f'}, {'A', 'r', 0, 'f'}, {'C', 'c', 1, 'f'}};
	appendTeam(&built, "1ac77d5", extra, 3);
	ASSERT_BOOL_EQ(builder.editCount == 2, true);
	ASSERT_BOOL_EQ(mergeEdits(&builder, &built, 7, &out), false);
	buffer_free(&out);
	trace_free(&built);
	seedBuilder_close(&builder);
}

TEST(pairs_tied_players_by_unique_id_then_place) {
	SeedBuilder builder;
	char why[128] = "";
	// Each input player shares as many records with both of the game's: the unique ID decides.
	const FakePlayer game[] = {{'A', 'a', 1, 'g'}, {'B', 'b', 1, 'g'}};
	const FakePlayer input[] = {{'B', 'a', 1, 'g'}, {'A', 'b', 1, 'g'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, input, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(pairedWith(&builder, 0, "BA") && builder.editCount == 2, true);
	const FakePlayer built[] = {{'A', 'q', 1, 'g'}, {'B', 'r', 1, 'g'}};
	const FakePlayer merged[] = {{'A', 'b', 1, 'g'}, {'B', 'a', 1, 'g'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, built, 2, merged, 2), true);
	seedBuilder_close(&builder);
	// With the same unique ID too, the same place decides before the input's order.
	const FakePlayer repeated[] = {{'A', 'a', 1, 'h'}, {'A', 'b', 1, 'h'}, {'C', 'c', 1, 'f'}};
	const FakePlayer moved[] = {{'C', 'c', 1, 'f'}, {'A', 'x', 1, 'h'}, {'A', 'y', 1, 'h'}};
	ASSERT_BOOL_EQ(openTeams(&builder, repeated, 3, moved, 3, why, sizeof why), true);
	ASSERT_BOOL_EQ(builder.players[0].count == 3 && builder.players[0].moved == 2, true);
	if (builder.players[0].count == 3) {
		ASSERT_BOOL_EQ(builder.players[0].list[1].occurrence == 1, true);
	}
	const FakePlayer repeatedBuilt[] = {{'A', 'q', 1, 'h'}, {'A', 'r', 1, 'h'}, {'C', 'c', 1, 'f'}};
	const FakePlayer repeatedMerged[] = {{'A', 'y', 1, 'h'}, {'A', 'x', 1, 'h'}, {'C', 'c', 1, 'f'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, repeatedBuilt, 3, repeatedMerged, 3), true);
	seedBuilder_close(&builder);
	// Players tied in every way stay in their places.
	const FakePlayer twice[] = {{'A', 'x', 1, 'h'}, {'A', 'y', 1, 'h'}};
	ASSERT_BOOL_EQ(openTeams(&builder, repeated, 2, twice, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(builder.players[0].count == 2 && builder.players[0].moved == 0, true);
	seedBuilder_close(&builder);
	ASSERT_INT_EQ(lockBalance, 0);
}

TEST(pairs_repeated_players_in_order) {
	SeedBuilder builder;
	char why[128] = "";
	const FakePlayer game[] = {{'A', 'a', 1, 'f'}, {'A', 'b', 1, 'f'}};
	const FakePlayer input[] = {{'A', 'a', 1, 'f'}, {'A', 'E', 1, 'f'}};
	ASSERT_BOOL_EQ(openTeams(&builder, game, 2, input, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(pairedWith(&builder, 0, "AA") && builder.editCount == 1, true);
	const FakePlayer built[] = {{'A', 'q', 1, 'f'}, {'A', 'r', 2, 'f'}};
	const FakePlayer merged[] = {{'A', 'q', 1, 'f'}, {'A', 'E', 2, 'f'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, built, 2, merged, 2), true);
	seedBuilder_close(&builder);
	// The input's first player pairs with the build's second, also where the layouts match.
	const FakePlayer kept[] = {{'A', 'a', 1, 'g'}, {'A', 'b', 1, 'h'}};
	const FakePlayer swapped[] = {{'A', 'E', 1, 'h'}, {'A', 'a', 1, 'g'}};
	ASSERT_BOOL_EQ(openTeams(&builder, kept, 2, swapped, 2, why, sizeof why), true);
	ASSERT_BOOL_EQ(builder.players[0].count == 2 && builder.players[0].moved == 2, true);
	const FakePlayer keptBuilt[] = {{'A', 'q', 1, 'g'}, {'A', 'r', 1, 'h'}};
	const FakePlayer keptMerged[] = {{'A', 'q', 1, 'g'}, {'A', 'E', 1, 'h'}};
	ASSERT_BOOL_EQ(mergesInto(&builder, keptBuilt, 2, keptMerged, 2), true);
	seedBuilder_close(&builder);
}

TEST(pairs_each_teams_players_by_person) {
	const FakePlayer home[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer away[] = {{'D', 'd', 0, 'f'}, {'E', 'e', 1, 'f'}};
	const FakePlayer awayInput[] = {{'E', 'e', 1, 'f'}, {'D', 'X', 0, 'f'}};
	SeedBuilder builder = {0};
	Serialised reference = {0};
	appendTeam(&reference, "1ac77d5", home, 2);
	appendTeam(&reference, "1ac77e8", away, 2);
	appendTeam(&builder.input, "1ac77d5", home, 2);
	appendTeam(&builder.input, "1ac77e8", awayInput, 2);
	char why[128] = "";
	ASSERT_BOOL_EQ(openOn(&builder, &reference, why, sizeof why), true);
	trace_free(&reference);
	ASSERT_BOOL_EQ(pairedWith(&builder, 0, "AB") && pairedWith(&builder, 1, "ED"), true);
	ASSERT_BOOL_EQ(builder.editCount == 1 && builder.players[1].moved == 2, true);
	// The away edit follows D, whichever order each team is built in.
	const FakePlayer homeBuilt[] = {{'B', 'q', 1, 'f'}, {'A', 'r', 0, 'f'}};
	const FakePlayer awayBuilt[] = {{'D', 's', 0, 'f'}, {'E', 't', 1, 'f'}};
	const FakePlayer awayMerged[] = {{'D', 'X', 0, 'f'}, {'E', 't', 1, 'f'}};
	Serialised build = {0};
	Serialised want = {0};
	appendTeam(&build, "1ac77d5", homeBuilt, 2);
	appendTeam(&build, "1ac77e8", awayBuilt, 2);
	appendTeam(&want, "1ac77d5", homeBuilt, 2);
	appendTeam(&want, "1ac77e8", awayMerged, 2);
	ASSERT_BOOL_EQ(mergesAs(&builder, &build, &want), true);
	seedBuilder_close(&builder);
	// Away players with one layout, built in the other order: the build has the input's layout, but
	// D's edit must still not land on E.
	const FakePlayer sameAway[] = {{'D', 'd', 1, 'g'}, {'E', 'e', 1, 'h'}};
	const FakePlayer sameAwayInput[] = {{'D', 'X', 1, 'g'}, {'E', 'e', 1, 'h'}};
	builder = (SeedBuilder){0};
	appendTeam(&reference, "1ac77d5", home, 2);
	appendTeam(&reference, "1ac77e8", sameAway, 2);
	appendTeam(&builder.input, "1ac77d5", home, 2);
	appendTeam(&builder.input, "1ac77e8", sameAwayInput, 2);
	ASSERT_BOOL_EQ(openOn(&builder, &reference, why, sizeof why), true);
	trace_free(&reference);
	const FakePlayer sameAwayBuilt[] = {{'E', 't', 1, 'h'}, {'D', 's', 1, 'g'}};
	const FakePlayer sameAwayMerged[] = {{'E', 't', 1, 'h'}, {'D', 'X', 1, 'g'}};
	appendTeam(&build, "1ac77d5", home, 2);
	appendTeam(&build, "1ac77e8", sameAwayBuilt, 2);
	appendTeam(&want, "1ac77d5", home, 2);
	appendTeam(&want, "1ac77e8", sameAwayMerged, 2);
	ASSERT_BOOL_EQ(sameLayout(&builder.input, &build), true);
	ASSERT_BOOL_EQ(mergesAs(&builder, &build, &want), true);
	seedBuilder_close(&builder);
	ASSERT_INT_EQ(lockBalance, 0);
}

TEST(pairs_players_in_order_when_the_build_cannot_be_split) {
	const FakePlayer game[] = {{'A', 'a', 0, 'f'}, {'B', 'b', 1, 'f'}};
	const FakePlayer orders[][2] = {
		{{'A', 'E', 0, 'f'}, {'B', 'b', 1, 'f'}}, {{'B', 'b', 1, 'f'}, {'A', 'a', 0, 'f'}}};
	for (size_t c = 0; c < 2; ++c) {
		Serialised reference = {0};
		Serialised input = {0};
		appendTeam(&reference, "1ac77d5", game, 2);
		appendTeam(&input, "1ac77d5", orders[c], 2);
		reference.records[1].site = 0x1B3C700; // no first write starts its players
		input.records[1].site = 0x1B3C700;
		Side sides[2] = {{.serialised = &reference}, {.serialised = &input}};
		ASSERT_BOOL_EQ(
			parseChains(&reference, &sides[0].chains) && parseChains(&input, &sides[1].chains), true
		);
		bool edited[16] = {0};
		SeedPlayers players[2] = {0};
		char why[64] = "";
		ASSERT_BOOL_EQ(input.recordCount <= 16, true);
		ASSERT_BOOL_EQ(findEdits(&sides[0], &sides[1], edited, players, why, sizeof why), c == 0);
		ASSERT_BOOL_EQ(players[0].count == 0 && players[0].list == NULL, true);
		if (c == 0) {
			ASSERT_BOOL_EQ(edited[3], true); // A's value, in its place
		}
		freeChains(&sides[0].chains);
		freeChains(&sides[1].chains);
		trace_free(&reference);
		trace_free(&input);
	}
}

TEST(opens_and_rebuilds_players_the_game_reorders) {
	uint32_t seed = 99;
	ByteBuffer input = {.data = (uint8_t *)&seed, .length = sizeof seed};
	SeedBuilder builder;
	char error[256] = "";
	fakePlayers = true;
	hasFixture = true;
	// As after a club's first build: the input's round trip (the third serialisation) lists A before
	// B, and the game's builds list B before A.
	reorderSerialisations = ~(1U << 3);
	serialisations = 0;
	bool ok = seedBuilder_open(&builder, &input, seed, NULL, error, sizeof error);
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 0 && builder.players[0].moved == 2, true);
		seedBuilder_close(&builder);
	}
	// Edits: the seed, and A's value, which depends on it. A rebuild keeps B as built and A's value.
	serialisations = 0;
	ok = seedBuilder_open(&builder, &input, seed - 1, NULL, error, sizeof error);
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		ASSERT_BOOL_EQ(seedBuilder_editCount(&builder) == 2, true);
		ByteBuffer out = {0};
		ASSERT_BOOL_EQ(seedBuilder_build(&builder, 7, &out), true);
		uint8_t expected[sizeof seed + 13] = {0};
		memcpy(expected, &seed, sizeof seed);
		memcpy(expected + sizeof seed, "pB\0\0\0bpA\0\0\0af", 13);
		ASSERT_BOOL_EQ(
			out.length == sizeof expected && memcmp(out.data, expected, sizeof expected) == 0, true
		);
		buffer_free(&out);
		seedBuilder_close(&builder);
	}
	// An input with a player the game's build doesn't have is not its build.
	thirdPlayerSerialisations = 1U << 3;
	serialisations = 0;
	ok = seedBuilder_open(&builder, &input, seed, NULL, error, sizeof error);
	ASSERT_BOOL_EQ(ok, false);
	ASSERT_BOOL_EQ(strstr(error, ": the home team has 3 players, the game's build 2") != NULL, true);
	hasFixture = false;
	fakePlayers = false;
	reorderSerialisations = 0;
	thirdPlayerSerialisations = 0;
	ASSERT_INT_EQ(lockBalance, 0);
}

typedef struct TestPlayer {
	_Alignas(void *) uint8_t object[PLAYER_STATS + sizeof(void *)];
	uint8_t stats[256];
} TestPlayer;

static void setPointer(uint8_t *bytes, size_t offset, const void *value) {
	memcpy(bytes + offset, &value, sizeof value);
}

static void setPlayer(TestPlayer *player, uint32_t uniqueId, uint8_t goals) {
	*player = (TestPlayer){0};
	memcpy(player->object + PLAYER_UNIQUE_ID, &uniqueId, sizeof uniqueId);
	setPointer(player->object, PLAYER_STATS, player->stats);
	player->stats[STATS_PERSON] = 1;
	uint16_t rating = 700;
	memcpy(player->stats + STATS_RATING, &rating, sizeof rating);
	player->stats[playerValues[V_GOALS].offset] = goals;
	player->stats[playerValues[V_PASSES_COMPLETED].offset] = 30;
}

TEST(keeps_statistics_with_player_identity) {
	TestPlayer players[3];
	setPlayer(&players[0], 10, 1);
	setPlayer(&players[1], 20, 2);
	setPlayer(&players[2], 5, 5);
	_Alignas(void *) uint8_t home[1024] = {0};
	_Alignas(void *) uint8_t away[1024] = {0};
	_Alignas(void *) uint8_t result[160] = {0};
	_Alignas(void *) uint8_t match[MATCH_TEAM_AWAY + sizeof(void *)] = {0};
	home[TEAM_PLAYER_COUNT] = 2;
	setPointer(home, TEAM_PLAYERS, players[0].object);
	setPointer(home, TEAM_PLAYERS + sizeof(void *), players[1].object);
	uint16_t length = 90;
	memcpy(result + RESULT_MATCH_LENGTH, &length, sizeof length);
	setPointer(match, MATCH_RESULT, result);
	setPointer(match, MATCH_TEAM_HOME, home);
	setPointer(match, MATCH_TEAM_AWAY, away);
	MatchStatsTotals totals = {0};
	ASSERT_BOOL_EQ(matchStats_add(&totals, match), true);
	setPlayer(&players[0], 10, 4);
	setPlayer(&players[1], 20, 3);
	home[TEAM_PLAYER_COUNT] = 3;
	setPointer(home, TEAM_PLAYERS, players[1].object);
	setPointer(home, TEAM_PLAYERS + sizeof(void *), players[0].object);
	setPointer(home, TEAM_PLAYERS + 2 * sizeof(void *), players[2].object);
	ASSERT_BOOL_EQ(matchStats_add(&totals, match), true);
	ASSERT_BOOL_EQ(totals.matches == 2 && totals.teams[0].playerCount == 3, true);
	TeamTotals *team = &totals.teams[0];
	if (team->playerCount == 3) {
		ASSERT_BOOL_EQ(team->players[0].uniqueId == 5 && team->players[0].values[V_GOALS] == 5, true);
		ASSERT_BOOL_EQ(team->players[1].uniqueId == 10 && team->players[1].values[V_GOALS] == 5, true);
		ASSERT_BOOL_EQ(team->players[2].uniqueId == 20 && team->players[2].values[V_GOALS] == 5, true);
		ASSERT_BOOL_EQ(team->players[1].appearances == 2 && team->players[0].appearances == 1, true);
		ASSERT_BOOL_EQ(team->values[1] == 200, true);
	}
	for (uint32_t i = 0; i < 2 * MATCH_STATS_MAX_PLAYERS; ++i) {
		ASSERT_BOOL_EQ(findPlayer(team, 1000 + i) != NULL, true);
	}
	ASSERT_BOOL_EQ(team->playerCount == 3 + 2 * MATCH_STATS_MAX_PLAYERS, true);
	matchStats_free(&totals);
	ASSERT_BOOL_EQ(totals.teams[0].players == NULL && totals.matches == 0, true);
}

TEST(rejects_truncated_statistics_paths) {
	wchar_t output[HARNESS_PATH_LENGTH];
	ASSERT_BOOL_EQ(siblingPath(L"match.csv", L"-players.csv", output, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(wcscmp(output, L"match-players.csv") == 0, true);
	wchar_t longPath[HARNESS_PATH_LENGTH];
	wmemset(longPath, L'a', HARNESS_PATH_LENGTH - 1);
	longPath[HARNESS_PATH_LENGTH - 1] = 0;
	ASSERT_BOOL_EQ(siblingPath(longPath, L"-players.csv", output, HARNESS_PATH_LENGTH), false);
	longPath[300] = 0;
	ASSERT_BOOL_EQ(siblingPath(longPath, L"-players.csv", output, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(wcslen(output) == 312, true);
}

TEST(labels_instruction_runs_in_order) {
	TraceRecord records[] = {
		{.site = SITE_TACTIC_TEAM_INSTRUCTIONS},
		{.site = SITE_TACTIC_TEAM_INSTRUCTIONS},
		{.site = SITE_TACTIC_TEAM_INSTRUCTIONS},
		{.site = SITE_TACTIC_MENTALITY},
		{.site = SITE_TACTIC_TEAM_INSTRUCTIONS},
	};
	char *paths[] = {"1ac77d5>1b6f34b>1a8971a"};
	Serialised serialised = {.records = records, .recordCount = 5, .paths = paths, .pathCount = 1};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	ASSERT_INT_EQ((int)labels[0].flagWord, 0);
	ASSERT_INT_EQ((int)labels[1].flagWord, 1);
	ASSERT_INT_EQ((int)labels[2].flagWord, 2);
	ASSERT_INT_EQ((int)labels[4].flagWord, 0);
	ASSERT_BOOL_EQ(
		strcmp(labels[2].name, "home.manager.tactic.plan[0].team_instructions[2]") == 0, true
	);
	free(labels);
}

TEST(labels_staff_attributes_byte_by_byte) {
	uint8_t bytes[4 + STAFF_ATTRIBUTE_COUNT] = {0};
	for (size_t k = 0; k < STAFF_ATTRIBUTE_COUNT; ++k) {
		bytes[4 + k] = (uint8_t)(k + 1);
	}
	TraceRecord records[] = {
		{.site = SITE_BACKROOM_FIRST, .offset = 0, .length = 4},
		{.site = SITE_STAFF_ATTRIBUTES, .offset = 4, .length = STAFF_ATTRIBUTE_COUNT},
	};
	char *paths[] = {"1ac77d5>1b6f51f>1ae992a"};
	Serialised serialised = {
		.bytes = {.data = bytes, .length = sizeof bytes},
		.records = records,
		.recordCount = 2,
		.paths = paths,
		.pathCount = 1,
	};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	ASSERT_BOOL_EQ(labels[1].staffAttributes, true);
	ASSERT_BOOL_EQ(strcmp(labels[1].name, "home.backroom[0].attributes") == 0, true);
#ifdef _WIN32
	wchar_t directory[MAX_PATH] = {0};
	wchar_t path[MAX_PATH] = {0};
	DWORD length = GetTempPathW(MAX_PATH, directory);
	ASSERT_BOOL_EQ(length > 0 && length < MAX_PATH, true);
	ASSERT_BOOL_EQ(length && length < MAX_PATH && GetTempFileNameW(directory, L"fma", 0, path), true);
	FILE *file = path[0] ? _wfopen(path, L"w+b") : NULL;
#else
	FILE *file = tmpfile();
#endif
	ASSERT_BOOL_EQ(file != NULL, true);
	if (file) {
		ASSERT_INT_EQ((int)writeField(file, &serialised, labels, 1, SIZE_MAX, false), 1);
		ASSERT_INT_EQ(fseek(file, 0, SEEK_SET), 0);
		char line[256];
		size_t fields = 0;
		while (fgets(line, sizeof line, file)) {
			if (fields == 0) {
				ASSERT_BOOL_EQ(
					strstr(
						line, "\"name\": \"home.backroom[0].attributes.tactical_attacking\", \"u8\": 1}"
					) != NULL,
					true
				);
			}
			if (fields == STAFF_ATTRIBUTE_COUNT - 1) {
				ASSERT_BOOL_EQ(
					strstr(line, ".attributes.coaching_set_pieces\", \"u8\": 52}") != NULL, true
				);
			}
			++fields;
		}
		ASSERT_INT_EQ((int)fields, STAFF_ATTRIBUTE_COUNT);
		(void)fclose(file);
	}
#ifdef _WIN32
	if (path[0]) {
		(void)DeleteFileW(path);
	}
#endif
	free(labels);
}

TEST(labels_weather_and_venue_fields) {
	TraceRecord records[] = {
		{.site = 0x1A7BED3, .path = 0},                      // match header: kickoff weather follows
		{.site = 0x1A7BEE9, .path = 0},                      // MATCH_BASE+0x6b0
		{.site = 0x43CC669, .path = 0},                      // first weather change's temperature
		{.site = SITE_WEATHER_CHANGE_TIME, .path = 0},       //   and its time
		{.site = 0x43CC669, .path = 0},                      // second change's temperature
		{.site = 0x1A8EB7E, .path = 1},                      // venue: the pitch length names the block
		{.site = SITE_PITCH_WIDTH, .path = 1},               //
		{.site = 0x1A8EC6C, .path = 1},                      // second landmark coordinate
		{.site = 0x456AA30, .path = 1},                      // fn_456a9d0+0x158
		{.site = 0x45844E1, .path = 1},                      // fn_45844b0+0x7
		{.site = 0x456AC96, .path = 1},                      // fn_456a9d0+0x10c
		{.site = 0x456ADB4, .path = 1},                      // fn_456a9d0+0x140
		{.site = 0x43D0774, .caller = 0x43D0256, .path = 1}, // the unnamed first section array
		{.site = 0x43D0774, .caller = SITE_OCCUPANCY_CALLER, .path = 1},
		{.site = SITE_TEXT, .caller = SITE_STADIUM_NAME_CALLER, .path = 1},
		{.site = 0x11758B46, .path = 2}, // fixture_extra: attendance names the block
		{.site = SITE_FIXTURE_EXTRA_AWAY_FANS, .path = 2},
	};
	char *paths[] = {"1ac76fb", "1ac778c", "1ac77c2"};
	Serialised serialised = {
		.records = records,
		.recordCount = sizeof records / sizeof records[0],
		.paths = paths,
		.pathCount = 3,
	};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	static const char *const expected[] = {
		"match",
		"match.weather.wind",
		"match.weather.change[0].temperature",
		"match.weather.change[0].time",
		"match.weather.change[1].temperature",
		"venue",
		"venue.pitch.width",
		"venue.pitch.centre_spot.y",
		"venue.attendance",
		"venue.stand[0].flags",
		"venue.stand[0].model[0]",
		"venue.stand[5].model[1]",
		"",
		"venue.section[1].occupancy",
		"venue.stadium.name",
		"fixture_extra",
		"fixture_extra.away_fans",
	};
	for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
		ASSERT_BOOL_EQ(strcmp(labels[i].name, expected[i]) == 0, true);
	}
	ASSERT_INT_EQ((int)labels[9].flags, FLAGS_STAND);
	ASSERT_BOOL_EQ(strcmp(labels_flagName(&labels[9], 3), "tunnel") == 0, true);
	free(labels);
}

TEST(labels_official_ratings) {
	TraceRecord records[] = {
		{.site = SITE_OFFICIAL_ALLOWING_FLOW, .path = 0}, // names the block
		{.site = SITE_OFFICIAL_DISCIPLINE, .path = 0},
		{.site = SITE_OFFICIAL_IMPORTANT_MATCHES, .path = 0},
		{.site = SITE_OFFICIAL_REFEREEING, .path = 0},
		{.site = SITE_OFFICIAL_RUNNING_LINE, .path = 0},
		{.site = SITE_OFFICIAL_TIME_KEEPING, .path = 0},
		{.site = SITE_OFFICIAL_ALLOWING_FLOW, .path = 1},
		{.site = SITE_OFFICIAL_TIME_KEEPING, .path = 1},
	};
	char *paths[] = {"1ac7719", "1ac7775"};
	Serialised serialised = {
		.records = records,
		.recordCount = sizeof records / sizeof records[0],
		.paths = paths,
		.pathCount = 2,
	};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	static const char *const expected[] = {
		"official[0]",
		"official[0].discipline",
		"official[0].important_matches",
		"official[0].refereeing",
		"official[0].running_line",
		"official[0].time_keeping",
		"referee",
		"referee.time_keeping",
	};
	for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
		ASSERT_BOOL_EQ(strcmp(labels[i].name, expected[i]) == 0, true);
	}
	free(labels);
}

TEST(labels_match_focus) {
	TraceRecord records[] = {
		{.site = SITE_BACKROOM_FIRST},
		{.site = SITE_STAFF_FOCUS_COUNT},
		{.site = SITE_STAFF_FOCUS_CODE},
		{.site = SITE_STAFF_FOCUS_CODE},
		{.site = SITE_STAFF_FOCUS_CODE},
	};
	char *paths[] = {"1ac77e8>1b6f34b>1ae992a"};
	Serialised serialised = {
		.records = records,
		.recordCount = sizeof records / sizeof records[0],
		.paths = paths,
		.pathCount = 1,
	};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	static const char *const expected[] = {
		"away.manager",
		"away.manager.match_focus.count",
		"away.manager.match_focus.code[0]",
		"away.manager.match_focus.code[1]",
		"away.manager.match_focus.code[2]",
	};
	for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
		ASSERT_BOOL_EQ(strcmp(labels[i].name, expected[i]) == 0, true);
	}
	free(labels);
}

TEST(labels_tactical_familiarity) {
	TraceRecord records[] = {
		{.site = SITE_PLAYER_FIRST},
		{.site = SITE_FAMILIARITY_VERSION},
		{.site = SITE_FAMILIARITY_ROLE_COUNT},
		{.site = SITE_FAMILIARITY_ROLE_KEY},
		{.site = SITE_FAMILIARITY_ROLE_POSITIONS},
		{.site = SITE_FAMILIARITY_ROLE_AMOUNT},
		{.site = SITE_FAMILIARITY_COUNT},
		{.site = SITE_FAMILIARITY_SETTING},
		{.site = SITE_FAMILIARITY_AMOUNT},
		{.site = SITE_FAMILIARITY_SETTING},
		{.site = SITE_FAMILIARITY_AMOUNT},
		{.site = SITE_FAMILIARITY_COUNT},
		{.site = SITE_FAMILIARITY_COUNT},
		{.site = SITE_FAMILIARITY_SETTING},
	};
	char *paths[] = {"1ac77d5>1b6f456>1a808b0"};
	Serialised serialised = {
		.records = records,
		.recordCount = sizeof records / sizeof records[0],
		.paths = paths,
		.pathCount = 1,
	};
	PeopleIndex people = {0};
	FieldLabel *labels = labels_compute(&serialised, &people);
	ASSERT_BOOL_EQ(labels != NULL, true);
	if (!labels) {
		return;
	}
	static const char *const expected[] = {
		"home.player[0]",
		"home.player[0].tactical_familiarity.version",
		"home.player[0].tactical_familiarity.position_role_duty.count",
		"home.player[0].tactical_familiarity.position_role_duty[0].key",
		"home.player[0].tactical_familiarity.position_role_duty[0].positions",
		"home.player[0].tactical_familiarity.position_role_duty[0].amount",
		"home.player[0].tactical_familiarity.mentality.count",
		"home.player[0].tactical_familiarity.mentality[0].setting",
		"home.player[0].tactical_familiarity.mentality[0].amount",
		"home.player[0].tactical_familiarity.mentality[1].setting",
		"home.player[0].tactical_familiarity.mentality[1].amount",
		"home.player[0].tactical_familiarity.passing_style.count",
		"home.player[0].tactical_familiarity.creative_freedom.count",
		"home.player[0].tactical_familiarity.creative_freedom[0].setting",
	};
	for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
		ASSERT_BOOL_EQ(strcmp(labels[i].name, expected[i]) == 0, true);
	}
	free(labels);
}

TEST(only_merges_valid_utf8_text) {
	static const char *const valid[] = {
		"text",
		"\xC2\xA0",
		"\xDF\xBF",
		"\xE0\xA0\x80",
		"\xED\x9F\xBF",
		"\xF0\x90\x80\x80",
		"\xF4\x8F\xBF\xBF",
	};
	static const char *const invalid[] = {
		"\x01",
		"\x7F",
		"\x80",
		"\xC0\xAF",
		"\xC1\xBF",
		"\xC2",
		"\xE0\x80\xAF",
		"\xED\xA0\x80",
		"\xF0\x80\x80\xAF",
		"\xF4\x90\x80\x80",
		"\xF5\x80\x80\x80",
		"\xFF",
	};
	for (size_t i = 0; i < sizeof valid / sizeof valid[0]; ++i) {
		ASSERT_BOOL_EQ(configWriter_isText((const uint8_t *)valid[i], strlen(valid[i])), true);
	}
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		ASSERT_BOOL_EQ(configWriter_isText((const uint8_t *)invalid[i], strlen(invalid[i])), false);
	}
}

void registerDllTests(void) {
	test_run(
		"DLL reports trace allocation failures and restores its hook",
		test_reports_trace_allocation_failures
	);
	test_run(
		"DLL only traces setup serialisations whose records are used",
		test_only_traces_setup_round_trips
	);
	test_run(
		"DLL rejects inputs whose writes differ from the game's build",
		test_rejects_inputs_with_another_layout
	);
	test_run(
		"DLL opens and rebuilds inputs whose observers differ from the game's build",
		test_opens_and_rebuilds_inputs_whose_observers_differ
	);
	test_run(
		"DLL drops only the replaced team's edits and reports failures",
		test_drops_only_replaced_team_edits
	);
	test_run(
		"DLL swaps either or both teams and restores game state",
		test_swaps_either_or_both_teams_and_restores_game_state
	);
	test_run(
		"DLL copies either team's block over the other's", test_copies_either_team_block_over_the_other
	);
	test_run(
		"DLL builds a team playing itself against the fixture's opponent",
		test_builds_a_team_playing_itself_against_the_fixtures_opponent
	);
	test_run("DLL writes both team overrides", test_writes_both_team_overrides);
	test_run(
		"DLL writes the save and game date after the source, only when known",
		test_writes_the_save_and_game_date_after_the_source
	);
	test_run(
		"DLL decodes binary fixtures as exports, with or without research keys",
		test_decodes_binary_fixtures_as_exports
	);
	test_run(
		"DLL names what the game is doing and formats its dates",
		test_names_what_the_game_is_doing_and_formats_its_dates
	);
#ifdef __APPLE__
	test_run(
		"DLL reads save names only as short UTF-8 text, without .fm",
		test_reads_save_names_only_as_short_utf8_text
	);
	test_run(
		"DLL reads the game's state from its objects", test_reads_the_game_state_from_its_objects
	);
#endif
	test_run(
		"DLL refuses jobs but status while the game loads or processes",
		test_refuses_jobs_but_status_while_the_game_is_busy
	);
	test_run("DLL interns call chains without repeated text formatting", test_interns_numeric_paths);
	test_run(
		"DLL rejects incomplete or invalid serialisation traces", test_rejects_incomplete_traces
	);
	test_run(
		"DLL skips tracing and alignment for unedited builds", test_skips_tracing_unedited_builds
	);
	test_run(
		"DLL merges resized edits without parsing unchanged layouts",
		test_merges_resized_edits_with_the_same_layout
	);
	test_run("DLL rejects edits that cannot be aligned", test_rejects_unplaceable_edits);
	test_run("DLL finds the match's observers by call chain", test_finds_observers_by_call_chain);
	test_run(
		"DLL compares inputs with the game's build apart from the match's observers",
		test_compares_inputs_apart_from_their_observers
	);
	test_run(
		"DLL merges edits around observers the build lists differently",
		test_merges_edits_around_other_observers
	);
	test_run(
		"DLL splits a team's players by their first write", test_splits_players_by_their_first_write
	);
	test_run(
		"DLL opens inputs whose players the game lists in another order",
		test_opens_inputs_whose_players_are_in_another_order
	);
	test_run(
		"DLL pairs players with the same layout by their records",
		test_pairs_players_with_the_same_layout_by_their_records
	);
	test_run(
		"DLL rejects inputs whose players are not the game's build's",
		test_rejects_inputs_with_other_players
	);
	test_run("DLL merges edits onto the same person", test_merges_edits_onto_the_same_person);
	test_run(
		"DLL fails seeds whose build leaves out an edited player",
		test_fails_seeds_whose_build_leaves_out_an_edited_player
	);
	test_run(
		"DLL pairs players by person where the teams' parts differ",
		test_pairs_players_when_the_teams_parts_differ
	);
	test_run(
		"DLL pairs tied players by unique ID, then place",
		test_pairs_tied_players_by_unique_id_then_place
	);
	test_run("DLL pairs repeated players in order", test_pairs_repeated_players_in_order);
	test_run("DLL pairs each team's players by person", test_pairs_each_teams_players_by_person);
	test_run(
		"DLL pairs players in order when the build's cannot be split",
		test_pairs_players_in_order_when_the_build_cannot_be_split
	);
	test_run(
		"DLL opens and rebuilds inputs whose players the game reorders",
		test_opens_and_rebuilds_players_the_game_reorders
	);
	test_run(
		"DLL statistics follow players across changed line-ups",
		test_keeps_statistics_with_player_identity
	);
	test_run("DLL rejects truncated detailed-report paths", test_rejects_truncated_statistics_paths);
	test_run("DLL numbers instruction runs consistently", test_labels_instruction_runs_in_order);
	test_run(
		"DLL exports staff attributes as one named u8 each", test_labels_staff_attributes_byte_by_byte
	);
	test_run("DLL names weather, venue and attendance fields", test_labels_weather_and_venue_fields);
	test_run("DLL names the officials' ratings", test_labels_official_ratings);
	test_run("DLL names the managers' match focus", test_labels_match_focus);
	test_run("DLL names the players' tactical familiarity", test_labels_tactical_familiarity);
	test_run("DLL only exports valid UTF-8 as JSON text", test_only_merges_valid_utf8_text);
	buffer_free(&engineOutput);
}
