// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/dll/job.h"
#include "batch_plan.h"
#include "batch_policy.h"
#include "batch_run.h"
#include "batch_store.h"
#include "batch_verify.h"
#include "build_info.h"
#include "files.h"
#include "fixture_samples.h"
#include "json_reader.h"
#include "manifest.h"
#include "platform.h"
#include "sha256.h"
#include "test.h"
#include "tests.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


void batchTest_realHost(BatchHost *host);

// The commands, with the fake host below in place of the computer's.
#define commands_batch batchTest_command
#define commands_defaultThreads batchTest_defaultThreads
#define batchHost_real batchTest_realHost
#include "../src/runner/batch.c" // NOLINT(bugprone-suspicious-include): commands with a fake host.
#undef commands_batch
#undef commands_defaultThreads
#undef batchHost_real


enum {
	TEST_PATH_LENGTH = HARNESS_PATH_LENGTH,
	MILLION = 1000000,
	MOST_ATTEMPTS = 120, // a driven batch that needs more has gone wrong
	FINE_STEP_MS = 100,  // a fine clock's steps through a wait (FakeGame.fineClock)
};

static const char fakeMagic[] = "FAKEGZ1\n";

// What the fake game does to a run job (see Fault).
typedef enum FaultKind {
	FAULT_NONE,
	FAULT_CRASH,         // the game exits at the index, with half its results row written
	FAULT_CRASH_SEEN,    // the same, but the connection stays open until the watchdog sees it
	FAULT_HANG,          // the job stops at the index; the game runs on
	FAULT_SLEEP,         // the computer sleeps two hours at the index
	FAULT_BUSY,          // the harness refuses the job: FM24 is busy
	FAULT_DIE,           // the runner gets a third signal at the index; the job runs on in the game
	FAULT_STOP,          // control/stop appears at the index
	FAULT_STOP_NOW,      // control/stop-now appears at the index
	FAULT_UNSUPPORTED,   // the harness does not support the game's build
	FAULT_HARNESS_GONE,  // the harness stops serving at the index; the game runs on
	FAULT_FOREIGN,       // another runner's three jobs ran before this one
	FAULT_SEED_FAILS,    // the seed at the index fails to deserialize, stopping the job
	FAULT_RESTART,       // the game exits at the index and starts again at once, as Steam may restart
											 // it; the connection stays open until the watchdog sees it
	FAULT_CRASH_STOP,    // control/stop appears, then the game exits at the index
	FAULT_CRASH_EXITING, // the game exits at the index, its process still seen for a few checks
} FaultKind;

// A fault the fake game has, for the `job`th run job (counting from 1; 0 for any job that plays
// `index`), at `index` (UINT64_MAX: its first), at most `times` times (0: every time).
typedef struct Fault {
	FaultKind kind;
	unsigned job;
	uint64_t index;
	unsigned times;
	const char *arm; // only jobs of this arm; NULL for any
	unsigned happened;
} Fault;

// What the check of the save answers, scripted (fakeEnsureSave).
typedef struct SaveAnswer {
	int code;
	SessionOutcome outcome;
} SaveAnswer;

// A job the harness runs on after the batch stopped waiting for it, which it finishes once
// `queries` status queries have waited behind it (UINT32_MAX: never), or, unless it `hung`, once
// its stop file is there.
typedef struct Background {
	bool active;
	bool hung;
	char job[HARNESS_JOB_LENGTH + 1];
	size_t position;
	unsigned queries;
} Background;

// What a person does while batch run runs: batch stop, batch stop --now, Ctrl-C (as the host's
// handler takes it: control/stop, then control/stop-now) or quitting FM24.
typedef enum ActionKind {
	ACT_STOP = 1,
	ACT_STOP_NOW,
	ACT_SIGNAL,
	ACT_QUIT,
} ActionKind;

// An action, done once `afterMs` of time awake have passed since a job hung, or instead at the
// status query or power check with that number (counting from the fake's reset), or once the
// uptime reaches `uptime` (0: not this).
typedef struct Action {
	ActionKind kind;
	uint64_t afterMs;
	unsigned query;
	unsigned powerCheck;
	uint64_t uptime;
	uint64_t at; // the uptime it was done at (0: not yet)
} Action;

// The game and its harness, as batch run sees them through the fake host.
typedef struct FakeGame {
	bool running;
	bool listening;
	uint32_t pid;
	int64_t loaded;
	uint64_t jobs; // received since it loaded, as the report counts them
	uint32_t slice;
	uint32_t launchSlice; // the slice a launch starts without --rosetta
	const char *date;
	const char *commit;
	uint32_t build;
	unsigned launches;
	unsigned kills;
	unsigned runJobs;
	char firstRunJob[HARNESS_JOB_LENGTH + 1]; // the text of the first
	unsigned statusQueries;
	unsigned variedFrom;    // from this launch on, results differ (0: never)
	unsigned variedFromJob; // and from this run job on
	unsigned hangQueries;   // status queries a hung job takes to finish (UINT32_MAX: never)
	Background background;
	SaveAnswer answers[8];
	size_t answerCount;
	size_t answersGiven;
	BatchPower power[16];
	size_t powerCount;
	size_t powerGiven;
	Fault faults[8];
	size_t faultCount;
	uint64_t uptime; // ms
	uint64_t monotonic;
	unsigned signals;
	uint32_t exitingPid;    // a process that is exiting, its harness gone, which checks still find
	unsigned exitingChecks; // that many more times
	// The game is quit at this wait for a job it runs on (0: never), its process then found by
	// that many more checks.
	unsigned quitAfterSleeps;
	unsigned quitExitingChecks;
	unsigned keptAwake;
	bool keptAwakeOnBattery;
	bool watched;
	ByteBuffer notes;       // the notifications, one a line
	uint64_t hangStartedAt; // the uptime a job last hung at (FAULT_HANG)
	unsigned powerChecks;
	// The save checks wait this many seconds in all, a sample a second, as behind another runner's
	// job, unless their SessionStop ends the wait.
	unsigned saveWaitS;
	// Waits move the clocks in FINE_STEP_MS steps, and a job waiting for the harness ticks every
	// tickMs, as the real client does, rather than every BATCH_TICK_S.
	bool fineClock;
	Action actions[4];
	size_t actionCount;
	wchar_t actionBatch[TEST_PATH_LENGTH]; // the batch they act on
	// A season's replacement jobs so far, and those with these numbers (0: none) crash the game
	// once they have written their fixture, are refused, keep an edit, build another away override
	// than the input's, build at another in-game date, or meet control/stop.
	unsigned replaceJobs;
	unsigned replaceCrashAt;
	unsigned replaceErrorAt;
	unsigned replaceEditsAt;
	unsigned replaceOverrideAt;
	unsigned replaceDateAt;
	unsigned replaceStopAt;
	unsigned editsJob;    // the run job whose report has edits=2, as if it rebuilt with edits
	bool editsReport;     // the current run job's report has
	uint32_t jobTeams[2]; // the teams a season fixture's run job plays, which its rows name
} FakeGame;

// The computer, as the batch code sees it in these tests: compression that changes every byte,
// syncs that are only noted, a clock that stands still unless a fake game's job or a wait moves
// it, steps that can stop the "process", and the fake game.
typedef struct FakeHost {
	time_t now;
	uint64_t freeBytes;
	unsigned steps;  // reached so far
	unsigned killAt; // the step that stops the process; 0 for none
	unsigned kills;
	bool failCompress;
	bool logging;
	ByteBuffer log; // a line for each sync and step
	unsigned decompressed;
	wchar_t scratch[TEST_PATH_LENGTH]; // where decompressed copies go
	// FM24's Logs folder ("" for none), which the game's builds add to and trims empty and delete
	// from; how often they ran, and the failures each reports besides.
	wchar_t logs[TEST_PATH_LENGTH];
	unsigned trims;
	const char *trimFailures[GAME_LOGS_PROBLEMS];
	FakeGame game;
} FakeHost;

static FakeHost fake;

unsigned long batchTest_defaultThreads(void) {
	return 7;
}

static void note(const char *text) {
	if (fake.logging) {
		(void)buffer_append(&fake.log, text, strlen(text));
		(void)buffer_append(&fake.log, "\n", 1);
	}
}

static bool fakeCompress(void *context, const wchar_t *path) {
	(void)context;
	wchar_t compressed[TEST_PATH_LENGTH];
	size_t length = wcslen(path);
	ByteBuffer bytes = {0};
	ByteBuffer out = {0};
	bool ok = !fake.failCompress && length + 4 < TEST_PATH_LENGTH && files_read(path, &bytes) &&
						buffer_append(&out, fakeMagic, sizeof fakeMagic - 1);
	for (size_t i = 0; ok && i < bytes.length; ++i) {
		uint8_t byte = bytes.data[i] ^ 0x5A;
		ok = buffer_append(&out, &byte, 1);
	}
	wmemcpy(compressed, path, length);
	wmemcpy(compressed + length, L".gz", 4);
	ok = ok && files_write(compressed, out.data, out.length) && platform_removeFile(path);
	buffer_free(&bytes);
	buffer_free(&out);
	note("compress");
	return ok;
}

static FILE *fakeOpenDecompressed(void *context, const wchar_t *path, void **handle) {
	(void)context;
	ByteBuffer bytes = {0};
	wchar_t *copy = malloc(TEST_PATH_LENGTH * sizeof *copy);
	char name[32];
	(void)snprintf(name, sizeof name, "decompressed-%u", ++fake.decompressed);
	size_t magic = sizeof fakeMagic - 1;
	bool ok = copy && files_read(path, &bytes) && bytes.length >= magic &&
						memcmp(bytes.data, fakeMagic, magic) == 0;
	for (size_t i = magic; ok && i < bytes.length; ++i) {
		bytes.data[i] ^= 0x5A;
	}
	size_t folder = wcslen(fake.scratch);
	if (ok) {
		wmemcpy(copy, fake.scratch, folder);
		copy[folder] = PLATFORM_PATH_SEPARATOR;
		for (size_t i = 0; i <= strlen(name); ++i) {
			copy[folder + 1 + i] = (wchar_t)name[i];
		}
		ok = files_write(copy, bytes.data + magic, bytes.length - magic);
	}
	buffer_free(&bytes);
	FILE *file = ok ? platform_openFile(copy, "rb") : NULL;
	if (!file) {
		free(copy);
		return NULL;
	}
	*handle = copy;
	return file;
}

static bool fakeCloseDecompressed(void *context, FILE *file, void *handle) {
	(void)context;
	bool ok = fclose(file) == 0 && platform_removeFile(handle);
	free(handle);
	return ok;
}

static bool fakeSyncFile(void *context, FILE *file) {
	(void)context;
	note("sync file");
	return fflush(file) == 0;
}

static bool fakeSyncDirectory(void *context, const wchar_t *path) {
	(void)context;
	char text[3 * TEST_PATH_LENGTH + 16];
	char utf8[3 * TEST_PATH_LENGTH];
	(void)snprintf(
		text, sizeof text, "sync folder %s", platform_toUtf8(path, utf8, sizeof utf8) ? utf8 : "?"
	);
	note(text);
	return true;
}

static bool fakeFreeBytes(void *context, const wchar_t *path, uint64_t *bytes) {
	(void)context;
	(void)path;
	*bytes = fake.freeBytes;
	return true;
}

static time_t fakeNow(void *context) {
	(void)context;
	return fake.now + (time_t)(fake.game.monotonic / 1000);
}

static bool fakeReached(void *context, BatchStep step) {
	(void)context;
	char text[32];
	(void)snprintf(text, sizeof text, "step %d", (int)step);
	note(text);
	if (fake.killAt && ++fake.steps == fake.killAt) {
		++fake.kills;
		return false;
	}
	if (!fake.killAt) {
		++fake.steps;
	}
	return true;
}

static ClientOutcome fakeSubmit(
	void *context,
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t size
);
static int fakeEnsureSave(
	void *context,
	const SessionNeed *need,
	char *date,
	size_t size,
	SessionOutcome *outcome
);
static uint32_t fakeFindGame(void *context);
static bool fakeGameRunning(void *context, uint32_t pid);
static uint32_t fakeGameSlice(void *context, uint32_t pid);
static bool fakeHarnessListening(void *context);
static bool fakeStopGame(void *context, uint32_t pid, unsigned graceS);
static uint64_t fakeResidentBytes(void *context, uint32_t pid);
static BatchPower fakePowerSource(void *context);
static bool fakeKeepAwake(void *context, bool onBattery);
static void fakeNotify(void *context, const char *text);
static bool fakeWatchSignals(void *context, const wchar_t *stop, const wchar_t *stopNow);
static unsigned fakeSignals(void *context);
static uint64_t fakeUptime(void *context);
static uint64_t fakeMonotonic(void *context);
static void fakeSleep(void *context, unsigned milliseconds);
static void fakeTrimGameLogs(void *context, GameLogsTrim *out);

static BatchHost fakeHost(void) {
	return (BatchHost){
		.context = &fake,
		.compress = fakeCompress,
		.openDecompressed = fakeOpenDecompressed,
		.closeDecompressed = fakeCloseDecompressed,
		.syncFile = fakeSyncFile,
		.syncDirectory = fakeSyncDirectory,
		.freeBytes = fakeFreeBytes,
		.utcNow = fakeNow,
		.reached = fakeReached,
		.submit = fakeSubmit,
		.ensureSave = fakeEnsureSave,
		.findGame = fakeFindGame,
		.gameRunning = fakeGameRunning,
		.gameSlice = fakeGameSlice,
		.harnessListening = fakeHarnessListening,
		.stopGame = fakeStopGame,
		.residentBytes = fakeResidentBytes,
		.powerSource = fakePowerSource,
		.keepAwake = fakeKeepAwake,
		.notify = fakeNotify,
		.watchSignals = fakeWatchSignals,
		.signals = fakeSignals,
		.uptimeMs = fakeUptime,
		.monotonicMs = fakeMonotonic,
		.sleepMs = fakeSleep,
		.trimGameLogs = fakeTrimGameLogs,
	};
}

void batchTest_realHost(BatchHost *host) {
	*host = fakeHost();
}

static const char fakeCommit[] = "0123456789abcdef0123456789abcdef01234567";

static void resetFake(const wchar_t *scratch) {
	buffer_free(&fake.log);
	buffer_free(&fake.game.notes);
	fake = (FakeHost){.now = 1761383000, .freeBytes = 500ULL << 30};
	fake.game = (FakeGame){
		.date = "2024 day 123 15:00",
		.commit = fakeCommit,
		.build = 2081827,
		.hangQueries = 2,
		.uptime = 1000,
	};
	wcscpy(fake.scratch, scratch);
}

// A new, empty folder for one test, in the temporary directory.
static bool makeTestFolder(wchar_t *out) {
#ifdef _WIN32
	wchar_t directory[MAX_PATH + 1];
	DWORD length = GetTempPathW(MAX_PATH + 1, directory);
	if (!length || length > MAX_PATH) {
		return false;
	}
	static unsigned made;
	int written = swprintf(
		out,
		TEST_PATH_LENGTH,
		L"%lsfm24-batch-%lu-%llu-%u",
		directory,
		platform_processId(),
		(unsigned long long)platform_ticks(),
		++made
	);
	return written > 0 && written < TEST_PATH_LENGTH && platform_makeDirectory(out);
#else
	char made[] = "/tmp/fm24-batch-XXXXXX";
	return mkdtemp(made) && platform_fromUtf8(made, out, TEST_PATH_LENGTH);
#endif
}

static bool joinPath(wchar_t *out, const wchar_t *folder, const char *name) {
	size_t base = wcslen(folder);
	size_t length = strlen(name);
	if (base + 1 + length >= TEST_PATH_LENGTH) {
		return false;
	}
	wmemcpy(out, folder, base);
	out[base] = PLATFORM_PATH_SEPARATOR;
	for (size_t i = 0; i <= length; ++i) {
		out[base + 1 + i] = name[i] == '/' ? PLATFORM_PATH_SEPARATOR : (wchar_t)(unsigned char)name[i];
	}
	return true;
}

static bool writeText(const wchar_t *folder, const char *name, const char *text) {
	wchar_t path[TEST_PATH_LENGTH];
	return joinPath(path, folder, name) && files_write(path, text, strlen(text));
}

static bool readText(const wchar_t *folder, const char *name, ByteBuffer *out) {
	wchar_t path[TEST_PATH_LENGTH];
	return joinPath(path, folder, name) && files_read(path, out);
}

static bool exists(const wchar_t *folder, const char *name) {
	wchar_t path[TEST_PATH_LENGTH];
	FILE *file = joinPath(path, folder, name) ? platform_openFile(path, "rb") : NULL;
	if (file) {
		(void)fclose(file);
	}
	return file != NULL;
}

static unsigned occurrences(const char *text, const char *needle) {
	unsigned found = 0;
	for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
		++found;
	}
	return found;
}

// How often `needle` is in `text`, zero bytes in it or not.
static unsigned occurrencesIn(const ByteBuffer *text, const char *needle) {
	const char *data = buffer_string(text);
	unsigned found = 0;
	for (size_t at = 0; at < text->length; at += strlen(data + at) + 1) {
		found += occurrences(data + at, needle);
	}
	return found;
}

// How often `needle` is in a file of the batch.
static unsigned countIn(const wchar_t *directory, const char *name, const char *needle) {
	ByteBuffer text = {0};
	unsigned found = readText(directory, name, &text) ? occurrencesIn(&text, needle) : 0;
	buffer_free(&text);
	return found;
}

typedef struct TreeCopy {
	const wchar_t *from;
	const wchar_t *to;
	bool ok;
} TreeCopy;

static bool copyTree(const wchar_t *from, const wchar_t *to);

static bool copyEntry(void *context, const wchar_t *name, bool isDirectory) {
	TreeCopy *copy = context;
	wchar_t source[TEST_PATH_LENGTH];
	wchar_t target[TEST_PATH_LENGTH];
	ByteBuffer bytes = {0};
	copy->ok =
		copy->ok && paths_join(source, TEST_PATH_LENGTH, copy->from, name) &&
		paths_join(target, TEST_PATH_LENGTH, copy->to, name) &&
		(isDirectory ? copyTree(source, target)
								 : files_read(source, &bytes) && files_write(target, bytes.data, bytes.length));
	buffer_free(&bytes);
	return copy->ok;
}

static bool copyTree(const wchar_t *from, const wchar_t *to) {
	TreeCopy copy = {from, to, true};
	return platform_makeDirectory(to) && platform_listDirectory(from, copyEntry, &copy) && copy.ok;
}

// A fixture exported from LIV_COPY at `date` on build `build`; `value` changes its bytes.
static bool writeFixture(
	const wchar_t *folder,
	const char *name,
	const char *save,
	const char *date,
	unsigned build,
	unsigned value
) {
	char text[512];
	(void)snprintf(
		text,
		sizeof text,
		"{\"format\":\"fm24-match-config/1\",\"game_build\":%u,\"save\":\"%s\",\"game_date\":\"%s\","
		"\"away_team\":677,\"fields\":[{\"u8\":%u}]}",
		build,
		save,
		date,
		value
	);
	return writeText(folder, name, text);
}

// A source folder with two fixtures and a plan made of `settings`; the batch goes to
// <folder>/batch.
static bool writeSources(const wchar_t *folder, const char *settings) {
	char plan[2048];
	(void)snprintf(
		plan,
		sizeof plan,
		"# a test plan\n"
		"format=fm24-batch-plan/1\n"
		"name=test-batch\n"
		"arm=baseline fixtures/baseline.json   # the fixture as exported\n"
		"arm=press fixtures/press.json\n"
		"%s",
		settings
	);
	wchar_t fixtures[TEST_PATH_LENGTH];
	return joinPath(fixtures, folder, "fixtures") && platform_makeDirectory(fixtures) &&
				 writeFixture(fixtures, "baseline.json", "LIV_COPY", "2024 day 123 15:00", 2081827, 42) &&
				 writeFixture(fixtures, "press.json", "LIV_COPY", "2024 day 123 15:00", 2081827, 43) &&
				 writeText(folder, "plan.txt", plan);
}

static int create(const wchar_t *folder) {
	wchar_t plan[TEST_PATH_LENGTH];
	wchar_t batch[TEST_PATH_LENGTH];
	BatchHost host = fakeHost();
	if (!joinPath(plan, folder, "plan.txt") || !joinPath(batch, folder, "batch")) {
		return -1;
	}
	return createBatch(plan, batch, &host);
}

static const char standardSettings[] = "seeds=scatter:9:100-133\nchunk=10\nthreads=4\ncanary=3\n";

// ---------------------------------------------------------------------------------------------
// Seeds and plans

// The inverse of lowbias32, to show batchSeeds_scatter is a bijection.
static uint32_t inverseMultiply(uint32_t x, uint32_t factor) {
	uint32_t inverse = factor; // Newton's iteration for factor's inverse modulo 2^32
	for (int i = 0; i < 5; ++i) {
		inverse *= 2U - factor * inverse;
	}
	return x * inverse;
}

static uint32_t inverseLowbias32(uint32_t x) {
	x ^= x >> 16;
	x = inverseMultiply(x, 0x846ca68bU);
	x ^= (x >> 15) ^ (x >> 30);
	x = inverseMultiply(x, 0x7feb352dU);
	x ^= x >> 16;
	return x;
}

static uint32_t unscatter(uint64_t key, uint32_t seed) {
	uint64_t mixed = key + 0x9E3779B97F4A7C15ULL;
	mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
	mixed ^= mixed >> 31;
	return inverseLowbias32(inverseLowbias32(seed) ^ (uint32_t)(mixed >> 32)) ^ (uint32_t)mixed;
}

TEST(scatters_the_golden_vectors) {
	static const struct {
		uint64_t key;
		uint32_t index;
		uint32_t seed;
	} vectors[] = {
		{20261025, 0, 4210832071U},
		{20261025, 1, 3117509974U},
		{20261025, 2, 3856835949U},
		{20261025, 3, 1809968782U},
		{20261025, 4, 1239285640U},
		{20261025, 999999, 1517702698U},
		{20261025, 4294967295U, 2575468186U},
		{1, 0, 1625406257U},
		{1, 1, 1840315587U},
		{1, 2, 1295555401U},
	};
	for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
		ASSERT_BOOL_EQ(batchSeeds_scatter(vectors[i].key, vectors[i].index) == vectors[i].seed, true);
		BatchSeeds seeds = {.kind = BATCH_SEEDS_SCATTER, .key = vectors[i].key, .last = UINT32_MAX};
		ASSERT_BOOL_EQ(batchSeeds_at(&seeds, vectors[i].index) == vectors[i].seed, true);
	}
}

static int compareSeeds32(const void *a, const void *b) {
	uint32_t left = *(const uint32_t *)a;
	uint32_t right = *(const uint32_t *)b;
	return (left > right) - (left < right);
}

// Over a million indices: each seed maps back to its index, no two neighbours are adjacent seeds,
// and the next key's seeds are another set, not the same seeds in another order.
TEST(scatter_is_a_bijection_and_keys_differ) {
	uint32_t *seeds = malloc(MILLION * sizeof *seeds);
	uint32_t *next = malloc(MILLION * sizeof *next);
	ASSERT_BOOL_EQ(seeds && next, true);
	if (!seeds || !next) {
		free(seeds);
		free(next);
		return;
	}
	bool inverse = true;
	bool apart = true;
	for (uint32_t i = 0; i < MILLION; ++i) {
		seeds[i] = batchSeeds_scatter(20261025, i);
		next[i] = batchSeeds_scatter(20261026, i);
		inverse = inverse && unscatter(20261025, seeds[i]) == i && unscatter(20261026, next[i]) == i;
		apart = apart && (!i || (seeds[i] != seeds[i - 1] + 1 && seeds[i - 1] != seeds[i] + 1));
	}
	ASSERT_BOOL_EQ(inverse, true);
	ASSERT_BOOL_EQ(apart, true);
	qsort(seeds, MILLION, sizeof *seeds, compareSeeds32);
	qsort(next, MILLION, sizeof *next, compareSeeds32);
	size_t shared = 0;
	bool distinct = true;
	for (size_t i = 0, j = 0; i < MILLION && j < MILLION;) {
		distinct = distinct && (!i || seeds[i] != seeds[i - 1]);
		if (seeds[i] == next[j]) {
			++shared;
			++i;
			++j;
		} else if (seeds[i] < next[j]) {
			++i;
		} else {
			++j;
		}
	}
	ASSERT_BOOL_EQ(distinct, true);
	// About 233 by chance (the spec measured 260); the same seeds in another order would be all.
	ASSERT_BOOL_EQ(shared < 1000, true);
	free(seeds);
	free(next);
}

TEST(formats_and_hashes_seeds) {
	char text[256];
	BatchSeeds range = {.kind = BATCH_SEEDS_RANGE, .first = 1, .last = 1000};
	ASSERT_BOOL_EQ(batchSeeds_format(&range, 37, 999, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "37-999"), 0);
	ASSERT_BOOL_EQ(batchSeeds_format(&range, 5, 5, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "5"), 0);
	ASSERT_BOOL_EQ(batchSeeds_at(&range, 77) == 77, true);
	BatchSeeds scatter = {.kind = BATCH_SEEDS_SCATTER, .key = 20261025, .last = 999999};
	ASSERT_BOOL_EQ(batchSeeds_format(&scatter, 0, 2, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "4210832071,3117509974,3856835949"), 0);
	ASSERT_BOOL_EQ(batchSeeds_format(&scatter, 0, 2, text, 31), false);
	static const uint32_t list[] = {9, 4, 7};
	BatchSeeds listed = {.kind = BATCH_SEEDS_LIST, .list = list, .last = 2};
	ASSERT_BOOL_EQ(batchSeeds_format(&listed, 1, 2, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "4,7"), 0);
	ASSERT_INT_EQ((int)batchSeeds_count(&listed), 3);
	char hash[SHA256_HEX_LENGTH];
	char expected[SHA256_HEX_LENGTH];
	batchSeeds_hash(&listed, 0, 2, hash);
	sha256_hexOf("9\n4\n7\n", 6, expected);
	ASSERT_INT_EQ(strcmp(hash, expected), 0);
}

TEST(reads_seed_lists) {
	uint32_t *seeds = NULL;
	size_t found = 0;
	char error[128];
	static const char good[] = "4294967295\r\n0\n17";
	ASSERT_BOOL_EQ(
		batchSeeds_readList(good, sizeof good - 1, &seeds, &found, error, sizeof error), true
	);
	ASSERT_BOOL_EQ(
		found == 3 && seeds && seeds[0] == UINT32_MAX && seeds[1] == 0 && seeds[2] == 17, true
	);
	free(seeds);
	static const char *const bad[] = {
		"", "1\n\n2\n", "1\n1\n", "4294967296\n", "12a\n", "-1\n", " 3\n"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		ASSERT_BOOL_EQ(
			batchSeeds_readList(bad[i], strlen(bad[i]), &seeds, &found, error, sizeof error), false
		);
		ASSERT_BOOL_EQ(seeds == NULL && found == 0, true);
	}
	ASSERT_BOOL_EQ(batchSeeds_readList("5\n6\n5\n", 6, &seeds, &found, error, sizeof error), false);
	ASSERT_BOOL_EQ(strstr(error, "seed 5 is listed more than once") != NULL, true);
}

static const BatchDefaults testDefaults = {7, BATCH_COMPRESS_GZIP};

static bool parsePlan(const char *text, bool frozen, BatchPlan *plan, char *error, size_t size) {
	return batchPlan_parse(text, strlen(text), frozen, &testDefaults, plan, error, size);
}

TEST(reads_a_plan_with_its_defaults) {
	static BatchPlan plan;
	char error[256];
	static const char text[] =
		"# comments, blank lines and spaces are fine\n"
		"\n"
		"format=fm24-batch-plan/1\n"
		"name=liv-ars-2026-10\n"
		"arm=baseline fixtures/next json#1.json    # a path may have # inside a word\n"
		"arm=gegenpress   /abs/next-gegenpress.json\n"
		"seeds = scatter:20261025:0-999999\r\n"
		"max_seed_errors=0.25%\n"
		"save=/Users/you/fm24-sandbox/LIV_COPY.fm\n";
	ASSERT_BOOL_EQ(parsePlan(text, false, &plan, error, sizeof error), true);
	ASSERT_INT_EQ(strcmp(plan.name, "liv-ars-2026-10"), 0);
	ASSERT_INT_EQ((int)plan.armCount, 2);
	ASSERT_INT_EQ(strcmp(plan.arms[0].path, "fixtures/next json#1.json"), 0);
	ASSERT_INT_EQ(strcmp(plan.arms[1].id, "gegenpress"), 0);
	ASSERT_INT_EQ(strcmp(plan.arms[1].path, "/abs/next-gegenpress.json"), 0);
	ASSERT_BOOL_EQ(plan.seeds.kind == BATCH_SEEDS_SCATTER && plan.seeds.key == 20261025, true);
	ASSERT_BOOL_EQ(plan.seeds.first == 0 && plan.seeds.last == 999999, true);
	ASSERT_BOOL_EQ(plan.chunk == 1000 && plan.threads == 7 && plan.perMatch && !plan.keepBuild, true);
	ASSERT_BOOL_EQ(
		plan.seedErrors == BATCH_SEED_ERRORS_SKIP && plan.compress == BATCH_COMPRESS_GZIP, true
	);
	ASSERT_BOOL_EQ(plan.maxSeedErrors.percent && plan.maxSeedErrors.value == 250, true);
	ASSERT_BOOL_EQ(
		plan.order == BATCH_ORDER_INTERLEAVE && plan.warmup == 1 && plan.canary == 3, true
	);
	ASSERT_BOOL_EQ(plan.canaryEvery == 50 && plan.onCanaryMismatch == BATCH_MISMATCH_STOP, true);
	ASSERT_BOOL_EQ(
		plan.slice == BATCH_SLICE_ARM64 && plan.launch && !plan.anySave && !plan.killGame, true
	);
	ASSERT_BOOL_EQ(
		plan.hangMinutes == 10 && plan.queueWaitMinutes == 360 && plan.busyWaitMinutes == 15, true
	);
	ASSERT_BOOL_EQ(plan.maxCrashesPerHour == 4 && plan.maxPoison == 3 && plan.pauseOnBattery, true);
	ASSERT_BOOL_EQ(plan.minFreeGb == 20 && plan.notify && plan.trimGameLogs, true);
	ASSERT_INT_EQ(strcmp(plan.save, "/Users/you/fm24-sandbox/LIV_COPY.fm"), 0);
	ASSERT_BOOL_EQ(batchPlan_chunksPerArm(&plan) == 1000, true);
	static const char other[] =
		"format=fm24-batch-plan/1\nname=b\narm=a a.json\nseeds=range:1-1000\nchunk=7\nthreads=16\n"
		"per_match=0\nseed_errors=stop\nmax_seed_errors=12\norder=sequential\ncompress=none\n"
		"warmup=0\ncanary=0\ncanary_every=1\non_canary_mismatch=continue\nslice=x86_64\nlaunch=0\n"
		"any_save=1\nkill_game=1\nhang_minutes=1\nqueue_wait_minutes=10080\nbusy_wait_minutes=1440\n"
		"max_crashes_per_hour=0\nmax_poison=0\npause_on_battery=0\nmin_free_gb=0\nnotify=0\n"
		"human_control=1\nkeep_build=1\ntrim_game_logs=0\n";
	ASSERT_BOOL_EQ(parsePlan(other, false, &plan, error, sizeof error), true);
	ASSERT_BOOL_EQ(
		plan.seeds.kind == BATCH_SEEDS_RANGE && plan.seeds.first == 1 && plan.seeds.last == 1000, true
	);
	ASSERT_BOOL_EQ(plan.chunk == 7 && plan.threads == 16 && !plan.perMatch, true);
	ASSERT_BOOL_EQ(
		plan.seedErrors == BATCH_SEED_ERRORS_STOP && !plan.maxSeedErrors.percent &&
			plan.maxSeedErrors.value == 12,
		true
	);
	ASSERT_BOOL_EQ(
		plan.order == BATCH_ORDER_SEQUENTIAL && plan.compress == BATCH_COMPRESS_NONE, true
	);
	ASSERT_BOOL_EQ(!plan.warmup && !plan.canary && plan.canaryEvery == 1, true);
	ASSERT_BOOL_EQ(
		plan.onCanaryMismatch == BATCH_MISMATCH_CONTINUE && plan.slice == BATCH_SLICE_X86_64, true
	);
	ASSERT_BOOL_EQ(!plan.launch && plan.anySave && plan.killGame && plan.hangMinutes == 1, true);
	ASSERT_BOOL_EQ(plan.queueWaitMinutes == 10080 && plan.busyWaitMinutes == 1440, true);
	ASSERT_BOOL_EQ(!plan.maxCrashesPerHour && !plan.maxPoison && !plan.pauseOnBattery, true);
	ASSERT_BOOL_EQ(!plan.minFreeGb && !plan.notify && plan.humanControl && plan.keepBuild, true);
	ASSERT_BOOL_EQ(!plan.trimGameLogs, true);
	ASSERT_BOOL_EQ(batchPlan_chunksPerArm(&plan) == 143, true);
}

TEST(refuses_bad_plans) {
	static BatchPlan plan;
	static const char head[] = "format=fm24-batch-plan/1\nname=b\narm=a a.json\n";
	static const struct {
		const char *text;  // after `head`, or the whole plan if it starts with "!"
		const char *error; // part of the message
	} cases[] = {
		{"!name=b\nformat=fm24-batch-plan/1\n", "a plan starts with format="},
		{"!format=fm24-batch-plan/2\n", "the format must be"},
		{"!format=fm24-batch-plan/1\narm=a a.json\nseeds=range:1-2\n", "the plan has no name="},
		{"!format=fm24-batch-plan/1\nname=b\nseeds=range:1-2\n", "the plan has no arm="},
		{"chunk=10\n", "the plan has no seeds="},
		{"seeds=range:1-2\nseeds=range:1-2\n", "seeds is given twice"},
		{"seeds=range:1-2\nchunk=1\nchunk=2\n", "chunk is given twice"},
		{"seeds=range:1-2\nchunk=0\n", "chunk must be 1-1000000"},
		{"seeds=range:1-2\nchunk=1000001\n", "chunk must be 1-1000000"},
		{"seeds=range:1-2\nthreads=17\n", "threads must be 1-16"},
		{"seeds=range:1-2\nthreads=x\n", "threads must be 1-16"},
		{"seeds=range:1-2\nper_match=2\n", "per_match must be 0 or 1"},
		{"seeds=range:1-2\nseed_errors=ignore\n", "seed_errors must be stop or skip"},
		{"seeds=range:1-2\norder=random\n", "order must be interleave or sequential"},
		{"seeds=range:1-2\ncompress=zlib\n", "compress must be none or gzip"},
		{"seeds=range:1-2\nslice=ppc\n", "slice must be arm64 or x86_64"},
		{"seeds=range:1-2\nhang_minutes=0\n", "hang_minutes must be 1-1440"},
		{"seeds=range:1-2\ntrim_game_logs=yes\n", "trim_game_logs must be 0 or 1"},
		{"seeds=range:1-2\ntrim_game_logs=0\ntrim_game_logs=1\n", "trim_game_logs is given twice"},
		{"seeds=range:1-2\nmax_seed_errors=101%\n", "max_seed_errors is a count"},
		{"seeds=range:1-2\nmax_seed_errors=1.2345%\n", "max_seed_errors is a count"},
		{"seeds=range:1-2\nmax_seed_errors=%\n", "max_seed_errors is a count"},
		{"seeds=range:1-2\ndetailed=1\n", "remove detailed="},
		{"seeds=range:1-2\nunknown=1\n", "unknown key unknown"},
		{"seeds=range:1-2\nno equals sign\n", "expected key=value"},
		{"seeds=range:2-1\n", "first <= last"},
		{"seeds=range:1-4294967296\n", "seeds must be"},
		{"seeds=scatter:18446744073709551616:0-9\n", "seeds must be"},
		{"seeds=scatter:9:0\n", "seeds must be"},
		{"seeds=random:1-2\n", "seeds must be"},
		{"seeds=list:\n", "list: needs"},
		{"seeds=range:1-2\narm=a b.json\n", "arm a is given twice"},
		{"seeds=range:1-2\narm=A b.json\n", "differ in more than case"},
		{"seeds=range:1-2\narm=b b.bin\n", "needs a JSON fixture"},
		{"seeds=range:1-2\narm=b\n", "arm=<id> <fixture.json>"},
		{"seeds=range:1-2\narm=b/c c.json\n", "an arm's ID must be"},
		{"seeds=range:1-2\narm=abcdefghijklmnopqrstuvwxyz0123456 c.json\n", "an arm's ID must be"},
		{"seeds=range:1-2\nfixture=a json_sha256=x\n", "unknown key fixture"},
		{"seeds=range:1-2\ngame_build=1\n", "only in a batch's plan.txt"},
		{"seeds=range:1-2\nsave_name=x\n", "only in a batch's plan.txt"},
		{"seeds=range:1-2\nplan_sha256=0\n", "unknown key plan_sha256"},
		{"!format=fm24-batch-plan/1\nname=bad name\n", "the name must be"},
	};
	char text[1024];
	char error[256];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		const char *body = cases[i].text;
		(void)snprintf(
			text, sizeof text, "%s%s", body[0] == '!' ? "" : head, body[0] == '!' ? body + 1 : body
		);
		error[0] = 0;
		bool parsed = parsePlan(text, false, &plan, error, sizeof error);
		bool named = strstr(error, cases[i].error) != NULL;
		if (parsed || !named) {
			fprintf(stderr, "case %zu: %s\n", i, error);
		}
		ASSERT_BOOL_EQ(parsed, false);
		ASSERT_BOOL_EQ(named, true);
	}
	char many[2048];
	size_t length =
		(size_t)snprintf(many, sizeof many, "format=fm24-batch-plan/1\nname=b\nseeds=range:1-2\n");
	for (int arm = 0; arm < 17; ++arm) {
		length += (size_t)snprintf(many + length, sizeof many - length, "arm=a%d a.json\n", arm);
	}
	ASSERT_BOOL_EQ(parsePlan(many, false, &plan, error, sizeof error), false);
	ASSERT_BOOL_EQ(strstr(error, "at most 16 arms") != NULL, true);
	ASSERT_BOOL_EQ(strstr(error, "line 20:") == error, true);
}

TEST(freezes_a_plan_that_reads_back) {
	static BatchPlan plan;
	static BatchPlan again;
	char error[256];
	static const char text[] = "format=fm24-batch-plan/1\nname=n\narm=a /x/a.json\narm=b /x/b.json\n"
														 "seeds=list:/x/seeds.txt\nchunk=5\nmax_seed_errors=3\nsave=LIV_COPY\n";
	ASSERT_BOOL_EQ(parsePlan(text, false, &plan, error, sizeof error), true);
	memset(plan.arms[0].jsonSha256, 'a', SHA256_HEX_LENGTH - 1);
	memset(plan.arms[0].binSha256, 'b', SHA256_HEX_LENGTH - 1);
	memset(plan.arms[1].jsonSha256, 'c', SHA256_HEX_LENGTH - 1);
	memset(plan.arms[1].binSha256, 'd', SHA256_HEX_LENGTH - 1);
	memset(plan.listSha256, 'e', SHA256_HEX_LENGTH - 1);
	plan.arms[1].teams[0] = 678;
	plan.gameBuild = 2081827;
	strcpy(plan.saveName, "LIV_COPY");
	strcpy(plan.gameDate, "2024 day 123 15:00");
	ByteBuffer frozen = {0};
	ByteBuffer twice = {0};
	ASSERT_BOOL_EQ(batchPlan_format(&plan, &frozen), true);
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&frozen), frozen.length, true, &testDefaults, &again, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(batchPlan_format(&again, &twice), true);
	ASSERT_BOOL_EQ(
		frozen.length == twice.length && memcmp(frozen.data, twice.data, frozen.length) == 0, true
	);
	ASSERT_INT_EQ(strcmp(again.planSha256, plan.planSha256), 0);
	ASSERT_BOOL_EQ(again.arms[1].teams[0] == 678 && again.arms[1].teams[1] == 0, true);
	ASSERT_BOOL_EQ(
		again.gameBuild == 2081827 && strcmp(again.gameDate, "2024 day 123 15:00") == 0, true
	);
	ASSERT_BOOL_EQ(
		again.chunk == 5 && !again.maxSeedErrors.percent && again.maxSeedErrors.value == 3, true
	);
	ASSERT_BOOL_EQ(again.compress == BATCH_COMPRESS_GZIP && again.threads == 7, true);
	// Every setting is spelled out, and the last line is the hash of the lines above it.
	ASSERT_BOOL_EQ(strstr(buffer_string(&frozen), "\nthreads=7\n") != NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&frozen), "\nnotify=1\n") != NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&frozen), "\ntrim_game_logs=1\n") != NULL, true);
	ASSERT_BOOL_EQ(again.trimGameLogs == 1, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&frozen), "\nfixture=b json_sha256=cccc") != NULL, true);
	char expected[SHA256_HEX_LENGTH];
	size_t body = frozen.length - (sizeof "plan_sha256=" - 1) - SHA256_HEX_LENGTH;
	sha256_hexOf(frozen.data, body, expected);
	ASSERT_INT_EQ(strcmp(expected, plan.planSha256), 0);
	// Any change shows; so does a frozen plan's line in a plan to create from.
	frozen.data[10] ^= 1;
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&frozen), frozen.length, true, &testDefaults, &again, error, sizeof error
		),
		false
	);
	ASSERT_BOOL_EQ(strstr(error, "plan.txt has been changed") != NULL, true);
	frozen.data[10] ^= 1;
	ASSERT_BOOL_EQ(
		batchPlan_parse(buffer_string(&frozen), body, true, &testDefaults, &again, error, sizeof error),
		false
	);
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&frozen), frozen.length, false, &testDefaults, &again, error, sizeof error
		),
		false
	);
	// trim_game_logs=0 is frozen as it is.
	ByteBuffer off = {0};
	plan.trimGameLogs = 0;
	ASSERT_BOOL_EQ(batchPlan_format(&plan, &off), true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&off), "\ntrim_game_logs=0\n") != NULL, true);
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&off), off.length, true, &testDefaults, &again, error, sizeof error
		) && !again.trimGameLogs,
		true
	);
	buffer_free(&off);
	buffer_free(&frozen);
	buffer_free(&twice);
}

TEST(formats_error_limits) {
	static const char *const limits[] = {"1%", "0.5%", "0.125%", "100%", "0%", "250", "0"};
	static BatchPlan plan;
	char error[128];
	for (size_t i = 0; i < sizeof limits / sizeof limits[0]; ++i) {
		char text[256];
		char formatted[32];
		(void)snprintf(
			text,
			sizeof text,
			"format=fm24-batch-plan/1\nname=b\narm=a a.json\nseeds=range:1-2\nmax_seed_errors=%s\n",
			limits[i]
		);
		ASSERT_BOOL_EQ(parsePlan(text, false, &plan, error, sizeof error), true);
		batchPlan_formatLimit(&plan.maxSeedErrors, formatted, sizeof formatted);
		ASSERT_INT_EQ(strcmp(formatted, limits[i]), 0);
	}
}

TEST(enumerates_chunks_in_both_orders) {
	static BatchPlan plan;
	char error[128];
	ASSERT_BOOL_EQ(
		parsePlan(
			"format=fm24-batch-plan/1\nname=b\narm=a a.json\narm=b b.json\narm=c "
			"c.json\nseeds=scatter:1:37000-37033\nchunk=10\n",
			false,
			&plan,
			error,
			sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(batchPlan_chunksPerArm(&plan) == 4, true);
	uint64_t first = 0;
	uint64_t last = 0;
	batchPlan_chunkRange(&plan, 0, &first, &last);
	ASSERT_BOOL_EQ(first == 37000 && last == 37009, true);
	batchPlan_chunkRange(&plan, 3, &first, &last);
	ASSERT_BOOL_EQ(first == 37030 && last == 37033, true);
	uint64_t chunk = 0;
	ASSERT_BOOL_EQ(batchPlan_chunkAt(&plan, 37020, &chunk) && chunk == 2, true);
	ASSERT_BOOL_EQ(batchPlan_chunkAt(&plan, 37021, &chunk), false);
	ASSERT_BOOL_EQ(batchPlan_chunkAt(&plan, 36990, &chunk), false);
	ASSERT_BOOL_EQ(batchPlan_chunkAt(&plan, 37040, &chunk), false);
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(37000, id);
	ASSERT_INT_EQ(strcmp(id, "0000037000"), 0);
	char interleaved[64] = "";
	char sequential[64] = "";
	for (uint64_t position = 0; position < 12; ++position) {
		size_t arm = 0;
		batchPlan_scheduled(&plan, position, &arm, &chunk);
		(void)snprintf(
			interleaved + strlen(interleaved), 8, "%c%llu ", 'a' + (int)arm, (unsigned long long)chunk
		);
	}
	plan.order = BATCH_ORDER_SEQUENTIAL;
	for (uint64_t position = 0; position < 12; ++position) {
		size_t arm = 0;
		batchPlan_scheduled(&plan, position, &arm, &chunk);
		(void)snprintf(
			sequential + strlen(sequential), 8, "%c%llu ", 'a' + (int)arm, (unsigned long long)chunk
		);
	}
	ASSERT_INT_EQ(strcmp(interleaved, "a0 b0 c0 a1 b1 c1 a2 b2 c2 a3 b3 c3 "), 0);
	ASSERT_INT_EQ(strcmp(sequential, "a0 a1 a2 a3 b0 b1 b2 b3 c0 c1 c2 c3 "), 0);
}

TEST(formats_part_jobs_and_caps_their_size) {
	static BatchPlan plan;
	char error[128];
	ASSERT_BOOL_EQ(
		parsePlan(
			"format=fm24-batch-plan/1\nname=b\narm=baseline "
			"a.json\nseeds=scatter:20261025:0-999999\nthreads=4\n",
			false,
			&plan,
			error,
			sizeof error
		),
		true
	);
	plan.arms[0].teams[1] = 677;
	BatchPartPaths paths;
	static char job[HARNESS_JOB_LENGTH];
	ASSERT_BOOL_EQ(batchPlan_partPaths("/b", "baseline", 37000, 2, &paths), true);
	ASSERT_BOOL_EQ(batchPlan_formatJob(&plan, 0, &paths, "1,2", 37412, 4, job, sizeof job), true);
#ifndef _WIN32
	static const char expected[] =
		"command=run\ninput=/b/inputs/baseline.bin\nseeds=1,2\n"
		"output=/b/attempts/baseline/0000037000/a2/results.csv\nthreads=4\nprofile=\ndetailed=0\n"
		"human_control=0\nkeep_build=0\nhome_team=\naway_team=677\nprogress_window=0\nper_match=1\n"
		"index_base=37412\nseed_errors=skip\nstop_file=/b/control/stop-now\nreport=1\n";
	ASSERT_INT_EQ(strcmp(job, expected), 0);
#endif
	// The cap: with paths of about 120 bytes, about 1,430 scattered seeds fit.
	static const char directory[] =
		"/Users/you/fm24-batches/liv-ars-2026-10-with-a-rather-long-name-for-its-folder-to-check-cap";
	uint32_t largest = batchPlan_largestChunk(&plan, directory);
	ASSERT_BOOL_EQ(largest > 1400 && largest < 1460, true);
	ASSERT_BOOL_EQ(batchPlan_longestJob(&plan, directory, largest) < HARNESS_JOB_LENGTH, true);
	ASSERT_BOOL_EQ(batchPlan_longestJob(&plan, directory, largest + 1) >= HARNESS_JOB_LENGTH, true);
	// The longest real job of that chunk size fits: the largest seeds, attempt and index.
	static char seeds[HARNESS_JOB_LENGTH];
	size_t length = 0;
	for (uint32_t i = 0; i < largest; ++i) {
		length += (size_t)snprintf(seeds + length, sizeof seeds - length, "%s4294967295", i ? "," : "");
	}
	ASSERT_BOOL_EQ(
		batchPlan_partPaths(directory, "baseline", 4294967295ULL, UINT32_MAX, &paths), true
	);
	ASSERT_BOOL_EQ(batchPlan_formatJob(&plan, 0, &paths, seeds, 999999, 16, job, sizeof job), true);
	ASSERT_BOOL_EQ(strlen(job) <= batchPlan_longestJob(&plan, directory, largest), true);
	plan.seeds.kind = BATCH_SEEDS_RANGE;
	ASSERT_BOOL_EQ(batchPlan_largestChunk(&plan, directory) == BATCH_CHUNK_LIMIT, true);
}

// ---------------------------------------------------------------------------------------------
// The verifier

typedef struct PartFiles {
	ByteBuffer results;
	ByteBuffer players;
	ByteBuffer teams;
	ByteBuffer failed;
} PartFiles;

static void appendText(ByteBuffer *out, const char *text) {
	(void)buffer_append(out, text, strlen(text));
}

static void appendf(ByteBuffer *out, const char *format, ...) PLATFORM_PRINTF(2, 3);
static void appendf(ByteBuffer *out, const char *format, ...) {
	char text[4096];
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(text, sizeof text, format, arguments);
	va_end(arguments);
	if (written > 0) {
		(void)buffer_append(out, text, (size_t)written);
	}
}

static bool isRebuildFailure(uint32_t seed) {
	return seed % 11 == 5;
}

// The results row the fake harness writes for a seed.
static void appendResult(ByteBuffer *out, uint32_t seed) {
	appendf(
		out,
		"%u,%u,%u,%u,%u,%016llx,%.1f\n",
		seed,
		seed % 5,
		seed / 5 % 4,
		21600 + seed % 7,
		seed % 13 ? 1U : 0U,
		(unsigned long long)seed * 0x9E3779B97F4A7C15ULL,
		2000.0 + (double)(seed % 1000) / 10.0
	);
}

// A small hand-made player row: slot `slot` of a squad, appeared or not.
static void appendPlayer(
	ByteBuffer *out,
	uint64_t index,
	uint32_t seed,
	const char *side,
	unsigned slot,
	bool appeared
) {
	appendf(
		out,
		"%llu,%u,%s,77,%u,%u,1,%d,%d,%d,0,0,0,0,0",
		(unsigned long long)index,
		seed,
		side,
		slot,
		1000 + slot,
		appeared,
		appeared,
		appeared ? 90 : 0
	);
	appendText(out, appeared ? ",7.12" : ",");
	for (int i = 0; i < 46; ++i) {
		const char *value = i == 6 ? ",0.412337601" : i == 45 ? ",10.4123301" : ",1";
		appendText(out, appeared ? value : ",");
	}
	appendText(out, "\n");
}

static void
appendTeam(ByteBuffer *out, uint64_t index, uint32_t seed, const char *side, unsigned squad) {
	appendf(out, "%llu,%u,%s,77,%u,%u", (unsigned long long)index, seed, side, squad, squad);
	for (int i = 0; i < 38; ++i) {
		appendText(out, i == 2 ? ",-0.05" : ",3");
	}
	appendText(out, "\n");
}

// A part's files for indices [first, first + count), with squads of 2 and 1.
static void makePart(PartFiles *files, const BatchSeeds *seeds, uint64_t first, uint64_t count) {
	*files = (PartFiles){0};
	appendText(&files->results, batchVerify_partResultsHeader);
	appendText(&files->players, batchVerify_playersHeader);
	appendText(&files->teams, batchVerify_teamsHeader);
	appendText(&files->failed, batchVerify_failedHeader);
	for (uint64_t index = first; index < first + count; ++index) {
		uint32_t seed = batchSeeds_at(seeds, index);
		if (isRebuildFailure(seed)) {
			appendf(&files->failed, "%llu,%u,rebuild\n", (unsigned long long)index, seed);
			continue;
		}
		appendTeam(&files->teams, index, seed, "home", 2);
		appendPlayer(&files->players, index, seed, "home", 0, true);
		appendPlayer(&files->players, index, seed, "home", 1, false);
		appendTeam(&files->teams, index, seed, "away", 1);
		appendPlayer(&files->players, index, seed, "away", 0, true);
		appendResult(&files->results, seed);
	}
}

static void freePart(PartFiles *files) {
	buffer_free(&files->results);
	buffer_free(&files->players);
	buffer_free(&files->teams);
	buffer_free(&files->failed);
}

// Writes the part's files to `folder` and verifies them (merging into `outputs` if given).
// Whether verifyPart's parts have per-match rows, and the teams their rows must name (0: any).
static bool partPerMatch = true;
static uint32_t partTeams[2];

static bool verifyPart(
	const wchar_t *folder,
	const PartFiles *files,
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t count,
	BatchSink *outputs,
	BatchVerified *out
) {
	static const char *const names[BATCH_ROLES] = {"r.csv", "p.csv", "t.csv", "f.csv"};
	const ByteBuffer *texts[BATCH_ROLES] = {
		&files->results, &files->players, &files->teams, &files->failed};
	BatchVerify request = {.seeds = seeds,
		.first = first,
		.count = count,
		.perMatch = partPerMatch,
		.failed = true,
		.outputs = outputs,
		.teams = {partTeams[0], partTeams[1]}};
	bool ok = true;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		wchar_t path[TEST_PATH_LENGTH];
		ok = ok && joinPath(path, folder, names[role]) &&
				 files_write(path, texts[role]->data, texts[role]->length);
		request.inputs[role] = ok ? platform_openFile(path, "rb") : NULL;
	}
	ok = ok && batchVerify_run(&request, out);
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		if (request.inputs[role]) {
			(void)fclose(request.inputs[role]);
		}
	}
	return ok;
}

static uint64_t verifiedCount(
	const wchar_t *folder,
	const PartFiles *files,
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t count
) {
	BatchVerified verified;
	return verifyPart(folder, files, seeds, first, count, NULL, &verified) ? verified.verified
																																				 : UINT64_MAX;
}

// Cuts a file after the last occurrence of `needle` plus `keep` bytes.
static void cutAfter(ByteBuffer *file, const char *needle, size_t keep) {
	const char *text = buffer_string(file);
	const char *last = NULL;
	for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
		last = at;
	}
	if (last) {
		file->length = (size_t)(last - text) + keep;
		file->data[file->length] = 0;
	}
}

// The line index `line` (0 is the header) of a file is replaced by `text`.
static void replaceLine(ByteBuffer *file, size_t line, const char *text) {
	ByteBuffer edited = {0};
	const char *at = buffer_string(file);
	for (size_t i = 0; *at; ++i) {
		const char *end = strchr(at, '\n');
		size_t length = end ? (size_t)(end - at) + 1 : strlen(at);
		if (i == line) {
			appendText(&edited, text);
		} else {
			(void)buffer_append(&edited, at, length);
		}
		at += length;
	}
	buffer_free(file);
	*file = edited;
}

TEST(verifies_parts_and_finds_their_consistent_prefix) {
	wchar_t folder[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	BatchSeeds seeds = {.kind = BATCH_SEEDS_SCATTER, .key = 9, .first = 0, .last = 999};
	// Indices 40-59, of which the failed ones are rebuild failures.
	uint64_t failedIndex = 0;
	unsigned failures = 0;
	for (uint64_t index = 40; index < 60; ++index) {
		if (isRebuildFailure(batchSeeds_at(&seeds, index))) {
			failedIndex = failedIndex ? failedIndex : index;
			++failures;
		}
	}
	ASSERT_BOOL_EQ(failures >= 1 && failedIndex > 40 && failedIndex < 58, true);
	PartFiles files;
	makePart(&files, &seeds, 40, 20);
	BatchVerified verified;
	ASSERT_BOOL_EQ(verifyPart(folder, &files, &seeds, 40, 20, NULL, &verified), true);
	ASSERT_BOOL_EQ(verified.verified == 20, true);
	ASSERT_BOOL_EQ(
		verified.rows[BATCH_FAILED] == failures && verified.rows[BATCH_RESULTS] == 20 - failures, true
	);
	ASSERT_BOOL_EQ(
		verified.rows[BATCH_PLAYERS] == 3 * (20 - failures) &&
			verified.rows[BATCH_TEAMS] == 2 * (20 - failures),
		true
	);
	ASSERT_BOOL_EQ(verified.summary.matches == 20 - failures, true);
	// Fewer asked for: only those.
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 5) == 5, true);
	// The files of indices from 40 do not verify from 39 or 41.
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 41, 19) == 0, true);
	freePart(&files);

	// A last results line without its newline, though its fields read, is torn; so is one with a
	// zero-filled tail after it.
	makePart(&files, &seeds, 40, 20);
	files.results.length -= 1;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 19, true);
	static const uint8_t zeros[64] = {0};
	(void)buffer_append(&files.results, zeros, sizeof zeros);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 19, true);
	freePart(&files);
	// So is a last player row cut inside its last value, which still has 62 fields.
	makePart(&files, &seeds, 40, 20);
	ASSERT_BOOL_EQ(memcmp(files.players.data + files.players.length - 5, "3301\n", 5) == 0, true);
	files.players.length -= 2;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 19, true);
	freePart(&files);
	// A complete line of zeros among good rows ends the prefix there: at the third played index.
	uint64_t third = 40;
	for (unsigned played = 0; played < 3; ++third) {
		played += !isRebuildFailure(batchSeeds_at(&seeds, third));
	}
	makePart(&files, &seeds, 40, 20);
	replaceLine(&files.results, 3, "@@@\n");
	for (size_t i = 0; i + 3 < files.results.length; ++i) {
		if (memcmp(files.results.data + i, "@@@\n", 4) == 0) {
			memset(files.results.data + i, 0, 3);
		}
	}
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == third - 1 - 40, true);
	freePart(&files);

	// Per-match rows past the last results row (phase one of a write the game did not finish).
	PartFiles longer;
	makePart(&files, &seeds, 40, 10);
	makePart(&longer, &seeds, 40, 12);
	buffer_free(&longer.results);
	longer.results = files.results;
	files.results = (ByteBuffer){0};
	ASSERT_BOOL_EQ(verifiedCount(folder, &longer, &seeds, 40, 12) == 10, true);
	freePart(&files);
	freePart(&longer);

	// A missing team row or player row stops at its index.
	makePart(&files, &seeds, 40, 20);
	cutAfter(&files.teams, ",away,", 0);
	uint64_t lastPlayed = 59;
	while (isRebuildFailure(batchSeeds_at(&seeds, lastPlayed))) {
		--lastPlayed;
	}
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == lastPlayed - 40, true);
	freePart(&files);
	makePart(&files, &seeds, 40, 20);
	cutAfter(&files.players, ",away,", 0);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == lastPlayed - 40, true);
	freePart(&files);

	// A failed row missing: its index has no results row, so the next row's seed does not match.
	makePart(&files, &seeds, 40, 20);
	buffer_clear(&files.failed);
	appendText(&files.failed, batchVerify_failedHeader);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == failedIndex - 40, true);
	freePart(&files);
	// Without per-match rows, only the results row's seed shows the failed row is missing.
	partPerMatch = false;
	makePart(&files, &seeds, 40, 20);
	buffer_clear(&files.failed);
	appendText(&files.failed, batchVerify_failedHeader);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == failedIndex - 40, true);
	freePart(&files);
	partPerMatch = true;
	// A failed row with the wrong seed, or out of order, ends the failed rows.
	makePart(&files, &seeds, 40, 20);
	buffer_clear(&files.failed);
	appendText(&files.failed, batchVerify_failedHeader);
	appendf(
		&files.failed,
		"%llu,%u,rebuild\n",
		(unsigned long long)failedIndex,
		batchSeeds_at(&seeds, failedIndex) + 1
	);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == failedIndex - 40, true);
	freePart(&files);

	// Headers that are not the files' end everything.
	makePart(&files, &seeds, 40, 20);
	replaceLine(&files.teams, 0, "index,seed\n");
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 0, true);
	freePart(&files);
	makePart(&files, &seeds, 40, 20);
	replaceLine(&files.results, 0, batchVerify_chunkResultsHeader);
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 0, true);
	freePart(&files);

	// Rows that do not agree: the wrong slot, side, team, field count or values.
	static const char *const breaks[][2] = {
		{",home,77,1,", ",home,77,2,"},           // a slot out of order
		{",away,77,0,", ",home,77,0,"},           // the wrong side
		{",home,77,0,1000,", ",home,78,0,1000,"}, // another team's player
		{",7.12,", ",7.1,"},                      // a rating with one decimal
		{",0.412337601,", ",,"},                  // an empty value of a player who appeared
	};
	for (size_t i = 0; i < sizeof breaks / sizeof breaks[0]; ++i) {
		makePart(&files, &seeds, 40, 20);
		char *at = strstr((char *)files.players.data, breaks[i][0]);
		ASSERT_BOOL_EQ(at != NULL, true);
		if (at) {
			ByteBuffer edited = {0};
			(void)buffer_append(
				&edited, files.players.data, (size_t)((uint8_t *)at - files.players.data)
			);
			appendText(&edited, breaks[i][1]);
			appendText(&edited, at + strlen(breaks[i][0]));
			buffer_free(&files.players);
			files.players = edited;
		}
		ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 40, 20) == 0, true);
		freePart(&files);
	}
	// A missing file: nothing verifies.
	makePart(&files, &seeds, 40, 20);
	BatchVerify request = {
		.seeds = &seeds, .first = 40, .count = 20, .perMatch = true, .failed = true};
	ASSERT_BOOL_EQ(batchVerify_run(&request, &verified) && verified.verified == 0, true);
	freePart(&files);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The harness's own rows, from its formatter, verify; and the verifier's headers are the harness's.
TEST(verifies_the_harness_rows) {
	ASSERT_INT_EQ(strcmp(batchVerify_playersHeader, matchStats_playerRowsHeader), 0);
	ASSERT_INT_EQ(strcmp(batchVerify_teamsHeader, matchStats_teamRowsHeader), 0);
	wchar_t folder[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	BatchSeeds seeds = {.kind = BATCH_SEEDS_RANGE, .first = 0, .last = UINT32_MAX};
	PartFiles files = {0};
	appendText(&files.results, batchVerify_partResultsHeader);
	appendText(&files.players, batchVerify_playersHeader);
	appendText(&files.teams, batchVerify_teamsHeader);
	appendText(&files.failed, batchVerify_failedHeader);
	bool formatted = true;
	for (uint64_t index = 1000; index < 1040; ++index) {
		formatted =
			formatted && dllTest_formatRows((uint32_t)index, index, &files.players, &files.teams);
		appendResult(&files.results, (uint32_t)index);
	}
	ASSERT_BOOL_EQ(formatted, true);
	BatchVerified verified;
	ASSERT_BOOL_EQ(verifyPart(folder, &files, &seeds, 1000, 40, NULL, &verified), true);
	ASSERT_BOOL_EQ(verified.verified == 40 && verified.rows[BATCH_TEAMS] == 80, true);
	freePart(&files);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// With the teams a season's fixture plays, every team row must name its side's: the rows of
// another team verify no further than the failed seeds before them.
TEST(checks_each_rows_teams_when_given) {
	wchar_t folder[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	BatchSeeds seeds = {.kind = BATCH_SEEDS_RANGE, .first = 0, .last = 1000};
	PartFiles files;
	makePart(&files, &seeds, 100, 20);
	partTeams[0] = partTeams[1] = 77;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 100, 20) == 20, true);
	partTeams[1] = 78;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 100, 20) == 0, true);
	partTeams[0] = 76;
	partTeams[1] = 77;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 100, 20) == 0, true);
	partTeams[0] = partTeams[1] = 0;
	ASSERT_BOOL_EQ(verifiedCount(folder, &files, &seeds, 100, 20) == 20, true);
	freePart(&files);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

static bool readFileText(const wchar_t *path, ByteBuffer *out) {
	return files_read(path, out);
}

// Two parts merge into a chunk's files: result rows get their index, the others are copied, and
// the content hash leaves out `ms` as a run manifest's does.
TEST(merges_parts_into_a_chunk) {
	wchar_t folder[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	BatchSeeds seeds = {.kind = BATCH_SEEDS_RANGE, .first = 0, .last = 100};
	static const char *const names[BATCH_ROLES] = {
		"results.csv", "players.csv", "teams.csv", "failed.csv"};
	const char *headers[BATCH_ROLES] = {batchVerify_chunkResultsHeader,
		batchVerify_playersHeader,
		batchVerify_teamsHeader,
		batchVerify_failedHeader};
	BatchSink sinks[BATCH_ROLES];
	FILE *outputs[BATCH_ROLES];
	wchar_t paths[BATCH_ROLES][TEST_PATH_LENGTH];
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		outputs[role] =
			joinPath(paths[role], folder, names[role]) ? platform_openFile(paths[role], "wb") : NULL;
		batchSink_start(&sinks[role], outputs[role], headers[role], role == BATCH_RESULTS);
	}
	PartFiles first;
	PartFiles second;
	makePart(&first, &seeds, 3, 4); // seed 5 fails to rebuild
	makePart(&second, &seeds, 7, 2);
	BatchVerified verified;
	ASSERT_BOOL_EQ(
		verifyPart(folder, &first, &seeds, 3, 3, sinks, &verified) && verified.verified == 3, true
	);
	ASSERT_BOOL_EQ(
		verifyPart(folder, &second, &seeds, 7, 2, sinks, &verified) && verified.verified == 2, true
	);
	char hashes[BATCH_ROLES][2][SHA256_HEX_LENGTH];
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		ASSERT_BOOL_EQ(outputs[role] && fclose(outputs[role]) == 0 && !sinks[role].failed, true);
		batchSink_finish(&sinks[role], hashes[role][0], hashes[role][1]);
	}
	ByteBuffer text = {0};
	ASSERT_BOOL_EQ(readFileText(paths[BATCH_RESULTS], &text), true);
	ByteBuffer expected = {0};
	appendText(&expected, batchVerify_chunkResultsHeader);
	static const uint32_t played[] = {3, 4, 7, 8};
	for (size_t i = 0; i < sizeof played / sizeof played[0]; ++i) {
		appendf(&expected, "%u,", played[i]);
		appendResult(&expected, played[i]);
	}
	ASSERT_INT_EQ(strcmp(buffer_string(&text), buffer_string(&expected)), 0);
	ASSERT_BOOL_EQ(sinks[BATCH_RESULTS].rows == 4 && sinks[BATCH_FAILED].rows == 1, true);
	ASSERT_BOOL_EQ(sinks[BATCH_PLAYERS].rows == 12 && sinks[BATCH_TEAMS].rows == 8, true);
	ASSERT_BOOL_EQ(sinks[BATCH_RESULTS].bytes == text.length, true);
	char raw[SHA256_HEX_LENGTH];
	sha256_hexOf(text.data, text.length, raw);
	ASSERT_INT_EQ(strcmp(raw, hashes[BATCH_RESULTS][0]), 0);
	ManifestOutput output;
	ResultsSummary summary;
	ASSERT_BOOL_EQ(manifest_readOutput(paths[BATCH_RESULTS], &output, &summary), true);
	ASSERT_INT_EQ(strcmp(output.contentSha256, hashes[BATCH_RESULTS][1]), 0);
	buffer_clear(&text);
	ASSERT_BOOL_EQ(readFileText(paths[BATCH_FAILED], &text), true);
	ASSERT_INT_EQ(strcmp(buffer_string(&text), "index,seed,stage\n5,5,rebuild\n"), 0);
	buffer_clear(&text);
	ASSERT_BOOL_EQ(readFileText(paths[BATCH_TEAMS], &text), true);
	ASSERT_BOOL_EQ(
		strncmp(
			buffer_string(&text) + strlen(batchVerify_teamsHeader), "3,3,home,77,2,2,3,3,-0.05,", 26
		) == 0,
		true
	);
	// The chunk's files cover indices 3-8 exactly once, with 5 failed and 6 excluded.
	static const uint64_t excluded[] = {6};
	BatchVerify request = {.seeds = &seeds,
		.first = 3,
		.count = 6,
		.chunk = true,
		.perMatch = true,
		.failed = true,
		.excluded = excluded,
		.excludedCount = 1,
		.hashInputs = true};
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		request.inputs[role] = platform_openFile(paths[role], "rb");
	}
	ASSERT_BOOL_EQ(batchVerify_run(&request, &verified), true);
	ASSERT_BOOL_EQ(verified.verified == 6 && !verified.trailing, true);
	ASSERT_INT_EQ(strcmp(verified.inputs[BATCH_RESULTS].sha256, hashes[BATCH_RESULTS][0]), 0);
	ASSERT_INT_EQ(strcmp(verified.inputs[BATCH_RESULTS].contentSha256, hashes[BATCH_RESULTS][1]), 0);
	ASSERT_INT_EQ(strcmp(verified.inputs[BATCH_PLAYERS].sha256, hashes[BATCH_PLAYERS][0]), 0);
	ASSERT_BOOL_EQ(verified.inputs[BATCH_TEAMS].rows == 8, true);
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		(void)fclose(request.inputs[role]);
	}
	// Without the exclusion, index 6 is missing; with only 5 asked for, rows are left over.
	request.excludedCount = 0;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		request.inputs[role] = platform_openFile(paths[role], "rb");
	}
	ASSERT_BOOL_EQ(
		batchVerify_run(&request, &verified) && verified.verified == 3 && verified.trailing, true
	);
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		(void)fclose(request.inputs[role]);
	}
	buffer_free(&text);
	buffer_free(&expected);
	freePart(&first);
	freePart(&second);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// ---------------------------------------------------------------------------------------------
// The part chain

static BatchPart
part(uint32_t attempt, uint64_t first, uint64_t count, uint64_t verified, bool evaluated) {
	return (BatchPart){.attempt = attempt,
		.first = first,
		.count = count,
		.verified = verified,
		.evaluated = evaluated,
		.outcome = BATCH_PART_CRASHED};
}

static uint64_t cursorOf(BatchPart *parts, size_t count, BatchChain *chain) {
	BatchChunk chunk = {.first = 37000, .last = 37999, .parts = parts, .partCount = count};
	return batchChain_compute(&chunk, chain) ? chain->cursor : 0;
}

TEST(follows_the_part_chain) {
	BatchChain chain;
	ASSERT_BOOL_EQ(
		cursorOf(NULL, 0, &chain) == 37000 && chain.count == 0 && chain.stopCount == 1, true
	);
	batchChain_free(&chain);
	// a1 crashed after 412; a2 and a3 (one thread) crashed at 37412; a3 marked it poison; a4 did
	// the rest.
	BatchPart parts[] = {
		part(1, 37000, 1000, 412, true),
		part(2, 37412, 588, 0, true),
		part(3, 37412, 588, 0, true),
		part(4, 37413, 587, 587, true),
	};
	parts[2].hasPoison = true;
	parts[2].poison = 37412;
	ASSERT_BOOL_EQ(cursorOf(parts, 4, &chain) == 38000, true);
	ASSERT_BOOL_EQ(
		chain.count == 3 && chain.parts[0] == 0 && chain.parts[1] == 2 && chain.parts[2] == 3, true
	);
	ASSERT_BOOL_EQ(chain.excludedCount == 1 && chain.excluded[0] == 37412, true);
	ASSERT_BOOL_EQ(chain.stopCount == 4 && chain.stops[1] == 37412 && chain.stops[2] == 37413, true);
	batchChain_free(&chain);
	// Without the poison mark the chain waits at 37412; a part off the chain is not used.
	parts[2].hasPoison = false;
	ASSERT_BOOL_EQ(cursorOf(parts, 4, &chain) == 37412 && chain.count == 1, true);
	batchChain_free(&chain);
	// A poison mark that is not where its part stopped does not count.
	parts[2].hasPoison = true;
	parts[2].poison = 37413;
	ASSERT_BOOL_EQ(cursorOf(parts, 4, &chain) == 37412, true);
	batchChain_free(&chain);
	// Of two parts at one cursor, the furthest, and of equals the later one; an unevaluated part
	// does not count.
	BatchPart same[] = {
		part(1, 37000, 1000, 10, true),
		part(2, 37000, 1000, 30, true),
		part(3, 37000, 1000, 30, true),
		part(4, 37030, 970, 970, false),
	};
	ASSERT_BOOL_EQ(cursorOf(same, 4, &chain) == 37030 && chain.parts[0] == 2, true);
	batchChain_free(&chain);
	same[3].evaluated = true;
	ASSERT_BOOL_EQ(cursorOf(same, 4, &chain) == 38000, true);
	batchChain_free(&chain);
	// A poison seed at a chunk's last index ends it.
	BatchPart end[] = {part(1, 37000, 1000, 999, true)};
	end[0].hasPoison = true;
	end[0].poison = 37999;
	ASSERT_BOOL_EQ(cursorOf(end, 1, &chain) == 38000 && chain.excludedCount == 1, true);
	batchChain_free(&chain);
}

// ---------------------------------------------------------------------------------------------
// A batch's folder: create, the fake game's jobs, kills and restarts

// The index in arm "press" whose seed crashes the game every time.
enum { POISON_INDEX = 124 };

// What the game does with an attempt's job.
typedef struct Scripted {
	uint64_t crashAt; // the index it crashes at; UINT64_MAX if it finishes
	bool nothing;     // it never ran
	bool zeroTail;    // the crash leaves a zero-filled tail on the results
} Scripted;

typedef struct Drive {
	unsigned attempts; // started
	unsigned rounds;   // times the "process" started
	bool failed;
	unsigned killEvery; // after a kill, the next one this many steps later; 0 for none
} Drive;

static Scripted scriptFor(
	const Batch *batch,
	size_t arm,
	uint64_t chunk,
	const BatchAttempt *attempt,
	bool killed
) {
	Scripted job = {.crashAt = UINT64_MAX};
	uint64_t first = 0;
	uint64_t last = 0;
	batchPlan_chunkRange(&batch->plan, chunk, &first, &last);
	uint64_t end = attempt->first + attempt->count;
	if (batch->plan.armCount > 1 && arm == 1 && attempt->first <= POISON_INDEX &&
			POISON_INDEX < end) {
		job.crashAt = POISON_INDEX;
	}
	// The first attempt at each arm's second chunk crashes partway; arm 0's third chunk's first
	// attempt crashes at once.
	if (chunk == 1 && attempt->attempt == 1 && first + 3 + arm < job.crashAt) {
		job.crashAt = first + 3 + arm;
		job.zeroTail = true;
	}
	if (chunk == 2 && arm == 0 && attempt->attempt == 1) {
		job.crashAt = attempt->first;
	}
	// Arm 0's last chunk crashes twice, each time one index further on: not the same seed twice.
	if (chunk == 3 && arm == 0 && attempt->attempt <= 2) {
		job.crashAt = attempt->first + 1;
	}
	// After every other kill the game was gone as well.
	job.nothing = killed && fake.kills % 2;
	return job;
}

static bool appendFile(const wchar_t *path, const void *bytes, size_t size) {
	FILE *file = platform_openFile(path, "ab");
	bool ok = file && fwrite(bytes, 1, size, file) == size;
	return file && fclose(file) == 0 && ok;
}

// Writes the attempt's files as the harness would: for each index, its other rows, then its
// results row; at a crash, the other rows of that index and half its results row.
static bool writeJob(const Batch *batch, const BatchAttempt *attempt, const Scripted *job) {
	const BatchPlan *plan = &batch->plan;
	if (job->nothing) {
		return true;
	}
	wchar_t output[TEST_PATH_LENGTH];
	wchar_t paths[BATCH_ROLES][TEST_PATH_LENGTH];
	static const char *const names[BATCH_ROLES] = {
		"results.csv", "results-match-players.csv", "results-match-teams.csv", "results-failed.csv"};
	bool wanted[BATCH_ROLES] = {
		true, plan->perMatch != 0, plan->perMatch != 0, plan->seedErrors == BATCH_SEED_ERRORS_SKIP};
	ByteBuffer texts[BATCH_ROLES] = {{0}};
	appendText(&texts[BATCH_RESULTS], batchVerify_partResultsHeader);
	appendText(&texts[BATCH_PLAYERS], batchVerify_playersHeader);
	appendText(&texts[BATCH_TEAMS], batchVerify_teamsHeader);
	appendText(&texts[BATCH_FAILED], batchVerify_failedHeader);
	bool ok = platform_fromUtf8(attempt->paths.output, output, TEST_PATH_LENGTH);
	wchar_t *slash = ok ? wcsrchr(output, PLATFORM_PATH_SEPARATOR) : NULL;
	ok = slash != NULL;
	for (uint64_t index = attempt->first; ok && index < attempt->first + attempt->count; ++index) {
		uint32_t seed = batchSeeds_at(&plan->seeds, index);
		if (wanted[BATCH_FAILED] && isRebuildFailure(seed)) {
			appendf(&texts[BATCH_FAILED], "%llu,%u,rebuild\n", (unsigned long long)index, seed);
		} else {
			ok = !plan->perMatch ||
					 dllTest_formatRows(seed, index, &texts[BATCH_PLAYERS], &texts[BATCH_TEAMS]);
			ByteBuffer row = {0};
			appendResult(&row, seed);
			(void)buffer_append(
				&texts[BATCH_RESULTS], row.data, index == job->crashAt ? row.length / 2 : row.length
			);
			buffer_free(&row);
		}
		if (index == job->crashAt) {
			break;
		}
	}
	if (job->zeroTail) {
		static const uint8_t zeros[100] = {0};
		(void)buffer_append(&texts[BATCH_RESULTS], zeros, sizeof zeros);
	}
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		if (ok && wanted[role]) {
			*slash = 0;
			ok = joinPath(paths[role], output, names[role]) &&
					 files_write(paths[role], texts[role].data, texts[role].length);
			*slash = PLATFORM_PATH_SEPARATOR;
		}
		buffer_free(&texts[role]);
	}
	return ok;
}

// One step of a run on a chunk: commit it if its parts cover it, else an attempt at it, with the
// policy's poison rule (the attempt before stopped where this one starts).
static BatchStatus runChunk(Batch *batch, size_t arm, uint64_t chunk, Drive *drive) {
	const BatchChunk *pending = batch_pending(batch, arm, chunk);
	uint64_t candidate = UINT64_MAX;
	if (pending && pending->partCount) {
		BatchChain chain;
		bool done = batchChain_compute(pending, &chain) && chain.cursor > pending->last;
		batchChain_free(&chain);
		if (done) {
			BatchCommitFacts facts = {.power = "ac", .gameRssBytes = 6123456789ULL, .canaryMatch = -1};
			return batch_commit(batch, arm, chunk, &facts);
		}
		const BatchPart *last = &pending->parts[pending->partCount - 1];
		candidate = last->outcome == BATCH_PART_CRASHED ? last->first + last->verified : UINT64_MAX;
	}
	if (++drive->attempts > MOST_ATTEMPTS) {
		return BATCH_STATUS_FAILED;
	}
	BatchAttemptFacts facts = {.threads = 4, .session = "s0001", .startedAt = fake.now};
	BatchAttempt attempt;
	BatchStatus status = batch_beginAttempt(batch, arm, chunk, &facts, &attempt);
	if (status == BATCH_STATUS_FAILED) {
		return status;
	}
	// The job runs in the game whether or not the process that submitted it lives on.
	Scripted job = scriptFor(batch, arm, chunk, &attempt, status == BATCH_STATUS_KILLED);
	if (!writeJob(batch, &attempt, &job) || status != BATCH_STATUS_OK) {
		return status == BATCH_STATUS_OK ? BATCH_STATUS_FAILED : status;
	}
	bool crashed = job.crashAt < attempt.first + attempt.count;
	BatchPartFacts part = {
		.outcome = crashed ? BATCH_PART_CRASHED : BATCH_PART_OK,
		.clientOutcome = crashed ? "lost" : "replied",
		.finishedAt = fake.now,
		.wallMs = attempt.count * 500,
		.hasPoison = candidate == attempt.first,
		.poison = candidate,
	};
	return batch_finishAttempt(batch, arm, chunk, attempt.attempt, &part);
}

// What a process stopped mid-write can leave: torn lines and a temporary file.
static void damage(const wchar_t *directory) {
	wchar_t path[TEST_PATH_LENGTH];
	if (joinPath(path, directory, "events.log")) {
		(void)appendFile(path, "2026-10-25T09:2", 15);
	}
	if (joinPath(path, directory, "index.csv")) {
		(void)appendFile(path, "baseline,0000", 13);
	}
	(void)writeText(directory, "batch.json.tmp", "{\"format\":");
}

// Runs a batch to the end as batch run would, restarting it after every kill.
static bool driveBatch(const wchar_t *directory, Drive *drive) {
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	bool complete = false;
	while (batch && !complete && !drive->failed && drive->rounds < 200) {
		++drive->rounds;
		if (batch_open(batch, directory, &host) != BATCH_OPENED) {
			drive->failed = true;
			break;
		}
		BatchRecovery recovery;
		BatchStatus status = batch_recover(batch, BATCH_PART_CRASHED, &recovery);
		size_t arm = 0;
		uint64_t chunk = 0;
		while (status == BATCH_STATUS_OK && !(complete = !batch_nextChunk(batch, &arm, &chunk))) {
			status = runChunk(batch, arm, chunk, drive);
		}
		if (complete && status == BATCH_STATUS_OK) {
			BatchOutcome outcome = {"complete", NULL, 0, fake.now};
			status = batch_writeManifest(batch, &outcome);
		}
		complete = complete && status == BATCH_STATUS_OK;
		drive->failed = status == BATCH_STATUS_FAILED;
		batch_close(batch);
		if (status == BATCH_STATUS_KILLED) {
			damage(directory);
			fake.killAt = drive->killEvery ? fake.steps + drive->killEvery : 0;
		}
	}
	free(batch);
	return complete && !drive->failed;
}

// A committed file's text, as the fake host's compression keeps it, decompressed by another host,
// or as it is.
static bool
readChunkFile(const Batch *batch, const BatchCommitted *chunk, const char *name, ByteBuffer *out) {
	char relative[128];
	char id[BATCH_CHUNK_ID_LENGTH];
	bool gzip = batch->plan.compress == BATCH_COMPRESS_GZIP;
	batchPlan_chunkId(chunk->first, id);
	(void)snprintf(
		relative,
		sizeof relative,
		"chunks/arm=%s/chunk=%s/%s%s",
		batch->plan.arms[chunk->arm].id,
		id,
		name,
		gzip ? ".gz" : ""
	);
	const BatchHost *host = batch->host;
	if (gzip && host->compress != fakeCompress) {
		wchar_t path[TEST_PATH_LENGTH];
		void *handle = NULL;
		FILE *file = joinPath(path, batch->directory, relative)
									 ? host->openDecompressed(host->context, path, &handle)
									 : NULL;
		bool ok = file != NULL;
		uint8_t block[4096];
		for (size_t got = 1; ok && got;) {
			got = fread(block, 1, sizeof block, file);
			ok = buffer_append(out, block, got);
		}
		return file && host->closeDecompressed(host->context, file, handle) && ok;
	}
	ByteBuffer raw = {0};
	bool ok = readText(batch->directory, relative, &raw);
	size_t magic = gzip ? sizeof fakeMagic - 1 : 0;
	ok = ok && raw.length >= magic && (!gzip || memcmp(raw.data, fakeMagic, magic) == 0);
	for (size_t i = magic; ok && i < raw.length; ++i) {
		uint8_t byte = gzip ? raw.data[i] ^ 0x5A : raw.data[i];
		ok = buffer_append(out, &byte, 1);
	}
	buffer_free(&raw);
	return ok;
}

// Counts each index a file's rows name ("index,seed,..."), checking each row's seed.
static bool
markRows(const Batch *batch, const BatchSeeds *seeds, const ByteBuffer *text, uint8_t *marks) {
	const char *line = strchr(buffer_string(text), '\n');
	bool ok = line != NULL;
	for (line = line ? line + 1 : NULL; ok && line && *line;
		line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
		char *end = NULL;
		unsigned long long index = strtoull(line, &end, 10);
		unsigned long long seed = end && *end == ',' ? strtoull(end + 1, &end, 10) : 0;
		ok = end && *end == ',' && index >= batch->plan.seeds.first &&
				 index <= batch->plan.seeds.last && seed == batchSeeds_at(seeds, index);
		if (ok) {
			++marks[index - batch->plan.seeds.first];
		}
	}
	return ok;
}

// Checks a finished batch from its files alone, through `host`: every planned index of every arm is
// in exactly one committed chunk, as a result row, a failed row or an exclusion, with its own seed.
// Returns the chunks' contents' hashes as text, to compare runs by. With `fakeClock`, batch.json's
// outcome is from the fake host's time.
static bool checkFinishedWith(
	const wchar_t *directory,
	const BatchHost *host,
	bool fakeClock,
	ByteBuffer *hashes
) {
	Batch *batch = malloc(sizeof *batch);
	if (!batch || batch_open(batch, directory, host) != BATCH_OPENED) {
		free(batch);
		return false;
	}
	const BatchPlan *plan = &batch->plan;
	uint64_t count = batchSeeds_count(&plan->seeds);
	bool ok = batch->committedCount == batchPlan_chunksPerArm(plan) * plan->armCount &&
						!batch->pendingCount && !batch->leftoverCount;
	if (!ok) {
		fprintf(
			stderr,
			"%zu chunks committed, %zu pending, %zu leftovers\n",
			batch->committedCount,
			batch->pendingCount,
			batch->leftoverCount
		);
	}
	for (size_t arm = 0; ok && arm < plan->armCount; ++arm) {
		uint8_t *marks = calloc(count, 1);
		BatchSeeds seeds = batchPlan_armSeeds(plan, arm);
		ok = marks != NULL;
		for (size_t i = 0; ok && i < batch->committedCount; ++i) {
			const BatchCommitted *chunk = &batch->committed[i];
			if (chunk->arm != arm) {
				continue;
			}
			ByteBuffer text = {0};
			JsonFlat flat = {0};
			char relative[128];
			char id[BATCH_CHUNK_ID_LENGTH];
			batchPlan_chunkId(chunk->first, id);
			ok =
				readChunkFile(batch, chunk, "results.csv", &text) && markRows(batch, &seeds, &text, marks);
			buffer_clear(&text);
			if (ok && plan->seedErrors == BATCH_SEED_ERRORS_SKIP) {
				ok =
					readChunkFile(batch, chunk, "failed.csv", &text) && markRows(batch, &seeds, &text, marks);
			}
			(void)snprintf(
				relative, sizeof relative, "chunks/arm=%s/chunk=%s/chunk.json", plan->arms[arm].id, id
			);
			buffer_clear(&text);
			ok = ok && readText(directory, relative, &text) &&
					 jsonFlat_read(&flat, buffer_string(&text), text.length);
			for (size_t e = 0; ok; ++e) {
				char key[64];
				uint64_t index = 0;
				(void)snprintf(key, sizeof key, "excluded.%zu.index", e);
				if (!jsonFlat_unsigned(&flat, key, &index)) {
					break;
				}
				ok = index >= chunk->first && index < chunk->first + chunk->count;
				if (ok) {
					++marks[index - plan->seeds.first];
				}
			}
			jsonFlat_free(&flat);
			buffer_free(&text);
			for (size_t role = 0; ok && role < BATCH_ROLES; ++role) {
				appendf(hashes, "%s/%s %s\n", plan->arms[arm].id, id, chunk->sha256[role]);
			}
			ok = ok && batch_verifyChunk(batch, chunk, true, stderr);
		}
		for (uint64_t i = 0; ok && i < count; ++i) {
			if (marks[i] != 1) {
				fprintf(
					stderr,
					"arm %s index %llu is covered %u times\n",
					plan->arms[arm].id,
					(unsigned long long)(plan->seeds.first + i),
					marks[i]
				);
				ok = false;
			}
		}
		free(marks);
	}
	// index.csv is whole, and what the chunks make; events.log logs each chunk once.
	ByteBuffer before = {0};
	ByteBuffer after = {0};
	ByteBuffer events = {0};
	bool covered = ok;
	ok = ok && readText(directory, "index.csv", &before) && batch_writeIndex(batch) &&
			 readText(directory, "index.csv", &after) && before.length == after.length &&
			 memcmp(before.data, after.data, before.length) == 0 &&
			 occurrences(buffer_string(&after), "\n") == batch->committedCount + 1 &&
			 readText(directory, "events.log", &events) && occurrencesIn(&events, " created name=") == 1;
	if (covered && !ok) {
		fprintf(
			stderr,
			"index.csv or events.log differs:\n%s---\n%s",
			buffer_string(&before),
			buffer_string(&after)
		);
	}
	for (size_t i = 0; ok && i < batch->committedCount; ++i) {
		char needle[96];
		char id[BATCH_CHUNK_ID_LENGTH];
		batchPlan_chunkId(batch->committed[i].first, id);
		(void)snprintf(
			needle,
			sizeof needle,
			" chunk_committed arm=%s chunk=%s ",
			plan->arms[batch->committed[i].arm].id,
			id
		);
		ok = occurrencesIn(&events, needle) == 1;
		if (!ok) {
			fprintf(
				stderr,
				"events.log logs %s/%s %u times\n",
				plan->arms[batch->committed[i].arm].id,
				id,
				occurrencesIn(&events, needle)
			);
		}
	}
	char state[32];
	char reason[64];
	int exitCode = -1;
	time_t at = 0;
	// Every event is on a line of its own, after its time, whatever a kill tore or a power cut
	// zeroed before it.
	for (size_t offset = 0; ok && offset < events.length;) {
		const char *line = buffer_string(&events) + offset;
		const char *end = memchr(line, '\n', events.length - offset);
		size_t length = end ? (size_t)(end - line) : events.length - offset;
		bool torn = length < 21 || line[4] != '-' || line[10] != 'T' || line[19] != 'Z' ||
								line[20] != ' ' || memchr(line + 1, 'Z', 18);
		bool event = memchr(line, '=', length) != NULL;
		ok = !(torn && event);
		if (!ok) {
			fprintf(stderr, "events.log has an event without its own line: %.*s\n", (int)length, line);
		}
		offset += length + (end != NULL);
	}
	bool logged = ok;
	ok = ok && batch_readOutcome(batch, state, sizeof state, reason, sizeof reason, &exitCode, &at) &&
			 strcmp(state, "complete") == 0 && exitCode == 0 && (!fakeClock || at == fakeNow(NULL));
	if (logged && !ok) {
		fprintf(stderr, "batch.json's outcome is %s (%d)\n", state, exitCode);
	}
	buffer_free(&before);
	buffer_free(&after);
	buffer_free(&events);
	batch_close(batch);
	free(batch);
	return ok;
}

static bool checkFinished(const wchar_t *directory, ByteBuffer *hashes) {
	BatchHost host = fakeHost();
	return checkFinishedWith(directory, &host, true, hashes);
}

bool batchTest_checkFinished(const wchar_t *directory, const BatchHost *host) {
	ByteBuffer hashes = {0};
	bool ok = checkFinishedWith(directory, host, false, &hashes);
	buffer_free(&hashes);
	return ok;
}

// A test folder with a created batch in <folder>/batch, made from `settings`.
static bool createdBatch(wchar_t *folder, wchar_t *directory, const char *settings) {
	if (!makeTestFolder(folder)) {
		return false;
	}
	resetFake(folder);
	return writeSources(folder, settings) && create(folder) == 0 &&
				 joinPath(directory, folder, "batch");
}

bool batchTest_createBatch(const char *settings, wchar_t *folder, wchar_t *directory) {
	char text[1024];
	(void)snprintf(text, sizeof text, "%s%s", standardSettings, settings);
	return createdBatch(folder, directory, text);
}

int batchTest_verify(const wchar_t *directory, const BatchHost *host) {
	return verifyBatch(directory, true, host, stdout);
}

int batchTest_stop(const wchar_t *directory, bool now) {
	return stopBatch(directory, now);
}

TEST(creates_a_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	static const char *const made[] = {
		"plan.txt",
		"index.csv",
		"events.log",
		"batch.json",
		"inputs/baseline.json",
		"inputs/baseline.bin",
		"inputs/press.json",
		"inputs/press.bin",
	};
	for (size_t i = 0; i < sizeof made / sizeof made[0]; ++i) {
		ASSERT_BOOL_EQ(exists(directory, made[i]), true);
	}
	ByteBuffer text = {0};
	static BatchPlan plan;
	char error[256];
	ASSERT_BOOL_EQ(readText(directory, "plan.txt", &text), true);
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&text), text.length, true, &testDefaults, &plan, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(
		plan.threads == 4 && plan.chunk == 10 && plan.compress == BATCH_COMPRESS_GZIP, true
	);
	ASSERT_BOOL_EQ(plan.gameBuild == 2081827 && strcmp(plan.saveName, "LIV_COPY") == 0, true);
	ASSERT_INT_EQ(strcmp(plan.gameDate, "2024 day 123 15:00"), 0);
	ASSERT_BOOL_EQ(plan.arms[0].teams[1] == 677 && plan.arms[0].teams[0] == 0, true);
	// The paths are absolute, and the frozen inputs are the fixtures' bytes and their encoding.
	char fixture[3 * TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(platform_toUtf8(folder, fixture, sizeof fixture), true);
	(void)snprintf(
		fixture + strlen(fixture),
		64,
		"%cfixtures%cbaseline.json",
		(char)PLATFORM_PATH_SEPARATOR,
		(char)PLATFORM_PATH_SEPARATOR
	);
	ASSERT_INT_EQ(strcmp(plan.arms[0].path, fixture), 0);
	ByteBuffer input = {0};
	char hash[SHA256_HEX_LENGTH];
	ASSERT_BOOL_EQ(readText(directory, "inputs/press.json", &input), true);
	sha256_hexOf(input.data, input.length, hash);
	ASSERT_INT_EQ(strcmp(hash, plan.arms[1].jsonSha256), 0);
	buffer_clear(&text);
	ASSERT_BOOL_EQ(readText(directory, "index.csv", &text), true);
	ASSERT_BOOL_EQ(
		strncmp(buffer_string(&text), "arm,first_index,count,results,", 30) == 0 &&
			occurrences(buffer_string(&text), "\n") == 1,
		true
	);
	buffer_clear(&text);
	ASSERT_BOOL_EQ(readText(directory, "events.log", &text), true);
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&text), "Z created name=test-batch arms=2 indices=68 chunks=8 plan_sha256="
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(strstr(buffer_string(&text), plan.planSha256) != NULL, true);
	buffer_clear(&text);
	JsonFlat flat = {0};
	char state[32];
	uint64_t value = 0;
	ASSERT_BOOL_EQ(
		readText(directory, "batch.json", &text) &&
			jsonFlat_read(&flat, buffer_string(&text), text.length),
		true
	);
	ASSERT_BOOL_EQ(
		jsonFlat_string(&flat, "outcome.state", state, sizeof state) && strcmp(state, "created") == 0,
		true
	);
	ASSERT_BOOL_EQ(jsonFlat_unsigned(&flat, "progress.planned", &value) && value == 68, true);
	ASSERT_BOOL_EQ(jsonFlat_unsigned(&flat, "arms.1.chunks_total", &value) && value == 4, true);
	ASSERT_BOOL_EQ(jsonFlat_isNull(&flat, "progress.eta"), true);
	jsonFlat_free(&flat);
	buffer_free(&text);
	buffer_free(&input);
	// A second create into the same folder is refused, and leaves it as it was.
	ASSERT_INT_EQ(create(folder), 1);
	ASSERT_BOOL_EQ(exists(directory, "plan.txt"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Each refusal leaves the batch folder without plan.txt.
static void expectRefused(const char *settings, const char *pressFixture, bool both) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t fixtures[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		makeTestFolder(folder) && joinPath(directory, folder, "batch") &&
			joinPath(fixtures, folder, "fixtures"),
		true
	);
	resetFake(folder);
	ASSERT_BOOL_EQ(writeSources(folder, settings), true);
	if (pressFixture) {
		ASSERT_BOOL_EQ(writeText(fixtures, "press.json", pressFixture), true);
		ASSERT_BOOL_EQ(!both || writeText(fixtures, "baseline.json", pressFixture), true);
	}
	(void)writeText(folder, "seeds.txt", "7\n9\n7\n");
	int status = create(folder);
	if (status != 1) {
		fprintf(stderr, "not refused: %s\n", settings);
	}
	ASSERT_INT_EQ(status, 1);
	ASSERT_BOOL_EQ(exists(directory, "plan.txt"), false);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

TEST(refuses_batches_it_cannot_run) {
	static const char other[] =
		"{\"format\":\"fm24-match-config/"
		"1\",\"game_build\":%u,\"save\":\"%s\",\"game_date\":\"%s\",\"fields\":[{\"u8\":1}]}";
	char fixture[512];
	(void)snprintf(fixture, sizeof fixture, other, 2081827U, "OTHER_SAVE", "2024 day 123 15:00");
	expectRefused(standardSettings, fixture, false);
	(void)snprintf(fixture, sizeof fixture, other, 2081828U, "LIV_COPY", "2024 day 123 15:00");
	expectRefused(standardSettings, fixture, false);
	(void)snprintf(fixture, sizeof fixture, other, 2081827U, "LIV_COPY", "2024 day 124 15:00");
	expectRefused(standardSettings, fixture, false);
	expectRefused(standardSettings, "{\"format\":\"fm24-player/1\",\"fields\":[]}", false);
	expectRefused(
		standardSettings, "{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u7\":1}]}", false
	);
	expectRefused("seeds=scatter:9:100-133\nsave=OTHER_SAVE\n", NULL, false);
	expectRefused("seeds=scatter:9:100-133\nsave=missing/LIV_COPY.fm\n", NULL, false);
	expectRefused("seeds=scatter:9:100-133\nchunk=10\ncanary=11\n", NULL, false);
	expectRefused("seeds=scatter:9:100-102\nwarmup=4\n", NULL, false);
	expectRefused("seeds=scatter:9:0-999999\nchunk=2000\n", NULL, false);
	expectRefused("seeds=list:seeds.txt\n", NULL, false);
	expectRefused("seeds=list:missing.txt\n", NULL, false);
	expectRefused("seeds=scatter:9:100-133\ndetailed=1\n", NULL, false);
	expectRefused("seeds=scatter:9:100-133\nthreads=0\n", NULL, false);
#ifdef _WIN32
	expectRefused("seeds=scatter:9:100-133\ncompress=gzip\n", NULL, false);
#endif
	// No save at all needs launch=0.
	static const char unsaved[] = "{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u8\":1}]}";
	expectRefused(standardSettings, unsaved, true);
}

TEST(creates_batches_that_bend_the_rules_allowed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t fixtures[TEST_PATH_LENGTH];
	static const struct {
		const char *settings;
		const char *press; // the second fixture, or NULL
		const char *saveName;
		const char *gameDate;
	} cases[] = {
		// keep_build=1 allows other dates, and then the batch has none.
		{"seeds=range:1-50\nkeep_build=1\n",
			"{\"format\":\"fm24-match-config/"
			"1\",\"game_build\":2081827,\"save\":\"LIV_COPY\",\"game_date\":\"2024 day 200 "
			"15:00\",\"fields\":[{\"u8\":1}]}",
			"LIV_COPY",
			""},
		// A save's .fm file, beside the plan, for relaunches.
		{"seeds=range:1-50\nsave=LIV_COPY.fm\n", NULL, "LIV_COPY", "2024 day 123 15:00"},
		{"seeds=range:1-50\nsave=LIV_COPY\n", NULL, "LIV_COPY", "2024 day 123 15:00"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(
			makeTestFolder(folder) && joinPath(directory, folder, "batch") &&
				joinPath(fixtures, folder, "fixtures"),
			true
		);
		resetFake(folder);
		ASSERT_BOOL_EQ(
			writeSources(folder, cases[i].settings) && writeText(folder, "LIV_COPY.fm", "save"), true
		);
		if (cases[i].press) {
			ASSERT_BOOL_EQ(writeText(fixtures, "press.json", cases[i].press), true);
		}
		ASSERT_INT_EQ(create(folder), 0);
		BatchHost host = fakeHost();
		Batch *batch = malloc(sizeof *batch);
		ASSERT_BOOL_EQ(batch && batch_open(batch, directory, &host) == BATCH_OPENED, true);
		if (batch) {
			ASSERT_INT_EQ(strcmp(batch->plan.saveName, cases[i].saveName), 0);
			ASSERT_INT_EQ(strcmp(batch->plan.gameDate, cases[i].gameDate), 0);
			batch_close(batch);
		}
		free(batch);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
	// Fixtures without a save, with launch=0.
	ASSERT_BOOL_EQ(makeTestFolder(folder) && joinPath(fixtures, folder, "fixtures"), true);
	resetFake(folder);
	static const char unsaved[] = "{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u8\":1}]}";
	ASSERT_BOOL_EQ(
		writeSources(folder, "seeds=range:1-50\nlaunch=0\n") &&
			writeText(fixtures, "baseline.json", unsaved) && writeText(fixtures, "press.json", unsaved),
		true
	);
	ASSERT_INT_EQ(create(folder), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A save's name, or a folder, with " #" in it reads back whole from plan.txt: only a plan as
// written has comments after its values.
TEST(freezes_names_and_folders_with_a_hash) {
	wchar_t root[TEST_PATH_LENGTH];
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t fixtures[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(root), true);
	resetFake(root);
	ASSERT_BOOL_EQ(
		joinPath(folder, root, "FM runs #2") && platform_makeDirectory(folder) &&
			joinPath(fixtures, folder, "fixtures") && platform_makeDirectory(fixtures) &&
			joinPath(directory, folder, "batch"),
		true
	);
	ASSERT_BOOL_EQ(
		writeFixture(fixtures, "baseline.json", "Liverpool #2", "2024 day 123 15:00", 2081827, 42) &&
			writeText(
				folder,
				"plan.txt",
				"format=fm24-batch-plan/1\nname=hash\narm=baseline fixtures/baseline.json  # relative\n"
				"seeds=range:1-20\nchunk=10\n"
			),
		true
	);
	ASSERT_INT_EQ(create(folder), 0);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	bool opened = batch && batch_open(batch, directory, &host) == BATCH_OPENED;
	ASSERT_BOOL_EQ(opened, true);
	if (opened) {
		ASSERT_INT_EQ(strcmp(batch->plan.saveName, "Liverpool #2"), 0);
		ASSERT_BOOL_EQ(strstr(batch->plan.arms[0].path, "FM runs #2") != NULL, true);
		batch_close(batch);
	}
	free(batch);
	ASSERT_BOOL_EQ(countIn(directory, "batch.json", "\"save\":\"Liverpool #2\"") >= 1, true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"save\":\"Liverpool\""), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(root), true);
}

// A batch needs a new or empty folder, the Finder's .DS_Store aside.
TEST(creates_a_batch_only_in_an_empty_folder) {
	static const char *const found[] = {".DS_Store", "notes.txt"};
	for (size_t i = 0; i < sizeof found / sizeof found[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(makeTestFolder(folder) && joinPath(directory, folder, "batch"), true);
		resetFake(folder);
		ASSERT_BOOL_EQ(writeSources(folder, standardSettings), true);
		ASSERT_BOOL_EQ(platform_makeDirectory(directory) && writeText(directory, found[i], "x"), true);
		ASSERT_INT_EQ(create(folder), i == 0 ? 0 : 1);
		ASSERT_BOOL_EQ(exists(directory, "plan.txt"), i == 0);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

#ifndef _WIN32
// What `command` prints to stderr for `folder` in `out`, with its exit code.
static int
printedBy(int (*command)(const wchar_t *), const wchar_t *folder, char *out, size_t size) {
	char printed[] = "/tmp/fm24-batch-errors-XXXXXX";
	int file = mkstemp(printed);
	int saved = dup(STDERR_FILENO);
	int code = -1;
	out[0] = 0;
	if (file >= 0 && saved >= 0 && dup2(file, STDERR_FILENO) >= 0) {
		code = command(folder);
		(void)dup2(saved, STDERR_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	if (file >= 0) {
		ssize_t length = pread(file, out, size - 1, 0);
		out[length > 0 ? length : 0] = 0;
		close(file);
		(void)unlink(printed);
	}
	return code;
}

// Less free space than the batch needs, and min_free_gb, is a warning; the batch is created.
TEST(warns_when_the_disk_has_too_little_space) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	char printed[1024];
	ASSERT_BOOL_EQ(makeTestFolder(folder) && joinPath(directory, folder, "batch"), true);
	resetFake(folder);
	ASSERT_BOOL_EQ(writeSources(folder, standardSettings), true);
	ASSERT_INT_EQ(printedBy(create, folder, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(strstr(printed, "warning:") == NULL, true);
	ASSERT_BOOL_EQ(batch_removeFolder(directory), true);
	fake.freeBytes = 19ULL << 30; // less than min_free_gb=20 alone
	ASSERT_INT_EQ(printedBy(create, folder, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(
		strstr(
			printed, "warning: 19.0 GB free, but the batch needs about 20.0 GB with min_free_gb=20"
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(exists(directory, "plan.txt"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The game would need permission to write in these, and the prompt would stop an unattended batch.
TEST(refuses_folders_macos_guards) {
	wchar_t folder[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	resetFake(folder);
	ASSERT_BOOL_EQ(writeSources(folder, standardSettings), true);
	wchar_t plan[TEST_PATH_LENGTH];
	BatchHost host = fakeHost();
	ASSERT_BOOL_EQ(joinPath(plan, folder, "plan.txt"), true);
	// A volume: nothing is made under one, here a folder of the test's own in place of /Volumes.
	ASSERT_INT_EQ(strcmp(volumesFolder, "/Volumes"), 0);
	ASSERT_BOOL_EQ(isProtectedFolder("/Volumes/fm24-batch-test-volume/batch"), true);
	ASSERT_BOOL_EQ(isProtectedFolder("/Volumesque/batch"), false);
	char volumes[3 * TEST_PATH_LENGTH];
	wchar_t onVolume[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		platform_toUtf8(folder, volumes, sizeof volumes) && strlen(volumes) + 8 < sizeof volumes &&
			joinPath(onVolume, folder, "Volumes/disk/batch"),
		true
	);
	strcat(volumes, "/Volumes");
	volumesFolder = volumes;
	ASSERT_INT_EQ(createBatch(plan, onVolume, &host), 1);
	ASSERT_BOOL_EQ(exists(folder, "Volumes"), false);
	volumesFolder = "/Volumes";
	char home[3 * TEST_PATH_LENGTH];
	const char *saved = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): the tests run on one thread
	char previous[4096] = "";
	(void)snprintf(previous, sizeof previous, "%s", saved ? saved : "");
	ASSERT_BOOL_EQ(platform_toUtf8(folder, home, sizeof home), true);
	(void)setenv("HOME", home, 1); // NOLINT(concurrency-mt-unsafe)
	wchar_t desktop[TEST_PATH_LENGTH];
	wchar_t elsewhere[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		joinPath(desktop, folder, "Desktop/batches/one") && joinPath(elsewhere, folder, "batches/one"),
		true
	);
	ASSERT_INT_EQ(createBatch(plan, desktop, &host), 1);
	ASSERT_BOOL_EQ(exists(folder, "Desktop"), false);
	ASSERT_INT_EQ(createBatch(plan, elsewhere, &host), 0);
	if (saved) {
		(void)setenv("HOME", previous, 1); // NOLINT(concurrency-mt-unsafe)
	}
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}
#endif

// An arm whose line-up `fm24ctl lineup` changed (its "<side>_lineup" marker) needs keep_build=1,
// and human_control=0 when the user manages that side (home in the lineup sample, away the AI's);
// the refusal says what to set.
TEST(refuses_an_edited_line_up_it_would_not_play) {
	ByteBuffer sample = {0};
	ByteBuffer press = {0};
	static const char format[] = "\"format\": \"fm24-match-config/1\",";
	bool made = fixtureSamples_lineup(&sample);
	const char *text = buffer_string(&sample);
	const char *after = made ? strstr(text, format) : NULL;
	ASSERT_BOOL_EQ(after != NULL, true);
	if (!after) {
		buffer_free(&sample);
		return;
	}
	after += sizeof format - 1;
	static const char keepBuild[] =
		"has an edited %s line-up (%s_lineup), which rebuilding each seed puts back: set "
		"keep_build=1 to play it\n";
	static const char humanControl[] =
		"has an edited %s line-up (%s_lineup), which human_control=1 may replace with the manager's "
		"own selection: set human_control=0\n";
	static const struct {
		const char *markers;
		const char *settings;
		const char *refusal; // NULL: created
		const char *side;
	} cases[] = {
		{"\"home_lineup\": true,", "", keepBuild, "home"},
		{"\"away_lineup\": true,", "", keepBuild, "away"},
		{"\"home_lineup\": true,", "keep_build=1\n", NULL, NULL},
		{"\"home_lineup\": true,", "keep_build=1\nhuman_control=1\n", humanControl, "home"},
		{"\"away_lineup\": true,", "keep_build=1\nhuman_control=1\n", NULL, NULL},
		{"\"home_lineup\": false,", "human_control=1\n", NULL, NULL},
		{"", "human_control=1\n", NULL, NULL},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		wchar_t fixtures[TEST_PATH_LENGTH];
		char settings[256];
		(void)snprintf(settings, sizeof settings, "%s%s", standardSettings, cases[i].settings);
		buffer_clear(&press);
		bool ok = buffer_append(&press, text, (size_t)(after - text)) &&
							fixtureSamples_append(
								&press,
								"\"save\": \"LIV_COPY\", \"game_date\": \"2024 day 123 15:00\", %s",
								cases[i].markers
							) &&
							buffer_append(&press, after, sample.length - (size_t)(after - text)) &&
							makeTestFolder(folder) && joinPath(directory, folder, "batch") &&
							joinPath(fixtures, folder, "fixtures");
		ASSERT_BOOL_EQ(ok, true);
		if (!ok) {
			break;
		}
		resetFake(folder);
		ASSERT_BOOL_EQ(
			writeSources(folder, settings) && writeText(fixtures, "press.json", buffer_string(&press)),
			true
		);
#ifdef _WIN32
		int status = create(folder);
#else
		char printed[2048] = "";
		int status = printedBy(create, folder, printed, sizeof printed);
#endif
		ASSERT_INT_EQ(status, cases[i].refusal ? 1 : 0);
		ASSERT_BOOL_EQ(exists(directory, "plan.txt"), !cases[i].refusal);
		if (cases[i].refusal) {
			char path[3 * TEST_PATH_LENGTH];
			char expected[3 * TEST_PATH_LENGTH + 512];
			wchar_t wide[TEST_PATH_LENGTH];
			ASSERT_BOOL_EQ(
				joinPath(wide, fixtures, "press.json") && platform_toUtf8(wide, path, sizeof path), true
			);
			int length = snprintf(expected, sizeof expected, "arm press: %s ", path);
			(void)snprintf(
				expected + length,
				sizeof expected - (size_t)length,
				cases[i].refusal,
				cases[i].side,
				cases[i].side
			);
#ifndef _WIN32
			ASSERT_INT_EQ(strcmp(printed, expected), 0);
			if (strcmp(printed, expected) != 0) {
				fprintf(stderr, "printed:\n%s\nexpected:\n%s\n", printed, expected);
			}
#endif
		}
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
	buffer_free(&sample);
	buffer_free(&press);
}

// A batch runs to the end without kills: every index exactly once, and the files hang together.
TEST(runs_a_batch_to_the_end) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	Drive drive = {0};
	ByteBuffer hashes = {0};
	ASSERT_BOOL_EQ(driveBatch(directory, &drive), true);
	ASSERT_BOOL_EQ(checkFinished(directory, &hashes), true);
	ASSERT_BOOL_EQ(drive.rounds == 1 && fake.kills == 0, true);
	// 8 chunks: the crashes give the second chunks and arm 0's third two attempts each, arm 0's
	// last three, and the poison seed costs arm 1's third chunk two more.
	ASSERT_INT_EQ((int)drive.attempts, 15);
	ByteBuffer text = {0};
	ASSERT_BOOL_EQ(readText(directory, "chunks/arm=press/chunk=0000000120/chunk.json", &text), true);
	char excluded[96];
	(void)snprintf(
		excluded,
		sizeof excluded,
		"\"excluded\":[{\"index\":%d,\"seed\":%u,\"reason\":\"poison\"}]",
		POISON_INDEX,
		batchSeeds_scatter(9, POISON_INDEX)
	);
	ASSERT_BOOL_EQ(strstr(buffer_string(&text), excluded) != NULL, true);
	// Two attempts crashed at the poison seed, the second one marking it; the third did the rest.
	ASSERT_BOOL_EQ(strstr(buffer_string(&text), "\"unused_attempts\":[]") != NULL, true);
	ASSERT_INT_EQ((int)occurrences(buffer_string(&text), "\"outcome\":\"crashed\""), 2);
	buffer_clear(&text);
	// The attempt that crashed at once did not advance its chunk.
	ASSERT_BOOL_EQ(
		readText(directory, "chunks/arm=baseline/chunk=0000000120/chunk.json", &text), true
	);
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&text),
			"\"unused_attempts\":[{\"attempt\":1,\"first_index\":120,\"count\":10,\"verified\":0,"
			"\"outcome\":\"crashed\""
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&text),
			"\"host\":{\"power\":\"ac\",\"free_bytes\":536870912000,\"game_rss_bytes\":6123456789}"
		) != NULL,
		true
	);
	buffer_clear(&text);
	ASSERT_BOOL_EQ(readText(directory, "events.log", &text), true);
	ASSERT_INT_EQ(
		(int)occurrences(buffer_string(&text), " poison arm=press chunk=0000000120 index=124 "), 1
	);
	// The attempts after the other crashes were offered the same mark, and did not stop there.
	ASSERT_INT_EQ((int)occurrences(buffer_string(&text), " poison "), 1);
	ASSERT_INT_EQ((int)occurrences(buffer_string(&text), " part_finished "), 15);
	ASSERT_BOOL_EQ(occurrences(buffer_string(&text), " seed_errors ") > 0, true);
	buffer_free(&text);
	buffer_free(&hashes);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The proof: the runner "killed" after each step of every change (an attempt started or
// evaluated, a leftover removed, and each commit step), then restarted, ends with every index in
// exactly one committed chunk, the same contents as an uninterrupted run, and consistent files.
TEST(covers_every_index_once_whatever_step_is_killed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t pristine[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, pristine, standardSettings), true);
	ASSERT_BOOL_EQ(joinPath(directory, folder, "run"), true);
	ByteBuffer expected = {0};
	Drive drive = {0};
	fake.steps = 0;
	ASSERT_BOOL_EQ(copyTree(pristine, directory) && driveBatch(directory, &drive), true);
	ASSERT_BOOL_EQ(checkFinished(directory, &expected), true);
	unsigned steps = fake.steps;
	ASSERT_BOOL_EQ(steps > 100, true);
	unsigned covered = 0;
	for (unsigned killAt = 1; killAt <= steps; ++killAt) {
		ByteBuffer hashes = {0};
		drive = (Drive){0};
		fake.steps = 0;
		fake.kills = 0;
		fake.killAt = killAt;
		bool driven = batch_removeFolder(directory) && copyTree(pristine, directory) &&
									driveBatch(directory, &drive);
		bool ok = driven && fake.kills == 1 && checkFinished(directory, &hashes) &&
							hashes.length == expected.length &&
							memcmp(hashes.data, expected.data, hashes.length) == 0;
		if (!ok) {
			fprintf(
				stderr,
				"killed at step %u (%s, %u kills, %u rounds): not covered exactly once\n",
				killAt,
				driven ? "driven" : "not driven",
				fake.kills,
				drive.rounds
			);
		}
		covered += ok;
		buffer_free(&hashes);
	}
	ASSERT_INT_EQ((int)covered, (int)steps);
	// And killed again and again, every few steps: enough for a commit (ten steps, with the removal
	// of what the kill before left) to get through, as a process killed more often never would.
	static const unsigned spacings[] = {11, 13, 17};
	for (size_t i = 0; i < sizeof spacings / sizeof spacings[0]; ++i) {
		ByteBuffer hashes = {0};
		drive = (Drive){.killEvery = spacings[i]};
		fake.steps = 0;
		fake.kills = 0;
		fake.killAt = spacings[i];
		ASSERT_BOOL_EQ(batch_removeFolder(directory) && copyTree(pristine, directory), true);
		ASSERT_BOOL_EQ(driveBatch(directory, &drive), true);
		if (fake.kills < steps / spacings[i]) {
			fprintf(stderr, "every %u steps: %u kills\n", spacings[i], fake.kills);
		}
		ASSERT_BOOL_EQ(fake.kills >= steps / spacings[i], true);
		ASSERT_BOOL_EQ(checkFinished(directory, &hashes), true);
		ASSERT_BOOL_EQ(
			hashes.length == expected.length && memcmp(hashes.data, expected.data, hashes.length) == 0,
			true
		);
		buffer_free(&hashes);
	}
	buffer_free(&expected);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The same for a list of seeds, kept uncompressed, without per-match rows, stopping at a seed that
// fails to rebuild (none do here).
TEST(covers_a_listed_batch_once_whatever_step_is_killed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t pristine[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(folder), true);
	resetFake(folder);
	ByteBuffer list = {0};
	for (uint32_t seed = 1000; list.length < 60; seed += 7) {
		if (!isRebuildFailure(seed)) {
			appendf(&list, "%u\n", seed);
		}
	}
	ASSERT_BOOL_EQ(writeText(folder, "seeds.txt", buffer_string(&list)), true);
	ASSERT_BOOL_EQ(
		writeSources(
			folder,
			"seeds=list:seeds.txt\nchunk=4\ncompress=none\nper_match=0\nseed_errors=stop\ncanary="
			"2\norder=sequential\n"
		),
		true
	);
	ASSERT_INT_EQ(create(folder), 0);
	ASSERT_BOOL_EQ(joinPath(pristine, folder, "batch") && joinPath(directory, folder, "run"), true);
	ByteBuffer expected = {0};
	Drive drive = {0};
	fake.steps = 0;
	ASSERT_BOOL_EQ(copyTree(pristine, directory) && driveBatch(directory, &drive), true);
	ASSERT_BOOL_EQ(checkFinished(directory, &expected), true);
	ASSERT_BOOL_EQ(
		!exists(directory, "chunks/arm=baseline/chunk=0000000000/results.csv.gz") &&
			exists(directory, "chunks/arm=baseline/chunk=0000000000/results.csv"),
		true
	);
	ASSERT_BOOL_EQ(
		!exists(directory, "chunks/arm=baseline/chunk=0000000000/match-players.csv"), true
	);
	unsigned steps = fake.steps;
	unsigned covered = 0;
	for (unsigned killAt = 1; killAt <= steps; ++killAt) {
		ByteBuffer hashes = {0};
		drive = (Drive){0};
		fake.steps = 0;
		fake.kills = 0;
		fake.killAt = killAt;
		bool ok = batch_removeFolder(directory) && copyTree(pristine, directory) &&
							driveBatch(directory, &drive) && checkFinished(directory, &hashes) &&
							hashes.length == expected.length &&
							memcmp(hashes.data, expected.data, hashes.length) == 0;
		covered += ok;
		buffer_free(&hashes);
	}
	ASSERT_INT_EQ((int)covered, (int)steps);
	buffer_free(&expected);
	buffer_free(&list);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A line of the fake host's log at or after `from`; NULL if there is none.
static const char *findLine(const char *from, const char *line) {
	size_t length = strlen(line);
	for (const char *at = strstr(from, line); at; at = strstr(at + 1, line)) {
		if ((at == from || at[-1] == '\n') && (at[length] == '\n' || !at[length])) {
			return at;
		}
	}
	return NULL;
}

// What reaches the disk before what: attempt.json before its job, and a chunk's files and folder
// before the rename that commits it, which reaches the disk before its attempts go.
TEST(writes_through_in_order) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	fake.logging = true;
	buffer_clear(&fake.log);
	Drive drive = {0};
	ASSERT_BOOL_EQ(driveBatch(directory, &drive), true);
	const char *log = buffer_string(&fake.log);
	char started[16];
	char text[64];
	(void)snprintf(started, sizeof started, "step %d", (int)BATCH_STEP_ATTEMPT_STARTED);
	const char *attempt = findLine(log, started);
	ASSERT_BOOL_EQ(attempt && strstr(log, "sync file") < attempt, true);
	// The attempt's four files and its folder, then its part.json, which vouches for their rows.
	(void)snprintf(text, sizeof text, "step %d", (int)BATCH_STEP_PART_WRITTEN);
	const char *written = attempt ? findLine(attempt, text) : NULL;
	ASSERT_BOOL_EQ(written != NULL, true);
	if (attempt && written) {
		ByteBuffer between = {0};
		(void)buffer_append(&between, attempt, (size_t)(written - attempt));
		const char *files = strstr(buffer_string(&between), "\nsync file\n");
		ASSERT_BOOL_EQ(
			files && strncmp(
								 files,
								 "\nsync file\nsync file\nsync file\nsync file\nsync folder ",
								 strlen("\nsync file\nsync file\nsync file\nsync file\nsync folder ")
							 ) == 0,
			true
		);
		ASSERT_INT_EQ((int)occurrences(buffer_string(&between), "sync file\n"), 5);
		ASSERT_INT_EQ((int)occurrences(buffer_string(&between), "/a1\n"), 2);
		buffer_free(&between);
	}
	// The first commit: four files and chunk.json, then the .tmp folder, then the rename.
	(void)snprintf(text, sizeof text, "step %d", (int)BATCH_STEP_DESCRIBED);
	const char *described = findLine(log, text);
	(void)snprintf(text, sizeof text, "step %d", (int)BATCH_STEP_SYNCED);
	const char *synced = described ? findLine(described, text) : NULL;
	(void)snprintf(text, sizeof text, "step %d", (int)BATCH_STEP_RENAMED);
	const char *renamed = synced ? findLine(synced, text) : NULL;
	ASSERT_BOOL_EQ(described && synced && renamed, true);
	if (described && synced && renamed) {
		ByteBuffer between = {0};
		(void)buffer_append(&between, described, (size_t)(synced - described));
		ASSERT_INT_EQ((int)occurrences(buffer_string(&between), "sync file\n"), 5);
		ASSERT_BOOL_EQ(strstr(buffer_string(&between), "chunk=0000000100.tmp\n") != NULL, true);
		buffer_clear(&between);
		(void)buffer_append(&between, synced, (size_t)(renamed - synced));
		ASSERT_BOOL_EQ(strstr(buffer_string(&between), "arm=baseline\n") != NULL, true);
		buffer_free(&between);
		(void)snprintf(text, sizeof text, "step %d", (int)BATCH_STEP_ATTEMPT_REMOVED);
		ASSERT_BOOL_EQ(findLine(log, text) > renamed, true);
	}
	fake.logging = false;
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A commit that fails before its rename leaves no trace, and the chunk commits later.
TEST(a_failed_commit_leaves_the_chunk_to_commit_again) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch && batch_open(batch, directory, &host) == BATCH_OPENED, true);
	if (!batch) {
		return;
	}
	Drive drive = {0};
	ASSERT_BOOL_EQ(runChunk(batch, 0, 0, &drive) == BATCH_STATUS_OK, true);
	fake.failCompress = true;
	ASSERT_BOOL_EQ(runChunk(batch, 0, 0, &drive) == BATCH_STATUS_FAILED, true);
	ASSERT_BOOL_EQ(batch_committed(batch, 0, 0) == NULL && batch_pending(batch, 0, 0) != NULL, true);
	ASSERT_BOOL_EQ(exists(directory, "chunks/arm=baseline/chunk=0000000100.tmp/results.csv"), false);
	fake.failCompress = false;
	// index.csv gone meanwhile: the commit writes it whole, with its header.
	wchar_t index[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(joinPath(index, directory, "index.csv") && platform_removeFile(index), true);
	ASSERT_BOOL_EQ(runChunk(batch, 0, 0, &drive) == BATCH_STATUS_OK, true);
	ASSERT_BOOL_EQ(batch_committed(batch, 0, 0) != NULL, true);
	ASSERT_BOOL_EQ(exists(directory, "chunks/arm=baseline/chunk=0000000100/chunk.json"), true);
	ByteBuffer text = {0};
	ASSERT_BOOL_EQ(readText(directory, "index.csv", &text), true);
	ASSERT_BOOL_EQ(strncmp(buffer_string(&text), "arm,first_index,", 16) == 0, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&text), "\nbaseline,100,10,") != NULL, true);
	buffer_free(&text);
	// Nothing more to do in a committed chunk.
	BatchAttemptFacts facts = {.threads = 4, .session = "s0001"};
	BatchAttempt attempt;
	ASSERT_BOOL_EQ(batch_beginAttempt(batch, 0, 0, &facts, &attempt) == BATCH_STATUS_FAILED, true);
	ASSERT_BOOL_EQ(batch_commit(batch, 0, 0, NULL) == BATCH_STATUS_FAILED, true);
	batch_close(batch);
	free(batch);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The output of a command that writes to a FILE.
static bool readBack(FILE *file, ByteBuffer *out) {
	char block[4096];
	size_t got = 0;
	rewind(file);
	while ((got = fread(block, 1, sizeof block, file)) > 0) {
		if (!buffer_append(out, block, got)) {
			return false;
		}
	}
	return true;
}

// A batch of one chunk per arm says "chunk", not "chunks".
TEST(counts_a_single_chunk_in_the_singular) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t report[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		createdBatch(folder, directory, "seeds=range:1-10\nchunk=10\nthreads=4\ncanary=3\n") &&
			joinPath(report, folder, "status.txt"),
		true
	);
	BatchHost host = fakeHost();
	FILE *out = platform_openFile(report, "w+b");
	ASSERT_BOOL_EQ(out != NULL, true);
	if (!out) {
		return;
	}
	ASSERT_INT_EQ(showStatus(directory, &host, out), 0);
	ByteBuffer text = {0};
	ASSERT_BOOL_EQ(readBack(out, &text), true);
	(void)fclose(out);
	const char *status = buffer_string(&text);
	static const char *const lines[] = {
		"progress: 0 of 20 indices (0.0%) in 0 of 2 chunks\n",
		"arm baseline: 0 of 1 chunk; ",
		"arm press: 0 of 1 chunk; ",
	};
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
		if (!strstr(status, lines[i])) {
			fprintf(stderr, "missing \"%s\" in:\n%s", lines[i], status);
		}
		ASSERT_BOOL_EQ(strstr(status, lines[i]) != NULL, true);
	}
	buffer_free(&text);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

TEST(shows_a_batch_status) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t report[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		createdBatch(folder, directory, standardSettings) && joinPath(report, folder, "status.txt"),
		true
	);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch && batch_open(batch, directory, &host) == BATCH_OPENED, true);
	if (!batch) {
		return;
	}
	Drive drive = {0};
	// Chunk 0 of each arm, then an attempt at baseline's chunk 1 that crashes after three.
	for (int step = 0; step < 5; ++step) {
		size_t arm = 0;
		uint64_t chunk = 0;
		ASSERT_BOOL_EQ(
			batch_nextChunk(batch, &arm, &chunk) &&
				runChunk(batch, arm, chunk, &drive) == BATCH_STATUS_OK,
			true
		);
	}
	batch_close(batch);
	free(batch);
	FILE *out = platform_openFile(report, "w+b");
	ASSERT_BOOL_EQ(out != NULL, true);
	if (!out) {
		return;
	}
	fake.now += 60;
	ASSERT_INT_EQ(showStatus(directory, &host, out), 0);
	ByteBuffer text = {0};
	ASSERT_BOOL_EQ(readBack(out, &text), true);
	(void)fclose(out);
	const char *status = buffer_string(&text);
	struct tm eta;
	char line[256];
	ASSERT_BOOL_EQ(platform_utcTime(fake.now + 23, &eta), true); // (68 - 20 - 3) / 2 per second
	(void)snprintf(
		line,
		sizeof line,
		"last: chunk press/0000000100 ok 10 in 5.0 s (2.00/s); 20/68 (29.4%%); 2.00/s over 2 chunks; "
		"ETA %04d-%02d-%02d %02d:%02d UTC\n",
		eta.tm_year + 1900,
		eta.tm_mon + 1,
		eta.tm_mday,
		eta.tm_hour,
		eta.tm_min
	);
	static const char *const lines[] = {
		"batch test-batch: 2 arms, chunks of 10, interleaved order\n",
		"progress: 20 of 68 indices (29.4%) in 2 of 8 chunks, and 3 in parts of chunks not yet "
		"committed\n",
		"next: chunk baseline/0000000110, 3 of 10 indices done in 1 attempt; the last, a1, ended "
		"crashed\n",
		"arm baseline: 1 of 4 chunks; ",
		"arm press: 1 of 4 chunks; ",
		"attempts: 3, 2 ok, 1 crashed; 0 poison seeds; 0 s asleep\n",
		"state: running at ",
	};
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
		if (!strstr(status, lines[i])) {
			fprintf(stderr, "missing \"%s\" in:\n%s", lines[i], status);
		}
		ASSERT_BOOL_EQ(strstr(status, lines[i]) != NULL, true);
	}
	ASSERT_BOOL_EQ(strstr(status, line) != NULL, true);
	buffer_free(&text);
	// A changed input is a frozen file that no longer matches: exit code 2.
	ASSERT_BOOL_EQ(writeText(directory, "inputs/press.json", "{}"), true);
	ASSERT_INT_EQ(showStatus(directory, &host, stderr), 2);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Replaces the first `from` in a file with `to`.
static bool editFile(const wchar_t *directory, const char *name, const char *from, const char *to) {
	ByteBuffer text = {0};
	ByteBuffer edited = {0};
	const char *at = readText(directory, name, &text) ? strstr(buffer_string(&text), from) : NULL;
	bool ok = at && buffer_append(&edited, text.data, (size_t)(at - (const char *)text.data)) &&
						buffer_append(&edited, to, strlen(to)) &&
						buffer_append(&edited, at + strlen(from), strlen(at + strlen(from))) &&
						writeText(directory, name, buffer_string(&edited));
	buffer_free(&text);
	buffer_free(&edited);
	return ok;
}

// Appends `text` to a committed file, kept by the fake compression, and changes chunk.json's
// record of it to match: its sizes, its uncompressed and stored SHA-256 and its content SHA-256.
static bool appendToChunkFile(
	const wchar_t *directory,
	const char *arm,
	const char *id,
	const char *name,
	const char *text
) {
	char relative[128];
	char manifest[128];
	(void)snprintf(relative, sizeof relative, "chunks/arm=%s/chunk=%s/%s.gz", arm, id, name);
	(void)snprintf(manifest, sizeof manifest, "chunks/arm=%s/chunk=%s/chunk.json", arm, id);
	ByteBuffer stored = {0};
	ByteBuffer raw = {0};
	wchar_t path[TEST_PATH_LENGTH];
	size_t magic = sizeof fakeMagic - 1;
	bool ok = readText(directory, relative, &stored) && stored.length >= magic;
	for (size_t i = magic; ok && i < stored.length; ++i) {
		uint8_t byte = stored.data[i] ^ 0x5A;
		ok = buffer_append(&raw, &byte, 1);
	}
	ManifestOutput before;
	ManifestOutput after;
	ResultsSummary summary;
	char rawName[64];
	(void)snprintf(rawName, sizeof rawName, "%s-raw.csv", name);
	ok = ok && joinPath(path, directory, rawName) && files_write(path, raw.data, raw.length) &&
			 manifest_readOutput(path, &before, &summary) && buffer_append(&raw, text, strlen(text)) &&
			 files_write(path, raw.data, raw.length) && manifest_readOutput(path, &after, &summary) &&
			 platform_removeFile(path);
	char storedBefore[SHA256_HEX_LENGTH];
	char storedAfter[SHA256_HEX_LENGTH];
	sha256_hexOf(stored.data, stored.length, storedBefore);
	for (size_t i = 0; ok && i < strlen(text); ++i) {
		uint8_t byte = (uint8_t)text[i] ^ 0x5A;
		ok = buffer_append(&stored, &byte, 1);
	}
	sha256_hexOf(stored.data, stored.length, storedAfter);
	char from[512];
	char to[512];
	(void)snprintf(
		from,
		sizeof from,
		"\"raw_bytes\":%llu,\"raw_sha256\":\"%s\",\"content_sha256\":\"%s\",\"bytes\":%zu,\"sha256\":"
		"\"%s\"",
		(unsigned long long)before.bytes,
		before.sha256,
		before.contentSha256,
		stored.length - strlen(text),
		storedBefore
	);
	(void)snprintf(
		to,
		sizeof to,
		"\"raw_bytes\":%llu,\"raw_sha256\":\"%s\",\"content_sha256\":\"%s\",\"bytes\":%zu,\"sha256\":"
		"\"%s\"",
		(unsigned long long)after.bytes,
		after.sha256,
		after.contentSha256,
		stored.length,
		storedAfter
	);
	ok = ok && joinPath(path, directory, relative) && files_write(path, stored.data, stored.length) &&
			 editFile(directory, manifest, from, to);
	buffer_free(&stored);
	buffer_free(&raw);
	return ok;
}

static int
verifyQuietly(const wchar_t *folder, const wchar_t *directory, bool deep, ByteBuffer *report) {
	wchar_t path[TEST_PATH_LENGTH];
	BatchHost host = fakeHost();
	FILE *out = joinPath(path, folder, "verify.txt") ? platform_openFile(path, "w+b") : NULL;
	int status = out ? verifyBatch(directory, deep, &host, out) : -1;
	buffer_clear(report);
	if (out) {
		(void)readBack(out, report);
		(void)fclose(out);
	}
	return status;
}

TEST(verifies_committed_chunks) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	Drive drive = {0};
	ASSERT_BOOL_EQ(driveBatch(directory, &drive), true);
	ByteBuffer report = {0};
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 0);
	ASSERT_BOOL_EQ(
		strstr(buffer_string(&report), "verified 8 committed chunks (68 indices): no problems") != NULL,
		true
	);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, true, &report), 0);
	// A stored byte changed.
	static const char file[] = "chunks/arm=press/chunk=0000000110/match-teams.csv.gz";
	ByteBuffer original = {0};
	ASSERT_BOOL_EQ(readText(directory, file, &original), true);
	ByteBuffer changed = {0};
	(void)buffer_append(&changed, original.data, original.length);
	changed.data[changed.length / 2] ^= 1;
	wchar_t path[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		joinPath(path, directory, file) && files_write(path, changed.data, changed.length), true
	);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 1);
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&report),
			"chunk press/0000000110: match-teams.csv.gz does not match its size and SHA-256 in chunk.json"
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(files_write(path, original.data, original.length), true);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 0);
	// Its last row dropped and the stored file's size and hash updated to match: only --deep sees
	// that the rows no longer cover the chunk.
	char oldHash[SHA256_HEX_LENGTH];
	char newHash[SHA256_HEX_LENGTH];
	size_t magic = sizeof fakeMagic - 1;
	size_t cut = original.length - 1;
	while (cut > magic && (original.data[cut - 1] ^ 0x5A) != '\n') {
		--cut;
	}
	sha256_hexOf(original.data, original.length, oldHash);
	sha256_hexOf(original.data, cut, newHash);
	char from[160];
	char to[160];
	(void)snprintf(from, sizeof from, "\"bytes\":%zu,\"sha256\":\"%s\"", original.length, oldHash);
	(void)snprintf(to, sizeof to, "\"bytes\":%zu,\"sha256\":\"%s\"", cut, newHash);
	ASSERT_BOOL_EQ(files_write(path, original.data, cut), true);
	ASSERT_BOOL_EQ(
		editFile(directory, "chunks/arm=press/chunk=0000000110/chunk.json", from, to), true
	);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 0);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, true, &report), 1);
	ASSERT_BOOL_EQ(
		strstr(buffer_string(&report), "chunk press/0000000110: its rows cover 9 of its 10 indices") !=
			NULL,
		true
	);
	// A torn line after the last results row, with every size and hash in chunk.json made to
	// match: only --deep, reading past the rows, sees it.
	ASSERT_BOOL_EQ(
		appendToChunkFile(directory, "baseline", "0000000110", "results.csv", "torn"), true
	);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 0);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, true, &report), 1);
	ASSERT_BOOL_EQ(
		strstr(
			buffer_string(&report),
			"chunk baseline/0000000110: its rows cover 10 of its 10 indices, with rows past them"
		) != NULL,
		true
	);
	// A file that should not be there, and a chunk committed under another plan.
	ASSERT_BOOL_EQ(writeText(directory, "chunks/arm=baseline/chunk=0000000100/extra.csv", "x"), true);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 1);
	ASSERT_BOOL_EQ(
		strstr(buffer_string(&report), "chunk baseline/0000000100: has 6 files, not 5") != NULL, true
	);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch && batch_open(batch, directory, &host) == BATCH_OPENED, true);
	char hash[SHA256_HEX_LENGTH] = "";
	if (batch) {
		(void)snprintf(hash, sizeof hash, "%s", batch->plan.planSha256);
		batch_close(batch);
	}
	free(batch);
	char other[SHA256_HEX_LENGTH];
	(void)snprintf(other, sizeof other, "%s", hash);
	other[0] = other[0] == '0' ? '1' : '0';
	ASSERT_BOOL_EQ(
		editFile(directory, "chunks/arm=baseline/chunk=0000000100/chunk.json", hash, other), true
	);
	ASSERT_INT_EQ(verifyQuietly(folder, directory, false, &report), 2);
	buffer_free(&original);
	buffer_free(&changed);
	buffer_free(&report);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

TEST(stops_a_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	ASSERT_INT_EQ(stopBatch(directory, false), 0);
	ASSERT_BOOL_EQ(exists(directory, "control/stop") && !exists(directory, "control/stop-now"), true);
	ASSERT_INT_EQ(stopBatch(directory, true), 0);
	ASSERT_BOOL_EQ(exists(directory, "control/stop-now"), true);
	ASSERT_INT_EQ(stopBatch(folder, false), 1);
	wchar_t *stopping[] = {L"fm24ctl", L"batch", L"stop", directory, L"--later", NULL};
	ASSERT_INT_EQ(batchTest_command(5, stopping), 2);
	stopping[2] = L"run";
	ASSERT_INT_EQ(batchTest_command(5, stopping), 2);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A plan.txt or input changed after create, or another plan's chunk, means exit code 2 everywhere.
TEST(refuses_a_changed_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch != NULL, true);
	if (!batch) {
		return;
	}
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPENED, true);
	batch_close(batch);
	ASSERT_BOOL_EQ(editFile(directory, "plan.txt", "\nthreads=4\n", "\nthreads=5\n"), true);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPEN_CHANGED, true);
	ASSERT_BOOL_EQ(editFile(directory, "plan.txt", "\nthreads=5\n", "\nthreads=4\n"), true);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPENED, true);
	batch_close(batch);
	ASSERT_BOOL_EQ(editFile(directory, "inputs/baseline.bin", "", "\x01"), true);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPEN_CHANGED, true);
	wchar_t *arguments[] = {L"fm24ctl", L"batch", L"verify", directory, NULL};
	ASSERT_INT_EQ(batchTest_command(4, arguments), 2);
	free(batch);
	// Not a batch at all.
	ASSERT_INT_EQ(showStatus(folder, &host, stderr), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// What a stopped process leaves is removed by the next start: unfinished chunk folders, attempts
// that never started a job, a committed chunk's attempts and parts off their chunk's chain.
TEST(tidies_what_a_stopped_process_left) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdBatch(folder, directory, standardSettings), true);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch && batch_open(batch, directory, &host) == BATCH_OPENED, true);
	if (!batch) {
		return;
	}
	Drive drive = {0};
	// baseline's chunk 0 committed, then an attempt at press's chunk 0 that gives no rows.
	ASSERT_BOOL_EQ(
		runChunk(batch, 0, 0, &drive) == BATCH_STATUS_OK &&
			runChunk(batch, 0, 0, &drive) == BATCH_STATUS_OK,
		true
	);
	BatchAttemptFacts facts = {
		.threads = 4, .session = "s0001", .startedAt = fake.now, .maxCount = 3};
	BatchAttempt attempt;
	BatchPartFacts crashed = {.outcome = BATCH_PART_CRASHED, .finishedAt = fake.now};
	ASSERT_BOOL_EQ(
		batch_beginAttempt(batch, 1, 0, &facts, &attempt) == BATCH_STATUS_OK && attempt.count == 3, true
	);
	ASSERT_BOOL_EQ(
		batch_finishAttempt(batch, 1, 0, attempt.attempt, &crashed) == BATCH_STATUS_OK, true
	);
	batch_close(batch);
	// A part that starts where no part ends, an attempt that wrote only a temporary attempt.json,
	// leftovers of a commit, and a committed chunk's attempt.
	static const char *const files[][2] = {
		{"attempts/press/0000000100/a1/attempt.json", "attempts/press/0000000100/a7/attempt.json"},
		{"attempts/press/0000000100/a1/part.json", "attempts/press/0000000100/a7/part.json"},
	};
	for (size_t i = 0; i < 2; ++i) {
		ByteBuffer text = {0};
		wchar_t a7[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(
			joinPath(a7, directory, "attempts/press/0000000100/a7") && platform_makeDirectory(a7), true
		);
		ASSERT_BOOL_EQ(
			readText(directory, files[i][0], &text) &&
				writeText(directory, files[i][1], buffer_string(&text)),
			true
		);
		ASSERT_BOOL_EQ(
			editFile(
				directory,
				files[i][1],
				"\"attempt\":1,\"first_index\":100,",
				"\"attempt\":7,\"first_index\":105,"
			),
			true
		);
		buffer_free(&text);
	}
	static const char *const folders[] = {
		"attempts/press/0000000110",
		"attempts/press/0000000110/a1",
		"chunks/arm=press",
		"chunks/arm=press/chunk=0000000110.tmp",
		"chunks/arm=press/chunk=0000000120",
		"attempts/baseline/0000000100",
		"attempts/baseline/0000000100/a3",
	};
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
		wchar_t path[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(joinPath(path, directory, folders[i]) && platform_makeDirectory(path), true);
	}
	ASSERT_BOOL_EQ(
		writeText(directory, "attempts/press/0000000110/a1/attempt.json.tmp", "{") &&
			writeText(directory, "attempts/press/0000000110/a1/results.csv", "seed\n"),
		true
	);
	ASSERT_BOOL_EQ(
		writeText(directory, "chunks/arm=press/chunk=0000000110.tmp/results.csv", "x") &&
			writeText(directory, "chunks/arm=press/chunk=0000000120/results.csv.gz", "x"),
		true
	);
	ASSERT_BOOL_EQ(writeText(directory, "attempts/baseline/0000000100/a3/results.csv", "x"), true);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPENED, true);
	ASSERT_INT_EQ((int)batch->leftoverCount, 4);
	const BatchChunk *pending = batch_pending(batch, 1, 0);
	ASSERT_BOOL_EQ(pending && pending->partCount == 2, true);
	BatchRecovery recovery;
	ASSERT_BOOL_EQ(batch_recover(batch, BATCH_PART_CRASHED, &recovery) == BATCH_STATUS_OK, true);
	ASSERT_BOOL_EQ(
		recovery.removed == 4 && recovery.stale == 1 && recovery.evaluated == 0 &&
			recovery.committed == 0,
		true
	);
	pending = batch_pending(batch, 1, 0);
	ASSERT_BOOL_EQ(pending && pending->partCount == 1 && pending->parts[0].attempt == 1, true);
	static const char *const gone[] = {
		"attempts/press/0000000100/a7/part.json",
		"attempts/press/0000000110/a1/results.csv",
		"chunks/arm=press/chunk=0000000110.tmp/results.csv",
		"chunks/arm=press/chunk=0000000120/results.csv.gz",
		"attempts/baseline/0000000100/a3/results.csv",
	};
	for (size_t i = 0; i < sizeof gone / sizeof gone[0]; ++i) {
		ASSERT_BOOL_EQ(exists(directory, gone[i]), false);
	}
	ASSERT_BOOL_EQ(exists(directory, "attempts/press/0000000100/a1/part.json"), true);
	// The next attempt starts where the part ends, as attempt 2.
	facts.maxCount = 0;
	ASSERT_BOOL_EQ(batch_beginAttempt(batch, 1, 0, &facts, &attempt) == BATCH_STATUS_OK, true);
	ASSERT_BOOL_EQ(attempt.attempt == 2 && attempt.first == 100 && attempt.count == 10, true);
	// Its job is not evaluated yet, so no other attempt can start.
	ASSERT_BOOL_EQ(batch_beginAttempt(batch, 1, 0, &facts, &attempt) == BATCH_STATUS_FAILED, true);
	batch_close(batch);
	free(batch);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

TEST(reads_json_back_by_path) {
	static const char text[] =
		" {\"a\":{\"b\":[1,{\"c\":\"x\\\"y\"}],\"t\":true,\"f\":false,\"n\":null},\"d\":-1.5e3,"
		"\"at\":\"2026-10-25T09:23:18Z\",\"leap\":\"2024-02-29T23:59:59Z\",\"big\":"
		"18446744073709551615} ";
	JsonFlat flat = {0};
	char value[16];
	uint64_t number = 0;
	bool flag = false;
	char kind = 0;
	time_t at = 0;
	ASSERT_BOOL_EQ(jsonFlat_read(&flat, text, sizeof text - 1), true);
	ASSERT_BOOL_EQ(jsonFlat_unsigned(&flat, "a.b.0", &number) && number == 1, true);
	ASSERT_BOOL_EQ(
		jsonFlat_string(&flat, "a.b.1.c", value, sizeof value) && strcmp(value, "x\"y") == 0, true
	);
	ASSERT_BOOL_EQ(jsonFlat_string(&flat, "a.b.1.c", value, 3), false);
	ASSERT_BOOL_EQ(jsonFlat_bool(&flat, "a.t", &flag) && flag, true);
	ASSERT_BOOL_EQ(jsonFlat_bool(&flat, "a.f", &flag) && !flag, true);
	ASSERT_BOOL_EQ(jsonFlat_isNull(&flat, "a.n") && !jsonFlat_isNull(&flat, "a.t"), true);
	ASSERT_BOOL_EQ(
		jsonFlat_find(&flat, "d", &kind) && kind == 'n' && !jsonFlat_unsigned(&flat, "d", &number), true
	);
	ASSERT_BOOL_EQ(jsonFlat_unsigned(&flat, "big", &number) && number == UINT64_MAX, true);
	ASSERT_BOOL_EQ(
		jsonFlat_find(&flat, "a", &kind) == NULL && jsonFlat_find(&flat, "a.b.2", &kind) == NULL, true
	);
	ASSERT_BOOL_EQ(jsonFlat_time(&flat, "at", &at) && at == 1792920198, true);
	ASSERT_BOOL_EQ(jsonFlat_time(&flat, "leap", &at) && at == 1709251199, true);
	ASSERT_BOOL_EQ(jsonFlat_time(&flat, "d", &at), false);
	static const char *const bad[] = {"", "{", "{\"a\":1}x", "[1,]", "{\"a\":\"\\u0000\"}", "nul"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		ASSERT_BOOL_EQ(jsonFlat_read(&flat, bad[i], strlen(bad[i])), false);
		ASSERT_BOOL_EQ(flat.entries.length == 0, true);
	}
	jsonFlat_free(&flat);
}

// ---------------------------------------------------------------------------------------------
// The fake game batch run plays against: a harness that serves status, lookup and run jobs, plays
// each seed as the verifier's tests' rows, moves the clocks, ticks the watchdog and has faults

static void doActions(void);

static void moveClocks(uint64_t milliseconds) {
	fake.game.uptime += milliseconds;
	fake.game.monotonic += milliseconds;
	doActions();
}

static bool tickWatch(ClientWatch *watch, ClientPhase phase) {
	return !watch || !watch->tick || !watch->tickMs || watch->tick(watch->context, phase);
}

// Ticks every BATCH_TICK_S of fake time (every tickMs with a fine clock) until the watchdog gives
// up. False once it has; true if it never did.
static bool tickUntilAbandoned(ClientWatch *watch, ClientPhase phase) {
	uint64_t step =
		fake.game.fineClock && watch && watch->tickMs ? watch->tickMs : BATCH_TICK_S * 1000;
	for (unsigned i = 0; i < 200000; ++i) {
		moveClocks(step);
		if (!tickWatch(watch, phase)) {
			return false;
		}
	}
	return true;
}

static void launchGame(bool rosetta) {
	FakeGame *game = &fake.game;
	++game->launches;
	game->running = true;
	game->listening = true;
	game->pid = 4000 + game->launches;
	game->loaded = (int64_t)fakeNow(NULL);
	game->jobs = 0;
	game->slice = rosetta ? BATCH_SLICE_X86_64 : game->launchSlice;
	game->launchSlice = BATCH_SLICE_ARM64;
	game->background.active = false;
}

static void killGame(void) {
	fake.game.running = false;
	fake.game.listening = false;
	fake.game.background.active = false;
}

// control/<name> as the host's signal handler makes it: created if missing, never rewritten.
static void signalFile(const char *name) {
	wchar_t path[TEST_PATH_LENGTH];
	char relative[32];
	(void)snprintf(relative, sizeof relative, "control/%s", name);
	FILE *file = joinPath(path, fake.game.actionBatch, relative) ? platform_openFile(path, "ab") : NULL;
	if (file) {
		(void)fclose(file);
	}
}

// Does the actions that are due.
static void doActions(void) {
	FakeGame *game = &fake.game;
	for (size_t i = 0; i < game->actionCount; ++i) {
		Action *action = &game->actions[i];
		bool due = action->query        ? game->statusQueries >= action->query
							 : action->powerCheck ? game->powerChecks >= action->powerCheck
							 : action->uptime
								 ? game->uptime >= action->uptime
								 : game->hangStartedAt && game->uptime - game->hangStartedAt >= action->afterMs;
		if (action->at || !due) {
			continue;
		}
		action->at = game->uptime;
		switch (action->kind) {
			case ACT_STOP: (void)stopBatch(game->actionBatch, false); break;
			case ACT_STOP_NOW: (void)stopBatch(game->actionBatch, true); break;
			case ACT_SIGNAL: signalFile(++game->signals == 1 ? "stop" : "stop-now"); break;
			case ACT_QUIT: killGame(); break;
		}
	}
}

// Does `kind` to the batch in `directory` at the time `action` gives.
static void addAction(const wchar_t *directory, ActionKind kind, Action when) {
	FakeGame *game = &fake.game;
	wcscpy(game->actionBatch, directory);
	if (game->actionCount < sizeof game->actions / sizeof game->actions[0]) {
		when.kind = kind;
		when.at = 0;
		game->actions[game->actionCount++] = when;
	}
}

static bool fileThere(const wchar_t *path) {
	FILE *file = platform_openFile(path, "rb");
	if (file) {
		(void)fclose(file);
	}
	return file != NULL;
}

// The harness's name for a results file's sibling: X.csv, X-match-players.csv...
static bool siblingPath(const wchar_t *output, const char *suffix, wchar_t *out) {
	size_t length = wcslen(output);
	size_t extra = strlen(suffix);
	if (length < 4 || length + extra >= TEST_PATH_LENGTH) {
		return false;
	}
	wmemcpy(out, output, length - 4);
	for (size_t i = 0; i < extra; ++i) {
		out[length - 4 + i] = (wchar_t)suffix[i];
	}
	wmemcpy(out + length - 4 + extra, L".csv", 5);
	return true;
}

static bool appendTo(const wchar_t *output, const char *suffix, const ByteBuffer *text) {
	wchar_t path[TEST_PATH_LENGTH];
	return siblingPath(output, suffix, path) && appendFile(path, text->data, text->length);
}

// Whether the fake game's results differ from now on: a session or a job from which they do not
// repeat.
static bool varied(void) {
	const FakeGame *game = &fake.game;
	return (game->variedFrom && game->launches >= game->variedFrom) ||
				 (game->variedFromJob && game->runJobs >= game->variedFromJob);
}

// FM24's file-list error log entry for a seed's build, in the log of the game's session.
static void addGameLog(const Job *job) {
	if (!fake.logs[0] || job->keepBuild) {
		return;
	}
	char name[64];
	(void)snprintf(
		name, sizeof name, "file llist errors_2026.10.26_10.%02u.00.txt", fake.game.launches % 60
	);
	static const char entry[] = "get_list outside of main thread\n";
	wchar_t path[TEST_PATH_LENGTH];
	(void)(joinPath(path, fake.logs, name) && appendFile(path, entry, sizeof entry - 1));
}

// The base of a season's fake game, and the teams it plays at home and away.
enum { BASE_HOME = 11, BASE_AWAY = 22 };

// A season fixture as the fake game builds it: its actual teams in its fields, so that its encoding
// holds them, its team overrides and the in-game date.
static bool writeSeasonFixture(
	const wchar_t *path,
	const uint32_t actual[2],
	const uint32_t overrides[2],
	const char *date
) {
	char teams[64] = "";
	char text[512];
	if (overrides[0]) {
		(void)snprintf(teams, sizeof teams, ",\"home_team\":%u", overrides[0]);
	}
	if (overrides[1]) {
		(void)snprintf(
			teams + strlen(teams), sizeof teams - strlen(teams), ",\"away_team\":%u", overrides[1]
		);
	}
	int length = snprintf(
		text,
		sizeof text,
		"{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"save\":\"LIV_COPY\","
		"\"game_date\":\"%s\"%s,\"fields\":[{\"u32\":%u},{\"u32\":%u}]}",
		date,
		teams,
		actual[0],
		actual[1]
	);
	return files_write(path, text, (size_t)length);
}

// The actual teams a season fixture's encoding holds; false for another fixture's.
static bool readSeasonTeams(const wchar_t *path, uint32_t teams[2]) {
	ByteBuffer bytes = {0};
	bool ok = files_read(path, &bytes) && bytes.length == 8;
	for (size_t side = 0; ok && side < 2; ++side) {
		const uint8_t *at = bytes.data + 4 * side;
		teams[side] = at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
	}
	buffer_free(&bytes);
	return ok;
}

// The teams a run job's season fixture plays: its overrides, else those its encoding holds; none
// for another fixture.
static void noteJobTeams(const Job *job) {
	uint32_t *teams = fake.game.jobTeams;
	teams[0] = teams[1] = 0;
	if (readSeasonTeams(job->input, teams)) {
		for (size_t side = 0; side < 2; ++side) {
			teams[side] =
				job->teams[side][0] ? (uint32_t)strtoul(job->teams[side], NULL, 10) : teams[side];
		}
	}
}

// Per-match rows ("index,seed,side,team_uid,...") with each side's team_uid `teams`.
static bool setRowTeams(ByteBuffer *rows, const uint32_t teams[2]) {
	ByteBuffer out = {0};
	bool ok = true;
	for (const char *line = buffer_string(rows); ok && *line;) {
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line) + 1 : strlen(line);
		const char *fields[4] = {line}; // where index, seed, side and team_uid start
		for (size_t i = 1; i < 4 && fields[i - 1]; ++i) {
			const char *comma = strchr(fields[i - 1], ',');
			fields[i] = comma ? comma + 1 : NULL;
		}
		const char *after = fields[3] ? strchr(fields[3], ',') : NULL;
		ok = after != NULL;
		if (ok) {
			bool away = strncmp(fields[2], "away,", 5) == 0;
			ok = buffer_append(&out, line, (size_t)(fields[3] - line));
			appendf(&out, "%u", teams[away]);
			ok = ok && buffer_append(&out, after, length - (size_t)(after - line));
		}
		line += length;
	}
	buffer_free(rows);
	*rows = out;
	return ok;
}

// A replacement job: the input's fixture with one side's team replaced, written to its output, as
// the harness does; the override of a side that gets the base's own team back is dropped.
static ClientOutcome fakeReplace(const char *text, char *result, size_t size) {
	FakeGame *game = &fake.game;
	unsigned number = ++game->replaceJobs;
	static char copy[HARNESS_JOB_LENGTH + 1];
	(void)snprintf(copy, sizeof copy, "%s", text);
	Job job;
	if (!job_parse(copy, &job)) {
		(void)snprintf(result, size, "error: cannot read the job; see harness.log");
		return CLIENT_REPLIED;
	}
	moveClocks(1000);
	addGameLog(&job);
	unsigned side = job.command == JOB_REPLACE_AWAY;
	uint32_t team = (uint32_t)strtoul(job.team, NULL, 10);
	uint32_t actual[2] = {0};
	uint32_t overrides[2] = {
		(uint32_t)strtoul(job.teams[0], NULL, 10), (uint32_t)strtoul(job.teams[1], NULL, 10)};
	ClientOutcome outcome = CLIENT_REPLIED;
	if (number == game->replaceStopAt) {
		wchar_t stop[TEST_PATH_LENGTH];
		(void)(joinPath(stop, game->actionBatch, "control/stop") && files_write(stop, "", 0));
	}
	if (!readSeasonTeams(job.input, actual)) {
		(void)snprintf(result, size, "error: cannot read the input, or it is not a serialised match");
	} else if (number == game->replaceErrorAt || !team) {
		(void)snprintf(result, size, "error: no team matches %s", job.team);
	} else {
		actual[side] = team;
		overrides[side] = team == (side ? BASE_AWAY : BASE_HOME) ? 0 : team;
		overrides[1 - side] += number == game->replaceOverrideAt;
		const char *date = number == game->replaceDateAt ? "2024 day 124 15:00" : game->date;
		if (!writeSeasonFixture(job.output, actual, overrides, date)) {
			(void)snprintf(result, size, "error: serialise or write failed; see harness.log");
		} else if (number == game->replaceCrashAt) {
			killGame();
			outcome = CLIENT_LOST;
		} else {
			(void)snprintf(
				result,
				size,
				"ok: Team %u vs Team %u, %s, seed 42 (team %u); %u edits kept, 0 edits to the old %s team "
				"dropped",
				actual[0],
				actual[1],
				game->date,
				team,
				number == game->replaceEditsAt ? 1U : 0U,
				side ? "away" : "home"
			);
		}
	}
	job_free(&job);
	return outcome;
}

// Writes the rows of the seed at `position`: a failed row for one that fails to rebuild (with
// seed_errors=skip), else its per-match rows and then its results row, half of it if `torn`.
static bool playSeed(const Job *job, size_t position, bool torn, size_t *skipped) {
	addGameLog(job);
	uint64_t index = job->indexBase + position;
	uint32_t seed = job->seeds[position];
	ByteBuffer row = {0};
	bool ok = true;
	if (job->skipSeedErrors && isRebuildFailure(seed)) {
		appendf(&row, "%llu,%u,rebuild\n", (unsigned long long)index, seed);
		ok = appendTo(job->output, "-failed", &row);
		++*skipped;
		buffer_free(&row);
		return ok;
	}
	if (job->perMatch) {
		ByteBuffer players = {0};
		ByteBuffer teams = {0};
		const uint32_t *season = fake.game.jobTeams;
		ok = dllTest_formatRows(seed, index, &players, &teams) &&
				 (!season[0] || (setRowTeams(&players, season) && setRowTeams(&teams, season))) &&
				 appendTo(job->output, "-match-players", &players) &&
				 appendTo(job->output, "-match-teams", &teams);
		buffer_free(&players);
		buffer_free(&teams);
	}
	appendResult(&row, varied() ? seed + 1 : seed);
	if (varied()) {
		// The same seed, another result: the first field is the seed again.
		ByteBuffer fixed = {0};
		const char *comma = strchr(buffer_string(&row), ',');
		appendf(&fixed, "%u%s", seed, comma ? comma : "\n");
		buffer_free(&row);
		row = fixed;
	}
	ok = ok && appendFile(job->output, row.data, torn ? row.length / 2 : row.length);
	buffer_free(&row);
	return ok;
}

// The files of a job from its first seed: each with its header.
static bool startFiles(const Job *job) {
	bool ok =
		files_write(job->output, batchVerify_partResultsHeader, strlen(batchVerify_partResultsHeader));
	wchar_t path[TEST_PATH_LENGTH];
	if (ok && job->perMatch) {
		ok = siblingPath(job->output, "-match-players", path) &&
				 files_write(path, batchVerify_playersHeader, strlen(batchVerify_playersHeader)) &&
				 siblingPath(job->output, "-match-teams", path) &&
				 files_write(path, batchVerify_teamsHeader, strlen(batchVerify_teamsHeader));
	}
	if (ok && job->skipSeedErrors) {
		ok = siblingPath(job->output, "-failed", path) &&
				 files_write(path, batchVerify_failedHeader, strlen(batchVerify_failedHeader));
	}
	return ok;
}

// The result line the harness gives a run job with report=1.
static void formatRunResult(
	const Job *job,
	size_t written,
	size_t skipped,
	bool stopped,
	bool hasFailure,
	uint64_t failedIndex,
	char *result,
	size_t size
) {
	const FakeGame *game = &fake.game;
	bool ok = written == job->seedCount && !stopped && !hasFailure;
	char skippedText[48] = "";
	char failedText[24] = "-";
	if (skipped) {
		(void)snprintf(skippedText, sizeof skippedText, ", %zu skipped (rebuild failed)", skipped);
	}
	if (hasFailure) {
		(void)snprintf(failedText, sizeof failedText, "%llu", (unsigned long long)failedIndex);
	}
	(void)snprintf(
		result,
		size,
		"%s: %zu/%zu runs%s%s%s%s; report protocol=%s commit=%s dirty=0 build=%u slice=%s "
		"translated=%d pid=%u loaded=%lld jobs=%llu threads=%u edits=%s elapsed_ms=%llu written=%zu "
		"skipped=%zu stopped=%d failed_index=%s failed_stage=%s output_failed=0",
		ok ? "ok" : "error",
		written,
		job->seedCount,
		skippedText,
		stopped ? ", stopped by the stop file" : "",
		job->keepBuild ? "" : ", each seed rebuilt by the game",
		hasFailure ? "; see harness.log" : "",
		HARNESS_PROTOCOL_TEXT,
		game->commit,
		game->build,
		game->slice == BATCH_SLICE_X86_64 ? "x86_64" : "arm64",
		game->slice == BATCH_SLICE_X86_64,
		game->pid,
		(long long)game->loaded,
		(unsigned long long)game->jobs,
		job->threads,
		job->keepBuild ? "-" : (game->editsReport ? "2" : "0"),
		(unsigned long long)written * 500,
		written,
		skipped,
		stopped,
		failedText,
		hasFailure ? "deserialize" : "-"
	);
}

// The batch's control/<name>, beside the inputs/ of the job's input.
static bool controlFile(const Job *job, const char *name, wchar_t *out) {
	wchar_t folder[TEST_PATH_LENGTH];
	wcscpy(folder, job->input);
	for (int up = 0; up < 2; ++up) {
		wchar_t *slash = wcsrchr(folder, PLATFORM_PATH_SEPARATOR);
		if (!slash) {
			return false;
		}
		*slash = 0;
	}
	char path[2 * TEST_PATH_LENGTH];
	return snprintf(path, sizeof path, "control/%s", name) > 0 && joinPath(out, folder, path);
}

// The fault the `number`th run job meets first, if any.
static Fault *faultFor(const Job *job, unsigned number) {
	FakeGame *game = &fake.game;
	for (size_t i = 0; i < game->faultCount; ++i) {
		Fault *fault = &game->faults[i];
		uint64_t index = fault->index == UINT64_MAX ? job->indexBase : fault->index;
		bool plays = index >= job->indexBase && index < job->indexBase + job->seedCount;
		char input[3 * TEST_PATH_LENGTH];
		bool arm = true;
		if (fault->arm && platform_toUtf8(job->input, input, sizeof input)) {
			char name[BATCH_ARM_LENGTH + 8];
			(void)snprintf(
				name, sizeof name, "%c%s.bin", PLATFORM_PATH_SEPARATOR == L'/' ? '/' : '\\', fault->arm
			);
			size_t length = strlen(input);
			arm = length >= strlen(name) && strcmp(input + length - strlen(name), name) == 0;
		}
		if ((fault->job ? fault->job == number : plays) && arm &&
				(!fault->times || fault->happened < fault->times)) {
			return fault;
		}
	}
	return NULL;
}

// Plays a run job from `from` on, meeting `fault` at its index.
static ClientOutcome playRun(
	const char *text,
	const Job *job,
	size_t from,
	Fault *fault,
	ClientWatch *watch,
	char *result,
	size_t size
) {
	FakeGame *game = &fake.game;
	if (from == 0 && !startFiles(job)) {
		(void)snprintf(result, size, "error: cannot start the run; see harness.log");
		return CLIENT_REPLIED;
	}
	uint64_t at = fault ? (fault->index == UINT64_MAX ? job->indexBase : fault->index) : UINT64_MAX;
	size_t skipped = 0;
	for (size_t p = 0; p < from; ++p) {
		skipped += job->skipSeedErrors && isRebuildFailure(job->seeds[p]);
	}
	bool stopped = false;
	bool hasFailure = false;
	size_t position = from;
	for (; position < job->seedCount; ++position) {
		uint64_t index = job->indexBase + position;
		if (job->stopFile[0] && fileThere(job->stopFile)) {
			stopped = true;
			break;
		}
		if (index == at) {
			++fault->happened;
			Background *background = &game->background;
			wchar_t stop[TEST_PATH_LENGTH];
			switch (fault->kind) {
				case FAULT_CRASH:
					(void)playSeed(job, position, true, &skipped);
					killGame();
					return CLIENT_LOST;
				case FAULT_CRASH_SEEN:
					(void)playSeed(job, position, true, &skipped);
					killGame();
					return tickWatch(watch, CLIENT_RUNNING) ? CLIENT_LOST : CLIENT_ABANDONED;
				case FAULT_HANG:
					game->hangStartedAt = game->uptime;
					*background = (Background){
						.active = true, .hung = true, .position = position, .queries = game->hangQueries};
					(void)snprintf(background->job, sizeof background->job, "%s", text);
					return tickUntilAbandoned(watch, CLIENT_RUNNING) ? CLIENT_RUN_TIMEOUT : CLIENT_ABANDONED;
				case FAULT_SLEEP: game->monotonic += 2 * 3600 * 1000; break;
				case FAULT_DIE:
					*background = (Background){.active = true, .position = position, .queries = 1};
					(void)snprintf(background->job, sizeof background->job, "%s", text);
					game->signals = 3;
					return tickWatch(watch, CLIENT_RUNNING) ? CLIENT_FAILED : CLIENT_ABANDONED;
				case FAULT_STOP:
				case FAULT_STOP_NOW:
					// As batch stop does; only a job with the stop file stops for stop-now.
					(void)(controlFile(job, fault->kind == FAULT_STOP ? "stop" : "stop-now", stop) &&
								 files_write(stop, "", 0));
					break;
				case FAULT_CRASH_STOP:
					(void)(controlFile(job, "stop", stop) && files_write(stop, "", 0));
					(void)playSeed(job, position, true, &skipped);
					killGame();
					return CLIENT_LOST;
				case FAULT_CRASH_EXITING:
					// The kernel closes the connection and frees the lock before the process goes.
					(void)playSeed(job, position, true, &skipped);
					game->exitingPid = game->pid;
					game->exitingChecks = 5;
					killGame();
					return CLIENT_LOST;
				case FAULT_RESTART:
					(void)playSeed(job, position, true, &skipped);
					killGame();
					launchGame(false);
					return tickUntilAbandoned(watch, CLIENT_RUNNING) ? CLIENT_RUN_TIMEOUT : CLIENT_ABANDONED;
				case FAULT_HARNESS_GONE:
					game->listening = false;
					return tickWatch(watch, CLIENT_RUNNING) ? CLIENT_LOST : CLIENT_ABANDONED;
				case FAULT_SEED_FAILS: hasFailure = true; break;
				default: break;
			}
			if (stopped || hasFailure) {
				break;
			}
		}
		if (!playSeed(job, position, false, &skipped)) {
			(void)snprintf(result, size, "error: cannot write; see harness.log");
			return CLIENT_REPLIED;
		}
		moveClocks(500);
		if (!tickWatch(watch, CLIENT_RUNNING)) {
			return CLIENT_ABANDONED;
		}
	}
	formatRunResult(
		job, position, skipped, stopped, hasFailure, job->indexBase + position, result, size
	);
	return CLIENT_REPLIED;
}

// A run job: refused while busy or on an unsupported build, else played.
static ClientOutcome fakeRun(const char *text, ClientWatch *watch, char *result, size_t size) {
	FakeGame *game = &fake.game;
	unsigned number = ++game->runJobs;
	static char copy[HARNESS_JOB_LENGTH + 1];
	(void)snprintf(copy, sizeof copy, "%s", text);
	if (number == 1) {
		(void)snprintf(game->firstRunJob, sizeof game->firstRunJob, "%s", text);
	}
	Job job;
	if (!job_parse(copy, &job)) {
		(void)snprintf(result, size, "error: cannot read the job; see harness.log");
		return CLIENT_REPLIED;
	}
	noteJobTeams(&job);
	game->editsReport = number == game->editsJob;
	Fault *fault = faultFor(&job, number);
	if (fault && fault->kind == FAULT_FOREIGN) {
		++fault->happened;
		game->jobs += 3;
		fault = NULL;
	}
	ClientOutcome outcome = CLIENT_REPLIED;
	if (fault && fault->kind == FAULT_BUSY) {
		++fault->happened;
		(void)snprintf(result, size, "error: FM24 is busy (processing); wait until it is idle");
	} else if (fault && fault->kind == FAULT_UNSUPPORTED) {
		++fault->happened;
		(void)snprintf(
			result,
			size,
			"error: harness %s does not support this game build; see harness.log",
			HARNESS_PROTOCOL_TEXT
		);
	} else {
		outcome = playRun(text, &job, 0, fault, watch, result, size);
	}
	job_free(&job);
	return outcome;
}

bool batchTest_playJob(const char *text, const BatchTestGame *game, char *result, size_t size) {
	if (strncmp(text, "command=replace_", 16) == 0) {
		(void)fakeReplace(text, result, size);
		return true;
	}
	static char copy[HARNESS_JOB_LENGTH + 1];
	(void)snprintf(copy, sizeof copy, "%s", text);
	Job job;
	if (!job_parse(copy, &job)) {
		(void)snprintf(result, size, "error: cannot read the job; see harness.log");
		return true;
	}
	noteJobTeams(&job);
	fake.game.editsReport = false;
	fake.game.pid = game->pid;
	fake.game.loaded = game->loaded;
	fake.game.jobs = game->jobs;
	bool played = startFiles(&job);
	bool stopped = false;
	size_t skipped = 0;
	size_t position = 0;
	for (; played && position < job.seedCount; ++position) {
		if (job.stopFile[0] && fileThere(job.stopFile)) {
			stopped = true;
			break;
		}
		if (game->atSeed && !game->atSeed(game->context, job.indexBase + position)) {
			(void)playSeed(&job, position, true, &skipped);
			job_free(&job);
			return false;
		}
		played = playSeed(&job, position, false, &skipped);
	}
	if (played) {
		formatRunResult(&job, position, skipped, stopped, false, 0, result, size);
	} else {
		(void)snprintf(result, size, "error: cannot write; see harness.log");
	}
	job_free(&job);
	return true;
}

// The stop file of the job the harness runs on is there.
static bool backgroundStopped(void) {
	static char copy[HARNESS_JOB_LENGTH + 1];
	(void)snprintf(copy, sizeof copy, "%s", fake.game.background.job);
	Job job;
	if (!job_parse(copy, &job)) {
		return false;
	}
	bool stopped = job.stopFile[0] && fileThere(job.stopFile);
	job_free(&job);
	return stopped;
}

// The job the harness ran on finishes.
static void finishBackground(void) {
	Background *background = &fake.game.background;
	static char copy[HARNESS_JOB_LENGTH + 1];
	(void)snprintf(copy, sizeof copy, "%s", background->job);
	background->active = false;
	Job job;
	char result[CLIENT_RESULT_LENGTH];
	if (job_parse(copy, &job)) {
		(void)playRun(background->job, &job, background->position, NULL, NULL, result, sizeof result);
		job_free(&job);
	}
}

// A status job: it waits behind a job the harness still runs, as a real one would.
static ClientOutcome fakeStatus(ClientWatch *watch, char *result, size_t size) {
	FakeGame *game = &fake.game;
	Background *background = &game->background;
	++game->statusQueries;
	doActions();
	if (background->active && !background->hung && backgroundStopped()) {
		background->queries = 0; // it finishes the matches under way, and ends
	}
	if (background->active && background->queries) {
		if (background->queries != UINT32_MAX) {
			--background->queries;
		}
		return tickUntilAbandoned(watch, CLIENT_QUEUED) ? CLIENT_QUEUE_TIMEOUT : CLIENT_ABANDONED;
	}
	if (background->active) {
		finishBackground();
	}
	if (watch) {
		watch->accepted = true;
	}
	(void)snprintf(
		result,
		size,
		"ok: ready in_progress=1 processing=0 kind=-1 people=9 human_team=1 menu_seen=1 autoplay=0 "
		"autoplay_read=0 date=%s save=LIV_COPY",
		game->date
	);
	return CLIENT_REPLIED;
}

// list_teams and list_players: a short list in the job's output.
static ClientOutcome fakeLookup(const char *job, char *result, size_t size) {
	const char *output = strstr(job, "output=");
	char path[3 * TEST_PATH_LENGTH];
	wchar_t wide[TEST_PATH_LENGTH];
	size_t length = output ? strcspn(output + 7, "\n") : 0;
	if (!output || length >= sizeof path) {
		(void)snprintf(result, size, "error: cannot read the job; see harness.log");
		return CLIENT_REPLIED;
	}
	memcpy(path, output + 7, length);
	path[length] = 0;
	static const char text[] = "1\tLiverpool\n677\tArsenal\n";
	bool ok =
		platform_fromUtf8(path, wide, TEST_PATH_LENGTH) && files_write(wide, text, sizeof text - 1);
	(void)snprintf(result, size, ok ? "ok: 2 written" : "error: cannot write; see harness.log");
	return CLIENT_REPLIED;
}

static ClientOutcome fakeSubmit(
	void *context,
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t size
) {
	(void)context;
	(void)timeoutMs;
	FakeGame *game = &fake.game;
	if (watch) {
		watch->accepted = false;
		watch->queuedMs = 0;
		watch->runMs = 0;
	}
	result[0] = 0;
	if (!game->running || !game->listening) {
		return CLIENT_NO_HARNESS;
	}
	if (strcmp(job, "command=status\n") == 0) {
		return fakeStatus(watch, result, size);
	}
	if (game->background.active) {
		return tickUntilAbandoned(watch, CLIENT_QUEUED) ? CLIENT_QUEUE_TIMEOUT : CLIENT_ABANDONED;
	}
	if (watch) {
		watch->accepted = true;
	}
	++game->jobs;
	if (strncmp(job, "command=replace_", 16) == 0) {
		return fakeReplace(job, result, size);
	}
	return strncmp(job, "command=run\n", 12) == 0 ? fakeRun(job, watch, result, size)
																								: fakeLookup(job, result, size);
}

static int fakeEnsureSave(
	void *context,
	const SessionNeed *need,
	char *date,
	size_t size,
	SessionOutcome *outcome
) {
	(void)context;
	FakeGame *game = &fake.game;
	*outcome = SESSION_WAITING;
	if (size) {
		date[0] = 0;
	}
	for (; game->saveWaitS && game->running; --game->saveWaitS) {
		moveClocks(1000);
		if (need->stop && need->stop->asked(need->stop->context)) {
			*outcome = SESSION_STOPPED;
			return 1;
		}
	}
	if (game->answersGiven < game->answerCount) {
		const SaveAnswer *answer = &game->answers[game->answersGiven++];
		*outcome = answer->outcome;
		return answer->code;
	}
	if (!game->running) {
		if (!need->launch || !need->save || !need->save[0]) {
			return 0;
		}
		launchGame(need->rosetta);
	}
	if (game->listening) {
		(void)snprintf(date, size, "%s", game->date);
		*outcome = SESSION_READY;
	}
	return 0;
}

static uint32_t fakeFindGame(void *context) {
	(void)context;
	return fake.game.running ? fake.game.pid : 0;
}

static bool fakeGameRunning(void *context, uint32_t pid) {
	(void)context;
	FakeGame *game = &fake.game;
	if (pid && pid == game->exitingPid && game->exitingChecks) {
		--game->exitingChecks;
		return true;
	}
	return game->running && pid == game->pid;
}

static uint32_t fakeGameSlice(void *context, uint32_t pid) {
	(void)context;
	(void)pid;
	return fake.game.slice;
}

static bool fakeHarnessListening(void *context) {
	(void)context;
	return fake.game.running && fake.game.listening;
}

static bool fakeStopGame(void *context, uint32_t pid, unsigned graceS) {
	(void)context;
	(void)graceS;
	if (fake.game.running && pid == fake.game.pid) {
		++fake.game.kills;
		killGame();
	}
	return true;
}

static uint64_t fakeResidentBytes(void *context, uint32_t pid) {
	(void)context;
	return pid ? 6123456789ULL : 0;
}

static BatchPower fakePowerSource(void *context) {
	(void)context;
	FakeGame *game = &fake.game;
	++game->powerChecks;
	doActions();
	return game->powerGiven < game->powerCount ? game->power[game->powerGiven++] : BATCH_POWER_AC;
}

static bool fakeKeepAwake(void *context, bool onBattery) {
	(void)context;
	++fake.game.keptAwake;
	fake.game.keptAwakeOnBattery = onBattery;
	return true;
}

static void fakeNotify(void *context, const char *text) {
	(void)context;
	appendf(&fake.game.notes, "%s\n", text);
}

static bool fakeWatchSignals(void *context, const wchar_t *stop, const wchar_t *stopNow) {
	(void)context;
	fake.game.watched = wcsstr(stop, L"control") && wcsstr(stopNow, L"stop-now");
	return true;
}

static unsigned fakeSignals(void *context) {
	(void)context;
	return fake.game.signals;
}

static uint64_t fakeUptime(void *context) {
	(void)context;
	return fake.game.uptime;
}

static uint64_t fakeMonotonic(void *context) {
	(void)context;
	return fake.game.monotonic;
}

// FM24's logs in FakeHost.logs, trimmed as the real host does on macOS, with the failures
// FakeHost.trimFailures adds.
static void fakeTrimGameLogs(void *context, GameLogsTrim *out) {
	(void)context;
	++fake.trims;
	note("trim game logs");
#ifndef _WIN32
	char folder[3 * TEST_PATH_LENGTH];
	if (fake.logs[0] && platform_toUtf8(fake.logs, folder, sizeof folder)) {
		gameLogs_trim(folder, out);
	}
#endif
	for (size_t kind = 0; kind < GAME_LOGS_PROBLEMS; ++kind) {
		if (fake.trimFailures[kind]) {
			(void)snprintf(out->failed[kind], sizeof out->failed[kind], "%s", fake.trimFailures[kind]);
			if (!out->folder[0]) {
				(void)snprintf(out->folder, sizeof out->folder, "/fake/Logs");
			}
		}
	}
}

static void fakeSleep(void *context, unsigned milliseconds) {
	(void)context;
	FakeGame *game = &fake.game;
	unsigned left = milliseconds;
	do {
		unsigned step = game->fineClock && left > FINE_STEP_MS ? FINE_STEP_MS : left;
		moveClocks(step);
		left -= step;
	} while (left);
	if (game->quitAfterSleeps && game->background.active && --game->quitAfterSleeps == 0) {
		// Quit by hand: the kernel frees the harness's lock before the process goes.
		game->exitingPid = game->pid;
		game->exitingChecks = game->quitExitingChecks;
		killGame();
	}
}

static void
addFault(FaultKind kind, unsigned job, uint64_t index, unsigned times, const char *arm) {
	FakeGame *game = &fake.game;
	if (game->faultCount < sizeof game->faults / sizeof game->faults[0]) {
		game->faults[game->faultCount++] =
			(Fault){.kind = kind, .job = job, .index = index, .times = times, .arm = arm};
	}
}

static void addAnswer(int code, SessionOutcome outcome) {
	FakeGame *game = &fake.game;
	if (game->answerCount < sizeof game->answers / sizeof game->answers[0]) {
		game->answers[game->answerCount++] = (SaveAnswer){code, outcome};
	}
}

// ---------------------------------------------------------------------------------------------
// The failure policy: a case for each row of the failure table (docs/CLI.md "When things go
// wrong")

static BatchPlan policyPlan;

static BatchPolicyState testPolicy(void) {
	static const char text[] = "format=fm24-batch-plan/1\nname=p\narm=a a.json\nseeds=range:1-100\n";
	char error[256];
	ASSERT_BOOL_EQ(parsePlan(text, false, &policyPlan, error, sizeof error), true);
	return (BatchPolicyState){.plan = &policyPlan, .threads = 4, .minuteMs = 60000};
}

static BatchDecision decided(const BatchPolicyState *state, BatchEventKind kind) {
	BatchEvent event = {.kind = kind};
	return batchPolicy_decide(state, &event);
}

static BatchDecision decidedJob(const BatchPolicyState *state, BatchJobEvent job) {
	BatchEvent event = {.kind = BATCH_EVENT_JOB, .job = job};
	return batchPolicy_decide(state, &event);
}

static BatchDecision
decidedSave(const BatchPolicyState *state, int code, SessionOutcome outcome, bool running) {
	BatchEvent event = {.kind = BATCH_EVENT_SAVE, .save = {code, outcome, running}};
	return batchPolicy_decide(state, &event);
}

static BatchPart evaluatedPart(
	BatchPartOutcome outcome,
	uint32_t threads,
	uint64_t first,
	uint64_t count,
	uint64_t verified
) {
	return (BatchPart){.attempt = 1,
		.first = first,
		.count = count,
		.threads = threads,
		.evaluated = true,
		.verified = verified,
		.outcome = outcome};
}

static bool stops(BatchDecision decision, int exitCode) {
	return decision.action == BATCH_STOP && decision.exitCode == exitCode && decision.reason;
}

// FM24 crashes or quits mid-chunk: relaunch and retry, unless it crashes too often or the plan
// never starts the game.
TEST(policy_relaunches_after_a_crash) {
	BatchPolicyState state = testPolicy();
	BatchPart part = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 6);
	BatchJobEvent lost = {.client = CLIENT_LOST, .accepted = true, .part = &part};
	BatchDecision decision = decidedJob(&state, lost);
	ASSERT_BOOL_EQ(decision.action == BATCH_RELAUNCH && decision.crash, true);
	BatchJobEvent seen = {
		.client = CLIENT_ABANDONED, .watch = BATCH_WATCH_GAME_GONE, .accepted = true, .part = &part};
	ASSERT_BOOL_EQ(decidedJob(&state, seen).action == BATCH_RELAUNCH, true);
	state.crashes = 3;
	ASSERT_BOOL_EQ(decidedJob(&state, lost).action == BATCH_RELAUNCH, true);
	state.crashes = 4; // the fifth in an hour is more than max_crashes_per_hour=4
	BatchDecision tooMany = decidedJob(&state, lost);
	ASSERT_BOOL_EQ(stops(tooMany, 1) && tooMany.crash && tooMany.notify, true);
	state.crashes = 0;
	policyPlan.launch = 0;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, lost), 1), true);
	// A game that went before the job reached it is not a crash; without launch=1 it stops.
	policyPlan.launch = 1;
	BatchJobEvent gone = {.client = CLIENT_NO_HARNESS};
	decision = decidedJob(&state, gone);
	ASSERT_BOOL_EQ(decision.action == BATCH_RELAUNCH && !decision.crash, true);
	policyPlan.launch = 0;
	decision = decidedJob(&state, gone);
	ASSERT_BOOL_EQ(stops(decision, 1) && !decision.crash, true);
	ASSERT_BOOL_EQ(strstr(decision.reason, "launch=0") != NULL, true);
	// No game after the save check: without launch=1 the batch stops there, before a session.
	decision = decidedSave(&state, 0, SESSION_WAITING, false);
	ASSERT_BOOL_EQ(stops(decision, 1) && strstr(decision.reason, "launch=0") != NULL, true);
	ASSERT_BOOL_EQ(decidedSave(&state, 0, SESSION_READY, true).action == BATCH_CONTINUE, true);
	policyPlan.launch = 1;
	ASSERT_BOOL_EQ(decidedSave(&state, 0, SESSION_WAITING, false).action == BATCH_CONTINUE, true);
}

// A seed that crashes the game: two crashes in a row with less than a worker's share played go to
// one worker over the seeds under way; one-worker crashes try the seed alone, then exclude it.
TEST(policy_isolates_a_seed_that_crashes_the_game) {
	BatchPolicyState state = testPolicy();
	BatchPart first = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 6);
	BatchPart second = evaluatedPart(BATCH_PART_CRASHED, 4, 6, 4, 2);
	second.attempt = 2;
	BatchJobEvent event = {
		.client = CLIENT_LOST, .accepted = true, .part = &second, .previous = &first};
	ASSERT_BOOL_EQ(decidedJob(&state, event).action == BATCH_RETRY_ONE_THREAD, true);
	BatchPart parts[4] = {first, second};
	BatchAttemptShape shape;
	batchPolicy_shape(parts, 2, 4, &shape);
	ASSERT_BOOL_EQ(shape.threads == 1 && shape.maxCount == 4 && !shape.hasCandidate, true);
	// With a worker's share or more played before the second crash, it is another seed: as many
	// workers as before.
	BatchPart later = evaluatedPart(BATCH_PART_CRASHED, 4, 6, 14, 4);
	later.attempt = 2;
	parts[1] = later;
	batchPolicy_shape(parts, 2, 4, &shape);
	ASSERT_BOOL_EQ(shape.threads == 4 && !shape.maxCount, true);
	BatchJobEvent laterEvent = event;
	laterEvent.part = &later;
	ASSERT_BOOL_EQ(decidedJob(&state, laterEvent).action == BATCH_RELAUNCH, true);
	parts[1] = second;
	// One worker crashed at index 9: try it alone, offered as poison.
	BatchPart lone = evaluatedPart(BATCH_PART_CRASHED, 1, 8, 4, 1);
	parts[2] = lone;
	batchPolicy_shape(parts, 3, 4, &shape);
	ASSERT_BOOL_EQ(
		shape.threads == 1 && shape.maxCount == 1 && shape.hasCandidate && shape.candidate == 9, true
	);
	event.part = &lone;
	event.previous = &second;
	ASSERT_BOOL_EQ(decidedJob(&state, event).action == BATCH_RETRY_ONE_THREAD, true);
	// An attempt at that seed that played nothing without a crash (a busy game, a stop before its
	// match) tells nothing: the seed is offered again.
	static const BatchPartOutcome nothing[] = {BATCH_PART_ERROR, BATCH_PART_STOPPED};
	for (size_t i = 0; i < sizeof nothing / sizeof nothing[0]; ++i) {
		parts[3] = evaluatedPart(nothing[i], 1, 9, 1, 0);
		batchPolicy_shape(parts, 4, 4, &shape);
		ASSERT_BOOL_EQ(
			shape.threads == 1 && shape.maxCount == 1 && shape.hasCandidate && shape.candidate == 9 &&
				shape.isolating,
			true
		);
	}
	// It crashed there again: poison, then back to every worker.
	BatchPart poison = evaluatedPart(BATCH_PART_CRASHED, 1, 9, 1, 0);
	poison.hasPoison = true;
	poison.poison = 9;
	parts[3] = poison;
	batchPolicy_shape(parts, 4, 4, &shape);
	ASSERT_BOOL_EQ(shape.threads == 4 && !shape.hasCandidate, true);
	event.part = &poison;
	BatchDecision decision = decidedJob(&state, event);
	ASSERT_BOOL_EQ(decision.action == BATCH_MARK_POISON && decision.crash, true);
	// Crashes while isolating a seed are not counted toward max_crashes_per_hour, nor stopped by it.
	event.isolating = true;
	state.crashes = 4;
	decision = decidedJob(&state, event);
	ASSERT_BOOL_EQ(decision.action == BATCH_MARK_POISON && decision.crash && !decision.counted, true);
	event.isolating = false;
	decision = decidedJob(&state, event);
	ASSERT_BOOL_EQ(stops(decision, 1) && decision.crash && decision.counted, true);
	state.crashes = 0;
	state.armPoison = 3; // a fourth is more than max_poison=3
	ASSERT_BOOL_EQ(stops(decidedJob(&state, event), 1), true);
	// An attempt that finished its indices before the game went implicates none of them.
	BatchPart whole = evaluatedPart(BATCH_PART_CRASHED, 1, 0, 4, 4);
	batchPolicy_shape(&whole, 1, 4, &shape);
	ASSERT_BOOL_EQ(shape.threads == 4 && !shape.hasCandidate, true);
	batchPolicy_shape(NULL, 0, 4, &shape);
	ASSERT_BOOL_EQ(shape.threads == 4 && !shape.maxCount && !shape.hasCandidate, true);
}

// An engine hang: quit the game with kill_game=1, else watch the job until it ends and keep it.
TEST(policy_handles_a_hang) {
	BatchPolicyState state = testPolicy();
	BatchJobEvent hang = {.client = CLIENT_ABANDONED,
		.watch = BATCH_WATCH_HANG,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true};
	BatchDecision decision = decidedJob(&state, hang);
	ASSERT_BOOL_EQ(decision.action == BATCH_WAIT && decision.notify && !decision.crash, true);
	policyPlan.killGame = 1;
	decision = decidedJob(&state, hang);
	ASSERT_BOOL_EQ(decision.action == BATCH_KILL_AND_RELAUNCH && decision.crash, true);
	hang.client = CLIENT_RUN_TIMEOUT;
	hang.watch = BATCH_WATCH_NONE;
	ASSERT_BOOL_EQ(decidedJob(&state, hang).action == BATCH_KILL_AND_RELAUNCH, true);
	// The job ended after the batch stopped waiting: what it played is kept.
	BatchPart adopted = evaluatedPart(BATCH_PART_ADOPTED, 4, 0, 10, 10);
	hang.part = &adopted;
	ASSERT_BOOL_EQ(decidedJob(&state, hang).action == BATCH_CONTINUE, true);
	adopted.verified = 7;
	ASSERT_BOOL_EQ(decidedJob(&state, hang).action == BATCH_RETRY, true);
	// Quit after the hang, the game is gone: the crash path.
	BatchPart killed = evaluatedPart(BATCH_PART_ABANDONED, 4, 0, 10, 5);
	BatchJobEvent quit = {
		.client = CLIENT_ABANDONED, .watch = BATCH_WATCH_HANG, .accepted = true, .part = &killed};
	ASSERT_BOOL_EQ(decidedJob(&state, quit).action == BATCH_RELAUNCH, true);
	ASSERT_BOOL_EQ(batchPolicy_crashLike(&killed), true);
}

// A stop asked for never starts the game again: a crash, a quit or a game gone before the job
// stops with 4 (also with launch=0), keeping the part and any poison seed; a hang leaves the job
// running (kill_game=0) or quits the game, whose crash then stops. Limits still stop with 1.
TEST(policy_stops_as_asked_instead_of_starting_the_game) {
	static const BatchAsked asks[] = {BATCH_ASKED_STOP, BATCH_ASKED_STOP_NOW};
	for (size_t i = 0; i < sizeof asks / sizeof asks[0]; ++i) {
		BatchPolicyState state = testPolicy();
		state.asked = asks[i];
		BatchPart part = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 6);
		BatchJobEvent lost = {.client = CLIENT_LOST, .accepted = true, .part = &part};
		BatchDecision decision = decidedJob(&state, lost);
		ASSERT_BOOL_EQ(stops(decision, 4) && decision.crash && !decision.notify, true);
		ASSERT_BOOL_EQ(strstr(decision.reason, "stopped as asked") != NULL, true);
		// Isolating a seed, and a seed found to be poison, which part.json keeps.
		BatchPart first = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 6);
		BatchPart second = evaluatedPart(BATCH_PART_CRASHED, 4, 6, 4, 2);
		BatchJobEvent twice = {
			.client = CLIENT_LOST, .accepted = true, .part = &second, .previous = &first};
		ASSERT_BOOL_EQ(stops(decidedJob(&state, twice), 4), true);
		BatchPart poison = evaluatedPart(BATCH_PART_CRASHED, 1, 9, 1, 0);
		poison.hasPoison = true;
		BatchJobEvent poisoned = {.client = CLIENT_LOST, .accepted = true, .part = &poison};
		ASSERT_BOOL_EQ(stops(decidedJob(&state, poisoned), 4), true);
		state.armPoison = 3;
		ASSERT_BOOL_EQ(stops(decidedJob(&state, poisoned), 1), true);
		state.armPoison = 0;
		state.crashes = 4;
		ASSERT_BOOL_EQ(stops(decidedJob(&state, lost), 1), true);
		state.crashes = 0;
		ASSERT_BOOL_EQ(
			strstr(decidedJob(&state, lost).reason, "the next batch run starts it") != NULL, true
		);
		policyPlan.launch = 0;
		decision = decidedJob(&state, lost);
		ASSERT_BOOL_EQ(stops(decision, 4), true);
		// With launch=0 the next batch run does not start the game.
		ASSERT_BOOL_EQ(
			strstr(decision.reason, "start it with the save, then run the batch again") != NULL, true
		);
		ASSERT_BOOL_EQ(strstr(decision.reason, "the next batch run starts it") == NULL, true);
		BatchJobEvent gone = {.client = CLIENT_NO_HARNESS};
		ASSERT_BOOL_EQ(stops(decidedJob(&state, gone), 4), true);
		policyPlan.launch = 1;
		ASSERT_BOOL_EQ(stops(decidedJob(&state, gone), 4), true);
		// A hung job is left running, saying so; with kill_game=1 the game is quit first.
		BatchJobEvent hang = {.client = CLIENT_ABANDONED,
			.watch = BATCH_WATCH_HANG,
			.accepted = true,
			.gameRunning = true,
			.harnessListening = true};
		decision = decidedJob(&state, hang);
		ASSERT_BOOL_EQ(stops(decision, 4) && decision.notify && !decision.crash, true);
		ASSERT_BOOL_EQ(strstr(decision.reason, "still runs in FM24") != NULL, true);
		BatchJobEvent unanswered = hang;
		unanswered.watch = BATCH_WATCH_STOP;
		ASSERT_BOOL_EQ(stops(decidedJob(&state, unanswered), 4), true);
		policyPlan.killGame = 1;
		decision = decidedJob(&state, unanswered);
		ASSERT_BOOL_EQ(decision.action == BATCH_KILL_AND_RELAUNCH && decision.crash, true);
		ASSERT_BOOL_EQ(decidedJob(&state, hang).action == BATCH_KILL_AND_RELAUNCH, true);
		BatchPart killed = evaluatedPart(BATCH_PART_ABANDONED, 4, 0, 10, 3);
		BatchJobEvent quit = {
			.client = CLIENT_ABANDONED, .watch = BATCH_WATCH_HANG, .accepted = true, .part = &killed};
		ASSERT_BOOL_EQ(stops(decidedJob(&state, quit), 4), true);
		// Asked before the harness took the job, which then never runs.
		BatchJobEvent queued = {.client = CLIENT_ABANDONED,
			.watch = BATCH_WATCH_STOP,
			.gameRunning = true,
			.harnessListening = true};
		decision = decidedJob(&state, queued);
		ASSERT_BOOL_EQ(stops(decision, 4) && !decision.notify && !decision.crash, true);
		// Asked while the save check waited, behind another runner's job or for a busy game.
		decision = decidedSave(&state, 1, SESSION_STOPPED, true);
		ASSERT_BOOL_EQ(stops(decision, 4) && !decision.notify && !decision.crash, true);
		ASSERT_BOOL_EQ(
			(strstr(decision.reason, "(stop-now)") != NULL) == (asks[i] == BATCH_ASKED_STOP_NOW), true
		);
	}
	// Without a stop, a game that exited is started again, and a hung job watched.
	BatchPolicyState state = testPolicy();
	BatchPart part = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 6);
	BatchJobEvent lost = {.client = CLIENT_LOST, .accepted = true, .part = &part};
	ASSERT_BOOL_EQ(decidedJob(&state, lost).action == BATCH_RELAUNCH, true);
	// Giving up on a job that may still run is a failure that says so, not a failed reply.
	BatchJobEvent gaveUp = {.client = CLIENT_ABANDONED,
		.watch = BATCH_WATCH_GAVE_UP,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true};
	BatchDecision decision = decidedJob(&state, gaveUp);
	ASSERT_BOOL_EQ(stops(decision, 1) && decision.notify, true);
	ASSERT_BOOL_EQ(strstr(decision.reason, "may still run in FM24") != NULL, true);
}

// The harness gone while the game runs: quit and relaunch with kill_game=1, else stop.
TEST(policy_handles_a_game_without_its_harness) {
	BatchPolicyState state = testPolicy();
	BatchJobEvent gone = {.client = CLIENT_ABANDONED,
		.watch = BATCH_WATCH_HARNESS_GONE,
		.accepted = true,
		.gameRunning = true};
	ASSERT_BOOL_EQ(stops(decidedJob(&state, gone), 1), true);
	BatchJobEvent stale = {.client = CLIENT_NO_HARNESS, .gameRunning = true};
	ASSERT_BOOL_EQ(stops(decidedJob(&state, stale), 1), true);
	policyPlan.killGame = 1;
	BatchDecision decision = decidedJob(&state, gone);
	ASSERT_BOOL_EQ(decision.action == BATCH_KILL_AND_RELAUNCH && decision.crash, true);
	decision = decidedJob(&state, stale);
	ASSERT_BOOL_EQ(decision.action == BATCH_KILL_AND_RELAUNCH && !decision.crash, true);
}

// fm24ctl interrupted, rebooted or power lost: the next run's barrier, then carry on; another
// runner's job: wait, then stop after queue_wait_minutes.
TEST(policy_resumes_and_waits_for_another_job) {
	BatchPolicyState state = testPolicy();
	ASSERT_BOOL_EQ(decided(&state, BATCH_EVENT_RESUMED).action == BATCH_CONTINUE, true);
	BatchJobEvent queued = {
		.client = CLIENT_QUEUE_TIMEOUT, .gameRunning = true, .harnessListening = true};
	ASSERT_BOOL_EQ(stops(decidedJob(&state, queued), 1), true);
	queued.client = CLIENT_ABANDONED;
	queued.watch = BATCH_WATCH_QUEUE;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, queued), 1), true);
}

// The game loading or busy: wait a minute and try again, until busy_wait_minutes.
TEST(policy_waits_for_a_busy_game) {
	BatchPolicyState state = testPolicy();
	BatchJobEvent busy = {.client = CLIENT_REPLIED,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true,
		.reply = BATCH_REPLY_BUSY};
	BatchDecision decision = decidedJob(&state, busy);
	ASSERT_BOOL_EQ(decision.action == BATCH_WAIT && decision.waitS == 60 && decision.busy, true);
	decision = decidedSave(&state, 3, SESSION_TIMED_OUT, true);
	ASSERT_BOOL_EQ(decision.action == BATCH_WAIT && decision.busy, true);
	state.busyMs = 15 * 60000 - 1;
	ASSERT_BOOL_EQ(decidedJob(&state, busy).action == BATCH_WAIT, true);
	state.busyMs = 15 * 60000;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, busy), 3), true);
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 3, SESSION_TIMED_OUT, true), 3), true);
}

// Another save loaded: stop with 3; the start screen: quit and relaunch with kill_game=1, else 3.
TEST(policy_stops_for_another_save_or_the_start_screen) {
	BatchPolicyState state = testPolicy();
	ASSERT_BOOL_EQ(decidedSave(&state, 0, SESSION_READY, true).action == BATCH_CONTINUE, true);
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 3, SESSION_OTHER_SAVE, true), 3), true);
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 3, SESSION_NO_TEAM, true), 3), true);
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 3, SESSION_AT_MENU, true), 3), true);
	policyPlan.killGame = 1;
	ASSERT_BOOL_EQ(
		decidedSave(&state, 3, SESSION_AT_MENU, true).action == BATCH_KILL_AND_RELAUNCH, true
	);
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 3, SESSION_OTHER_SAVE, true), 3), true);
}

// The save's date moved, results no longer repeat, the harness changed, an unsupported build.
TEST(policy_stops_when_the_game_is_no_longer_the_same) {
	BatchPolicyState state = testPolicy();
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_DATE), 1), true);
	BatchDecision canary = decided(&state, BATCH_EVENT_CANARY);
	ASSERT_BOOL_EQ(stops(canary, 1) && canary.notify, true);
	policyPlan.onCanaryMismatch = BATCH_MISMATCH_CONTINUE;
	canary = decided(&state, BATCH_EVENT_CANARY);
	ASSERT_BOOL_EQ(canary.action == BATCH_CONTINUE && canary.notify, true);
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_IDENTITY), 1), true);
	state.allowHarnessChange = true;
	ASSERT_BOOL_EQ(decided(&state, BATCH_EVENT_IDENTITY).action == BATCH_CONTINUE, true);
	BatchJobEvent unsupported = {.client = CLIENT_REPLIED,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true,
		.reply = BATCH_REPLY_UNSUPPORTED};
	ASSERT_BOOL_EQ(stops(decidedJob(&state, unsupported), 1), true);
	// A stale harness: the save check fails with the game running.
	ASSERT_BOOL_EQ(stops(decidedSave(&state, 1, SESSION_WAITING, true), 1), true);
}

// The game runs the other slice: stop with 3, or quit and relaunch with kill_game=1.
TEST(policy_handles_the_other_slice) {
	BatchPolicyState state = testPolicy();
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_SLICE), 3), true);
	policyPlan.killGame = 1;
	ASSERT_BOOL_EQ(decided(&state, BATCH_EVENT_SLICE).action == BATCH_KILL_AND_RELAUNCH, true);
}

// Steam not running or asking to log in: try again after 5 and 15 minutes, then stop and notify.
TEST(policy_retries_a_game_that_did_not_start) {
	BatchPolicyState state = testPolicy();
	BatchDecision decision = decidedSave(&state, 1, SESSION_EXITED, false);
	ASSERT_BOOL_EQ(
		decision.action == BATCH_WAIT && decision.waitS == 300 && decision.launchFailed, true
	);
	state.launchFailures = 1;
	decision = decidedSave(&state, 1, SESSION_WAITING, false);
	ASSERT_BOOL_EQ(decision.action == BATCH_WAIT && decision.waitS == 900, true);
	state.launchFailures = 2;
	decision = decidedSave(&state, 1, SESSION_EXITED, false);
	ASSERT_BOOL_EQ(stops(decision, 1) && decision.notify, true);
}

// Seeds that fail to rebuild: recorded, until an arm's share passes max_seed_errors.
TEST(policy_limits_seeds_that_fail_to_rebuild) {
	BatchPolicyState state = testPolicy();
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_SEED_ERRORS), 1), true);
	BatchErrorLimit percent = {true, 1000};                                   // 1%
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&percent, 500, 999), false); // too few done yet
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&percent, 10, 1000), false);
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&percent, 11, 1000), true);
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&percent, 100, 10000), false);
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&percent, 101, 10000), true);
	BatchErrorLimit count = {false, 2};
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&count, 2, 3), false);
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&count, 3, 3), true);
	BatchErrorLimit none = {false, 0};
	ASSERT_BOOL_EQ(batchPolicy_tooManySeedErrors(&none, 1, 1), true);
}

// A seed failed (rebuild with seed_errors=stop, deserialize or statistics): retry once from it;
// the same index failing again stops the batch.
TEST(policy_retries_a_failed_seed_once) {
	BatchPolicyState state = testPolicy();
	BatchPart part = evaluatedPart(BATCH_PART_ERROR, 4, 0, 10, 3);
	BatchJobEvent failed = {.client = CLIENT_REPLIED,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true,
		.reply = BATCH_REPLY_SEED_FAILED,
		.hasReport = true,
		.report = {.hasFailure = true, .failedIndex = 3, .failedStage = "deserialize"},
		.part = &part};
	ASSERT_BOOL_EQ(decidedJob(&state, failed).action == BATCH_RETRY, true);
	BatchPart before = evaluatedPart(BATCH_PART_ERROR, 4, 0, 10, 3);
	jobRunTest_reportLine(false, before.resultLine, sizeof before.resultLine);
	RunReport report;
	ASSERT_BOOL_EQ(runReport_parse(before.resultLine, &report), true);
	// The plainest report has no failure; give it the same one as the event's.
	char *index = strstr(before.resultLine, "failed_index=-");
	char *stage = strstr(before.resultLine, "failed_stage=-");
	ASSERT_BOOL_EQ(index && stage, true);
	if (!index || !stage) {
		return;
	}
	char line[HARNESS_RESULT_LENGTH];
	(void)snprintf(
		line,
		sizeof line,
		"%.*sfailed_index=3 failed_stage=deserialize%s",
		(int)(index - before.resultLine),
		before.resultLine,
		stage + strlen("failed_stage=-")
	);
	(void)snprintf(before.resultLine, sizeof before.resultLine, "%s", line);
	ASSERT_BOOL_EQ(runReport_parse(before.resultLine, &report) && report.hasFailure, true);
	failed.previous = &before;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, failed), 1), true);
	// The same index failing at another stage is another failure.
	(void)snprintf(failed.report.failedStage, sizeof failed.report.failedStage, "stats");
	ASSERT_BOOL_EQ(decidedJob(&state, failed).action == BATCH_RETRY, true);
	(void)snprintf(failed.report.failedStage, sizeof failed.report.failedStage, "deserialize");
	failed.report.failedIndex = 4;
	ASSERT_BOOL_EQ(decidedJob(&state, failed).action == BATCH_RETRY, true);
	// A warm-up or canary has no second chance.
	failed.part = NULL;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, failed), 1), true);
}

// The fixture no longer rebuildable, a full disk or an output error, a job the harness cannot read.
TEST(policy_stops_for_the_fixture_the_disk_and_bad_jobs) {
	BatchPolicyState state = testPolicy();
	BatchJobEvent reply = {
		.client = CLIENT_REPLIED, .accepted = true, .gameRunning = true, .harnessListening = true};
	reply.reply = BATCH_REPLY_NOT_REBUILDABLE;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, reply), 1), true);
	reply.reply = BATCH_REPLY_OUTPUT_FAILED;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, reply), 1), true);
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_DISK), 1), true);
	reply.reply = BATCH_REPLY_BAD_JOB;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, reply), 2), true);
	reply.reply = BATCH_REPLY_NO_REPORT;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, reply), 1), true);
	reply.reply = BATCH_REPLY_STOPPED;
	BatchDecision stopped = decidedJob(&state, reply);
	ASSERT_BOOL_EQ(stops(stopped, 4) && !stopped.notify, true);
}

// Torn or inconsistent files: only the prefix that verifies counts; a reply that claims more is
// retried once.
TEST(policy_retries_files_that_hold_less_than_reported) {
	BatchPolicyState state = testPolicy();
	BatchPart part = evaluatedPart(BATCH_PART_OK, 4, 0, 10, 7);
	BatchJobEvent ok = {.client = CLIENT_REPLIED,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true,
		.reply = BATCH_REPLY_OK,
		.part = &part};
	ASSERT_BOOL_EQ(decidedJob(&state, ok).action == BATCH_RETRY, true);
	BatchPart before = part;
	ok.previous = &before;
	ASSERT_BOOL_EQ(stops(decidedJob(&state, ok), 1), true);
	part.verified = 10;
	ASSERT_BOOL_EQ(decidedJob(&state, ok).action == BATCH_CONTINUE, true);
	// A part that verified nothing after a crash is retried as a crash.
	BatchPart nothing = evaluatedPart(BATCH_PART_CRASHED, 4, 0, 10, 0);
	BatchJobEvent lost = {.client = CLIENT_LOST, .accepted = true, .part = &nothing};
	ASSERT_BOOL_EQ(decidedJob(&state, lost).action == BATCH_RELAUNCH, true);
}

// On battery: pause (pause_on_battery=1); a sleep is noted; a second runner or a changed plan.
TEST(policy_pauses_notes_sleep_and_refuses_a_second_runner) {
	BatchPolicyState state = testPolicy();
	BatchDecision battery = decided(&state, BATCH_EVENT_BATTERY);
	ASSERT_BOOL_EQ(battery.action == BATCH_WAIT && battery.waitS == 60, true);
	policyPlan.pauseOnBattery = 0;
	ASSERT_BOOL_EQ(decided(&state, BATCH_EVENT_BATTERY).action == BATCH_CONTINUE, true);
	ASSERT_BOOL_EQ(decided(&state, BATCH_EVENT_SLEPT).action == BATCH_CONTINUE, true);
	BatchDecision locked = decided(&state, BATCH_EVENT_LOCKED);
	ASSERT_BOOL_EQ(stops(locked, 1) && !locked.notify, true);
	ASSERT_BOOL_EQ(stops(decided(&state, BATCH_EVENT_CHANGED), 2), true);
	// A third signal stops at once.
	BatchJobEvent signalled = {.client = CLIENT_ABANDONED,
		.watch = BATCH_WATCH_SIGNAL,
		.accepted = true,
		.gameRunning = true,
		.harnessListening = true};
	ASSERT_BOOL_EQ(stops(decidedJob(&state, signalled), 4), true);
}

// How replies read: the harness's own result lines.
TEST(policy_reads_the_harness_replies) {
	RunReport report;
	char line[HARNESS_RESULT_LENGTH];
	jobRunTest_reportLine(false, line, sizeof line);
	ASSERT_BOOL_EQ(batchPolicy_readReply(line, true, &report) == BATCH_REPLY_OK, true);
	jobRunTest_reportLine(true, line, sizeof line);
	ASSERT_BOOL_EQ(batchPolicy_readReply(line, true, &report) == BATCH_REPLY_OUTPUT_FAILED, true);
	static const struct {
		const char *line;
		bool reportAsked;
		BatchReply reply;
	} cases[] = {
		{"", true, BATCH_REPLY_NONE},
		{"error: FM24 is busy (loading); wait until it is idle", true, BATCH_REPLY_BUSY},
		{"error: cannot read the job; see harness.log", true, BATCH_REPLY_BAD_JOB},
		{"error: the job is too long", true, BATCH_REPLY_BAD_JOB},
		{"error: harness 1.4.0 does not support this game build; see harness.log",
			true,
			BATCH_REPLY_UNSUPPORTED},
		{"error: player 12 is not in the build (--keep-build plays the input with only its seed "
		 "changed)",
			true,
			BATCH_REPLY_NOT_REBUILDABLE},
		{"error: cannot start the run; see harness.log", true, BATCH_REPLY_OUTPUT_FAILED},
		{"ok: 10/10 runs, each seed rebuilt by the game", true, BATCH_REPLY_NO_REPORT},
		{"ok: 2 teams written", false, BATCH_REPLY_OK},
		{"error: something else", false, BATCH_REPLY_ERROR},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		ASSERT_BOOL_EQ(
			batchPolicy_readReply(cases[i].line, cases[i].reportAsked, &report) == cases[i].reply, true
		);
	}
	// A report's stop and failure.
	char stopped[HARNESS_RESULT_LENGTH];
	jobRunTest_reportLine(false, line, sizeof line);
	char *at = strstr(line, "stopped=0");
	ASSERT_BOOL_EQ(at != NULL, true);
	if (at) {
		at[8] = '1';
		(void)snprintf(stopped, sizeof stopped, "error%s", strchr(line, ':'));
		ASSERT_BOOL_EQ(batchPolicy_readReply(stopped, true, &report) == BATCH_REPLY_STOPPED, true);
	}
}

// ---------------------------------------------------------------------------------------------
// batch run against the fake game

// A created batch of the standard settings and `settings`, the game not yet running.
static bool runnableBatch(wchar_t *folder, wchar_t *directory, const char *settings) {
	char text[1024];
	(void)snprintf(text, sizeof text, "%s%s", standardSettings, settings);
	return createdBatch(folder, directory, text);
}

static int runFake(const wchar_t *directory, const BatchRunOptions *options) {
	BatchHost host = fakeHost();
	BatchRunOptions none = {0};
	fake.game.signals = 0;
	return batchRun_run(directory, options ? options : &none, &host);
}


static bool finishedOnce(const wchar_t *directory) {
	ByteBuffer hashes = {0};
	bool ok = checkFinished(directory, &hashes);
	buffer_free(&hashes);
	return ok;
}

static bool notified(const char *needle) {
	return strstr(buffer_string(&fake.game.notes), needle) != NULL;
}

// batch.json's outcome is `state` with `exitCode`.
static bool endedAs(const wchar_t *directory, const char *state, int exitCode) {
	char text[64] = "";
	(void)snprintf(text, sizeof text, "\"state\":\"%s\"", state);
	char code[32];
	(void)snprintf(code, sizeof code, "\"exit_code\":%d", exitCode);
	return countIn(directory, "batch.json", text) == 1 && countIn(directory, "batch.json", code) == 1;
}

// A batch from start to end: one session, warmed up, with the canary as the reference, the
// lookups, chunk 0 checked against the canary, every index once, the computer kept awake.
TEST(runs_a_batch_with_a_session) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	const FakeGame *game = &fake.game;
	ASSERT_BOOL_EQ(game->launches == 1 && game->kills == 0 && game->keptAwake == 1, true);
	ASSERT_BOOL_EQ(game->watched, true);
	// Two warm-ups, two canaries and the eight chunks' attempts.
	ASSERT_INT_EQ((int)game->runJobs, 12);
	// The first, a warm-up: one worker, no per-match rows, seeds that fail to rebuild skipped, a
	// report, and the stop file.
	static const char *const warmup[] = {
		"\nthreads=1\n", "\nseed_errors=skip\n", "\nreport=1\n", "\nstop_file=", "\nindex_base=100\n"};
	for (size_t i = 0; i < sizeof warmup / sizeof warmup[0]; ++i) {
		ASSERT_BOOL_EQ(strstr(game->firstRunJob, warmup[i]) != NULL, true);
	}
	ASSERT_BOOL_EQ(strstr(game->firstRunJob, "per_match=1") == NULL, true);
	static const char *const files[] = {
		"sessions/s0001/session.json",
		"sessions/s0001/warmup-baseline.csv",
		"sessions/s0001/canary-press-match-players.csv",
		"canary/baseline.csv",
		"canary/baseline-match-players.csv",
		"canary/baseline-match-teams.csv",
		"canary/press-failed.csv",
		"lookups/teams.txt",
		"lookups/players.txt",
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		ASSERT_BOOL_EQ(exists(directory, files[i]), true);
	}
	ASSERT_BOOL_EQ(!exists(directory, "lookups/teams.txt.tmp"), true);
	const char *session = "sessions/s0001/session.json";
	ASSERT_INT_EQ((int)countIn(directory, session, "\"canary\":\"reference\""), 1);
	ASSERT_INT_EQ((int)countIn(directory, session, "\"warmup\":true"), 1);
	ASSERT_INT_EQ((int)countIn(directory, session, "\"launched_by_batch\":true"), 1);
	ASSERT_INT_EQ((int)countIn(directory, session, "\"jobs_at_start\":0"), 1);
	ASSERT_INT_EQ((int)countIn(directory, session, fakeCommit), 1);
	ASSERT_INT_EQ(
		(int)countIn(directory, "batch.json", "\"pinned\":{\"harness\":{\"session\":\"s0001\""), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"reason\":\"batch run started\""), 1);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000100/chunk.json",
			"\"canary_reference_match\":true"
		),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"canary_reference_match\":null"
		),
		1
	);
	const char *events = "events.log";
	ASSERT_INT_EQ((int)countIn(directory, events, " run_started "), 1);
	ASSERT_INT_EQ((int)countIn(directory, events, " session_started id=s0001 "), 1);
	ASSERT_INT_EQ((int)countIn(directory, events, " warmup session=s0001 "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " canary session=s0001 "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " reference=match"), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " lookup "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " complete"), 1);
	ASSERT_BOOL_EQ(notified("batch test-batch is complete"), true);
	// A complete batch has nothing more to play.
	unsigned jobs = game->runJobs;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(game->runJobs == jobs && finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The game crashes mid-chunk, seen as the connection closing or by the watchdog: the prefix is a
// part, the game starts again, warms up and repeats the canary, and the chunk goes on from there.
TEST(relaunches_after_a_crash) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, 113, 1, "baseline");
	addFault(FAULT_CRASH_SEEN, 0, 125, 1, "press");
	addFault(FAULT_RESTART, 0, 131, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(fake.game.launches == 4 && fake.game.kills == 0, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 3);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_launched "), 3);
	// The game Steam started again is seen at the next tick, not after hang_minutes.
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang "), 0);
	ASSERT_BOOL_EQ(fake.game.monotonic < 300000, true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"sessions/s0004/session.json",
			"\"reason\":\"the game was restarted outside the batch\""
		),
		1
	);
	ASSERT_INT_EQ((int)countIn(directory, "sessions/s0002/session.json", "\"canary\":\"match\""), 1);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "sessions/s0002/session.json", "\"reason\":\"started again after the game exited\""
		),
		1
	);
	ASSERT_BOOL_EQ(exists(directory, "sessions/s0003/warmup-press.csv"), true);
	const char *chunk = "chunks/arm=baseline/chunk=0000000110/chunk.json";
	ASSERT_INT_EQ(
		(int)countIn(directory, chunk, "\"count\":10,\"verified\":3,\"outcome\":\"crashed\""), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, chunk, "\"client_outcome\":\"lost\""), 1);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=press/chunk=0000000120/chunk.json", "\"client_outcome\":\"abandoned\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A busy game: a minute's wait and a retry, until busy_wait_minutes stops the batch with 3.
TEST(waits_for_a_busy_game) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "busy_wait_minutes=2\n"), true);
	addFault(FAULT_BUSY, 5, UINT64_MAX, 1, NULL);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(fake.game.monotonic >= 60000, true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000100/chunk.json",
			"\"verified\":0,\"outcome\":\"error\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "busy_wait_minutes=2\n"), true);
	addFault(FAULT_BUSY, 0, UINT64_MAX, 0, NULL);
	ASSERT_INT_EQ(runFake(directory, NULL), 3);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 3), true);
	ASSERT_BOOL_EQ(fake.game.monotonic >= 120000 && fake.game.monotonic < 240000, true);
	ASSERT_BOOL_EQ(notified("stayed busy"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Another save stops the batch with 3, keeping its state; the start screen too, unless kill_game=1
// lets the batch quit the game and start it with the save.
TEST(stops_for_another_save_or_the_start_screen) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addAnswer(3, SESSION_OTHER_SAVE);
	ASSERT_INT_EQ(runFake(directory, NULL), 3);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 3), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "another save is loaded"), 1);
	ASSERT_BOOL_EQ(fake.game.runJobs == 0 && notified("another save is loaded"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	launchGame(false);
	addAnswer(3, SESSION_AT_MENU);
	ASSERT_INT_EQ(runFake(directory, NULL), 3);
	ASSERT_BOOL_EQ(fake.game.kills == 0, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\n"), true);
	launchGame(false);
	addAnswer(3, SESSION_AT_MENU);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && fake.game.launches == 2, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 1);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// With launch=0 and the game not running, the batch stops at the save check, before a session or
// a job; once the game runs, it plays.
TEST(stops_without_the_game_with_launch_0) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "launch=0\n"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "batch.json", "the game is not running, and launch=0"), 1
	);
	ASSERT_BOOL_EQ(fake.game.runJobs == 0 && fake.game.launches == 0, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " session_started "), 0);
	ASSERT_BOOL_EQ(!exists(directory, "sessions/s0001/session.json"), true);
	launchGame(false);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// On battery the batch pauses, checking each minute, until the computer is on AC power again.
TEST(pauses_on_battery) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	for (size_t i = 0; i < 3; ++i) {
		fake.game.power[fake.game.powerCount++] = BATCH_POWER_BATTERY;
	}
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " paused reason=battery"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " resumed paused_ms=180000"), 1);
	// Kept awake on AC power only, so that the computer can sleep while the batch pauses.
	ASSERT_BOOL_EQ(fake.game.keptAwake == 1 && !fake.game.keptAwakeOnBattery, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Played on battery power too, and kept awake there.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "pause_on_battery=0\n"), true);
	for (size_t i = 0; i < 16; ++i) {
		fake.game.power[fake.game.powerCount++] = BATCH_POWER_BATTERY;
	}
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.keptAwake == 1 && fake.game.keptAwakeOnBattery, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " paused "), 0);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=baseline/chunk=0000000100/chunk.json", "\"power\":\"battery\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Less free space than min_free_gb and three chunks' files stops the batch before it plays.
TEST(stops_when_the_disk_is_nearly_full) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.freeBytes = 20ULL << 30; // min_free_gb=20, with no room for a chunk
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(fake.game.runJobs == 0 && endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " disk free_bytes="), 1);
	ASSERT_BOOL_EQ(notified("not enough free space"), true);
	fake.freeBytes = 21ULL << 30;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A job that stops making progress: quit with kill_game=1 (then the crash path), else watched
// until it ends and kept, or until queue_wait_minutes stop the batch.
TEST(handles_a_hung_job) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\nhang_minutes=1\n"), true);
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && fake.game.launches == 2, true);
	// The watchdog gave up on it after a minute without growth, not the client's own timeout.
	ASSERT_BOOL_EQ(fake.game.monotonic < 3600 * 1000, true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"client_outcome\":\"abandoned\""
		),
		1
	);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 1);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"verified\":3,\"outcome\":\"abandoned\""
		),
		1
	);
	ASSERT_BOOL_EQ(notified("made no progress"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Without kill_game the job is watched: it ends, and every index it played is kept.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "hang_minutes=1\n"), true);
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(fake.game.kills == 0 && fake.game.launches == 1, true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"verified\":10,\"outcome\":\"adopted\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// One that never ends: the batch stops, leaving the attempt to the next run, which finds the
	// game gone (quit by hand) and keeps what the job played.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "hang_minutes=1\nqueue_wait_minutes=1\n"), true);
	fake.game.hangQueries = UINT32_MAX;
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(fake.game.kills == 0 && endedAs(directory, "stopped", 1), true);
	ASSERT_BOOL_EQ(
		exists(directory, "attempts/baseline/0000000110/a1/attempt.json") &&
			!exists(directory, "attempts/baseline/0000000110/a1/part.json"),
		true
	);
	// Its reason, and notification, say that the job may still run.
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"batch.json",
			"a job that may still run in FM24 did not end within queue_wait_minutes"
		),
		1
	);
	ASSERT_BOOL_EQ(notified("may still run in FM24") && !notified("a job failed"), true);
	killGame();
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"verified\":3,\"outcome\":\"crashed\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The computer slept mid-job: noted, not a hang, and the part's slept_ms leaves it out of rates.
TEST(notes_a_sleep_that_is_not_a_hang) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "hang_minutes=1\n"), true);
	addFault(FAULT_SLEEP, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " slept ms=7200000"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang "), 0);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=baseline/chunk=0000000110/chunk.json", "\"slept_ms\":7200000"
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Results that no longer repeat after a restart stop the batch (on_canary_mismatch=stop), or tag
// the session and go on; a periodic canary that differs makes the chunks since the last good one
// suspect.
TEST(checks_that_results_repeat) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.game.variedFrom = 2;
	addFault(FAULT_CRASH, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "sessions/s0002/session.json", "\"canary\":\"mismatch\""), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " result=mismatch"), 2);
	ASSERT_BOOL_EQ(notified("no longer repeat"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "on_canary_mismatch=continue\n"), true);
	fake.game.variedFrom = 2;
	addFault(FAULT_CRASH, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "sessions/s0002/session.json", "\"nonrepeatable\":true"), 1
	);
	ASSERT_INT_EQ(
		(int)countIn(directory, "sessions/s0001/session.json", "\"nonrepeatable\":false"), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Every chunk a canary: from the third chunk's attempt on, results differ, so the canary after
	// it differs and its chunk is suspect.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "canary_every=1\n"), true);
	fake.game.variedFromJob = 9;
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "suspect.csv", "arm,chunk,session,reason\n"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "suspect.csv", ",s0001,canary_mismatch\n"), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Chunk 0 against the reference: it differs if the game's results changed before it.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "on_canary_mismatch=continue\n"), true);
	fake.game.variedFromJob = 5;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000100/chunk.json",
			"\"canary_reference_match\":false"
		),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(directory, "suspect.csv", "baseline,0000000100,s0001,reference_mismatch\n"), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Another harness commit, game build or slice than the first session's stops the batch, unless
// --allow-harness-change records it.
TEST(pins_the_harness_identity) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.maxChunks = 1}), 4);
	killGame();
	fake.game.build = 2081900; // a Steam update
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " identity_changed session=s0002 "), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"session\":\"s0001\""), 1);
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.allowHarnessChange = true}), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "sessions/s0003/session.json", "\"identity_changed\":true"), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"identity_changed\":true"), 2);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// A change found by a chunk's own job (with no warm-up or canary) leaves its rows out.
	ASSERT_BOOL_EQ(
		createdBatch(folder, directory, "seeds=scatter:9:100-133\nchunk=10\nwarmup=0\ncanary=0\n"), true
	);
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.maxChunks = 1}), 4);
	killGame();
	fake.game.commit = "fedcba9876543210fedcba9876543210fedcba98";
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ(
		(int)countIn(directory, "attempts/press/0000000100/a1/part.json", "\"verified\":0,"), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A seed that crashes the game every time: one worker over the seeds under way, that seed alone,
// then excluded as poison; more poison seeds than max_poison stop the batch.
TEST(excludes_a_seed_that_crashes_the_game) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)fake.game.launches, 5);
	const char *chunk = "chunks/arm=press/chunk=0000000120/chunk.json";
	char excluded[96];
	(void)snprintf(
		excluded,
		sizeof excluded,
		"\"excluded\":[{\"index\":%d,\"seed\":%u,\"reason\":\"poison\"}]",
		POISON_INDEX,
		batchSeeds_scatter(9, POISON_INDEX)
	);
	ASSERT_INT_EQ((int)countIn(directory, chunk, excluded), 1);
	ASSERT_BOOL_EQ(countIn(directory, chunk, "\"threads\":1,") >= 2, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " poison arm=press "), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"poison\":1"), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "max_poison=0\n"), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "crashed the game than max_poison\""), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// max_poison seeds are allowed: only one more stops the batch.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "max_poison=1\n"), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"poison\":1"), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// control/stop finishes the chunk under way, control/stop-now keeps what the job played; a third
// signal leaves the job running, which the next run waits for and adopts.
TEST(stops_as_asked) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_STOP, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
	ASSERT_BOOL_EQ(
		exists(directory, "chunks/arm=baseline/chunk=0000000110/chunk.json") &&
			!exists(directory, "attempts/press/0000000110"),
		true
	);
	ASSERT_BOOL_EQ(!notified("stopped"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(!exists(directory, "control/stop") && finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_STOP_NOW, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	// The match under way when control/stop-now appeared, 113, finished.
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"attempts/baseline/0000000110/a1/part.json",
			"\"verified\":4,\"outcome\":\"stopped\""
		),
		1
	);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// control/stop with a chunk under way, then the game crashes: the batch stops without starting
	// it again, keeping the crashed part, and the next run finishes the chunk in a new session.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH_STOP, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(
		!exists(directory, "chunks/arm=baseline/chunk=0000000110/chunk.json") &&
			fake.game.launches == 1,
		true
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"attempts/baseline/0000000110/a1/part.json",
			"\"verified\":3,\"outcome\":\"crashed\""
		),
		1
	);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_DIE, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(!exists(directory, "attempts/baseline/0000000110/a1/part.json"), true);
	unsigned queries = fake.game.statusQueries;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.statusQueries >= queries + 2, true);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"verified\":10,\"outcome\":\"adopted\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The proof again, through batch run: the runner killed after each step of every change, then
// started again, plays every index once, with the contents of an uninterrupted run.
TEST(runs_every_index_once_whatever_step_is_killed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t pristine[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, pristine, ""), true);
	ASSERT_BOOL_EQ(joinPath(directory, folder, "run"), true);
	ByteBuffer reference = {0};
	ASSERT_BOOL_EQ(copyTree(pristine, directory) && runFake(directory, NULL) == 0, true);
	ASSERT_BOOL_EQ(checkFinished(directory, &reference), true);
	unsigned total = fake.steps;
	ASSERT_BOOL_EQ(total > 60, true);
	bool same = true;
	unsigned killed = 0;
	for (unsigned step = 1; step <= total && same; ++step) {
		ASSERT_BOOL_EQ(batch_removeFolder(directory) && copyTree(pristine, directory), true);
		resetFake(folder);
		addFault(FAULT_CRASH, 0, 113, 1, "baseline");
		fake.killAt = step;
		int first = runFake(directory, NULL);
		killed += first == BATCH_RUN_KILLED;
		fake.killAt = 0;
		int second = first == BATCH_RUN_KILLED ? runFake(directory, NULL) : first;
		ByteBuffer hashes = {0};
		same = second == 0 && checkFinished(directory, &hashes) && hashes.length == reference.length &&
					 memcmp(hashes.data, reference.data, hashes.length) == 0;
		if (!same) {
			fprintf(stderr, "killed at step %u: %d then %d\n", step, first, second);
		}
		buffer_free(&hashes);
	}
	ASSERT_BOOL_EQ(same, true);
	ASSERT_BOOL_EQ(killed > 60, true);
	buffer_free(&reference);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// --max-chunks and --until stop at a chunk boundary with 4; another runner's lock gives 1, a
// changed plan 2.
TEST(stops_at_limits_and_refuses_a_second_runner) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.maxChunks = 2}), 4);
	ASSERT_INT_EQ((int)countIn(directory, "index.csv", "\n"), 3);
	ASSERT_INT_EQ(
		runFake(directory, &(BatchRunOptions){.hasUntil = true, .until = fakeNow(NULL)}), 4
	);
	ASSERT_INT_EQ((int)countIn(directory, "index.csv", "\n"), 3);
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.threads = 2}), 0);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=press/chunk=0000000130/chunk.json", "\"threads\":2,\"started_at\""
		),
		1
	);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	wchar_t lockPath[TEST_PATH_LENGTH];
	PlatformFileLock lock;
	ASSERT_BOOL_EQ(joinPath(lockPath, directory, "batch.lock"), true);
	ASSERT_BOOL_EQ(platform_lockFile(lockPath, &lock) == PLATFORM_LOCK_TAKEN, true);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	platform_unlockFile(lock);
	ASSERT_BOOL_EQ(editFile(directory, "inputs/press.json", "\"u8\":43", "\"u8\":44"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 2);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	time_t now = 1761383000;
	time_t next = batchRun_nextLocalTime(now, 6, 30);
	struct tm local = {0};
#ifdef _WIN32
	(void)localtime_s(&local, &next);
#else
	(void)localtime_r(&next, &local);
#endif
	ASSERT_BOOL_EQ(next >= now && next - now <= 25 * 3600, true);
	ASSERT_BOOL_EQ(local.tm_hour == 6 && local.tm_min == 30 && local.tm_sec == 0, true);
	ASSERT_BOOL_EQ(batchRun_nextLocalTime(next, 6, 30) == next, true);
}

// The fixtures' date and slice, a game that does not start, seeds that fail, a harness gone, and
// foreign jobs.
TEST(checks_the_game_before_and_between_chunks) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.game.date = "2024 day 124 15:00"; // Continue was pressed
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(fake.game.runJobs == 0, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// The other slice: 3, or quit and started again in the plan's.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.game.launchSlice = BATCH_SLICE_X86_64;
	ASSERT_INT_EQ(runFake(directory, NULL), 3);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\n"), true);
	fake.game.launchSlice = BATCH_SLICE_X86_64;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && fake.game.slice == BATCH_SLICE_ARM64, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// The game fails to start twice (Steam), then starts.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addAnswer(1, SESSION_EXITED);
	addAnswer(1, SESSION_WAITING);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.monotonic >= (300 + 900) * 1000ULL, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	for (int i = 0; i < 3; ++i) {
		addAnswer(1, SESSION_EXITED);
	}
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(notified("Steam"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// A seed that fails once is tried again; failing twice stops the batch (seed_errors=stop).
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "seed_errors=stop\n"), true);
	addFault(FAULT_SEED_FAILS, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "seed_errors=stop\n"), true);
	addFault(FAULT_SEED_FAILS, 0, 113, 0, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "the same seed failed twice"), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Rebuild failures beyond max_seed_errors, counted in parts before their chunk commits too.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "max_seed_errors=0\n"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "failed to rebuild (max_seed_errors)\""), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(
		createdBatch(
			folder,
			directory,
			"seeds=scatter:9:100-133\nchunk=40\nthreads=4\ncanary=3\nmax_seed_errors=0\n"
		),
		true
	);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "failed to rebuild (max_seed_errors)\""), 1);
	ASSERT_BOOL_EQ(!exists(directory, "chunks/arm=baseline/chunk=0000000100/chunk.json"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// The harness stops serving in a running game: stop, or quit it with kill_game=1.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_HARNESS_GONE, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\n"), true);
	addFault(FAULT_HARNESS_GONE, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Another runner's jobs between chunks: the canary runs again before the next.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_FOREIGN, 7, UINT64_MAX, 1, NULL);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " foreign_jobs session=s0001 count=3"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " canary session=s0001 "), 4);
	ASSERT_INT_EQ((int)countIn(directory, "sessions/s0001/session.json", "\"foreign_jobs\":3"), 1);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// An unsupported build.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_UNSUPPORTED, 1, UINT64_MAX, 0, NULL);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The game crashes, and the kernel closes its connection and frees its harness's lock before its
// process has gone: a crash, which relaunches the game, not a game running without its harness
// (which would stop the batch, or quit the game with kill_game=1).
TEST(relaunches_a_game_that_is_still_exiting) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	static const char *const settings[] = {"", "kill_game=1\n"};
	for (size_t i = 0; i < sizeof settings / sizeof settings[0]; ++i) {
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, settings[i]), true);
		addFault(FAULT_CRASH_EXITING, 0, 113, 1, "baseline");
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_BOOL_EQ(fake.game.launches == 2 && fake.game.kills == 0, true);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 1);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 0);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// A hung job is watched (no kill_game), and the game is quit meanwhile, its process seen for most
// of BATCH_EXIT_GRACE_S after its harness has gone: a crash, which relaunches the game. Seen for
// longer, it is a game running without its harness, which stops the batch until it has gone.
TEST(counts_quitting_a_game_whose_hung_job_is_watched_as_a_crash) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	// The checks that still find the process: two, then one every 0.1 s of the grace, so 100 see it
	// for 9.7 of its 10 seconds.
	static const struct {
		unsigned checks;
		int exitCode;
	} cases[] = {{100, 0}, {150, 1}};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, "hang_minutes=1\n"), true);
		fake.game.hangQueries = UINT32_MAX;
		fake.game.quitAfterSleeps = 3;
		fake.game.quitExitingChecks = cases[i].checks;
		addFault(FAULT_HANG, 0, 113, 1, "baseline");
		ASSERT_INT_EQ(runFake(directory, NULL), cases[i].exitCode);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 1);
		ASSERT_BOOL_EQ(fake.game.kills == 0, true);
		if (cases[i].exitCode == 0) {
			ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
			ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 1);
			ASSERT_INT_EQ(
				(int)countIn(
					directory,
					"chunks/arm=baseline/chunk=0000000110/chunk.json",
					"\"verified\":3,\"outcome\":\"crashed\""
				),
				1
			);
		} else {
			ASSERT_BOOL_EQ(fake.game.launches == 1 && endedAs(directory, "stopped", 1), true);
			ASSERT_INT_EQ((int)countIn(directory, "batch.json", "runs without this harness"), 1);
			fake.game.exitingChecks = 0; // it has gone
			ASSERT_INT_EQ(runFake(directory, NULL), 0);
			ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
		}
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// A seed is poison only once it has crashed the game twice with one worker: an attempt at it that
// played nothing without a crash (here a busy game) leaves it to be tried again.
TEST(excludes_a_seed_only_after_two_crashes_with_one_worker) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	const char *chunk = "chunks/arm=press/chunk=0000000120/chunk.json";
	// Three crashes (four workers, four, then one), a busy reply, then the seed plays.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 3, "press");
	addFault(FAULT_BUSY, 0, POISON_INDEX, 1, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, chunk, "\"excluded\":[]"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " poison arm=press "), 0);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, chunk, "\"first_index\":124,\"count\":1,\"verified\":0,\"outcome\":\"error\""
		),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, chunk, "\"first_index\":124,\"count\":1,\"verified\":1,\"outcome\":\"ok\""
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// The same, but the seed alone crashes the game again after the busy reply: now it is poison.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 3, "press");
	addFault(FAULT_BUSY, 0, POISON_INDEX, 1, "press");
	addFault(FAULT_CRASH, 0, POISON_INDEX, 1, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, chunk, "\"reason\":\"poison\""), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " poison arm=press "), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Isolating a seed takes four crashes, two of them with one worker, which max_crashes_per_hour
// does not count: a seed that crashes the game in both arms, or a crash besides one, does not stop
// a batch with the default of 4.
TEST(isolates_seeds_within_the_crash_limit) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "baseline");
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"poison\":2"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 8);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " isolating=1"), 4);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_CRASH, 0, 103, 1, "baseline");
	addFault(FAULT_CRASH, 0, 113, 1, "press");
	addFault(FAULT_CRASH, 0, POISON_INDEX, 0, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"poison\":1"), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// max_crashes_per_hour counts the crashes of the last hour, time asleep included: one more stops
// the batch, and the same crashes hours apart do not.
TEST(stops_after_too_many_crashes_in_an_hour) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "max_crashes_per_hour=1\n"), true);
	addFault(FAULT_CRASH, 0, 113, 1, "baseline");
	addFault(FAULT_CRASH, 0, 125, 1, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "crashed more than max_crashes_per_hour"), 1);
	ASSERT_BOOL_EQ(notified("max_crashes_per_hour"), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "max_crashes_per_hour=1\n"), true);
	addFault(FAULT_CRASH, 0, 113, 1, "baseline");
	addFault(FAULT_SLEEP, 0, 117, 1, "baseline");
	addFault(FAULT_CRASH, 0, 125, 1, "press");
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 2);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A job a stopped run left in the game hangs while the next run waits for it: without kill_game
// the game is left alone (noted, then the wait gives up), and with kill_game=1 it is quit and
// started again.
TEST(handles_a_hang_in_a_job_a_stopped_run_left) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "hang_minutes=1\nqueue_wait_minutes=3\n"), true);
	fake.game.hangQueries = UINT32_MAX;
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 1);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(fake.game.kills == 0 && fake.game.launches == 1, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 2);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", "may still run in FM24"), 2);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 0);
	killGame();
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(
		runnableBatch(folder, directory, "kill_game=1\nhang_minutes=1\nqueue_wait_minutes=3\n"), true
	);
	addFault(FAULT_DIE, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	fake.game.background.queries = UINT32_MAX;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && fake.game.launches == 2, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The attempt at baseline's chunk 110 that a hang at 113 leaves running, without part.json.
static bool jobLeftRunning(const wchar_t *directory) {
	return fake.game.background.active && fake.game.running &&
				 exists(directory, "attempts/baseline/0000000110/a1/attempt.json") &&
				 !exists(directory, "attempts/baseline/0000000110/a1/part.json") &&
				 endedAs(directory, "stopped", 4) &&
				 countIn(directory, "batch.json", "a job that made no progress still runs in FM24 (pid 4001)") ==
					 1 &&
				 notified("still runs in FM24 (pid 4001)");
}

// How long the run went on after the `index`th action, in seconds of time awake.
static uint64_t secondsAfter(size_t index) {
	const Action *action = &fake.game.actions[index];
	return action->at ? (fake.game.uptime - action->at) / 1000 : UINT64_MAX;
}

// Ends the batch: the job left running ends (unless the game has gone), and the next run keeps it
// and plays the rest, starting the game if it has to.
static bool finishesAfterwards(const wchar_t *directory) {
	fake.game.background.hung = false;
	fake.game.background.queries = 2;
	return runFake(directory, NULL) == 0 && finishedOnce(directory);
}

// A stop asked while batch run watches a hung job (kill_game=0) ends the wait within a tick, not
// after queue_wait_minutes: batch stop, batch stop --now and a signal each exit with 4, leaving
// the job running in the game without part.json and saying so. The next run keeps what it played.
TEST(stops_while_a_hung_job_is_watched) {
	static const ActionKind kinds[] = {ACT_STOP, ACT_STOP_NOW, ACT_SIGNAL};
	for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		fake.game.hangQueries = UINT32_MAX;
		addFault(FAULT_HANG, 0, 113, 1, "baseline");
		// The hang is noted after hang_minutes without growth; the stop comes a minute later.
		addAction(directory, kinds[i], (Action){.afterMs = 11 * 60 * 1000});
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		ASSERT_BOOL_EQ(secondsAfter(0) <= 2 * BATCH_TICK_S, true);
		ASSERT_BOOL_EQ(jobLeftRunning(directory), true);
		ASSERT_BOOL_EQ(fake.game.launches == 1 && fake.game.kills == 0, true);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 1);
		ASSERT_BOOL_EQ(finishesAfterwards(directory) && fake.game.launches == 1, true);
		ASSERT_INT_EQ(
			(int)countIn(
				directory, "chunks/arm=baseline/chunk=0000000110/chunk.json", "\"outcome\":\"adopted\""
			),
			1
		);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// Before the hang is noted: a stop takes effect once it is (hang_minutes without growth), and a
// stop-now, or a second signal, once the job has made no progress for BATCH_STOP_NOW_GRACE_S
// after it, with kill_game=0 leaving the job running.
TEST(stops_a_job_that_hangs_once_it_is_noted_or_left_unanswered) {
	static const struct {
		ActionKind kinds[2];
		bool now;
	} cases[] = {
		{{ACT_STOP}, false},
		{{ACT_SIGNAL}, false},
		{{ACT_STOP_NOW}, true},
		{{ACT_SIGNAL, ACT_SIGNAL}, true},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		fake.game.hangQueries = UINT32_MAX;
		addFault(FAULT_HANG, 0, 113, 1, "baseline");
		size_t count = 0;
		for (size_t k = 0; k < 2 && cases[i].kinds[k]; ++k) {
			addAction(directory, cases[i].kinds[k], (Action){.afterMs = (60 + 10 * k) * 1000});
			++count;
		}
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		uint64_t afterHang = (fake.game.uptime - fake.game.hangStartedAt) / 1000;
		if (cases[i].now) {
			uint64_t after = secondsAfter(count - 1);
			ASSERT_BOOL_EQ(
				after >= BATCH_STOP_NOW_GRACE_S && after <= BATCH_STOP_NOW_GRACE_S + 2 * BATCH_TICK_S,
				true
			);
			ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 0);
		} else {
			ASSERT_BOOL_EQ(afterHang >= 10 * 60 && afterHang <= 10 * 60 + 2 * BATCH_TICK_S, true);
			ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 1);
		}
		ASSERT_BOOL_EQ(jobLeftRunning(directory), true);
		ASSERT_BOOL_EQ(fake.game.launches == 1 && fake.game.kills == 0, true);
		ASSERT_BOOL_EQ(finishesAfterwards(directory) && fake.game.launches == 1, true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// With kill_game=1, a stop asked while a job hangs quits the game once the hang is noted, and a
// stop-now once the job has left it unanswered; either way the part is kept as abandoned and the
// batch stops with 4 without starting the game again.
TEST(quits_a_hung_game_but_does_not_start_it_again_once_asked_to_stop) {
	static const struct {
		ActionKind kinds[2];
		bool now;
	} cases[] = {
		{{ACT_STOP}, false},
		{{ACT_SIGNAL}, false},
		{{ACT_STOP_NOW}, true},
		{{ACT_SIGNAL, ACT_SIGNAL}, true},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\n"), true);
		fake.game.hangQueries = UINT32_MAX;
		addFault(FAULT_HANG, 0, 113, 1, "baseline");
		size_t count = 0;
		for (size_t k = 0; k < 2 && cases[i].kinds[k]; ++k) {
			addAction(directory, cases[i].kinds[k], (Action){.afterMs = (60 + 10 * k) * 1000});
			++count;
		}
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		uint64_t after = cases[i].now ? secondsAfter(count - 1)
																	: (fake.game.uptime - fake.game.hangStartedAt) / 1000;
		uint64_t expected = cases[i].now ? BATCH_STOP_NOW_GRACE_S : 10 * 60;
		ASSERT_BOOL_EQ(after >= expected && after <= expected + 2 * BATCH_TICK_S, true);
		ASSERT_BOOL_EQ(fake.game.launches == 1 && fake.game.kills == 1 && !fake.game.running, true);
		ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", "stopped as asked after the game exited"), 1);
		ASSERT_INT_EQ(
			(int)countIn(
				directory,
				"attempts/baseline/0000000110/a1/part.json",
				"\"verified\":3,\"outcome\":\"abandoned\""
			),
			1
		);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " session_started "), 1);
		ASSERT_BOOL_EQ(finishesAfterwards(directory) && fake.game.launches == 2, true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// Once a stop is asked, a game that exits mid-chunk, quit by hand or crashed, is not started again:
// its part is kept as crashed and the batch stops with 4, with launch=0 too, and the next run
// finishes the chunk. The game found not running before an attempt is not started either.
TEST(never_starts_the_game_again_once_asked_to_stop) {
	static const struct {
		const char *settings;
		ActionKind kind;
	} cases[] = {
		{"", ACT_STOP},
		{"", ACT_SIGNAL},
		{"kill_game=1\n", ACT_STOP},
		{"launch=0\n", ACT_STOP},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, cases[i].settings), true);
		bool launch = strstr(cases[i].settings, "launch=0") == NULL;
		if (!launch) {
			launchGame(false); // started by hand, as launch=0 needs
		}
		fake.game.hangQueries = UINT32_MAX;
		addFault(FAULT_HANG, 0, 113, 1, "baseline");
		addAction(directory, cases[i].kind, (Action){.afterMs = 60 * 1000});
		addAction(directory, ACT_QUIT, (Action){.afterMs = 120 * 1000});
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		ASSERT_BOOL_EQ(secondsAfter(1) <= 2 * BATCH_TICK_S, true);
		ASSERT_BOOL_EQ(fake.game.launches == 1 && fake.game.kills == 0, true);
		ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", "stopped as asked after the game exited"), 1);
		// What the next run does: start the game, or, with launch=0, need it started.
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", "the next batch run starts it"), launch);
		ASSERT_INT_EQ(
			(int)countIn(directory, "batch.json", "start it with the save, then run the batch again"),
			!launch
		);
		ASSERT_INT_EQ(
			(int)countIn(
				directory,
				"attempts/baseline/0000000110/a1/part.json",
				"\"verified\":3,\"outcome\":\"crashed\""
			),
			1
		);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 1);
		if (!launch) {
			launchGame(false);
		}
		ASSERT_BOOL_EQ(finishesAfterwards(directory) && fake.game.launches == 2, true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
	// A run that finds the game not running with a chunk under way, and a stop asked before it
	// would start the game: it stops without starting it.
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_STOP_NOW, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	killGame();
	addAction(directory, ACT_STOP, (Action){.powerCheck = fake.game.powerChecks + 1});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(fake.game.launches == 1 && endedAs(directory, "stopped", 4), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "stopped as asked; the game is not running"), 1);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A job waiting behind another runner's job is abandoned as soon as a stop is asked, rather than
// after queue_wait_minutes: the harness never takes it, and the batch stops with 4.
TEST(abandons_a_job_not_yet_taken_when_asked_to_stop) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	launchGame(false);
	fake.game.background = (Background){.active = true, .hung = true, .queries = UINT32_MAX};
	// The new session's status query waits behind the other job; the stop comes then.
	addAction(directory, ACT_STOP, (Action){.query = fake.game.statusQueries + 1});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(secondsAfter(0) <= BATCH_STATUS_WAIT_S + 2 * BATCH_TICK_S, true);
	ASSERT_BOOL_EQ(fake.game.runJobs == 0 && fake.game.launches == 1, true);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
	ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"reason\":\"stopped as asked\""), 1);
	fake.game.background.active = false;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A stop asked while the barrier waits for a job a stopped run left is acted on, not thrown away:
// a stop once the job has ended and its chunk is committed, a stop-now (which the job stops at
// too) once it has ended; and, while it hangs, a stop-now or a second signal it leaves unanswered
// at once, and a stop once the hang is noted, leaving it running.
TEST(stops_as_asked_while_waiting_for_a_job_a_stopped_run_left) {
	static const struct {
		ActionKind kinds[2];
		bool hung;
		unsigned query; // the status query of the barrier the first action comes at
		const char *reason;
	} cases[] = {
		{{ACT_STOP}, false, 3, "\"reason\":\"stopped as asked\""},
		{{ACT_SIGNAL}, false, 3, "\"reason\":\"stopped as asked\""},
		{{ACT_STOP_NOW}, false, 3, "\"reason\":\"stopped as asked (stop-now)\""},
		{{ACT_STOP_NOW}, true, 3, "still runs in FM24 (pid 4001)"},
		{{ACT_SIGNAL, ACT_SIGNAL}, true, 3, "still runs in FM24 (pid 4001)"},
		{{ACT_STOP}, true, 3, "still runs in FM24 (pid 4001)"},
		{{ACT_STOP}, true, 60, "still runs in FM24 (pid 4001)"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		addFault(FAULT_DIE, 0, 113, 1, "baseline");
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		// The job the third signal left ends after 20 status queries, or hangs.
		fake.game.background.hung = cases[i].hung;
		fake.game.background.queries = cases[i].hung ? UINT32_MAX : 20;
		size_t count = 0;
		for (size_t k = 0; k < 2 && cases[i].kinds[k]; ++k) {
			unsigned query = fake.game.statusQueries + cases[i].query + (unsigned)k;
			addAction(directory, cases[i].kinds[k], (Action){.query = query});
			++count;
		}
		unsigned runJobs = fake.game.runJobs;
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", cases[i].reason), 1);
		ASSERT_BOOL_EQ(fake.game.runJobs == runJobs && fake.game.launches == 1, true);
		if (!cases[i].hung) {
			// The job ended and was kept; a stop let its chunk be committed first.
			bool now = cases[i].kinds[0] == ACT_STOP_NOW;
			ASSERT_BOOL_EQ(!fake.game.background.active, true);
			ASSERT_BOOL_EQ(
				exists(directory, "chunks/arm=baseline/chunk=0000000110/chunk.json") == !now, true
			);
			if (now) {
				ASSERT_INT_EQ(
					(int)countIn(
						directory,
						"attempts/baseline/0000000110/a1/part.json",
						"\"verified\":3,\"outcome\":\"adopted\""
					),
					1
				);
			}
		} else {
			ASSERT_BOOL_EQ(jobLeftRunning(directory), true);
			uint64_t after = secondsAfter(count - 1);
			bool now = cases[i].kinds[0] != ACT_STOP;
			if (now) {
				ASSERT_BOOL_EQ(after >= BATCH_STOP_NOW_GRACE_S && after <= BATCH_STOP_NOW_GRACE_S + 30, true);
			} else if (cases[i].query == 3) {
				// Asked before the hang was noted: the stop waits for it.
				ASSERT_BOOL_EQ(after >= 9 * 60 && after <= 11 * 60, true);
			} else {
				ASSERT_BOOL_EQ(after <= 2 * BATCH_TICK_S, true);
			}
			ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), !now);
		}
		ASSERT_BOOL_EQ(finishesAfterwards(directory) && fake.game.launches == 1, true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// The stop-now a stopped run left is kept for its job alone: the next run waits for that job as
// usual (here giving up after queue_wait_minutes, with a reason that says it may still run),
// while a new batch stop --now during that wait stops it. Once the job has ended, the old stop-now
// is removed and the batch goes on.
TEST(keeps_a_stale_stop_now_for_the_job_it_was_meant_for) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "queue_wait_minutes=3\n"), true);
	fake.game.hangQueries = UINT32_MAX;
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	addAction(directory, ACT_STOP_NOW, (Action){.afterMs = 60 * 1000});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(jobLeftRunning(directory) && exists(directory, "control/stop-now"), true);
	ByteBuffer stale = {0};
	ASSERT_BOOL_EQ(readText(directory, "control/stop-now", &stale) && stale.length > 0, true);
	fake.game.actionCount = 0;
	buffer_free(&fake.game.notes);
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"batch.json",
			"a job that may still run in FM24 did not end within queue_wait_minutes"
		),
		1
	);
	ASSERT_BOOL_EQ(notified("may still run in FM24") && !notified("a job failed"), true);
	ByteBuffer kept = {0};
	ASSERT_BOOL_EQ(
		readText(directory, "control/stop-now", &kept) && kept.length == stale.length &&
			memcmp(kept.data, stale.data, kept.length) == 0,
		true
	);
	ASSERT_BOOL_EQ(fake.game.background.active, true);
	addAction(directory, ACT_STOP_NOW, (Action){.query = fake.game.statusQueries + 3});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(secondsAfter(0) <= BATCH_STOP_NOW_GRACE_S + 30, true);
	ASSERT_BOOL_EQ(fake.game.background.active && endedAs(directory, "stopped", 4), true);
	// The job ends at the stop-now; the next run keeps it, removes the stop-now and goes on.
	ASSERT_BOOL_EQ(finishesAfterwards(directory) && !exists(directory, "control/stop-now"), true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000110/chunk.json",
			"\"verified\":3,\"outcome\":\"adopted\""
		),
		1
	);
	buffer_free(&stale);
	buffer_free(&kept);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A stop asked while the save check waits, behind another runner's job or for a busy game, ends
// the wait within a sample: batch run stops with 4 before any job of its own (a stop, with a chunk
// under way too), rather than once the other job ends or queue_wait_minutes pass.
TEST(stops_while_the_save_check_waits) {
	static const struct {
		ActionKind kind;
		bool underWay; // a stop-now stopped the run before mid-chunk
		const char *reason;
	} cases[] = {
		{ACT_STOP, false, "\"reason\":\"stopped as asked\""},
		{ACT_STOP_NOW, false, "\"reason\":\"stopped as asked (stop-now)\""},
		{ACT_SIGNAL, false, "\"reason\":\"stopped as asked\""},
		{ACT_STOP, true, "\"reason\":\"stopped as asked\""},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		if (cases[i].underWay) {
			addFault(FAULT_STOP_NOW, 0, 113, 1, "baseline");
			ASSERT_INT_EQ(runFake(directory, NULL), 4);
		} else {
			launchGame(false);
		}
		unsigned runJobs = fake.game.runJobs;
		fake.game.saveWaitS = 3600;
		addAction(directory, cases[i].kind, (Action){.uptime = fake.game.uptime + 30 * 1000});
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		const Action *action = &fake.game.actions[0];
		ASSERT_BOOL_EQ(action->at && fake.game.uptime - action->at <= 1000, true);
		ASSERT_BOOL_EQ(fake.game.saveWaitS > 0 && fake.game.runJobs == runJobs, true);
		ASSERT_BOOL_EQ(fake.game.launches == 1 && endedAs(directory, "stopped", 4), true);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", cases[i].reason), 1);
		fake.game.saveWaitS = 0;
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// A stop asked while no chunk is under way, as the game is checked or a session starts (its
// warm-ups, lookups and canaries, or a canary between chunks), stops batch run before the next
// chunk begins, rather than once it is committed.
TEST(stops_before_a_chunk_begins_when_asked_as_a_session_starts) {
	static const struct {
		const char *settings;
		unsigned job;     // the run job control/stop appears in; 0 at the first status query
		unsigned runJobs; // run jobs played in all
		bool committed;   // baseline's chunk 100 was committed
	} cases[] = {
		{"", 1, 4, false},                // the first warm-up
		{"", 3, 4, false},                // the first canary
		{"", 0, 4, false},                // the new session's status query
		{"canary_every=1\n", 6, 7, true}, // the canary after the first chunk
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, cases[i].settings), true);
		if (cases[i].job) {
			addFault(FAULT_STOP, cases[i].job, UINT64_MAX, 1, NULL);
		} else {
			addAction(directory, ACT_STOP, (Action){.query = 1});
		}
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", "\"reason\":\"stopped as asked\""), 1);
		ASSERT_INT_EQ((int)fake.game.runJobs, (int)cases[i].runJobs);
		ASSERT_BOOL_EQ(
			exists(directory, "chunks/arm=baseline/chunk=0000000100/chunk.json") == cases[i].committed,
			true
		);
		ASSERT_BOOL_EQ(
			!exists(directory, "attempts/baseline/0000000100") &&
				!exists(directory, "attempts/press/0000000100"),
			true
		);
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// Wherever a stop falls while batch run watches a hung job (kill_game=0), in a status query the
// job holds for BATCH_STATUS_WAIT_S or in the wait of BATCH_TICK_S between queries, it ends the
// wait within BATCH_TICK_S, on a clock that moves as the real one does.
TEST(stops_a_watched_hung_job_within_a_tick_wherever_asked) {
	static const ActionKind kinds[] = {ACT_STOP, ACT_STOP_NOW, ACT_SIGNAL};
	for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
		for (unsigned second = 0; second < BATCH_STATUS_WAIT_S + BATCH_TICK_S; ++second) {
			wchar_t folder[TEST_PATH_LENGTH];
			wchar_t directory[TEST_PATH_LENGTH];
			ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
			fake.game.fineClock = true;
			fake.game.hangQueries = UINT32_MAX;
			addFault(FAULT_HANG, 0, 113, 1, "baseline");
			// The hang is noted after hang_minutes; the stop comes in the minute after.
			uint64_t afterMs = (11 * 60 + second) * 1000 + 500;
			addAction(directory, kinds[i], (Action){.afterMs = afterMs});
			ASSERT_INT_EQ(runFake(directory, NULL), 4);
			const Action *action = &fake.game.actions[0];
			ASSERT_BOOL_EQ(action->at && fake.game.uptime - action->at <= BATCH_TICK_S * 1000, true);
			ASSERT_BOOL_EQ(jobLeftRunning(directory), true);
			ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
		}
	}
}

// A stop-now asked while the barrier waits for a job a stopped run left, which ignores it, ends the
// wait once the job has made no progress for BATCH_STOP_NOW_GRACE_S since it was seen, which is
// within BATCH_TICK_S of its being asked, wherever that falls.
TEST(stops_now_on_time_while_waiting_for_a_job_a_stopped_run_left) {
	for (unsigned second = 0; second < BATCH_STATUS_WAIT_S + BATCH_TICK_S; ++second) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		addFault(FAULT_DIE, 0, 113, 1, "baseline");
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		fake.game.fineClock = true;
		fake.game.background.hung = true;
		fake.game.background.queries = UINT32_MAX;
		uint64_t at = fake.game.uptime + (60 + second) * 1000 + 500;
		addAction(directory, ACT_STOP_NOW, (Action){.uptime = at});
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		const Action *action = &fake.game.actions[0];
		uint64_t after = action->at ? fake.game.uptime - action->at : UINT64_MAX;
		ASSERT_BOOL_EQ(
			after >= BATCH_STOP_NOW_GRACE_S * 1000 &&
				after <= (BATCH_STOP_NOW_GRACE_S + BATCH_TICK_S) * 1000,
			true
		);
		ASSERT_BOOL_EQ(jobLeftRunning(directory), true);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", " hang pid="), 0);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// A stop-now asked while the barrier waits for a job a stopped run left is kept, unlike the one
// that run left: once the job has ended (adopted), it stops this run. With kill_game=1 a job that
// leaves it unanswered has the game quit, its part kept as crashed, and the run stops without
// starting the game again.
TEST(acts_on_a_stop_now_asked_while_waiting_for_a_job_a_stopped_run_left) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	const char *part = "attempts/baseline/0000000110/a1/part.json";
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.game.hangQueries = UINT32_MAX;
	addFault(FAULT_HANG, 0, 113, 1, "baseline");
	addAction(directory, ACT_STOP_NOW, (Action){.afterMs = 60 * 1000});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(jobLeftRunning(directory) && exists(directory, "control/stop-now"), true);
	// The job, which ignores its stop-now, ends behind the third status query of the next run's
	// barrier; a new stop-now comes at the second.
	fake.game.actionCount = 0;
	fake.game.background.queries = 3;
	unsigned runJobs = fake.game.runJobs;
	addAction(directory, ACT_STOP_NOW, (Action){.query = fake.game.statusQueries + 2});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(!fake.game.background.active && fake.game.runJobs == runJobs, true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "batch.json", "\"reason\":\"stopped as asked (stop-now)\""), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, part, "\"verified\":3,\"outcome\":\"adopted\""), 1);
	ASSERT_BOOL_EQ(exists(directory, "control/stop-now"), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 1, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "kill_game=1\n"), true);
	addFault(FAULT_DIE, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	fake.game.background.hung = true;
	fake.game.background.queries = UINT32_MAX;
	addAction(directory, ACT_STOP_NOW, (Action){.query = fake.game.statusQueries + 2});
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(fake.game.kills == 1 && !fake.game.running && fake.game.launches == 1, true);
	ASSERT_INT_EQ(
		(int)countIn(directory, "batch.json", "\"reason\":\"stopped as asked (stop-now)\""), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, part, "\"verified\":3,\"outcome\":\"crashed\""), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_killed "), 1);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A stop asked while batch run pauses, on battery power, for a game busy at the save check or
// after the game failed to start, ends the pause within BATCH_TICK_S: a stop and a first signal as
// well as a stop-now.
TEST(stops_while_paused) {
	static const ActionKind kinds[] = {ACT_STOP, ACT_SIGNAL, ACT_STOP_NOW};
	enum { ON_BATTERY, BUSY, NOT_STARTED, PAUSES };
	for (int pause = 0; pause < PAUSES; ++pause) {
		for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; ++i) {
			wchar_t folder[TEST_PATH_LENGTH];
			wchar_t directory[TEST_PATH_LENGTH];
			ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
			if (pause == ON_BATTERY) {
				for (size_t k = 0; k < 16; ++k) {
					fake.game.power[fake.game.powerCount++] = BATCH_POWER_BATTERY;
				}
			} else if (pause == BUSY) {
				launchGame(false);
				addAnswer(3, SESSION_TIMED_OUT);
			} else {
				addAnswer(1, SESSION_EXITED);
			}
			addAction(directory, kinds[i], (Action){.uptime = fake.game.uptime + 30 * 1000});
			ASSERT_INT_EQ(runFake(directory, NULL), 4);
			const Action *action = &fake.game.actions[0];
			ASSERT_BOOL_EQ(action->at && fake.game.uptime - action->at <= BATCH_TICK_S * 1000, true);
			ASSERT_BOOL_EQ(fake.game.runJobs == 0 && fake.game.launches == (pause == BUSY), true);
			ASSERT_BOOL_EQ(endedAs(directory, "stopped", 4), true);
			ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
		}
	}
}

#ifndef _WIN32
static int runPlainly(const wchar_t *directory) {
	return runFake(directory, NULL);
}

// A job left running in FM24 by a stop says what becomes of it: the next run's barrier waits for
// an attempt and keeps its rows, while a warm-up or canary keeps none, and the next run waits
// behind it as behind another runner's job (here, until queue_wait_minutes stop it), so quitting
// FM24 is what ends it.
TEST(says_what_becomes_of_a_job_left_running) {
	static const struct {
		unsigned job; // the run job that hangs at its first index, or 0 for baseline's at 113
		const char *file;
		const char *next; // why the next run stops
	} cases[] = {
		{1, "sessions/s0001/warmup-baseline.csv", "a job held the harness for queue_wait_minutes"},
		{3, "sessions/s0001/canary-baseline.csv", "a job held the harness for queue_wait_minutes"},
		{0, "attempts/baseline/0000000110/a1/", "may still run in FM24"},
	};
	static char printed[16384];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t folder[TEST_PATH_LENGTH];
		wchar_t directory[TEST_PATH_LENGTH];
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, "queue_wait_minutes=3\n"), true);
		bool attempt = !cases[i].job;
		fake.game.hangQueries = UINT32_MAX;
		addFault(FAULT_HANG, cases[i].job, attempt ? 113 : UINT64_MAX, 1, attempt ? "baseline" : NULL);
		addAction(directory, ACT_STOP, (Action){.afterMs = 11 * 60 * 1000});
		ASSERT_INT_EQ(printedBy(runPlainly, directory, printed, sizeof printed), 4);
		char said[256];
		(void)snprintf(
			said, sizeof said, "FM24 (pid 4001) still runs the job writing %s", cases[i].file
		);
		ASSERT_BOOL_EQ(strstr(printed, said) != NULL && fake.game.background.active, true);
		ASSERT_BOOL_EQ(
			(strstr(printed, "The next batch run waits for it to end and keeps the rows it wrote") !=
				NULL) == attempt,
			true
		);
		ASSERT_BOOL_EQ(
			(strstr(
				 printed,
				 "whose rows are not kept. The next batch run waits behind it as behind "
				 "another runner's job; quit FM24 to end it"
			 ) != NULL) == !attempt,
			true
		);
		ASSERT_INT_EQ(runFake(directory, NULL), 1);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", cases[i].next), 1);
		killGame(); // quitting FM24 ends it
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}
#endif

// Chunk 0 is compared with the canary reference however its run was stopped: by the run that
// committed it, or the next, which commits it itself, or acts on a comparison its committing run
// stopped before acting on.
TEST(checks_chunk_0_whatever_step_is_killed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t pristine[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, pristine, "on_canary_mismatch=continue\n"), true);
	ASSERT_BOOL_EQ(joinPath(directory, folder, "run"), true);
	ASSERT_BOOL_EQ(copyTree(pristine, directory), true);
	fake.game.variedFromJob = 5; // from chunk 0's first attempt on, results differ
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	unsigned total = fake.steps;
	static const char *const arms[] = {"baseline", "press"};
	bool checked = true;
	for (unsigned step = 1; step <= total && checked; ++step) {
		ASSERT_BOOL_EQ(batch_removeFolder(directory) && copyTree(pristine, directory), true);
		resetFake(folder);
		fake.game.variedFromJob = 5;
		fake.killAt = step;
		int first = runFake(directory, NULL);
		fake.killAt = 0;
		int second = first == BATCH_RUN_KILLED ? runFake(directory, NULL) : first;
		checked = second == 0 && countIn(directory, "suspect.csv", ",reference_mismatch\n") == 2;
		for (size_t arm = 0; arm < sizeof arms / sizeof arms[0]; ++arm) {
			char name[64];
			char line[32];
			(void)snprintf(name, sizeof name, "chunks/arm=%s/chunk=0000000100/chunk.json", arms[arm]);
			(void)snprintf(line, sizeof line, "%s,0000000100,", arms[arm]);
			checked = checked && countIn(directory, name, "\"canary_reference_match\":false") == 1 &&
								countIn(directory, "suspect.csv", line) == 1;
		}
		if (!checked) {
			fprintf(stderr, "killed at step %u: %d then %d\n", step, first, second);
		}
	}
	ASSERT_BOOL_EQ(checked, true);
	const char *chunk = "chunks/arm=baseline/chunk=0000000100/chunk.json";
	// A job left running by a third signal, which the next run adopts.
	ASSERT_BOOL_EQ(batch_removeFolder(directory) && copyTree(pristine, directory), true);
	resetFake(folder);
	fake.game.variedFromJob = 5;
	addFault(FAULT_DIE, 0, 105, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_INT_EQ((int)countIn(directory, chunk, "\"outcome\":\"adopted\""), 1);
	ASSERT_INT_EQ((int)countIn(directory, chunk, "\"canary_reference_match\":false"), 1);
	ASSERT_INT_EQ(
		(int)countIn(directory, "suspect.csv", "baseline,0000000100,s0001,reference_mismatch\n"), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Cuts a file of the batch to its first `lines` lines, then `zeros` zero bytes, as a power cut
// that lost what the disk had not written leaves it.
static bool cutFile(const wchar_t *directory, const char *name, size_t lines, size_t zeros) {
	ByteBuffer text = {0};
	bool ok = readText(directory, name, &text);
	size_t length = 0;
	for (size_t line = 0; ok && line < lines && length < text.length; ++line) {
		const char *end = memchr(text.data + length, '\n', text.length - length);
		length = end ? (size_t)(end - (const char *)text.data) + 1 : text.length;
	}
	text.length = ok ? length : 0;
	static const uint8_t zero[64] = {0};
	ok = ok && zeros <= sizeof zero && buffer_append(&text, zero, zeros);
	wchar_t path[TEST_PATH_LENGTH];
	ok = ok && joinPath(path, directory, name) && files_write(path, text.data, text.length);
	buffer_free(&text);
	return ok;
}

// The step (counting from 1) at which an uninterrupted run reaches `step` for the `nth` time.
static unsigned stepOf(BatchStep step, unsigned nth) {
	const char *log = buffer_string(&fake.log);
	char text[32];
	(void)snprintf(text, sizeof text, "step %d", (int)step);
	unsigned count = 0;
	for (const char *at = strstr(log, "step "); at; at = strstr(at + 1, "step ")) {
		++count;
		if ((at == log || at[-1] == '\n') && strncmp(at, text, strlen(text)) == 0 &&
				at[strlen(text)] == '\n' && --nth == 0) {
			return count;
		}
	}
	return 0;
}

// A power cut that loses the end of a part's files after its part.json reached the disk (the files
// are written through first, but a disk may still lose them): the part is shortened to what its
// files hold, and its indices after that play again.
TEST(plays_again_what_a_power_cut_took_from_a_part) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	const char *part = "attempts/baseline/0000000110/a1/part.json";
	const char *results = "attempts/baseline/0000000110/a1/results.csv";
	for (size_t zeros = 0; zeros <= 40; zeros += 40) {
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		addFault(FAULT_STOP_NOW, 0, 113, 1, "baseline");
		ASSERT_INT_EQ(runFake(directory, NULL), 4);
		ASSERT_INT_EQ((int)countIn(directory, part, "\"verified\":4,"), 1);
		ASSERT_BOOL_EQ(cutFile(directory, results, 3, zeros), true);
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_INT_EQ(
			(int)countIn(
				directory,
				"events.log",
				" part_shortened arm=baseline chunk=0000000110 attempt=1 verified=2 was=4\n"
			),
			1
		);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
	// A chunk played in one attempt, cut after its part.json and before its commit.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.logging = true;
	buffer_clear(&fake.log);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	fake.logging = false;
	unsigned step = stepOf(BATCH_STEP_PART_WRITTEN, 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	ASSERT_BOOL_EQ(step > 0, true);
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.steps = 0;
	fake.killAt = step;
	ASSERT_INT_EQ(runFake(directory, NULL), BATCH_RUN_KILLED);
	fake.killAt = 0;
	const char *first = "attempts/baseline/0000000100/a1/part.json";
	ASSERT_INT_EQ((int)countIn(directory, first, "\"verified\":10,\"outcome\":\"ok\""), 1);
	ASSERT_BOOL_EQ(cutFile(directory, "attempts/baseline/0000000100/a1/results.csv", 8, 0), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " part_shortened "), 1);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"chunks/arm=baseline/chunk=0000000100/chunk.json",
			"\"canary_reference_match\":true"
		),
		1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// Zeroes a file of the batch from the start of the line holding `needle` to its end, keeping its
// length, as a power cut that lost what the disk had not written can leave it.
static bool zeroFrom(const wchar_t *directory, const char *name, const char *needle) {
	ByteBuffer text = {0};
	const char *found =
		readText(directory, name, &text) ? strstr(buffer_string(&text), needle) : NULL;
	size_t start = found ? (size_t)(found - (const char *)text.data) : 0;
	while (start && text.data[start - 1] != '\n') {
		--start;
	}
	if (found) {
		memset(text.data + start, 0, text.length - start);
	}
	wchar_t path[TEST_PATH_LENGTH];
	bool ok = found && joinPath(path, directory, name) && files_write(path, text.data, text.length);
	buffer_free(&text);
	return ok;
}

// A power cut that zeroed the end of suspect.csv, as it can a file appended to and not yet written
// through: run acts once more on the chunk 0 that differs from the reference and that suspect.csv
// no longer lists, after the zeros, and the run after that goes on (with on_canary_mismatch=stop).
TEST(acts_once_more_on_chunk_0_after_a_power_cut_zeroed_suspect_csv) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	static const struct {
		size_t lines; // kept
		size_t zeros;
	} cuts[] = {{0, 0}, {1, 45}, {0, 64}};
	const char *line = "baseline,0000000100,s0001,reference_mismatch\n";
	const char *recovered = " canary arm=baseline chunk=0 reference=mismatch recovered=1\n";
	for (size_t i = 0; i < sizeof cuts / sizeof cuts[0]; ++i) {
		ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
		fake.game.variedFromJob = 5; // from chunk 0's first attempt on, results differ
		ASSERT_INT_EQ(runFake(directory, NULL), 1);
		ASSERT_INT_EQ((int)countIn(directory, "suspect.csv", line), 1);
		fake.game.variedFromJob = 0;
		bool cut = cuts[i].lines || cuts[i].zeros;
		ASSERT_BOOL_EQ(!cut || cutFile(directory, "suspect.csv", cuts[i].lines, cuts[i].zeros), true);
		if (cut) {
			ASSERT_INT_EQ((int)countIn(directory, "suspect.csv", line), 0);
			ASSERT_INT_EQ(runFake(directory, NULL), 1);
			ASSERT_INT_EQ((int)countIn(directory, "events.log", recovered), 1);
		}
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_INT_EQ((int)countIn(directory, "suspect.csv", line), 1);
		ASSERT_INT_EQ((int)countIn(directory, "events.log", recovered), cut ? 1 : 0);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
}

// A power cut that zeroed the end of events.log, from the line of the first chunk committed on:
// the next run logs those chunks again, with recovered=1, and the lines it and later runs add
// after the zeros count, so that each chunk is logged once whatever more runs start.
TEST(logs_each_commit_once_after_a_power_cut_zeroed_events_log) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	addFault(FAULT_STOP_NOW, 0, 113, 1, "baseline");
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(
		zeroFrom(directory, "events.log", " chunk_committed arm=baseline chunk=0000000100 "), true
	);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " chunk_committed "), 0);
	for (unsigned start = 0; start < 3; ++start) {
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
	}
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " chunk_committed "), 8);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " recovered=1\n"), 2);
	ASSERT_INT_EQ(
		(int)countIn(directory, "events.log", " chunk_committed arm=press chunk=0000000100 "), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// ---------------------------------------------------------------------------------------------
// FM24's file-list error logs

// A batch run whose host cannot trim FM24's logs, as on Windows, plays on without.
TEST(trims_no_logs_where_the_host_cannot) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	BatchHost host = fakeHost();
	host.trimGameLogs = NULL;
	BatchRunOptions none = {0};
	ASSERT_INT_EQ(batchRun_run(directory, &none, &host), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.trims == 0, true);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " game_logs_"), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A failure to trim never stops the batch: each kind is logged once a run, and the batch goes on.
TEST(warns_once_a_run_of_logs_it_cannot_trim) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	fake.trimFailures[GAME_LOGS_DELETE] = "Permission denied";
	fake.trimFailures[GAME_LOGS_FOLDER] = "HOME is not set, or too long";
	ASSERT_INT_EQ(runFake(directory, &(BatchRunOptions){.maxChunks = 2}), 4);
	ASSERT_INT_EQ((int)fake.trims, 3);
	const char *events = "events.log";
	ASSERT_INT_EQ(
		(int)countIn(directory, events, " game_logs_failed kind=delete error=Permission denied\n"), 1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, events, " game_logs_failed kind=folder error=HOME is not set, or too long\n"
		),
		1
	);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_failed "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed "), 0);
	// The next run says so once more, and finishes the batch.
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)fake.trims, 10);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_failed kind=delete "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_failed "), 4);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

#ifndef _WIN32
// events.log's events by name, one a line, after a line break: "\nrun_started\n...".
static bool eventNames(const wchar_t *directory, ByteBuffer *out) {
	ByteBuffer text = {0};
	bool ok = readText(directory, "events.log", &text) && buffer_append(out, "\n", 1);
	const char *data = buffer_string(&text);
	for (const char *line = data; ok && *line;) {
		size_t length = strcspn(line, "\n");
		const char *name = memchr(line, ' ', length);
		if (name) {
			++name;
			size_t named = strcspn(name, " \n");
			ok = buffer_append(out, name, named) && buffer_append(out, "\n", 1);
		}
		line += length + (line[length] == '\n');
	}
	buffer_free(&text);
	return ok;
}

// How often `sequence` (event names, each followed by a line break) is in events.log.
static unsigned eventsInOrder(const wchar_t *directory, const char *sequence) {
	ByteBuffer names = {0};
	unsigned found = eventNames(directory, &names) ? occurrences(buffer_string(&names), sequence) : 0;
	buffer_free(&names);
	return found;
}

// A file of `size` bytes in `folder`, last modified `age` seconds ago.
static bool writeAged(const char *folder, const char *name, size_t size, time_t age) {
	char path[3 * TEST_PATH_LENGTH];
	char bytes[1024];
	memset(bytes, 'x', sizeof bytes);
	(void)snprintf(path, sizeof path, "%s/%s", folder, name);
	FILE *file = fopen(path, "wb");
	bool ok = file != NULL;
	for (size_t left = size; ok && left;) {
		size_t part = left < sizeof bytes ? left : sizeof bytes;
		ok = fwrite(bytes, 1, part, file) == part;
		left -= part;
	}
	ok = file && fclose(file) == 0 && ok;
	time_t at = time(NULL) - age;
	struct timespec times[2] = {{.tv_sec = at}, {.tv_sec = at}};
	return ok && utimensat(AT_FDCWD, path, times, 0) == 0;
}

// The size of `name` in `folder` as lstat sees it, and in `bytes` its space on the disk; -1 if it
// is not there.
static long long sizeIn(const char *folder, const char *name, uint64_t *bytes) {
	char path[3 * TEST_PATH_LENGTH];
	struct stat status;
	(void)snprintf(path, sizeof path, "%s/%s", folder, name);
	if (lstat(path, &status) != 0) {
		return -1;
	}
	if (bytes) {
		*bytes = (uint64_t)status.st_blocks * 512U;
	}
	return S_ISLNK(status.st_mode) ? -2 : (long long)status.st_size;
}

static bool noFailures(const GameLogsTrim *trim) {
	for (size_t kind = 0; kind < GAME_LOGS_PROBLEMS; ++kind) {
		if (trim->failed[kind][0]) {
			fprintf(stderr, "failure %zu: %s\n", kind, trim->failed[kind]);
			return false;
		}
	}
	return true;
}

// Of the regular files directly in the folder named as FM24's logs, the newest by time is emptied
// in place and the others deleted; nothing else is touched, a link is never followed, and what
// cannot be done is recorded while the rest is done.
TEST(trims_fm24s_file_list_error_logs) {
	ASSERT_BOOL_EQ(gameLogs_isLogName("file llist errors_2026.10.22_10.00.00.txt"), true);
	ASSERT_BOOL_EQ(gameLogs_isLogName("file llist errors_.txt"), true);
	static const char *const others[] = {
		"file llist errors_2026.10.22_10.00.00.txt.bak",
		"file llist errors_2026.10.22_10.00.00.log",
		"File llist errors_2026.10.19_10.00.00.txt", // a name of its own where case does not count
		"file llist errors.txt",
		"file llist errors_",
		"other.txt",
	};
	for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i) {
		ASSERT_BOOL_EQ(gameLogs_isLogName(others[i]), false);
	}
	char root[] = "/tmp/fm24-game-logs-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(root) != NULL, true);
	char logs[256];
	char elsewhere[256];
	char named[512];
	char inside[256];
	char target[512];
	char path[1024];
	(void)snprintf(logs, sizeof logs, "%s/Logs", root);
	(void)snprintf(elsewhere, sizeof elsewhere, "%s/elsewhere", root);
	(void)snprintf(named, sizeof named, "%s/file llist errors_folder.txt", logs);
	(void)snprintf(inside, sizeof inside, "%s/sub", logs);
	(void)snprintf(target, sizeof target, "%s/target.txt", elsewhere);
	ASSERT_BOOL_EQ(
		mkdir(logs, 0755) == 0 && mkdir(elsewhere, 0755) == 0 && mkdir(named, 0755) == 0 &&
			mkdir(inside, 0755) == 0,
		true
	);
	// The newest by modification time, not by name, is emptied; the others go.
	static const struct {
		const char *name;
		size_t size;
		time_t age;
	} found[] = {
		{"file llist errors_2026.10.22_10.00.00.txt", 7000, 3600}, // the newest
		{"file llist errors_2026.10.20_10.00.00.txt", 3000, 3 * 86400},
		{"file llist errors_2026.10.21_10.00.00.txt", 5000, 2 * 86400},
		{"file llist errors_2026.10.29_10.00.00.txt", 2000, 86400},
	};
	uint64_t taken = 0;
	for (size_t i = 0; i < sizeof found / sizeof found[0]; ++i) {
		uint64_t bytes = 0;
		ASSERT_BOOL_EQ(writeAged(logs, found[i].name, found[i].size, found[i].age), true);
		ASSERT_INT_EQ((int)sizeIn(logs, found[i].name, &bytes), (int)found[i].size);
		taken += bytes;
	}
	// Left alone: other names, a folder named as a log and a log in it, a log in a subfolder, and
	// links named as logs, newer than any log, one to a file outside the folder and one to nothing.
	for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i) {
		ASSERT_BOOL_EQ(writeAged(logs, others[i], 100, 5 * 86400), true);
	}
	static const char older[] = "file llist errors_2026.10.01_10.00.00.txt";
	ASSERT_BOOL_EQ(writeAged(named, older, 100, 6 * 86400), true);
	ASSERT_BOOL_EQ(writeAged(inside, older, 100, 6 * 86400), true);
	ASSERT_BOOL_EQ(writeAged(elsewhere, "target.txt", 9000, 7 * 86400), true);
	static const char *const links[] = {
		"file llist errors_2026.10.30_10.00.00.txt", "file llist errors_2026.10.31_10.00.00.txt"};
	(void)snprintf(path, sizeof path, "%s/%s", logs, links[0]);
	ASSERT_BOOL_EQ(symlink(target, path) == 0, true);
	(void)snprintf(path, sizeof path, "%s/%s", logs, links[1]);
	ASSERT_BOOL_EQ(symlink("/nonexistent/fm24-game-log.txt", path) == 0, true);
	// FM24 holds the newest open, appending or writing at its own offset.
	(void)snprintf(path, sizeof path, "%s/%s", logs, found[0].name);
	int appending = open(path, O_WRONLY | O_APPEND);
	int writing = open(path, O_WRONLY);
	ASSERT_BOOL_EQ(appending >= 0 && writing >= 0 && lseek(writing, 0, SEEK_END) == 7000, true);
	GameLogsTrim trim = {0};
	gameLogs_trim(logs, &trim);
	ASSERT_BOOL_EQ(noFailures(&trim) && strcmp(trim.folder, logs) == 0, true);
	ASSERT_BOOL_EQ(trim.emptied == 1 && trim.deleted == 3 && trim.freedBytes == taken, true);
	ASSERT_INT_EQ((int)sizeIn(logs, found[0].name, NULL), 0);
	for (size_t i = 1; i < sizeof found / sizeof found[0]; ++i) {
		ASSERT_INT_EQ((int)sizeIn(logs, found[i].name, NULL), -1);
	}
	for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i) {
		ASSERT_INT_EQ((int)sizeIn(logs, others[i], NULL), 100);
	}
	ASSERT_INT_EQ((int)sizeIn(named, older, NULL), 100);
	ASSERT_INT_EQ((int)sizeIn(inside, older, NULL), 100);
	ASSERT_INT_EQ((int)sizeIn(logs, links[0], NULL), -2);
	ASSERT_INT_EQ((int)sizeIn(logs, links[1], NULL), -2);
	ASSERT_INT_EQ((int)sizeIn(elsewhere, "target.txt", NULL), 9000);
	// An append lands at the start of the emptied log; a write at FM24's offset leaves a hole
	// before it.
	ASSERT_BOOL_EQ(write(appending, "ab", 2) == 2, true);
	ASSERT_INT_EQ((int)sizeIn(logs, found[0].name, NULL), 2);
	ASSERT_BOOL_EQ(write(writing, "cd", 2) == 2, true);
	ASSERT_INT_EQ((int)sizeIn(logs, found[0].name, NULL), 7002);
	(void)close(appending);
	(void)close(writing);
	// Then it is the one log, so it is only emptied; once empty, there is nothing to do.
	trim = (GameLogsTrim){0};
	gameLogs_trim(logs, &trim);
	ASSERT_BOOL_EQ(noFailures(&trim) && trim.emptied == 1 && trim.deleted == 0, true);
	trim = (GameLogsTrim){0};
	gameLogs_trim(logs, &trim);
	ASSERT_BOOL_EQ(noFailures(&trim) && !trim.emptied && !trim.deleted && !trim.freedBytes, true);
	// A missing folder is nothing to trim; a file in its place cannot be read.
	trim = (GameLogsTrim){0};
	(void)snprintf(path, sizeof path, "%s/missing/Logs", root);
	gameLogs_trim(path, &trim);
	ASSERT_BOOL_EQ(noFailures(&trim) && !trim.emptied && !trim.deleted, true);
	trim = (GameLogsTrim){0};
	gameLogs_trim(target, &trim);
	ASSERT_BOOL_EQ(trim.failed[GAME_LOGS_READ][0] != 0 && !trim.emptied && !trim.deleted, true);
	ASSERT_INT_EQ((int)sizeIn(elsewhere, "target.txt", NULL), 9000);
	// In a folder whose entries cannot change, the older log stays and the newest is still
	// emptied; a newest log that cannot be written stays as it is, and the older one goes.
	if (geteuid() != 0) {
		static const char old[] = "file llist errors_2026.10.23_10.00.00.txt";
		ASSERT_BOOL_EQ(writeAged(logs, old, 4000, 2 * 86400), true);
		ASSERT_BOOL_EQ(writeAged(logs, found[0].name, 100, 60), true);
		ASSERT_BOOL_EQ(chmod(logs, 0555) == 0, true);
		trim = (GameLogsTrim){0};
		gameLogs_trim(logs, &trim);
		ASSERT_BOOL_EQ(chmod(logs, 0755) == 0, true);
		ASSERT_BOOL_EQ(trim.emptied == 1 && trim.deleted == 0, true);
		ASSERT_BOOL_EQ(strstr(trim.failed[GAME_LOGS_DELETE], "ermission") != NULL, true);
		ASSERT_BOOL_EQ(!trim.failed[GAME_LOGS_EMPTY][0] && !trim.failed[GAME_LOGS_READ][0], true);
		ASSERT_INT_EQ((int)sizeIn(logs, old, NULL), 4000);
		ASSERT_INT_EQ((int)sizeIn(logs, found[0].name, NULL), 0);
		ASSERT_BOOL_EQ(writeAged(logs, found[0].name, 100, 60), true);
		(void)snprintf(path, sizeof path, "%s/%s", logs, found[0].name);
		ASSERT_BOOL_EQ(chmod(path, 0444) == 0, true);
		trim = (GameLogsTrim){0};
		gameLogs_trim(logs, &trim);
		ASSERT_BOOL_EQ(chmod(path, 0644) == 0, true);
		ASSERT_BOOL_EQ(trim.emptied == 0 && trim.deleted == 1, true);
		ASSERT_BOOL_EQ(strstr(trim.failed[GAME_LOGS_EMPTY], "ermission") != NULL, true);
		ASSERT_BOOL_EQ(!trim.failed[GAME_LOGS_DELETE][0], true);
		ASSERT_INT_EQ((int)sizeIn(logs, old, NULL), -1);
		ASSERT_INT_EQ((int)sizeIn(logs, found[0].name, NULL), 100);
	}
	(void)snprintf(path, sizeof path, "/bin/rm -rf '%s'", root);
	ASSERT_INT_EQ(system(path), 0);
}

// The fake host's Logs folder, at <folder>/<name>, made if `make`, with an earlier session's log
// and an older one, and a file of another name; its path in `logs`.
static bool startGameLogs(const wchar_t *folder, const char *name, bool make, char *logs) {
	if (!joinPath(fake.logs, folder, name) || !platform_toUtf8(fake.logs, logs, 1024)) {
		return false;
	}
	return !make || (platform_makeDirectory(fake.logs) &&
										writeAged(logs, "file llist errors_2026.10.24_09.00.00.txt", 3000, 2 * 86400) &&
										writeAged(logs, "file llist errors_2026.10.25_09.00.00.txt", 5000, 86400) &&
										writeAged(logs, "notes.txt", 10, 3 * 86400));
}

// Before a batch run's first chunk, and after each chunk it commits, FM24's newest file-list error
// log is emptied and the older ones deleted; trim_game_logs=0 leaves them, and a missing folder is
// quiet.
TEST(trims_fm24s_logs_as_a_batch_starts_and_between_chunks) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	char logs[1024];
	static char printed[16384];
	static const char session[] = "file llist errors_2026.10.26_10.01.00.txt";
	static const char *const earlier[] = {
		"file llist errors_2026.10.24_09.00.00.txt", "file llist errors_2026.10.25_09.00.00.txt"};
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_BOOL_EQ(startGameLogs(folder, "Logs", true, logs), true);
	ASSERT_INT_EQ(printedBy(runPlainly, directory, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && strstr(printed, "warning") == NULL, true);
	// Once before the first chunk, then after each of the eight.
	ASSERT_INT_EQ((int)fake.trims, 9);
	const char *events = "events.log";
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed "), 9);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_failed "), 0);
	ASSERT_INT_EQ(
		(int)eventsInOrder(directory, "\nrun_started\ngame_logs_trimmed\ngame_launched\n"), 1
	);
	ASSERT_INT_EQ((int)eventsInOrder(directory, "\nchunk_committed\ngame_logs_trimmed\n"), 8);
	// The first two delete the earlier sessions' logs; the others only empty this session's.
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed emptied=1 deleted=1 "), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed emptied=1 deleted=0 "), 7);
	// The session's log is left, empty; the earlier ones are gone, and the other file is there.
	ASSERT_INT_EQ((int)sizeIn(logs, session, NULL), 0);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[0], NULL), -1);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[1], NULL), -1);
	ASSERT_INT_EQ((int)sizeIn(logs, "notes.txt", NULL), 10);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// trim_game_logs=0: never trimmed.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, "trim_game_logs=0\n"), true);
	ASSERT_BOOL_EQ(startGameLogs(folder, "Logs", true, logs), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.trims == 0, true);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_"), 0);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[0], NULL), 3000);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[1], NULL), 5000);
	ASSERT_BOOL_EQ(sizeIn(logs, session, NULL) > 0, true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// No Logs folder: nothing said, nothing logged.
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_BOOL_EQ(startGameLogs(folder, "NoLogs", false, logs), true);
	ASSERT_INT_EQ(printedBy(runPlainly, directory, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.trims == 9, true);
	ASSERT_BOOL_EQ(strstr(printed, "warning") == NULL, true);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_"), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A run that plays no chunk leaves FM24's logs alone: one stopped before its first, and one of a
// complete batch, such as a LaunchAgent's at each login, while FM24 may have written more.
TEST(leaves_fm24s_logs_alone_in_a_run_that_plays_no_chunk) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	char logs[1024];
	static const char *const earlier[] = {
		"file llist errors_2026.10.24_09.00.00.txt", "file llist errors_2026.10.25_09.00.00.txt"};
	static const char *const later[] = {
		"file llist errors_2026.10.27_09.00.00.txt", "file llist errors_2026.10.27_11.00.00.txt"};
	const char *events = "events.log";
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_BOOL_EQ(startGameLogs(folder, "Logs", true, logs), true);
	ASSERT_INT_EQ(
		runFake(directory, &(BatchRunOptions){.hasUntil = true, .until = fakeNow(NULL)}), 4
	);
	ASSERT_BOOL_EQ(fake.trims == 0 && fake.game.launches == 0, true);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_"), 0);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[0], NULL), 3000);
	ASSERT_INT_EQ((int)sizeIn(logs, earlier[1], NULL), 5000);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.trims == 9, true);
	unsigned launches = fake.game.launches;
	ASSERT_BOOL_EQ(writeAged(logs, later[0], 4000, 7200) && writeAged(logs, later[1], 6000, 0), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(fake.trims == 9 && fake.game.launches == launches, true);
	ASSERT_INT_EQ((int)countIn(directory, events, " run_started "), 3);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed "), 9);
	ASSERT_INT_EQ((int)sizeIn(logs, later[0], NULL), 4000);
	ASSERT_INT_EQ((int)sizeIn(logs, later[1], NULL), 6000);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A folder the batch cannot delete from: the batch says so once a run, on standard error and in
// events.log, and plays on, still emptying the newest log after each chunk.
TEST(says_once_that_it_cannot_delete_old_logs_and_plays_on) {
	if (geteuid() == 0) {
		return;
	}
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	char logs[1024];
	static char printed[16384];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	ASSERT_BOOL_EQ(startGameLogs(folder, "Logs", true, logs), true);
	// The session's log is there already, as the game cannot add it to the folder.
	static const char session[] = "file llist errors_2026.10.26_10.01.00.txt";
	ASSERT_BOOL_EQ(writeAged(logs, session, 0, 0) && chmod(logs, 0555) == 0, true);
	int code = printedBy(runPlainly, directory, printed, sizeof printed);
	ASSERT_BOOL_EQ(chmod(logs, 0755) == 0, true);
	ASSERT_INT_EQ(code, 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	char said[1200];
	(void)snprintf(
		said,
		sizeof said,
		"warning: cannot delete an older FM24 file-list error log in %s (Permission denied); the "
		"batch goes on, and says this once a run\n",
		logs
	);
	ASSERT_INT_EQ((int)occurrences(printed, said), 1);
	ASSERT_INT_EQ((int)occurrences(printed, "warning:"), 1);
	const char *events = "events.log";
	ASSERT_INT_EQ(
		(int)countIn(directory, events, " game_logs_failed kind=delete error=Permission denied\n"), 1
	);
	// None before the first chunk: the session's log is empty, and the others cannot go.
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed emptied=1 deleted=0 "), 8);
	ASSERT_INT_EQ((int)countIn(directory, events, " game_logs_trimmed "), 8);
	ASSERT_INT_EQ((int)sizeIn(logs, "file llist errors_2026.10.24_09.00.00.txt", NULL), 3000);
	ASSERT_INT_EQ((int)sizeIn(logs, session, NULL), 0);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}
#endif

// `fm24ctl batch run`'s options, and on Windows that it is not there yet.
TEST(reads_batch_run_options) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(runnableBatch(folder, directory, ""), true);
	static const wchar_t *const bad[][2] = {
		{L"--threads", L"0"},
		{L"--threads", L"17"},
		{L"--max-chunks", L"x"},
		{L"--until", L"24:00"},
		{L"--until", L"6:30"},
		{L"--wait", L"1"},
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		wchar_t *arguments[] = {
			L"fm24ctl", L"batch", L"run", directory, (wchar_t *)bad[i][0], (wchar_t *)bad[i][1], NULL};
		ASSERT_INT_EQ(batchTest_command(6, arguments), 2);
	}
	wchar_t *good[] = {L"fm24ctl",
		L"batch",
		L"run",
		directory,
		L"--threads",
		L"3",
		L"--max-chunks",
		L"1",
		L"--until",
		L"23:59",
		L"--allow-harness-change",
		NULL};
#ifdef _WIN32
	ASSERT_INT_EQ(batchTest_command(11, good), 1);
#else
	ASSERT_INT_EQ(batchTest_command(11, good), 4);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=baseline/chunk=0000000100/chunk.json", "\"threads\":3,\"started_at\""
		),
		1
	);
#endif
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// ---------------------------------------------------------------------------------------------
// Season batches

static const char seasonTeams[] = "team=11\nteam=22\nteam=33\n";

// A source folder with a season's base, BASE_HOME at home to BASE_AWAY exported at the fake game's
// date, and a plan of seasons 0-3 in blocks of 2 of `teams` and `settings`; the batch goes to
// <folder>/batch.
static bool writeSeasonSources(const wchar_t *folder, const char *teams, const char *settings) {
	char plan[2048];
	(void)snprintf(
		plan,
		sizeof plan,
		"format=fm24-batch-plan/1\nname=test-season\nmode=season\nbase=fixtures/next.json\n%s"
		"seeds=season:20261027:0-3\nchunk=2\nthreads=4\n%s",
		teams,
		settings
	);
	static const uint32_t actual[2] = {BASE_HOME, BASE_AWAY};
	static const uint32_t none[2] = {0, 0};
	wchar_t fixtures[TEST_PATH_LENGTH];
	wchar_t path[TEST_PATH_LENGTH];
	return joinPath(fixtures, folder, "fixtures") && platform_makeDirectory(fixtures) &&
				 joinPath(path, fixtures, "next.json") &&
				 writeSeasonFixture(path, actual, none, "2024 day 123 15:00") &&
				 writeText(folder, "plan.txt", plan);
}

// A test folder with a created season of three teams in <folder>/batch, which the fake game's
// faults act on.
static bool createdSeason(wchar_t *folder, wchar_t *directory, const char *settings) {
	if (!makeTestFolder(folder)) {
		return false;
	}
	resetFake(folder);
	bool ok = writeSeasonSources(folder, seasonTeams, settings) && create(folder) == 0 &&
						joinPath(directory, folder, "batch");
	if (ok) {
		wcscpy(fake.game.actionBatch, directory);
	}
	return ok;
}

bool batchTest_createSeasonBatch(const char *settings, wchar_t *folder, wchar_t *directory) {
	return createdSeason(folder, directory, settings);
}

// What batch status (or with `verify`, batch verify --deep) prints, and its exit code.
static int reportOf(const wchar_t *folder, const wchar_t *directory, bool verify, ByteBuffer *out) {
	wchar_t path[TEST_PATH_LENGTH];
	BatchHost host = fakeHost();
	FILE *file = joinPath(path, folder, "report.txt") ? platform_openFile(path, "w+b") : NULL;
	int code = !file    ? -1
						 : verify ? verifyBatch(directory, true, &host, file)
											: showStatus(directory, &host, file);
	buffer_clear(out);
	if (file) {
		(void)readBack(file, out);
		(void)fclose(file);
	}
	return code;
}

static bool hasLines(const ByteBuffer *text, const char *const *lines, size_t count) {
	bool all = true;
	for (size_t i = 0; i < count; ++i) {
		if (!strstr(buffer_string(text), lines[i])) {
			fprintf(stderr, "missing \"%s\" in:\n%s", lines[i], buffer_string(text));
			all = false;
		}
	}
	return all;
}

static void expectSeasonRefused(const char *base, const char *settings) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	wchar_t fixtures[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(
		makeTestFolder(folder) && joinPath(directory, folder, "batch") &&
			joinPath(fixtures, folder, "fixtures"),
		true
	);
	resetFake(folder);
	ASSERT_BOOL_EQ(writeSeasonSources(folder, seasonTeams, settings), true);
	ASSERT_BOOL_EQ(!base || writeText(fixtures, "next.json", base), true);
	int status = create(folder);
	if (status != 1) {
		fprintf(stderr, "not refused: %s %s\n", base ? base : "", settings);
	}
	ASSERT_INT_EQ(status, 1);
	ASSERT_BOOL_EQ(exists(directory, "plan.txt"), false);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// create freezes the plan and the base, not yet any fixture, and takes the save, game build and
// date from the base; status and verify say that the fixtures are still to be built.
TEST(creates_a_season_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	static const char *const made[] = {
		"plan.txt", "inputs/base.json", "inputs/base.bin", "batch.json"};
	for (size_t i = 0; i < sizeof made / sizeof made[0]; ++i) {
		ASSERT_BOOL_EQ(exists(directory, made[i]), true);
	}
	ASSERT_BOOL_EQ(
		!exists(directory, "inputs/f0001.bin") && !exists(directory, "inputs/fixtures.txt"), true
	);
	static BatchPlan plan;
	ByteBuffer text = {0};
	char error[256];
	ASSERT_BOOL_EQ(readText(directory, "plan.txt", &text), true);
	ASSERT_BOOL_EQ(
		batchPlan_parse(
			buffer_string(&text), text.length, true, &testDefaults, &plan, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(plan.mode == BATCH_MODE_SEASON && plan.teamCount == 3 && plan.armCount == 6, true);
	ASSERT_BOOL_EQ(plan.gameBuild == 2081827 && strcmp(plan.saveName, "LIV_COPY") == 0, true);
	ASSERT_INT_EQ(strcmp(plan.gameDate, "2024 day 123 15:00"), 0);
	ASSERT_BOOL_EQ(plan.base.teams[0] == 0 && plan.base.teams[1] == 0, true);
	ByteBuffer base = {0};
	char hash[SHA256_HEX_LENGTH];
	ASSERT_BOOL_EQ(readText(directory, "inputs/base.json", &base), true);
	sha256_hexOf(base.data, base.length, hash);
	ASSERT_INT_EQ(strcmp(hash, plan.base.jsonSha256), 0);
	buffer_free(&base);
	batchPlan_free(&plan);
	ASSERT_INT_EQ(
		(int)countIn(directory, "events.log", " created name=test-season arms=6 indices=24 chunks=12 "),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"batch.json",
			"\"seasons\":{\"teams\":3,\"fixtures\":6,\"first\":0,\"last\":3,\"block\":2,"
			"\"fixtures_built\":0,\"frozen\":false,\"blocks_complete\":0,\"seasons_complete\":0,"
			"\"matches_missing\":0}"
		),
		1
	);
	ASSERT_INT_EQ(reportOf(folder, directory, false, &text), 0);
	static const char *const status[] = {
		"batch test-season: season run of 3 teams, 6 fixtures, seasons 0-3 in blocks of 2\n",
		"fixtures: 0 of 6 built; batch run builds the others before the first chunk\n",
		"progress: 0 of 24 matches (0.0%) in 0 of 12 chunks\n",
		"next: chunk f0001/0000000000, not started\n",
		"seasons: 0 of 4 in complete blocks; block 0: 0 of 6 fixtures; 0 matches missing\n",
	};
	ASSERT_BOOL_EQ(hasLines(&text, status, sizeof status / sizeof status[0]), true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&text), "\narm ") == NULL, true);
	ASSERT_INT_EQ(reportOf(folder, directory, true, &text), 0);
	static const char *const verified[] = {
		"fixtures: 0 of 6 built; not yet frozen, so not checked\n"};
	ASSERT_BOOL_EQ(hasLines(&text, verified, 1), true);
	buffer_free(&text);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// A base with an edited line-up, one that does not encode, another save, and a plan's refusals.
	static const char lineup[] =
		"{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"save\":\"LIV_COPY\","
		"\"game_date\":\"2024 day 123 15:00\",\"away_lineup\":true,\"fields\":[{\"u32\":11},{\"u32\":"
		"22}]}";
	expectSeasonRefused(lineup, "");
	expectSeasonRefused("{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u7\":1}]}", "");
	expectSeasonRefused(NULL, "save=OTHER_SAVE\n");
	expectSeasonRefused(NULL, "keep_build=1\n");
	expectSeasonRefused(NULL, "canary=3\n");
}

// A season from start to end: the build of every fixture once, the check fixtures' warm-ups and
// canaries, chunks block by block, every (season, fixture) once with its own seed and teams.
TEST(runs_a_season_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	BatchHost host = fakeHost();
	ASSERT_INT_EQ(batchTest_verify(directory, &host), 0);
	const FakeGame *game = &fake.game;
	// Three home intermediates and six fixtures; the base's warm-up, two check fixtures' warm-ups
	// and canaries, and twelve chunks.
	ASSERT_INT_EQ((int)game->replaceJobs, 9);
	ASSERT_INT_EQ((int)game->runJobs, 17);
	ASSERT_BOOL_EQ(strstr(game->firstRunJob, "inputs/base.bin\n") != NULL, true);
	ASSERT_BOOL_EQ(strstr(game->firstRunJob, "\nthreads=1\n") != NULL, true);
	static const char *const files[] = {
		"inputs/fixtures.txt",
		"inputs/f0003.bin",
		"inputs/f0003.json.gz",
		"canary/f0001.csv",
		"canary/f0005.csv",
		"sessions/s0001/warmup-base.csv",
		"sessions/s0001/warmup-f0005.csv",
		"lookups/teams.txt",
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		ASSERT_BOOL_EQ(exists(directory, files[i]), true);
	}
	ASSERT_BOOL_EQ(
		!exists(directory, "inputs/build") && !exists(directory, "canary/f0002.csv") &&
			!exists(directory, "inputs/f0003.json"),
		true
	);
	const char *events = "events.log";
	ASSERT_INT_EQ((int)countIn(directory, events, " fixtures_frozen fixtures=6 jobs=9 wall_ms="), 1);
	ASSERT_INT_EQ((int)countIn(directory, events, " warmup session=s0001 arm=base seeds=1"), 1);
	ASSERT_INT_EQ((int)countIn(directory, events, " warmup session=s0001 arm=f"), 2);
	ASSERT_INT_EQ(
		(int)countIn(directory, events, " canary session=s0001 arm=f0001 result=reference"), 1
	);
	ASSERT_INT_EQ(
		(int)countIn(directory, events, " canary session=s0001 arm=f0005 result=reference"), 1
	);
	ASSERT_INT_EQ((int)countIn(directory, events, " canary session="), 2);
	ASSERT_INT_EQ((int)countIn(directory, events, " seasons_complete first=0 last=1"), 1);
	ASSERT_INT_EQ((int)countIn(directory, events, " seasons_complete first=2 last=3"), 1);
	// Block by block: chunk 0 of every fixture, then chunk 1.
	ByteBuffer index = {0};
	ASSERT_BOOL_EQ(readText(directory, "index.csv", &index), true);
	const char *f6 = strstr(buffer_string(&index), "\nf0006,0,2,");
	const char *f1 = strstr(buffer_string(&index), "\nf0001,2,2,");
	ASSERT_BOOL_EQ(f6 && f1 && f6 < f1, true);
	buffer_free(&index);
	// Fixture 3 is team 22 at home to 11: its overrides, its seeds, its rows' teams.
	const char *chunk = "chunks/arm=f0003/chunk=0000000000/chunk.json";
	ASSERT_INT_EQ(
		(int)countIn(directory, chunk, "\"home_team\":22,\"away_team\":11,\"fixtures_sha256\":\""), 1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			chunk,
			"\"season_fixture\":{\"number\":3,\"fixtures\":6,\"home_team_uid\":22,\"away_team_uid\":11},"
			"\"seeds\":{\"spec\":\"season:20261027:6:2:0-3\""
		),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=f0001/chunk=0000000000/chunk.json", "\"canary_reference_match\":true"
		),
		1
	);
	ASSERT_INT_EQ(
		(int)countIn(
			directory, "chunks/arm=f0002/chunk=0000000000/chunk.json", "\"canary_reference_match\":null"
		),
		1
	);
	ASSERT_INT_EQ((int)countIn(directory, "inputs/fixtures.txt", "\nfixture=f0003 json_sha256="), 1);
	ASSERT_INT_EQ((int)countIn(directory, "inputs/fixtures.txt", " home_team=22 away_team=11\n"), 1);
	ASSERT_INT_EQ((int)countIn(directory, "inputs/fixtures.txt", " home_team=33 away_team=-\n"), 1);
	ByteBuffer text = {0};
	ASSERT_INT_EQ(reportOf(folder, directory, false, &text), 0);
	static const char *const status[] = {
		"fixtures: 6 of 6 built, frozen in inputs/fixtures.txt\n",
		"progress: 24 of 24 matches (100.0%) in 12 of 12 chunks\n",
		// One seed of the 24 fails to rebuild (the fake game's isRebuildFailure), costing its season.
		"seasons: 4 of 4 in complete blocks (0-3); 1 match missing\n",
		"next: nothing; every chunk is committed\n",
	};
	ASSERT_BOOL_EQ(hasLines(&text, status, sizeof status / sizeof status[0]), true);
	ASSERT_INT_EQ(reportOf(folder, directory, true, &text), 0);
	static const char *const verified[] = {
		"verified 6 fixtures against inputs/fixtures.txt: no problems\n",
		"verified 12 committed chunks (24 indices), decompressed and every row checked: no problems\n",
	};
	ASSERT_BOOL_EQ(hasLines(&text, verified, 2), true);
	ASSERT_INT_EQ(
		(int)countIn(
			directory,
			"batch.json",
			"\"fixtures_built\":6,\"frozen\":true,\"blocks_complete\":2,\"seasons_complete\":4,"
			"\"matches_missing\":1}"
		),
		1
	);
	// A complete season has nothing more to build or play.
	unsigned jobs = game->runJobs + game->replaceJobs;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(game->runJobs + game->replaceJobs == jobs, true);
	buffer_free(&text);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// The runner killed after any step, of the build too, once or every few steps, then started again:
// every (season, fixture) once, with the contents of an uninterrupted run.
TEST(builds_and_plays_a_season_whatever_step_is_killed) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t pristine[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdSeason(folder, pristine, ""), true);
	ASSERT_BOOL_EQ(joinPath(directory, folder, "run"), true);
	ByteBuffer reference = {0};
	ASSERT_BOOL_EQ(copyTree(pristine, directory) && runFake(directory, NULL) == 0, true);
	ASSERT_BOOL_EQ(checkFinished(directory, &reference), true);
	unsigned total = fake.steps;
	ASSERT_BOOL_EQ(total > 100, true);
	static const unsigned every[] = {0, 11, 13, 17};
	bool same = true;
	unsigned killed = 0;
	for (size_t e = 0; e < sizeof every / sizeof every[0] && same; ++e) {
		for (unsigned step = 1; step <= total && same; step += every[e] ? 5 : 1) {
			ASSERT_BOOL_EQ(batch_removeFolder(directory) && copyTree(pristine, directory), true);
			resetFake(folder);
			fake.killAt = step;
			int code = runFake(directory, NULL);
			unsigned rounds = 0;
			for (; code == BATCH_RUN_KILLED && rounds < 100; ++rounds) {
				++killed;
				fake.killAt = every[e] ? fake.steps + every[e] : 0;
				code = runFake(directory, NULL);
			}
			ByteBuffer hashes = {0};
			same = code == 0 && checkFinished(directory, &hashes) && hashes.length == reference.length &&
						 memcmp(hashes.data, reference.data, hashes.length) == 0 &&
						 !exists(directory, "inputs/build");
			if (!same) {
				fprintf(stderr, "killed at step %u (then every %u): %d\n", step, every[e], code);
			}
			buffer_free(&hashes);
		}
	}
	ASSERT_BOOL_EQ(same, true);
	ASSERT_BOOL_EQ(killed > 100, true);
	buffer_free(&reference);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// What goes wrong while the fixtures are built: a crash, a refusal, an edit kept, the wrong
// override or date, a stop or --until between jobs, and what a stopped build left.
TEST(handles_what_goes_wrong_while_building_a_season) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	// The game crashes in the fifth replacement (fixture 2): it starts again, the new session warms
	// the base up and rebuilds the home intermediates, and the build goes on.
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	fake.game.replaceCrashAt = 5;
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory) && fake.game.launches == 2, true);
	ASSERT_INT_EQ((int)fake.game.replaceJobs, 13);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " crash pid="), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " warmup session=s0002 arm=base "), 1);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// A refusal, an edit kept, another override and another date each stop the batch with 1,
	// naming the fixture; the next run builds it.
	static const struct {
		unsigned job;
		unsigned *fault;
		const char *logged;
	} stops[] = {
		{2,
			&fake.game.replaceErrorAt,
			" build_failed fixture=home-22 home=22 away=- reply=error: no team matches 22"},
		{4,
			&fake.game.replaceEditsAt,
			" build_failed fixture=f0001 home=11 away=22 reply=it kept 1 edits"},
		{6,
			&fake.game.replaceOverrideAt,
			" build_failed fixture=f0003 home=22 away=11 reply=its team overrides are home 23 and away "
			"11"},
		{1,
			&fake.game.replaceDateAt,
			" build_failed fixture=home-11 home=11 away=- reply=it is of in-game date"},
	};
	for (size_t i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
		ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
		*stops[i].fault = stops[i].job;
		ASSERT_INT_EQ(runFake(directory, NULL), 1);
		bool logged = countIn(directory, "events.log", stops[i].logged) == 1;
		if (!logged) {
			ByteBuffer log = {0};
			(void)readText(directory, "events.log", &log);
			fprintf(stderr, "case %zu: %s", i, buffer_string(&log));
			buffer_free(&log);
		}
		ASSERT_BOOL_EQ(logged, true);
		ASSERT_BOOL_EQ(
			endedAs(directory, "stopped", 1) && !exists(directory, "inputs/fixtures.txt"), true
		);
		ASSERT_INT_EQ((int)countIn(directory, "batch.json", "failed to build; see events.log"), 1);
		ASSERT_INT_EQ(runFake(directory, NULL), 0);
		ASSERT_BOOL_EQ(finishedOnce(directory), true);
		ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	}
	// control/stop during the fifth job: the run stops once it is kept, with two fixtures built.
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	fake.game.replaceStopAt = 5;
	ASSERT_INT_EQ(runFake(directory, NULL), 4);
	ASSERT_BOOL_EQ(
		exists(directory, "inputs/f0002.json.gz") && !exists(directory, "inputs/f0003.bin"), true
	);
	ByteBuffer text = {0};
	ASSERT_INT_EQ(reportOf(folder, directory, false, &text), 0);
	static const char *const building[] = {
		"fixtures: 2 of 6 built; batch run builds the others before the first chunk\n",
		"state: stopped at "};
	ASSERT_BOOL_EQ(hasLines(&text, building, 2), true);
	// --until stops before any job; then what a stopped build left: temporary files, and an
	// encoding without its JSON, which the next run removes and builds again.
	ASSERT_INT_EQ(
		runFake(directory, &(BatchRunOptions){.hasUntil = true, .until = fakeNow(NULL)}), 4
	);
	ASSERT_INT_EQ((int)fake.game.replaceJobs, 5);
	ASSERT_BOOL_EQ(
		writeText(directory, "inputs/f0003.bin", "not it") &&
			writeText(directory, "inputs/f0004.tmp.json", "{") &&
			writeText(directory, "inputs/f0001.tmp.json.gz", "x") &&
			writeText(directory, "inputs/f0002.bin.tmp", "x") &&
			writeText(directory, "inputs/build/home-77.tmp.json", "{"),
		true
	);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_INT_EQ((int)fake.game.replaceJobs, 12);
	ASSERT_BOOL_EQ(
		!exists(directory, "inputs/f0004.tmp.json") && !exists(directory, "inputs/f0001.tmp.json.gz") &&
			!exists(directory, "inputs/f0002.bin.tmp") && !exists(directory, "inputs/build"),
		true
	);
	BatchHost host = fakeHost();
	ASSERT_INT_EQ(batchTest_verify(directory, &host), 0);
	buffer_free(&text);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// A season's fixture that plays with edits does not rebuild to itself: its rows are discarded and
// the batch stops, at an attempt or at a canary.
TEST(stops_when_a_season_fixture_does_not_rebuild_to_itself) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	// Run job 7 is fixture 2's first attempt: the base's warm-up, two check warm-ups and two
	// canaries come first, then fixture 1's.
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	fake.game.editsJob = 7;
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " edits arm=f0002 edits=2"), 1);
	ASSERT_BOOL_EQ(endedAs(directory, "stopped", 1), true);
	ASSERT_BOOL_EQ(
		exists(directory, "chunks/arm=f0001/chunk=0000000000/chunk.json") &&
			!exists(directory, "chunks/arm=f0002/chunk=0000000000/chunk.json"),
		true
	);
	ASSERT_INT_EQ(
		(int)countIn(directory, "attempts/f0002/0000000000/a1/part.json", "\"verified\":0,"), 1
	);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
	// Run job 4 is fixture 1's canary: it is not kept as the reference.
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	fake.game.editsJob = 4;
	ASSERT_INT_EQ(runFake(directory, NULL), 1);
	ASSERT_INT_EQ((int)countIn(directory, "events.log", " edits arm=f0001 edits=2"), 1);
	ASSERT_BOOL_EQ(
		!exists(directory, "canary/f0001.csv") && !exists(directory, "chunks/arm=f0001"), true
	);
	// Played again with no edits, it goes on.
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	ASSERT_BOOL_EQ(finishedOnce(directory), true);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

// inputs/fixtures.txt or a fixture's encoding changed, or chunks without fixtures.txt, are a
// changed batch (2); a fixture's JSON changed is a problem verify finds.
TEST(refuses_a_changed_season_batch) {
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t directory[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(createdSeason(folder, directory, ""), true);
	ASSERT_INT_EQ(runFake(directory, NULL), 0);
	BatchHost host = fakeHost();
	Batch *batch = malloc(sizeof *batch);
	ASSERT_BOOL_EQ(batch != NULL, true);
	if (!batch) {
		return;
	}
	ASSERT_BOOL_EQ(
		batch_open(batch, directory, &host) == BATCH_OPENED && batch->fixturesFrozen, true
	);
	batch_close(batch);
	ASSERT_BOOL_EQ(
		editFile(
			directory,
			"inputs/fixtures.txt",
			" home_team=22 away_team=11\n",
			" home_team=22 away_team=12\n"
		),
		true
	);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPEN_CHANGED, true);
	ASSERT_BOOL_EQ(
		editFile(
			directory,
			"inputs/fixtures.txt",
			" home_team=22 away_team=12\n",
			" home_team=22 away_team=11\n"
		),
		true
	);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPENED, true);
	batch_close(batch);
	wchar_t bin[TEST_PATH_LENGTH];
	ByteBuffer encoding = {0};
	ASSERT_BOOL_EQ(joinPath(bin, directory, "inputs/f0004.bin") && files_read(bin, &encoding), true);
	ASSERT_BOOL_EQ(appendFile(bin, "\x01", 1), true);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPEN_CHANGED, true);
	ASSERT_INT_EQ(runFake(directory, NULL), 2);
	ASSERT_BOOL_EQ(files_write(bin, encoding.data, encoding.length), true);
	buffer_free(&encoding);
	wchar_t path[TEST_PATH_LENGTH];
	ByteBuffer fixtures = {0};
	ASSERT_BOOL_EQ(readText(directory, "inputs/fixtures.txt", &fixtures), true);
	ASSERT_BOOL_EQ(
		joinPath(path, directory, "inputs/fixtures.txt") && platform_removeFile(path), true
	);
	ASSERT_BOOL_EQ(batch_open(batch, directory, &host) == BATCH_OPEN_CHANGED, true);
	wchar_t *arguments[] = {L"fm24ctl", L"batch", L"status", directory, NULL};
	ASSERT_INT_EQ(batchTest_command(4, arguments), 2);
	// A fixture's JSON is checked by verify, not as the batch opens.
	ASSERT_BOOL_EQ(writeText(directory, "inputs/fixtures.txt", buffer_string(&fixtures)), true);
	ASSERT_BOOL_EQ(editFile(directory, "inputs/f0002.json.gz", "FAKEGZ1\n", "FAKEGZ1\n "), true);
	ByteBuffer text = {0};
	ASSERT_INT_EQ(reportOf(folder, directory, true, &text), 1);
	static const char *const problems[] = {
		"fixture f0002: inputs/f0002.json.gz does not match inputs/fixtures.txt\n",
		"checked 6 fixtures against inputs/fixtures.txt: problems found\n",
	};
	ASSERT_BOOL_EQ(hasLines(&text, problems, 2), true);
	buffer_free(&text);
	buffer_free(&fixtures);
	free(batch);
	ASSERT_BOOL_EQ(batch_removeFolder(folder), true);
}

void registerBatchTests(void) {
	test_run("scatters_the_golden_vectors", test_scatters_the_golden_vectors);
	test_run("scatter_is_a_bijection_and_keys_differ", test_scatter_is_a_bijection_and_keys_differ);
	test_run("formats_and_hashes_seeds", test_formats_and_hashes_seeds);
	test_run("reads_seed_lists", test_reads_seed_lists);
	test_run("reads_a_plan_with_its_defaults", test_reads_a_plan_with_its_defaults);
	test_run("refuses_bad_plans", test_refuses_bad_plans);
	test_run("freezes_a_plan_that_reads_back", test_freezes_a_plan_that_reads_back);
	test_run("formats_error_limits", test_formats_error_limits);
	test_run("enumerates_chunks_in_both_orders", test_enumerates_chunks_in_both_orders);
	test_run("formats_part_jobs_and_caps_their_size", test_formats_part_jobs_and_caps_their_size);
	test_run(
		"verifies_parts_and_finds_their_consistent_prefix",
		test_verifies_parts_and_finds_their_consistent_prefix
	);
	test_run("verifies_the_harness_rows", test_verifies_the_harness_rows);
	test_run("checks_each_rows_teams_when_given", test_checks_each_rows_teams_when_given);
	test_run("merges_parts_into_a_chunk", test_merges_parts_into_a_chunk);
	test_run("follows_the_part_chain", test_follows_the_part_chain);
	test_run("creates_a_batch", test_creates_a_batch);
	test_run("refuses_batches_it_cannot_run", test_refuses_batches_it_cannot_run);
	test_run(
		"creates_batches_that_bend_the_rules_allowed", test_creates_batches_that_bend_the_rules_allowed
	);
	test_run("freezes_names_and_folders_with_a_hash", test_freezes_names_and_folders_with_a_hash);
	test_run("creates_a_batch_only_in_an_empty_folder", test_creates_a_batch_only_in_an_empty_folder);
#ifndef _WIN32
	test_run(
		"warns_when_the_disk_has_too_little_space", test_warns_when_the_disk_has_too_little_space
	);
	test_run("refuses_folders_macos_guards", test_refuses_folders_macos_guards);
#endif
	test_run(
		"refuses_an_edited_line_up_it_would_not_play", test_refuses_an_edited_line_up_it_would_not_play
	);
	test_run("runs_a_batch_to_the_end", test_runs_a_batch_to_the_end);
	test_run(
		"covers_every_index_once_whatever_step_is_killed",
		test_covers_every_index_once_whatever_step_is_killed
	);
	test_run(
		"covers_a_listed_batch_once_whatever_step_is_killed",
		test_covers_a_listed_batch_once_whatever_step_is_killed
	);
	test_run("writes_through_in_order", test_writes_through_in_order);
	test_run(
		"a_failed_commit_leaves_the_chunk_to_commit_again",
		test_a_failed_commit_leaves_the_chunk_to_commit_again
	);
	test_run("shows_a_batch_status", test_shows_a_batch_status);
	test_run("counts_a_single_chunk_in_the_singular", test_counts_a_single_chunk_in_the_singular);
	test_run("verifies_committed_chunks", test_verifies_committed_chunks);
	test_run("stops_a_batch", test_stops_a_batch);
	test_run("refuses_a_changed_batch", test_refuses_a_changed_batch);
	test_run("tidies_what_a_stopped_process_left", test_tidies_what_a_stopped_process_left);
	test_run("reads_json_back_by_path", test_reads_json_back_by_path);
	test_run("policy_relaunches_after_a_crash", test_policy_relaunches_after_a_crash);
	test_run(
		"policy_isolates_a_seed_that_crashes_the_game",
		test_policy_isolates_a_seed_that_crashes_the_game
	);
	test_run("policy_handles_a_hang", test_policy_handles_a_hang);
	test_run(
		"policy_stops_as_asked_instead_of_starting_the_game",
		test_policy_stops_as_asked_instead_of_starting_the_game
	);
	test_run(
		"policy_handles_a_game_without_its_harness", test_policy_handles_a_game_without_its_harness
	);
	test_run(
		"policy_resumes_and_waits_for_another_job", test_policy_resumes_and_waits_for_another_job
	);
	test_run("policy_waits_for_a_busy_game", test_policy_waits_for_a_busy_game);
	test_run(
		"policy_stops_for_another_save_or_the_start_screen",
		test_policy_stops_for_another_save_or_the_start_screen
	);
	test_run(
		"policy_stops_when_the_game_is_no_longer_the_same",
		test_policy_stops_when_the_game_is_no_longer_the_same
	);
	test_run("policy_handles_the_other_slice", test_policy_handles_the_other_slice);
	test_run(
		"policy_retries_a_game_that_did_not_start", test_policy_retries_a_game_that_did_not_start
	);
	test_run(
		"policy_limits_seeds_that_fail_to_rebuild", test_policy_limits_seeds_that_fail_to_rebuild
	);
	test_run("policy_retries_a_failed_seed_once", test_policy_retries_a_failed_seed_once);
	test_run(
		"policy_stops_for_the_fixture_the_disk_and_bad_jobs",
		test_policy_stops_for_the_fixture_the_disk_and_bad_jobs
	);
	test_run(
		"policy_retries_files_that_hold_less_than_reported",
		test_policy_retries_files_that_hold_less_than_reported
	);
	test_run(
		"policy_pauses_notes_sleep_and_refuses_a_second_runner",
		test_policy_pauses_notes_sleep_and_refuses_a_second_runner
	);
	test_run("policy_reads_the_harness_replies", test_policy_reads_the_harness_replies);
	test_run("runs_a_batch_with_a_session", test_runs_a_batch_with_a_session);
	test_run("relaunches_after_a_crash", test_relaunches_after_a_crash);
	test_run("waits_for_a_busy_game", test_waits_for_a_busy_game);
	test_run(
		"stops_for_another_save_or_the_start_screen", test_stops_for_another_save_or_the_start_screen
	);
	test_run("stops_without_the_game_with_launch_0", test_stops_without_the_game_with_launch_0);
	test_run("pauses_on_battery", test_pauses_on_battery);
	test_run("stops_when_the_disk_is_nearly_full", test_stops_when_the_disk_is_nearly_full);
	test_run("handles_a_hung_job", test_handles_a_hung_job);
	test_run("notes_a_sleep_that_is_not_a_hang", test_notes_a_sleep_that_is_not_a_hang);
	test_run("checks_that_results_repeat", test_checks_that_results_repeat);
	test_run("pins_the_harness_identity", test_pins_the_harness_identity);
	test_run("excludes_a_seed_that_crashes_the_game", test_excludes_a_seed_that_crashes_the_game);
	test_run("stops_as_asked", test_stops_as_asked);
	test_run(
		"runs_every_index_once_whatever_step_is_killed",
		test_runs_every_index_once_whatever_step_is_killed
	);
	test_run(
		"stops_at_limits_and_refuses_a_second_runner", test_stops_at_limits_and_refuses_a_second_runner
	);
	test_run(
		"checks_the_game_before_and_between_chunks", test_checks_the_game_before_and_between_chunks
	);
	test_run("relaunches_a_game_that_is_still_exiting", test_relaunches_a_game_that_is_still_exiting);
	test_run(
		"counts_quitting_a_game_whose_hung_job_is_watched_as_a_crash",
		test_counts_quitting_a_game_whose_hung_job_is_watched_as_a_crash
	);
	test_run(
		"excludes_a_seed_only_after_two_crashes_with_one_worker",
		test_excludes_a_seed_only_after_two_crashes_with_one_worker
	);
	test_run("isolates_seeds_within_the_crash_limit", test_isolates_seeds_within_the_crash_limit);
	test_run("stops_after_too_many_crashes_in_an_hour", test_stops_after_too_many_crashes_in_an_hour);
	test_run(
		"handles_a_hang_in_a_job_a_stopped_run_left", test_handles_a_hang_in_a_job_a_stopped_run_left
	);
	test_run("stops_while_a_hung_job_is_watched", test_stops_while_a_hung_job_is_watched);
	test_run(
		"stops_a_job_that_hangs_once_it_is_noted_or_left_unanswered",
		test_stops_a_job_that_hangs_once_it_is_noted_or_left_unanswered
	);
	test_run(
		"quits_a_hung_game_but_does_not_start_it_again_once_asked_to_stop",
		test_quits_a_hung_game_but_does_not_start_it_again_once_asked_to_stop
	);
	test_run(
		"never_starts_the_game_again_once_asked_to_stop",
		test_never_starts_the_game_again_once_asked_to_stop
	);
	test_run(
		"abandons_a_job_not_yet_taken_when_asked_to_stop",
		test_abandons_a_job_not_yet_taken_when_asked_to_stop
	);
	test_run(
		"stops_as_asked_while_waiting_for_a_job_a_stopped_run_left",
		test_stops_as_asked_while_waiting_for_a_job_a_stopped_run_left
	);
	test_run(
		"keeps_a_stale_stop_now_for_the_job_it_was_meant_for",
		test_keeps_a_stale_stop_now_for_the_job_it_was_meant_for
	);
	test_run("stops_while_the_save_check_waits", test_stops_while_the_save_check_waits);
	test_run(
		"stops_before_a_chunk_begins_when_asked_as_a_session_starts",
		test_stops_before_a_chunk_begins_when_asked_as_a_session_starts
	);
	test_run(
		"stops_a_watched_hung_job_within_a_tick_wherever_asked",
		test_stops_a_watched_hung_job_within_a_tick_wherever_asked
	);
	test_run(
		"stops_now_on_time_while_waiting_for_a_job_a_stopped_run_left",
		test_stops_now_on_time_while_waiting_for_a_job_a_stopped_run_left
	);
	test_run(
		"acts_on_a_stop_now_asked_while_waiting_for_a_job_a_stopped_run_left",
		test_acts_on_a_stop_now_asked_while_waiting_for_a_job_a_stopped_run_left
	);
	test_run("stops_while_paused", test_stops_while_paused);
#ifndef _WIN32
	test_run("says_what_becomes_of_a_job_left_running", test_says_what_becomes_of_a_job_left_running);
#endif
	test_run("checks_chunk_0_whatever_step_is_killed", test_checks_chunk_0_whatever_step_is_killed);
	test_run(
		"plays_again_what_a_power_cut_took_from_a_part",
		test_plays_again_what_a_power_cut_took_from_a_part
	);
	test_run(
		"acts_once_more_on_chunk_0_after_a_power_cut_zeroed_suspect_csv",
		test_acts_once_more_on_chunk_0_after_a_power_cut_zeroed_suspect_csv
	);
	test_run(
		"logs_each_commit_once_after_a_power_cut_zeroed_events_log",
		test_logs_each_commit_once_after_a_power_cut_zeroed_events_log
	);
	test_run("trims_no_logs_where_the_host_cannot", test_trims_no_logs_where_the_host_cannot);
	test_run(
		"warns_once_a_run_of_logs_it_cannot_trim", test_warns_once_a_run_of_logs_it_cannot_trim
	);
#ifndef _WIN32
	test_run("trims_fm24s_file_list_error_logs", test_trims_fm24s_file_list_error_logs);
	test_run(
		"trims_fm24s_logs_as_a_batch_starts_and_between_chunks",
		test_trims_fm24s_logs_as_a_batch_starts_and_between_chunks
	);
	test_run(
		"leaves_fm24s_logs_alone_in_a_run_that_plays_no_chunk",
		test_leaves_fm24s_logs_alone_in_a_run_that_plays_no_chunk
	);
	test_run(
		"says_once_that_it_cannot_delete_old_logs_and_plays_on",
		test_says_once_that_it_cannot_delete_old_logs_and_plays_on
	);
#endif
	test_run("reads_batch_run_options", test_reads_batch_run_options);
	test_run("creates_a_season_batch", test_creates_a_season_batch);
	test_run("runs_a_season_batch", test_runs_a_season_batch);
	test_run(
		"builds_and_plays_a_season_whatever_step_is_killed",
		test_builds_and_plays_a_season_whatever_step_is_killed
	);
	test_run(
		"handles_what_goes_wrong_while_building_a_season",
		test_handles_what_goes_wrong_while_building_a_season
	);
	test_run(
		"stops_when_a_season_fixture_does_not_rebuild_to_itself",
		test_stops_when_a_season_fixture_does_not_rebuild_to_itself
	);
	test_run("refuses_a_changed_season_batch", test_refuses_a_changed_season_batch);
	registerBatchSeasonTests();
}
