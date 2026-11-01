// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The run job (src/dll/job_run.c) with fake engine calls: each seed plays dll_test.c's synthetic
// match for it, scripts make seeds slow, fail or create the stop file, and a fake clock makes each
// match's time follow from its seed. The statistics are matchStats' own, from dll_test.c.

#include "../src/dll/database.h"
#include "../src/dll/game.h"
#include "../src/dll/job.h"
#include "../src/dll/log.h"
#include "../src/dll/match.h"
#include "../src/dll/match_stats.h"
#include "../src/dll/memory.h"
#include "../src/dll/profiler.h"
#include "../src/dll/seed_build.h"
#include "build_info.h"
#include "files.h"
#include "platform.h"
#include "sha256.h"
#include "test.h"
#include "tests.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif


enum {
	FAKE_TICK_FREQUENCY = 10000, // fake ticks per second
	MAX_SCRIPTED = 16,
};

typedef enum Fault {
	FAULT_NONE,
	FAULT_REBUILD,     // seedBuilder_build fails
	FAULT_DESERIALIZE, // match_deserializeOn fails
	FAULT_STOP,        // playing it creates the stop file
} Fault;

typedef struct Script {
	uint32_t seed;
	Fault fault;
	unsigned delayMs;
} Script;

static Script scripts[MAX_SCRIPTED];
static size_t scriptCount;
static unsigned defaultDelayMs;
static wchar_t stopPath[HARNESS_PATH_LENGTH];
static atomic_uint matchesAlive; // synthetic matches not yet deleted
static _Thread_local uint64_t fakeNow;
static _Thread_local uint64_t fakeStep = 1;

static bool fakeSeedOffset(const uint8_t *bytes, size_t size, size_t *offset);
static uint8_t *fakeDeserializeOn(unsigned slot, ByteBuffer *bytes);
static void fakeHandToAssistant(const uint8_t *match);
static void fakePlay(uint8_t *match);
static MatchResult fakeResult(const uint8_t *match);
static void fakeDelete(uint8_t *match);
static DataBuffer *fakeInputStream(unsigned slot);
static uint8_t *fakeInitThread(void);
static Profiler *fakeProfilerStart(const wchar_t *path);
static void fakeProfilerMark(Profiler *profiler, const char *text);
static void fakeProfilerStop(Profiler *profiler);
static bool fakeFindTeam(const char *spec, uint8_t **team, char *error, size_t errorSize);
static bool fakeBuilderOpen(
	SeedBuilder *builder,
	const ByteBuffer *input,
	uint32_t inputSeed,
	const uint8_t *const teams[2],
	char *error,
	size_t errorSize
);
static bool fakeBuild(SeedBuilder *builder, uint32_t seed, ByteBuffer *out);
static size_t fakeEditCount(const SeedBuilder *builder);
static void fakeBuilderClose(SeedBuilder *builder);
static uint64_t fakeTicks(void);
static uint64_t fakeTickFrequency(void);
static unsigned long fakeProcessId(void);
static int64_t fakeLoadedAt(void);
static uint64_t fakeJobCount(void);
static uint32_t fakeTimestamp(void);
static const char *fakeArchitecture(void);
static bool fakeTranslated(void);
static int recordFlush(FILE *file);
static int recordClose(FILE *file);
static FILE *openRecorded(const wchar_t *path, const char *mode);
#ifdef _WIN32
static BOOL recordPostMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
#define PostMessageW recordPostMessage
#endif
void jobRunTest_run(const Job *job);

// The build the report names (instead of this build's commit).
static const char fakeCommit[] = "e74df0b3c1a2d4e5f60718293a4b5c6d7e8f9012";
static const int fakeDirty = 0;

#define jobs_run jobRunTest_run
#define match_seedOffset fakeSeedOffset
#define match_deserializeOn fakeDeserializeOn
#define match_handToAssistant fakeHandToAssistant
#define match_play fakePlay
#define match_result fakeResult
#define match_delete fakeDelete
#define match_inputStream fakeInputStream
#define game_initThread fakeInitThread
#define profiler_start fakeProfilerStart
#define profiler_mark fakeProfilerMark
#define profiler_stop fakeProfilerStop
#define database_findTeam fakeFindTeam
#define seedBuilder_open fakeBuilderOpen
#define seedBuilder_build fakeBuild
#define seedBuilder_editCount fakeEditCount
#define seedBuilder_close fakeBuilderClose
#define platform_ticks fakeTicks
#define platform_tickFrequency fakeTickFrequency
#define platform_processId fakeProcessId
#define platform_openFile openRecorded
#define job_loadedAt fakeLoadedAt
#define job_count fakeJobCount
#define game_timestamp fakeTimestamp
#define game_architecture fakeArchitecture
#define game_translated fakeTranslated
#define buildInfo_commit fakeCommit
#define buildInfo_dirty fakeDirty
#define fflush recordFlush
#define fclose recordClose
#include "../src/dll/job_run.c" // NOLINT(bugprone-suspicious-include): run jobs on fake matches.
#undef fclose
#undef platform_processId
#undef platform_openFile
#undef job_loadedAt
#undef job_count
#undef game_timestamp
#undef game_architecture
#undef game_translated
#undef buildInfo_commit
#undef buildInfo_dirty
#undef fflush
#ifdef _WIN32
#undef PostMessageW
#endif
#undef jobs_run
#undef match_seedOffset
#undef match_deserializeOn
#undef match_handToAssistant
#undef match_play
#undef match_result
#undef match_delete
#undef match_inputStream
#undef game_initThread
#undef profiler_start
#undef profiler_mark
#undef profiler_stop
#undef database_findTeam
#undef seedBuilder_open
#undef seedBuilder_build
#undef seedBuilder_editCount
#undef seedBuilder_close
#undef platform_ticks
#undef platform_tickFrequency


static const Script *scriptFor(uint32_t seed) {
	for (size_t i = 0; i < scriptCount; ++i) {
		if (scripts[i].seed == seed) {
			return &scripts[i];
		}
	}
	return NULL;
}

static Fault faultFor(uint32_t seed) {
	const Script *script = scriptFor(seed);
	return script ? script->fault : FAULT_NONE;
}

// The input: its own seed (99) at offset 0, as fakeSeedOffset says.
static bool fakeSeedOffset(const uint8_t *bytes, size_t size, size_t *offset) {
	(void)bytes;
	*offset = 0;
	return size >= sizeof(uint32_t);
}

static uint8_t *fakeDeserializeOn(unsigned slot, ByteBuffer *bytes) {
	(void)slot;
	uint32_t seed = memory_loadU32(bytes->data);
	const Script *script = scriptFor(seed);
	if (script && script->fault == FAULT_DESERIALIZE) {
		if (script->delayMs) {
			platform_sleep(script->delayMs);
		}
		return NULL;
	}
	uint8_t *match = dllTest_makeMatch(seed);
	if (match) {
		atomic_fetch_add(&matchesAlive, 1);
	}
	return match;
}

static void fakeHandToAssistant(const uint8_t *match) {
	(void)match;
}

static void fakePlay(uint8_t *match) {
	uint32_t seed = 0;
	unsigned goals[2];
	dllTest_matchScore(match, &seed, goals);
	const Script *script = scriptFor(seed);
	unsigned delay = script && script->delayMs ? script->delayMs : defaultDelayMs;
	if (delay) {
		platform_sleep(delay);
	}
	if (script && script->fault == FAULT_STOP) {
		(void)files_write(stopPath, "", 0);
	}
	fakeStep = 12345 + seed % 7 * 1111; // the match's time: the next tick comes this much later
}

static MatchResult fakeResult(const uint8_t *match) {
	uint32_t seed = 0;
	unsigned goals[2];
	dllTest_matchScore(match, &seed, goals);
	return (MatchResult){
		.seed = seed,
		.homeGoals = goals[0],
		.awayGoals = goals[1],
		.clockTicks = 21600 + seed % 97,
		.finished = seed % 7 != 3,
		.finalRngState = 0x9E3779B97F4A7C15ULL * (seed + 1),
	};
}

static void fakeDelete(uint8_t *match) {
	fakeStep = 1;
	atomic_fetch_sub(&matchesAlive, 1);
	dllTest_freeMatch(match);
}

static DataBuffer *fakeInputStream(unsigned slot) {
	static DataBuffer streams[HARNESS_MAX_THREADS];
	return &streams[slot];
}

static uint8_t *fakeInitThread(void) {
	return NULL;
}

static Profiler *fakeProfilerStart(const wchar_t *path) {
	(void)path;
	return NULL;
}

static void fakeProfilerMark(Profiler *profiler, const char *text) {
	(void)profiler;
	(void)text;
}

static void fakeProfilerStop(Profiler *profiler) {
	(void)profiler;
}

static bool fakeFindTeam(const char *spec, uint8_t **team, char *error, size_t errorSize) {
	(void)spec;
	*team = NULL;
	(void)snprintf(error, errorSize, "no teams in this test");
	return false;
}

static bool fakeBuilderOpen(
	SeedBuilder *builder,
	const ByteBuffer *input,
	uint32_t inputSeed,
	const uint8_t *const teams[2],
	char *error,
	size_t errorSize
) {
	(void)input;
	(void)inputSeed;
	(void)teams;
	(void)error;
	(void)errorSize;
	*builder = (SeedBuilder){.editCount = 3};
	return true;
}

static bool fakeBuild(SeedBuilder *builder, uint32_t seed, ByteBuffer *out) {
	(void)builder;
	return faultFor(seed) != FAULT_REBUILD && buffer_append(out, &seed, sizeof seed);
}

static size_t fakeEditCount(const SeedBuilder *builder) {
	return builder->editCount;
}

static void fakeBuilderClose(SeedBuilder *builder) {
	(void)builder;
}

static uint64_t fakeTicks(void) {
	fakeNow += fakeStep;
	return fakeNow;
}

static uint64_t fakeTickFrequency(void) {
	return FAKE_TICK_FREQUENCY;
}

static unsigned long fakeProcessId(void) {
	return 81234;
}

static int64_t fakeLoadedAt(void) {
	return 1761400012;
}

static uint64_t fakeJobCount(void) {
	return 17;
}

static uint32_t fakeTimestamp(void) {
	return 2081827;
}

static const char *fakeArchitecture(void) {
	return "arm64";
}

static bool fakeTranslated(void) {
	return false;
}

// A job's files, in a folder of their own.
typedef struct RunFiles {
	wchar_t folder[HARNESS_PATH_LENGTH];
	wchar_t input[HARNESS_PATH_LENGTH];
	wchar_t output[HARNESS_PATH_LENGTH];
	char folderUtf8[HARNESS_PATH_LENGTH];
} RunFiles;

// What a job left: its result line and its files' contents ("" for a file it did not write).
typedef struct RunOutput {
	char result[HARNESS_RESULT_LENGTH];
	ByteBuffer csv;
	ByteBuffer detailedPlayers;
	ByteBuffer detailedTeams;
	ByteBuffer players; // per-match
	ByteBuffer teams;
	ByteBuffer failed;
} RunOutput;

static RunFiles files;
static bool folderReady;
static bool unbufferedResults; // the main CSV reaches the disk as each row is written
static bool checkFlushes;      // check the files on disk around every flush (checkOnDisk)
static unsigned flushes;       // since the job started
static unsigned failingFlush;  // the flush (counting from 1) that reports a failure; 0: none
// The files a job writes beside its results: each one open, and the one whose fclose reports a
// failure, as a full disk makes it (a SIBLING_* index; SIBLING_COUNT for none).
enum { SIBLING_PLAYERS, SIBLING_TEAMS, SIBLING_FAILED, SIBLING_COUNT };
static FILE *openSiblings[SIBLING_COUNT];
static size_t failingClose = SIBLING_COUNT;
static unsigned diskChecks;
static unsigned diskViolations;
static unsigned mainRowsChecked;

static void joinPath(wchar_t *out, const wchar_t *folder, const wchar_t *name) {
	size_t length = wcslen(folder);
	wmemcpy(out, folder, length);
	out[length] = PLATFORM_PATH_SEPARATOR;
	wcscpy(out + length + 1, name);
}

static bool makeFolder(void) {
#ifdef _WIN32
	wchar_t directory[MAX_PATH + 1];
	DWORD length = GetTempPathW(MAX_PATH + 1, directory);
	if (!length || length > MAX_PATH) {
		return false;
	}
	int written = swprintf(
		files.folder,
		HARNESS_PATH_LENGTH,
		L"%lsfm24-job-run-%lu-%llu",
		directory,
		platform_processId(),
		(unsigned long long)platform_ticks()
	);
	if (written <= 0 || written >= HARNESS_PATH_LENGTH || !platform_makeDirectory(files.folder)) {
		return false;
	}
#else
	char made[] = "/tmp/fm24-job-run-XXXXXX";
	if (!mkdtemp(made) || !platform_fromUtf8(made, files.folder, HARNESS_PATH_LENGTH)) {
		return false;
	}
#endif
	joinPath(files.input, files.folder, L"input.bin");
	joinPath(files.output, files.folder, L"results.csv");
	joinPath(stopPath, files.folder, L"stop");
	uint32_t ownSeed = 99;
	return platform_toUtf8(files.folder, files.folderUtf8, sizeof files.folderUtf8) &&
				 files_write(files.input, &ownSeed, sizeof ownSeed);
}

static void removeIn(const wchar_t *name) {
	wchar_t path[HARNESS_PATH_LENGTH];
	joinPath(path, files.folder, name);
	(void)platform_removeFile(path);
}

static void removeOutputs(void) {
	static const wchar_t *const names[] = {
		L"results.csv",
		L"results-players.csv",
		L"results-teams.csv",
		L"results-match-players.csv",
		L"results-match-teams.csv",
		L"results-failed.csv",
	};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
		removeIn(names[i]);
	}
}

static void readIn(const wchar_t *name, ByteBuffer *out) {
	wchar_t path[HARNESS_PATH_LENGTH];
	joinPath(path, files.folder, name);
	if (!files_read(path, out)) {
		buffer_free(out);
	}
}

static void freeOutput(RunOutput *output) {
	buffer_free(&output->csv);
	buffer_free(&output->detailedPlayers);
	buffer_free(&output->detailedTeams);
	buffer_free(&output->players);
	buffer_free(&output->teams);
	buffer_free(&output->failed);
}

// Runs a job of `keys` (after command, input and output) on the test's input; false if it could
// not be parsed.
static bool runJob(const char *keys, RunOutput *output) {
	*output = (RunOutput){0};
	removeOutputs();
	flushes = 0;
	char text[HARNESS_JOB_LENGTH];
	int written = snprintf(
		text,
		sizeof text,
		"command=run\ninput=%s/input.bin\noutput=%s/results.csv\n%s",
		files.folderUtf8,
		files.folderUtf8,
		keys
	);
	Job job;
	if (written < 0 || (size_t)written >= sizeof text || !job_parse(text, &job)) {
		return false;
	}
	jobRunTest_run(&job);
	job_free(&job);
	(void)snprintf(output->result, sizeof output->result, "%s", job_takeResult());
	readIn(L"results.csv", &output->csv);
	readIn(L"results-players.csv", &output->detailedPlayers);
	readIn(L"results-teams.csv", &output->detailedTeams);
	readIn(L"results-match-players.csv", &output->players);
	readIn(L"results-match-teams.csv", &output->teams);
	readIn(L"results-failed.csv", &output->failed);
	return true;
}

// The complete lines of a file of the test's folder: up to its last newline ("" if none).
static void readComplete(const wchar_t *name, ByteBuffer *out) {
	readIn(name, out);
	const char *last = strrchr(buffer_string(out), '\n');
	if (out->data) {
		out->length = last ? (size_t)(last - (const char *)out->data) + 1 : 0;
		out->data[out->length] = 0;
	}
}

static unsigned countIn(const char *text, const char *needle) {
	unsigned count = 0;
	for (const char *at = strstr(text, needle); at; at = strstr(at + 1, needle)) {
		++count;
	}
	return count;
}

// The `field`th comma-separated field (0-based) of the line at `line`, as a number.
static long fieldNumber(const char *line, int field) {
	for (int i = 0; i < field && line; ++i) {
		line = strchr(line, ',');
		line = line ? line + 1 : NULL;
	}
	return line ? strtol(line, NULL, 10) : -1;
}

// For seeds 1..N at positions 0..N-1 (index_base 0): each main row on disk must have its match's
// two team rows, its squads' player rows and every earlier failed seed's row on disk too.
static void checkOnDisk(void) {
	ByteBuffer csv = {0};
	ByteBuffer players = {0};
	ByteBuffer teams = {0};
	ByteBuffer failed = {0};
	readComplete(L"results.csv", &csv);
	readComplete(L"results-match-players.csv", &players);
	readComplete(L"results-match-teams.csv", &teams);
	readComplete(L"results-failed.csv", &failed);
	++diskChecks;
	for (const char *row = strchr(buffer_string(&csv), '\n'); row && row[1];
		row = strchr(row, '\n')) {
		++row;
		unsigned seed = (unsigned)strtoul(row, NULL, 10);
		++mainRowsChecked;
		bool ok = true;
		for (int side = 0; side < 2; ++side) {
			char prefix[48];
			(void)snprintf(prefix, sizeof prefix, "\n%u,%u,%s,", seed - 1, seed, side ? "away" : "home");
			const char *team = strstr(buffer_string(&teams), prefix);
			long squad = team ? fieldNumber(team + 1, 4) : -1;
			ok = ok && squad > 0 && countIn(buffer_string(&players), prefix) == (unsigned long)squad;
		}
		for (size_t i = 0; i < scriptCount; ++i) {
			char failedRow[48];
			(void)snprintf(
				failedRow, sizeof failedRow, "\n%u,%u,rebuild\n", scripts[i].seed - 1, scripts[i].seed
			);
			bool before = scripts[i].fault == FAULT_REBUILD && scripts[i].seed < seed;
			ok = ok && (!before || strstr(buffer_string(&failed), failedRow));
		}
		diskViolations += !ok;
	}
	buffer_free(&csv);
	buffer_free(&players);
	buffer_free(&teams);
	buffer_free(&failed);
}

static int recordFlush(FILE *file) {
	if (checkFlushes) {
		checkOnDisk();
	}
	int result = fflush(file);
	if (checkFlushes) {
		checkOnDisk();
	}
	return ++flushes == failingFlush ? EOF : result;
}

static int recordClose(FILE *file) {
	bool failing = false;
	for (size_t i = 0; file && i < SIBLING_COUNT; ++i) {
		if (openSiblings[i] == file) {
			openSiblings[i] = NULL;
			failing = i == failingClose;
		}
	}
	int result = fclose(file);
	return failing ? EOF : result;
}

static bool endsWith(const wchar_t *path, const wchar_t *suffix) {
	size_t length = wcslen(path);
	size_t extra = wcslen(suffix);
	return length >= extra && wcscmp(path + length - extra, suffix) == 0;
}

static FILE *openRecorded(const wchar_t *path, const char *mode) {
	static const wchar_t *const siblings[SIBLING_COUNT] = {
		L"-match-players.csv", L"-match-teams.csv", L"-failed.csv"};
	FILE *file = platform_openFile(path, mode);
	if (file && unbufferedResults && strcmp(mode, "wb") == 0 && endsWith(path, L"results.csv")) {
		(void)setvbuf(file, NULL, _IONBF, 0);
	}
	for (size_t i = 0; file && strcmp(mode, "wb") == 0 && i < SIBLING_COUNT; ++i) {
		if (endsWith(path, siblings[i])) {
			openSiblings[i] = file;
		}
	}
	return file;
}

static void resetScripts(void) {
	scriptCount = 0;
	defaultDelayMs = 0;
}

static void script(uint32_t seed, Fault fault, unsigned delayMs) {
	if (scriptCount < MAX_SCRIPTED) {
		scripts[scriptCount++] = (Script){seed, fault, delayMs};
	}
}

static void checkText(const char *name, const ByteBuffer *actual, const char *expected) {
	bool same = strcmp(buffer_string(actual), expected) == 0;
	ASSERT_BOOL_EQ(same, true);
	if (!same) {
		fprintf(stderr, "%s is:\n%s\n", name, buffer_string(actual));
	}
}

static void checkResult(const RunOutput *output, const char *expected) {
	bool same = strcmp(output->result, expected) == 0;
	ASSERT_BOOL_EQ(same, true);
	if (!same) {
		fprintf(stderr, "the result is: %s\n", output->result);
	}
}

static void checkDigest(const char *name, const ByteBuffer *actual, const char *expected) {
	char hex[SHA256_HEX_LENGTH];
	sha256_hexOf(actual->data, actual->length, hex);
	bool same = strcmp(hex, expected) == 0;
	ASSERT_BOOL_EQ(same, true);
	if (!same) {
		fprintf(stderr, "%s (SHA-256 %s) is:\n%s\n", name, hex, buffer_string(actual));
	}
}

// The main CSV of seeds 5-12 and 5 again, as the run job wrote it before the new keys.
static const char goldenCsv[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
																"5,1,1,21605,1,b54cda58fbbee87e,1790.0\n"
																"6,2,1,21606,1,538454127b096493,1901.1\n"
																"7,3,1,21607,1,f1bbcdcbfa53e0a8,1234.5\n"
																"8,0,2,21608,1,8ff34785799e5cbd,1345.6\n"
																"9,1,2,21609,1,2e2ac13ef8e8d8d2,1456.7\n"
																"10,2,2,21610,0,cc623af8783354e7,1567.8\n"
																"11,3,2,21611,1,6a99b4b1f77dd0fc,1678.9\n"
																"12,0,0,21612,1,08d12e6b76c84d11,1790.0\n"
																"5,1,1,21605,1,b54cda58fbbee87e,1790.0\n";

// Without the new keys, a job's result line and files are those of the run job before them.
TEST(writes_jobs_without_new_keys_as_before) {
	static const char *const variants[] = {
		"seeds=5-12,5\nkeep_build=1\n",
		"seeds=5-12,5\nkeep_build=1\nthreads=4\n",
		"seeds=5-12,5\nthreads=3\n",
	};
	static const char *const results[] = {
		"ok: 9/9 runs",
		"ok: 9/9 runs",
		"ok: 9/9 runs, each seed rebuilt by the game",
	};
	resetScripts();
	defaultDelayMs = 2;
	for (size_t i = 0; i < sizeof variants / sizeof variants[0]; ++i) {
		RunOutput output;
		ASSERT_BOOL_EQ(runJob(variants[i], &output), true);
		checkResult(&output, results[i]);
		checkText("results.csv", &output.csv, goldenCsv);
		checkText("results-players.csv", &output.detailedPlayers, "");
		ASSERT_BOOL_EQ(!output.players.data && !output.teams.data && !output.failed.data, true);
		freeOutput(&output);
	}
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

// --detailed, and failed seeds, without the new keys.
TEST(writes_detailed_and_failed_jobs_as_before) {
	resetScripts();
	RunOutput output;
	for (int threads = 1; threads <= 4; threads += 3) {
		char keys[64];
		(void)snprintf(keys, sizeof keys, "seeds=5-12,5\nthreads=%d\ndetailed=1\n", threads);
		ASSERT_BOOL_EQ(runJob(keys, &output), true);
		checkResult(&output, "ok: 9/9 runs, each seed rebuilt by the game");
		checkText("results.csv", &output.csv, goldenCsv);
		checkDigest(
			"results-players.csv",
			&output.detailedPlayers,
			"63f72f45fcc716848a746fd2b760dfae7578beb300975d598284c67406b7b525"
		);
		checkDigest(
			"results-teams.csv",
			&output.detailedTeams,
			"c29673e3c42653e993fa89c41492fb39272992669206c2358488ea147a5e9400"
		);
		freeOutput(&output);
	}
	script(8, FAULT_REBUILD, 0);
	ASSERT_BOOL_EQ(runJob("seeds=5-12\ndetailed=1\n", &output), true);
	checkResult(&output, "error: 3/8 runs, each seed rebuilt by the game; see harness.log");
	checkText(
		"results.csv",
		&output.csv,
		"seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
		"5,1,1,21605,1,b54cda58fbbee87e,1790.0\n"
		"6,2,1,21606,1,538454127b096493,1901.1\n"
		"7,3,1,21607,1,f1bbcdcbfa53e0a8,1234.5\n"
	);
	checkText("results-players.csv", &output.detailedPlayers, "");
	checkText("results-teams.csv", &output.detailedTeams, "");
	freeOutput(&output);
	resetScripts();
	script(7, FAULT_DESERIALIZE, 0);
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nkeep_build=1\n", &output), true);
	checkResult(&output, "error: 2/8 runs; see harness.log");
	checkText(
		"results.csv",
		&output.csv,
		"seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
		"5,1,1,21605,1,b54cda58fbbee87e,1790.0\n"
		"6,2,1,21606,1,538454127b096493,1901.1\n"
	);
	freeOutput(&output);
	resetScripts();
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

#define REPORT(                                                                                    \
	threads, edits, elapsed, written, skipped, stopped, failedIndex, failedStage, output             \
)                                                                                                  \
	"; report protocol=" HARNESS_PROTOCOL_TEXT " commit=e74df0b3c1a2d4e5f60718293a4b5c6d7e8f9012 "   \
	"dirty=0 build=2081827 slice=arm64 translated=0 pid=81234 loaded=1761400012 jobs=17 "            \
	"threads=" threads " edits=" edits " elapsed_ms=" elapsed " written=" written                    \
	" skipped=" skipped " stopped=" stopped " failed_index=" failedIndex                             \
	" failed_stage=" failedStage " output_failed=" output

// goldenCsv's header and its rows for seeds 5-12 that are not `leftOut`, at most `rows` of them.
static void goldenRows(char *out, size_t size, size_t rows, uint32_t leftOut) {
	const char *line = goldenCsv;
	size_t length = strcspn(line, "\n") + 1;
	(void)snprintf(out, size, "%.*s", (int)length, line);
	size_t used = strlen(out);
	for (uint32_t seed = 5; seed <= 12 && rows; ++seed) {
		line += length;
		length = strcspn(line, "\n") + 1;
		if (seed != leftOut && used < size) {
			used += (size_t)snprintf(out + used, size - used, "%.*s", (int)length, line);
			--rows;
		}
	}
}

static size_t lineCount(const ByteBuffer *text) {
	return countIn(buffer_string(text), "\n");
}

// Per-match rows are written in list order however the workers finish, and do not depend on the
// number of workers.
TEST(writes_rows_in_list_order_whatever_finishes_first) {
	resetScripts();
	for (uint32_t seed = 1; seed <= 8; ++seed) {
		script(seed, FAULT_NONE, (9 - seed) * 15); // later seeds finish first
	}
	RunOutput parallel;
	ASSERT_BOOL_EQ(runJob("seeds=1-8\nthreads=4\nper_match=1\n", &parallel), true);
	resetScripts();
	RunOutput serial;
	ASSERT_BOOL_EQ(runJob("seeds=1-8\nthreads=1\nper_match=1\n", &serial), true);
	checkResult(&parallel, "ok: 8/8 runs, each seed rebuilt by the game");
	checkResult(&serial, "ok: 8/8 runs, each seed rebuilt by the game");
	checkText("results.csv", &parallel.csv, buffer_string(&serial.csv));
	// Seeds 1-8 at indices 0-7, as tests/synthetic_rows.py computes them independently.
	checkDigest(
		"results-match-players.csv",
		&parallel.players,
		"d575aec5695ea801b29afcede06694b1976cf781e3b6d372add873031aa072b6"
	);
	checkDigest(
		"results-match-teams.csv",
		&parallel.teams,
		"d254718f2cfb5f6e5a7159b144af76579aaa78e0564d0eac9c65315a92b5bd4b"
	);
	checkText("results-match-players.csv", &serial.players, buffer_string(&parallel.players));
	checkText("results-match-teams.csv", &serial.teams, buffer_string(&parallel.teams));
	ASSERT_BOOL_EQ(!parallel.failed.data, true);
	freeOutput(&parallel);
	freeOutput(&serial);
	resetScripts();
}

// A main row on disk means its match's other rows, and those of earlier failed seeds, are there.
TEST(flushes_the_other_rows_before_the_main_rows) {
	resetScripts();
	defaultDelayMs = 3;
	script(2, FAULT_NONE, 40); // holds back the rows after it
	script(4, FAULT_REBUILD, 0);
	script(9, FAULT_REBUILD, 0);
	diskChecks = 0;
	diskViolations = 0;
	mainRowsChecked = 0;
	checkFlushes = true;
	unbufferedResults = true;
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=1-12\nthreads=3\nper_match=1\nseed_errors=skip\n", &output), true);
	checkFlushes = false;
	unbufferedResults = false;
	checkOnDisk();
	ASSERT_INT_EQ((int)diskViolations, 0);
	ASSERT_BOOL_EQ(diskChecks > 10 && mainRowsChecked > 10, true);
	checkResult(&output, "ok: 12/12 runs, 2 skipped (rebuild failed), each seed rebuilt by the game");
	checkText("results-failed.csv", &output.failed, "index,seed,stage\n3,4,rebuild\n8,9,rebuild\n");
	ASSERT_INT_EQ((int)lineCount(&output.csv), 11);
	ASSERT_INT_EQ((int)lineCount(&output.teams), 21);
	ASSERT_BOOL_EQ(
		!strstr(buffer_string(&output.csv), "\n4,") && !strstr(buffer_string(&output.csv), "\n9,"), true
	);
	ASSERT_BOOL_EQ(
		!strstr(buffer_string(&output.teams), "\n3,4,") &&
			!strstr(buffer_string(&output.teams), "\n8,9,"),
		true
	);
	freeOutput(&output);
	resetScripts();
}

// seed_errors=skip records a seed whose rebuild fails and goes on; other failures still stop it.
TEST(skips_seeds_that_fail_to_rebuild) {
	resetScripts();
	script(8, FAULT_REBUILD, 0);
	RunOutput output;
	ASSERT_BOOL_EQ(
		runJob(
			"seeds=5-12\nthreads=1\nper_match=1\nseed_errors=skip\nindex_base=1000\nreport=1\n", &output
		),
		true
	);
	checkResult(
		&output,
		"ok: 8/8 runs, 1 skipped (rebuild failed), each seed rebuilt by the game" REPORT(
			"1", "3", "11420", "8", "1", "0", "-", "-", "0"
		)
	);
	char expected[1024];
	goldenRows(expected, sizeof expected, 8, 8);
	checkText("results.csv", &output.csv, expected);
	checkText("results-failed.csv", &output.failed, "index,seed,stage\n1003,8,rebuild\n");
	checkDigest(
		"results-match-players.csv",
		&output.players,
		"f11f66fc0c24332b60896b28c111726fed157300b727996447823afc88287f3e"
	);
	checkDigest(
		"results-match-teams.csv",
		&output.teams,
		"d21199ab03d4b8f16df057a1acd2d2c345746cf92830f22af0addcd7203a2ba5"
	);
	freeOutput(&output);
	// --detailed averages the matches played.
	RunOutput skipped;
	RunOutput played;
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nthreads=2\ndetailed=1\nseed_errors=skip\n", &skipped), true);
	ASSERT_BOOL_EQ(runJob("seeds=5-7,9-12\nthreads=2\ndetailed=1\n", &played), true);
	checkResult(&skipped, "ok: 8/8 runs, 1 skipped (rebuild failed), each seed rebuilt by the game");
	checkResult(&played, "ok: 7/7 runs, each seed rebuilt by the game");
	ASSERT_BOOL_EQ(skipped.detailedPlayers.length > 0 && skipped.detailedTeams.length > 0, true);
	checkText(
		"results-players.csv", &skipped.detailedPlayers, buffer_string(&played.detailedPlayers)
	);
	checkText("results-teams.csv", &skipped.detailedTeams, buffer_string(&played.detailedTeams));
	freeOutput(&skipped);
	freeOutput(&played);
	// Without skip the rebuild failure stops the job, and the report names it.
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nthreads=1\nindex_base=1000\nreport=1\n", &output), true);
	checkResult(
		&output,
		"error: 3/8 runs, each seed rebuilt by the game; see harness.log" REPORT(
			"1", "3", "4926", "3", "0", "0", "1003", "rebuild", "0"
		)
	);
	ASSERT_BOOL_EQ(!output.failed.data, true);
	freeOutput(&output);
	// A load failure stops the job even with skip.
	resetScripts();
	script(6, FAULT_REBUILD, 0);
	script(7, FAULT_DESERIALIZE, 0);
	ASSERT_BOOL_EQ(
		runJob("seeds=5-12\nthreads=1\nseed_errors=skip\nindex_base=500\nreport=1\n", &output), true
	);
	checkResult(
		&output,
		"error: 2/8 runs, 1 skipped (rebuild failed), each seed rebuilt by the game; see "
		"harness.log" REPORT("1", "3", "1790", "2", "1", "0", "502", "deserialize", "0")
	);
	goldenRows(expected, sizeof expected, 1, 0);
	checkText("results.csv", &output.csv, expected);
	checkText("results-failed.csv", &output.failed, "index,seed,stage\n501,6,rebuild\n");
	freeOutput(&output);
	resetScripts();
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

// The report names the first failed place in the list, whichever failure came first.
TEST(names_the_first_failure_in_the_list) {
	resetScripts();
	script(6, FAULT_DESERIALIZE, 300); // fails after seed 7 has, at the place before it
	script(7, FAULT_DESERIALIZE, 0);
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nthreads=2\nindex_base=100\nreport=1\n", &output), true);
	static const char start[] =
		"error: 1/8 runs, each seed rebuilt by the game; see harness.log; report ";
	ASSERT_INT_EQ(strncmp(output.result, start, sizeof start - 1), 0);
	ASSERT_BOOL_EQ(
		strstr(
			output.result,
			" written=1 skipped=0 stopped=0 failed_index=101 failed_stage=deserialize output_failed=0"
		) != NULL,
		true
	);
	char expected[1024];
	goldenRows(expected, sizeof expected, 1, 0);
	checkText("results.csv", &output.csv, expected);
	freeOutput(&output);
	resetScripts();
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

TEST(stops_at_the_stop_file) {
	char keys[HARNESS_JOB_LENGTH];
	(void)snprintf(
		keys,
		sizeof keys,
		"seeds=5-12\nthreads=1\nper_match=1\nreport=1\nstop_file=%s/stop\n",
		files.folderUtf8
	);
	resetScripts();
	removeIn(L"stop");
	script(7, FAULT_STOP, 0); // the stop file appears while seed 7 plays
	RunOutput output;
	ASSERT_BOOL_EQ(runJob(keys, &output), true);
	checkResult(
		&output,
		"error: 3/8 runs, stopped by the stop file, each seed rebuilt by the game" REPORT(
			"1", "3", "4926", "3", "0", "1", "-", "-", "0"
		)
	);
	char expected[1024];
	goldenRows(expected, sizeof expected, 3, 0);
	checkText("results.csv", &output.csv, expected);
	ASSERT_INT_EQ((int)lineCount(&output.teams), 7);
	freeOutput(&output);
	// There before the job: no seed starts, and the files have their headers.
	resetScripts();
	ASSERT_BOOL_EQ(runJob(keys, &output), true);
	checkResult(
		&output,
		"error: 0/8 runs, stopped by the stop file, each seed rebuilt by the game" REPORT(
			"1", "3", "0", "0", "0", "1", "-", "-", "0"
		)
	);
	checkText("results-match-teams.csv", &output.teams, matchStats_teamRowsHeader);
	checkText("results-match-players.csv", &output.players, matchStats_playerRowsHeader);
	freeOutput(&output);
	(void)snprintf(
		keys, sizeof keys, "seeds=5-12\nkeep_build=1\nstop_file=%s/stop\n", files.folderUtf8
	);
	ASSERT_BOOL_EQ(runJob(keys, &output), true);
	checkResult(&output, "error: 0/8 runs, stopped by the stop file");
	freeOutput(&output);
	// With four workers the seeds already started finish, and the rows stay a prefix of the list.
	removeIn(L"stop");
	resetScripts();
	defaultDelayMs = 40;
	script(6, FAULT_STOP, 1); // while the other workers play their first seeds
	(void)snprintf(
		keys, sizeof keys, "seeds=5-12\nthreads=4\nper_match=1\nstop_file=%s/stop\n", files.folderUtf8
	);
	ASSERT_BOOL_EQ(runJob(keys, &output), true);
	unsigned long written = strtoul(output.result + strlen("error: "), NULL, 10);
	ASSERT_BOOL_EQ(strncmp(output.result, "error: ", 7) == 0 && written >= 2 && written < 8, true);
	if (written < 2 || written >= 8) {
		fprintf(stderr, "the result is: %s\n", output.result);
	}
	ASSERT_BOOL_EQ(strstr(output.result, "/8 runs, stopped by the stop file, each") != NULL, true);
	goldenRows(expected, sizeof expected, written, 0);
	checkText("results.csv", &output.csv, expected);
	ASSERT_INT_EQ((int)lineCount(&output.teams), 1 + 2 * (int)written);
	freeOutput(&output);
	// A stop file that appears as the last seed plays stops nothing.
	removeIn(L"stop");
	resetScripts();
	script(12, FAULT_STOP, 0);
	(void)snprintf(keys, sizeof keys, "seeds=5-12\nthreads=1\nstop_file=%s/stop\n", files.folderUtf8);
	ASSERT_BOOL_EQ(runJob(keys, &output), true);
	checkResult(&output, "ok: 8/8 runs, each seed rebuilt by the game");
	freeOutput(&output);
	removeIn(L"stop");
	resetScripts();
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

// A failed write stops the job; no main row follows per-match rows that may not be on disk.
TEST(reports_output_failures) {
	resetScripts();
	failingFlush = 1; // the first seed's per-match rows
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nthreads=1\nper_match=1\nreport=1\n", &output), true);
	failingFlush = 0;
	checkResult(
		&output,
		"error: 0/8 runs, each seed rebuilt by the game; see harness.log" REPORT(
			"1", "3", "1790", "0", "0", "0", "-", "-", "1"
		)
	);
	char expected[1024];
	goldenRows(expected, sizeof expected, 0, 0);
	checkText("results.csv", &output.csv, expected);
	freeOutput(&output);
	failingFlush = 1; // the first main row
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nkeep_build=1\nreport=1\n", &output), true);
	failingFlush = 0;
	checkResult(
		&output,
		"error: 1/8 runs; see harness.log" REPORT("1", "-", "1790", "1", "0", "0", "-", "-", "1")
	);
	freeOutput(&output);
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

// A file beside the results that cannot be finished (its fclose fails, as on a full disk) is
// reported, and every other file is still closed: none stays open in the game.
TEST(closes_every_file_when_one_cannot_be_finished) {
	for (size_t failing = 0; failing < SIBLING_COUNT; ++failing) {
		resetScripts();
		script(8, FAULT_REBUILD, 0);
		failingClose = failing;
		RunOutput output;
		ASSERT_BOOL_EQ(
			runJob(
				"seeds=5-12\nthreads=1\nkeep_build=1\nper_match=1\nseed_errors=skip\nreport=1\n", &output
			),
			true
		);
		failingClose = SIBLING_COUNT;
		ASSERT_BOOL_EQ(strncmp(output.result, "error: ", 7) == 0, true);
		ASSERT_BOOL_EQ(strstr(output.result, " output_failed=1") != NULL, true);
		for (size_t i = 0; i < SIBLING_COUNT; ++i) {
			ASSERT_BOOL_EQ(openSiblings[i] == NULL, true);
		}
		freeOutput(&output);
	}
	resetScripts();
	ASSERT_INT_EQ((int)atomic_load(&matchesAlive), 0);
}

// A summary with every field at its longest: counts of seeds have at most 11 digits (a job of
// 16,384 bytes lists at most 8,192 ranges of 10,000,001 seeds); jobs, edits and milliseconds 13
// (300 years at one a millisecond), loaded 11 (until the year 5138), the pid and build 32 bits, and
// an index any 64-bit value.
static RunSummary longestSummary(void) {
	return (RunSummary){
		.ok = false,
		.stopped = true,
		.rebuilt = true,
		.seeLog = true,
		.report = true,
		.written = 99999999999U,
		.total = 99999999999U,
		.skipped = 99999999999U,
		.protocol = HARNESS_PROTOCOL_TEXT,
		.commit = fakeCommit,
		.dirty = 1,
		.build = UINT32_MAX,
		.slice = "x86_64",
		.translated = true,
		.pid = UINT32_MAX,
		.loaded = 99999999999,
		.jobs = 9999999999999U,
		.threads = HARNESS_MAX_THREADS,
		.hasEdits = true,
		.edits = 9999999999999U,
		.elapsedMs = 9999999999999U,
		.hasFailure = true,
		.failedIndex = UINT64_MAX,
		.failedStage = STAGE_DESERIALIZE,
		.outputFailed = true,
	};
}

// A summary of nothing, from a harness built outside a Git checkout.
static RunSummary plainSummary(void) {
	return (RunSummary){.ok = true,
		.report = true,
		.protocol = "1.4.0",
		.commit = "unknown",
		.dirty = -1,
		.slice = "arm64"};
}

void jobRunTest_reportLine(bool longest, char *out, size_t size) {
	RunSummary summary = longest ? longestSummary() : plainSummary();
	formatSummary(&summary, out, size);
}

// The longest result line a job can give still fits a reply (HARNESS_RESULT_LENGTH).
TEST(fits_the_longest_report_in_a_reply) {
	RunSummary summary = longestSummary();
	char line[2 * HARNESS_RESULT_LENGTH];
	formatSummary(&summary, line, sizeof line);
	ASSERT_BOOL_EQ(strlen(line) < HARNESS_RESULT_LENGTH, true);
	ASSERT_BOOL_EQ(strstr(line, " failed_stage=deserialize output_failed=1") != NULL, true);
	if (strlen(line) >= HARNESS_RESULT_LENGTH) {
		fprintf(stderr, "%zu bytes: %s\n", strlen(line), line);
	}
	// Unknown builds and no failure.
	summary = plainSummary();
	formatSummary(&summary, line, sizeof line);
	ASSERT_INT_EQ(
		strcmp(
			line,
			"ok: 0/0 runs; report protocol=1.4.0 commit=unknown dirty=- build=0 slice=arm64 "
			"translated=0 pid=0 loaded=0 jobs=0 threads=0 edits=- elapsed_ms=0 written=0 skipped=0 "
			"stopped=0 failed_index=- failed_stage=- output_failed=0"
		),
		0
	);
}

// A row's values: each field of a line, at most `capacity`; returns how many.
static size_t splitFields(const char *line, double *values, const char **texts, size_t capacity) {
	size_t count = 0;
	for (const char *field = line; count < capacity;) {
		texts[count] = field;
		values[count++] = strtod(field, NULL);
		size_t length = strcspn(field, ",\n");
		if (field[length] != ',') {
			break;
		}
		field += length + 1;
	}
	return count;
}

typedef struct PersonSums {
	int side;
	long uid;
	unsigned appearances;
	unsigned starts;
	double minutes;
	double ratings; // hundredths
	double values[MATCH_STATS_PLAYER_VALUES];
} PersonSums;

static bool closeTo(double a, double b, double tolerance) {
	bool close = a - b <= tolerance && b - a <= tolerance;
	if (!close) {
		fprintf(stderr, "%.9g and %.9g differ by more than %g\n", a, b, tolerance);
	}
	return close;
}

// The per-match rows give back what --detailed averages, to its three decimals (live check L1).
TEST(averages_of_the_rows_are_the_detailed_ones) {
	enum { MAX_PEOPLE = 64, FIELDS = 64 };
	const double matches = 24;
	resetScripts();
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=1-24\nthreads=4\nper_match=1\ndetailed=1\n", &output), true);
	checkResult(&output, "ok: 24/24 runs, each seed rebuilt by the game");
	double values[FIELDS];
	const char *texts[FIELDS];
	double teamSums[2][MATCH_STATS_TEAM_VALUES] = {{0}};
	unsigned teamRows[2] = {0, 0};
	for (const char *line = strchr(buffer_string(&output.teams), '\n'); line && line[1];
		line = strchr(line, '\n')) {
		++line;
		size_t count = splitFields(line, values, texts, FIELDS);
		int side = strncmp(texts[2], "away", 4) == 0;
		ASSERT_INT_EQ((int)count, 44);
		for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES && count == 44; ++v) {
			teamSums[side][v] += values[6 + v];
		}
		++teamRows[side];
	}
	ASSERT_BOOL_EQ(teamRows[0] == matches && teamRows[1] == matches, true);
	int sidesCompared = 0;
	for (const char *line = strchr(buffer_string(&output.detailedTeams), '\n'); line && line[1];
		line = strchr(line, '\n')) {
		++line;
		size_t count = splitFields(line, values, texts, FIELDS);
		int side = strncmp(texts[0], "away", 4) == 0;
		ASSERT_INT_EQ((int)count, 3 + MATCH_STATS_TEAM_VALUES);
		for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES && count == 3 + MATCH_STATS_TEAM_VALUES; ++v) {
			ASSERT_BOOL_EQ(closeTo(teamSums[side][v] / matches, values[3 + v], 0.0005 + 1e-6), true);
		}
		++sidesCompared;
	}
	ASSERT_INT_EQ(sidesCompared, 2);
	static PersonSums people[MAX_PEOPLE];
	size_t peopleCount = 0;
	for (const char *line = strchr(buffer_string(&output.players), '\n'); line && line[1];
		line = strchr(line, '\n')) {
		++line;
		size_t count = splitFields(line, values, texts, FIELDS);
		ASSERT_INT_EQ((int)count, 62);
		int side = strncmp(texts[2], "away", 4) == 0;
		long uid = (long)values[5];
		size_t at = 0;
		while (at < peopleCount && (people[at].side != side || people[at].uid != uid)) {
			++at;
		}
		if (at == peopleCount && peopleCount < MAX_PEOPLE) {
			people[peopleCount++] = (PersonSums){.side = side, .uid = uid};
		}
		if (at == peopleCount || count != 62 || values[8] == 0) {
			continue; // did not appear
		}
		PersonSums *person = &people[at];
		++person->appearances;
		person->starts += values[7] != 0;
		person->minutes += values[9];
		person->ratings += values[15] * 100;
		for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
			person->values[v] += values[16 + v];
		}
	}
	size_t compared = 0;
	for (const char *line = strchr(buffer_string(&output.detailedPlayers), '\n'); line && line[1];
		line = strchr(line, '\n')) {
		++line;
		size_t count = splitFields(line, values, texts, FIELDS);
		ASSERT_INT_EQ((int)count, 9 + MATCH_STATS_PLAYER_VALUES);
		int side = strncmp(texts[0], "away", 4) == 0;
		const PersonSums *person = NULL;
		for (size_t i = 0; i < peopleCount; ++i) {
			if (people[i].side == side && people[i].uid == (long)values[3]) {
				person = &people[i];
			}
		}
		ASSERT_BOOL_EQ(person != NULL && count == 9 + MATCH_STATS_PLAYER_VALUES, true);
		if (!person || count != 9 + MATCH_STATS_PLAYER_VALUES) {
			continue;
		}
		ASSERT_BOOL_EQ(values[4] == matches && values[5] == person->appearances, true);
		ASSERT_BOOL_EQ(values[6] == person->starts, true);
		ASSERT_BOOL_EQ(closeTo(person->minutes / matches, values[7], 0.05 + 1e-9), true);
		if (person->appearances) {
			ASSERT_BOOL_EQ(
				closeTo(person->ratings / 100 / person->appearances, values[8], 0.005 + 1e-9), true
			);
		}
		for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
			ASSERT_BOOL_EQ(closeTo(person->values[v] / matches, values[9 + v], 0.0005 + 1e-6), true);
		}
		++compared;
	}
	ASSERT_BOOL_EQ(compared == peopleCount && compared > 15, true);
	freeOutput(&output);
}

// The index column is index_base plus the place in the list, so repeated seeds stay apart.
TEST(indexes_each_place_in_the_list) {
	resetScripts();
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=5,5,5\nthreads=2\nper_match=1\nindex_base=10\n", &output), true);
	checkResult(&output, "ok: 3/3 runs, each seed rebuilt by the game");
	checkDigest(
		"results-match-players.csv",
		&output.players,
		"8c8897b79eb786b465e44d8e0f2712a6282cf192f2cb3911547fc2df10bae272"
	);
	checkDigest(
		"results-match-teams.csv",
		&output.teams,
		"3a996aa5c17248219492a6839e6e92376a335411461133b6de09c3d2e534006e"
	);
	freeOutput(&output);
	// The input's own seed (99) has one place.
	ASSERT_BOOL_EQ(runJob("seeds=keep\nper_match=1\nindex_base=7\n", &output), true);
	checkResult(&output, "ok: 1/1 runs");
	ASSERT_BOOL_EQ(strstr(buffer_string(&output.teams), "\n7,99,home,2000123,10,9,") != NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&output.teams), "\n7,99,away,677,6,5,") != NULL, true);
	ASSERT_INT_EQ((int)lineCount(&output.teams), 3);
	freeOutput(&output);
}

#ifdef _WIN32
enum { MAX_PROGRESS = 32 };
static WPARAM progressHigh[MAX_PROGRESS]; // the snapshots posted to the viewer: home wins | done
static LPARAM progressLow[MAX_PROGRESS];  // draws | total
static size_t progressCount;

static BOOL recordPostMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
	ASSERT_BOOL_EQ((uintptr_t)window == 1 && message == HARNESS_PROGRESS_MESSAGE, true);
	if (progressCount < MAX_PROGRESS) {
		progressHigh[progressCount] = wParam;
		progressLow[progressCount++] = lParam;
	}
	return TRUE;
}

// The viewer's progress messages: a snapshot before the first seed and after each one.
TEST(posts_progress_to_the_viewer_as_before) {
	resetScripts();
	progressCount = 0;
	RunOutput output;
	ASSERT_BOOL_EQ(runJob("seeds=5-12\nthreads=1\nprogress_window=1\n", &output), true);
	checkResult(&output, "ok: 8/8 runs, each seed rebuilt by the game");
	ASSERT_INT_EQ((int)progressCount, 9);
	uint64_t homeWins = 0;
	uint64_t draws = 0;
	for (size_t i = 0; i < progressCount && i < 9; ++i) {
		if (i) {
			uint32_t seed = 4 + (uint32_t)i;
			homeWins += seed % 4 > seed / 4 % 3;
			draws += seed % 4 == seed / 4 % 3;
		}
		ASSERT_BOOL_EQ((uint64_t)progressHigh[i] == (homeWins << 32 | i), true);
		ASSERT_BOOL_EQ((uint64_t)progressLow[i] == (draws << 32 | 8), true);
	}
	freeOutput(&output);
}
#endif

TEST(makes_its_folder) {
	ASSERT_BOOL_EQ(folderReady, true);
}

void registerJobRunTests(void) {
	folderReady = makeFolder();
	test_run("job_run makes its test folder", test_makes_its_folder);
	if (!folderReady) {
		return;
	}
	test_run(
		"job_run writes jobs without the new keys as before",
		test_writes_jobs_without_new_keys_as_before
	);
	test_run(
		"job_run writes detailed and failed jobs without the new keys as before",
		test_writes_detailed_and_failed_jobs_as_before
	);
	test_run(
		"job_run writes rows in list order whatever finishes first",
		test_writes_rows_in_list_order_whatever_finishes_first
	);
	test_run(
		"job_run flushes the other rows before the main rows",
		test_flushes_the_other_rows_before_the_main_rows
	);
	test_run(
		"job_run skips seeds that fail to rebuild, and stops for other failures",
		test_skips_seeds_that_fail_to_rebuild
	);
	test_run("job_run names the first failure in the list", test_names_the_first_failure_in_the_list);
	test_run("job_run stops at the stop file", test_stops_at_the_stop_file);
	test_run("job_run reports output failures", test_reports_output_failures);
	test_run(
		"job_run closes every file when one cannot be finished",
		test_closes_every_file_when_one_cannot_be_finished
	);
	test_run("job_run fits the longest report in a reply", test_fits_the_longest_report_in_a_reply);
	test_run(
		"job_run per-match rows average to the detailed statistics",
		test_averages_of_the_rows_are_the_detailed_ones
	);
	test_run("job_run indexes each place in the list", test_indexes_each_place_in_the_list);
#ifdef _WIN32
	test_run(
		"job_run posts progress to the viewer as before", test_posts_progress_to_the_viewer_as_before
	);
#endif
	removeOutputs();
	removeIn(L"stop");
	removeIn(L"input.bin");
	(void)platform_removeDirectory(files.folder);
}
