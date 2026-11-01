// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Job "run": plays one match input once per seed and writes a CSV of results.
//
// For a list of seeds the input's fixture is rebuilt by the game with each seed and the input's
// edits re-applied (seed_build.h), so a seed plays as it would in the game. With keep_build the
// seed is only patched into the input, keeping the pre-match values it was built with.
//
// With `threads` > 1 the seeds are shared out between worker threads, each playing whole matches
// with its own copy of the input and its own game stream. Rows are still written in seed order.
//
// With per_match, each match's player and team rows (match_stats.h) go to files of their own, and
// with seed_errors=skip, seeds whose rebuild fails to a file of failed seeds. Those rows are
// written and flushed before the main CSV's rows for the same seeds, so a row in the main CSV means
// its match's other rows reached the system first.

#include "build_info.h"
#include "database.h"
#include "files.h"
#include "game.h"
#include "job.h"
#include "log.h"
#include "match.h"
#include "match_stats.h"
#include "memory.h"
#include "platform.h"
#include "profiler.h"
#include "seed_build.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// Where a seed failed.
typedef enum SeedStage {
	STAGE_NONE,
	STAGE_REBUILD,     // seedBuilder_build
	STAGE_DESERIALIZE, // loading the rebuilt or patched input
	STAGE_STATS,       // out of memory for its statistics
} SeedStage;

typedef struct SeedResult {
	bool done;
	bool ok;
	bool skipped;  // failed to rebuild with seed_errors=skip: a row in the failed seeds' file
	uint8_t stage; // SeedStage, for a failed seed
	MatchResult match;
	double milliseconds;
} SeedResult;

// A played seed's per-match rows, until they are written.
typedef struct SeedRows {
	ByteBuffer players;
	ByteBuffer teams;
} SeedRows;

// Shared by the workers of one job.
typedef struct Run {
	const Job *job;
	const ByteBuffer *input;
	size_t seedOffset;
	uint32_t fileSeed;
	size_t total;
	atomic_size_t next;
	atomic_bool failed;
	atomic_bool stopped; // the stop file kept a seed from starting
	SeedResult *results;
	SeedRows *rows;    // per_match: each seed's rows; NULL without
	PlatformLock lock; // guards results, rows, counters, progress, the files and details
	size_t written;    // results written so far (always a prefix of the seed list)
	size_t completed;
	size_t skipped;
	unsigned homeWins;
	unsigned draws;
	uintptr_t progressWindow;
	FILE *csv;
	FILE *players;     // per_match: "<output>-match-players.csv"
	FILE *teams;       // per_match: "<output>-match-teams.csv"
	FILE *failedSeeds; // seed_errors=skip: "<output>-failed.csv"
	bool outputFailed;
	bool hasFailure; // a failed seed stopped the job: the first in the list, and where it failed
	size_t failedPosition;
	uint8_t failedStage;
	unsigned threads;          // workers started
	uint64_t frequency;        // platform_ticks per second
	Profiler *profiler;        // samples worker 0 (the job thread)
	MatchStatsTotals *details; // --detailed: player and team statistics summed over the seeds
	SeedBuilder *seedBuilder;  // rebuilds the input per seed; NULL: patch the seed into the input
} Run;

typedef struct Worker {
	Run *run;
	unsigned index;
} Worker;

// A finished job's outcome: its result line, and with report=1 the report appended to it.
typedef struct RunSummary {
	bool ok;
	bool stopped;
	bool rebuilt; // each seed rebuilt by the game
	bool seeLog;  // the log says what went wrong
	bool report;
	size_t written;
	size_t total;
	size_t skipped;
	const char *protocol;
	const char *commit;
	int dirty; // -1: unknown
	uint32_t build;
	const char *slice;
	bool translated;
	unsigned long pid;
	int64_t loaded;
	uint64_t jobs;
	unsigned threads;
	bool hasEdits;
	size_t edits;
	uint64_t elapsedMs;
	bool hasFailure;
	uint64_t failedIndex;
	uint8_t failedStage;
	bool outputFailed;
} RunSummary;

static void finishRun(Run *run, double elapsed);
static double elapsedMilliseconds(uint64_t start, uint64_t end, uint64_t frequency);
static double threadCpuMilliseconds(void);
static void
markPhase(Profiler *profiler, const char *phase, uint32_t seed, double wall, double cpu);
static uint32_t seedAt(const Run *run, size_t position);
static bool playSeed(
	Run *run,
	unsigned slot,
	ByteBuffer *input,
	SeedBuilder *builder,
	size_t position,
	MatchStats *stats,
	SeedResult *out,
	SeedRows *rows
);
static bool
readStatistics(Run *run, const uint8_t *match, size_t position, MatchStats *stats, SeedRows *rows);
static void freeRows(SeedRows *rows);
static void noteFailure(Run *run, size_t position, uint8_t stage);
static bool writeBytes(const ByteBuffer *bytes, FILE *file);
static bool writeOtherRows(Run *run, size_t end);
static void writeReadyRows(Run *run);
static void failOutput(Run *run, const char *what);
static void reportProgress(Run *run);
static bool stopRequested(Run *run);
static void worker(void *parameter);
static bool finishSeed(Run *run, size_t index, SeedResult *result, SeedRows *rows);
static unsigned runWorkers(Run *run, unsigned count);
static FILE *openSibling(const wchar_t *output, const wchar_t *suffix, const char *header);
static bool openRun(const Job *job, const ByteBuffer *input, Run *run);
static bool closeFile(FILE *file);
static bool closeRun(Run *run);
static const char *stageName(uint8_t stage);
static void formatSummary(const RunSummary *summary, char *out, size_t size);

void jobs_run(const Job *job) {
	ByteBuffer input = {0};
	Run run;
	if (!files_read(job->input, &input) || !openRun(job, &input, &run)) {
		log_message("cannot start the run");
		job_writeResult("error: cannot start the run; see harness.log");
		buffer_free(&input);
		return;
	}
	SeedBuilder seedBuilder;
	if (!job->keepSeed && !job->keepBuild) {
		char error[512] = "";
		uint8_t *teams[2] = {0};
		if ((job->teams[0][0] && !database_findTeam(job->teams[0], &teams[0], error, sizeof error)) ||
				(job->teams[1][0] && !database_findTeam(job->teams[1], &teams[1], error, sizeof error)) ||
				!seedBuilder_open(
					&seedBuilder,
					&input,
					run.fileSeed,
					(const uint8_t *[]){teams[0], teams[1]},
					error,
					sizeof error
				)) {
			log_message("cannot rebuild per seed: %s", error);
			job_writeResult("error: %s (--keep-build plays the input with only its seed changed)", error);
			(void)closeRun(&run);
			buffer_free(&input);
			return;
		}
		run.seedBuilder = &seedBuilder;
	}
	unsigned threads = job->threads < run.total ? job->threads : (unsigned)run.total;
	log_message(
		"input %zu bytes, seed at +%zu = %u; %zu runs on %u threads",
		input.length,
		run.seedOffset,
		run.fileSeed,
		run.total,
		threads
	);
	uint64_t start = platform_ticks();
	run.profiler = job->profile[0] ? profiler_start(job->profile) : NULL;
	reportProgress(&run);
	if (job->profile[0] && !run.profiler) {
		atomic_store_explicit(&run.failed, true, memory_order_relaxed);
	} else {
		run.threads = runWorkers(&run, threads);
	}
	profiler_stop(run.profiler);
	uint64_t end = platform_ticks();
	double elapsed = elapsedMilliseconds(start, end, run.frequency);
	finishRun(&run, elapsed);
	buffer_free(&input);
	log_message("job finished: %zu/%zu runs in %.1f s", run.written, run.total, elapsed / 1000.0);
}

// Writes --detailed's averages if every seed played, closes the files and sets the result line.
static void finishRun(Run *run, double elapsed) {
	const Job *job = run->job;
	bool failed = atomic_load_explicit(&run->failed, memory_order_relaxed);
	// A stop file that appeared once every seed had started stopped nothing.
	bool stopped =
		atomic_load_explicit(&run->stopped, memory_order_relaxed) && run->written < run->total;
	bool ok = run->written == run->total && !failed && !stopped;
	bool detailsWritten = !ok || !run->details || matchStats_write(run->details, job->output);
	if (!detailsWritten) {
		log_message("cannot write the detailed statistics");
		ok = false;
	}
	size_t edits = run->seedBuilder ? seedBuilder_editCount(run->seedBuilder) : 0;
	if (run->seedBuilder) {
		seedBuilder_close(run->seedBuilder);
	}
	bool closed = closeRun(run);
	ok = closed && ok;
	RunSummary summary = {
		.ok = ok,
		.stopped = stopped,
		.rebuilt = run->seedBuilder != NULL,
		.seeLog = !ok && (failed || !closed || !detailsWritten || !stopped),
		.report = job->report,
		.written = run->written,
		.total = run->total,
		.skipped = run->skipped,
		.protocol = HARNESS_PROTOCOL_TEXT,
		.commit = buildInfo_commit,
		.dirty = buildInfo_dirty,
		.build = game_timestamp(),
		.slice = game_architecture(),
		.translated = game_translated(),
		.pid = platform_processId(),
		.loaded = job_loadedAt(),
		.jobs = job_count(),
		.threads = run->threads,
		.hasEdits = run->seedBuilder != NULL,
		.edits = edits,
		.elapsedMs = (uint64_t)(elapsed + 0.5),
		.hasFailure = run->hasFailure,
		.failedIndex = job->indexBase + run->failedPosition,
		.failedStage = run->failedStage,
		.outputFailed = run->outputFailed || !closed,
	};
	char line[2 * HARNESS_RESULT_LENGTH];
	formatSummary(&summary, line, sizeof line);
	if (strlen(line) >= HARNESS_RESULT_LENGTH) {
		log_message("the result line is too long to send whole: %s", line);
	}
	job_writeResult("%s", line);
}

static double elapsedMilliseconds(uint64_t start, uint64_t end, uint64_t frequency) {
	return 1000.0 * (double)(end - start) / (double)frequency;
}

// CPU time the calling thread has used, in milliseconds.
static double threadCpuMilliseconds(void) {
	return (double)platform_threadCpuTime() / 10000.0; // 100 ns units
}

// Notes one phase's wall and CPU time in the profile.
static void
markPhase(Profiler *profiler, const char *phase, uint32_t seed, double wall, double cpu) {
	char text[128];
	(void)snprintf(text, sizeof text, "%s seed=%u wall_ms=%.1f cpu_ms=%.1f", phase, seed, wall, cpu);
	profiler_mark(profiler, text);
}

static uint32_t seedAt(const Run *run, size_t position) {
	return run->job->keepSeed ? run->fileSeed : run->job->seeds[position];
}

// Plays the seed at `position` in the list: the game's rebuild for it, or `input` with the seed
// patched in. Returns false, with the stage in `out`, if the match could not be built or loaded,
// or its statistics not kept. `stats` is the worker's (for per-match rows or --detailed), and
// `rows` receives the rows with per_match.
static bool playSeed(
	Run *run,
	unsigned slot,
	ByteBuffer *input,
	SeedBuilder *builder,
	size_t position,
	MatchStats *stats,
	SeedResult *out,
	SeedRows *rows
) {
	uint32_t seed = seedAt(run, position);
	Profiler *profiler = slot == 0 ? run->profiler : NULL;
	ByteBuffer rebuilt = {0};
	ByteBuffer *bytes = input;
	if (builder) {
		if (!seedBuilder_build(builder, seed, &rebuilt)) {
			log_message("seed %u: rebuild failed", seed);
			buffer_free(&rebuilt);
			out->stage = STAGE_REBUILD;
			return false;
		}
		bytes = &rebuilt;
	} else {
		memcpy(input->data + run->seedOffset, &seed, sizeof seed);
	}
	uint64_t loaded = 0;
	double cpuStart = profiler ? threadCpuMilliseconds() : 0;
	double cpuLoaded = 0;
	uint64_t start = platform_ticks();
	uint8_t *match = match_deserializeOn(slot, bytes);
	buffer_free(&rebuilt);
	if (!match) {
		log_message("seed %u: deserialize failed", seed);
		out->stage = STAGE_DESERIALIZE;
		return false;
	}
	if (profiler) {
		loaded = platform_ticks();
		cpuLoaded = threadCpuMilliseconds();
	}
	if (!run->job->humanControl) {
		match_handToAssistant(match);
	}
	match_play(match);
	uint64_t end = platform_ticks();
	if (profiler) {
		double cpuEnd = threadCpuMilliseconds();
		double wallLoad = elapsedMilliseconds(start, loaded, run->frequency);
		markPhase(profiler, "deserialize", seed, wallLoad, cpuLoaded - cpuStart);
		markPhase(
			profiler, "play", seed, elapsedMilliseconds(loaded, end, run->frequency), cpuEnd - cpuLoaded
		);
	}
	out->match = match_result(match);
	bool ok = !stats || readStatistics(run, match, position, stats, rows);
	if (!ok) {
		out->stage = STAGE_STATS;
	}
	out->milliseconds = elapsedMilliseconds(start, end, run->frequency);
	match_delete(match);
	return ok;
}

// Reads the played match's statistics into the worker's `stats`, formats its rows (outside the
// lock) and adds it to the --detailed totals. False if memory runs out.
static bool
readStatistics(Run *run, const uint8_t *match, size_t position, MatchStats *stats, SeedRows *rows) {
	uint32_t seed = seedAt(run, position);
	matchStats_read(match, stats);
	if (rows && !matchStats_formatRows(
								stats, run->job->indexBase + position, seed, &rows->players, &rows->teams
							)) {
		log_message("seed %u: out of memory formatting the per-match rows", seed);
		return false;
	}
	if (!run->details) {
		return true;
	}
	platform_lock(&run->lock);
	bool ok = matchStats_accumulate(run->details, stats);
	platform_unlock(&run->lock);
	if (!ok) {
		log_message("seed %u: out of memory accumulating player statistics", seed);
	}
	return ok;
}

static void freeRows(SeedRows *rows) {
	buffer_free(&rows->players);
	buffer_free(&rows->teams);
}

// Keeps the failure that stops the job: the first in the list, as the rows written stop there.
// Called with run->lock held.
static void noteFailure(Run *run, size_t position, uint8_t stage) {
	if (!run->hasFailure || position < run->failedPosition) {
		run->hasFailure = true;
		run->failedPosition = position;
		run->failedStage = stage;
	}
}

static bool writeBytes(const ByteBuffer *bytes, FILE *file) {
	return !bytes->length || fwrite(bytes->data, 1, bytes->length, file) == bytes->length;
}

// The first phase of writeReadyRows: the per-match rows and failed seeds of results [written, end),
// flushed. Called with run->lock held.
static bool writeOtherRows(Run *run, size_t end) {
	bool ok = true;
	for (size_t i = run->written; i < end; ++i) {
		if (run->results[i].skipped) {
			ok = ok && fprintf(
									 run->failedSeeds,
									 "%llu,%u,rebuild\n",
									 (unsigned long long)(run->job->indexBase + i),
									 seedAt(run, i)
								 ) >= 0;
		} else if (run->rows) {
			ok = ok && writeBytes(&run->rows[i].players, run->players) &&
					 writeBytes(&run->rows[i].teams, run->teams);
		}
		if (run->rows) {
			freeRows(&run->rows[i]);
		}
	}
	FILE *files[] = {run->players, run->teams, run->failedSeeds};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		ok = ok && (!files[i] || fflush(files[i]) == 0);
	}
	return ok;
}

// Writes the rows that are now complete in seed order: first every other file's rows, flushed,
// then the main CSV's, so a main row is never on disk before its other rows. Called with run->lock
// held.
static void writeReadyRows(Run *run) {
	if (run->outputFailed) {
		return;
	}
	size_t end = run->written;
	while (end < run->total && run->results[end].done &&
				 (run->results[end].ok || run->results[end].skipped)) {
		++end;
	}
	if (end == run->written) {
		return;
	}
	if ((run->rows || run->failedSeeds) && !writeOtherRows(run, end)) {
		failOutput(run, "the per-match rows or the failed seeds");
		return;
	}
	bool ok = true;
	bool wroteRows = false;
	for (; run->written < end; ++run->written) {
		const SeedResult *result = &run->results[run->written];
		if (!result->ok) {
			continue; // skipped
		}
		if (fprintf(
					run->csv,
					"%u,%u,%u,%u,%u,%016llx,%.1f\n",
					result->match.seed,
					result->match.homeGoals,
					result->match.awayGoals,
					result->match.clockTicks,
					result->match.finished ? 1U : 0U,
					(unsigned long long)result->match.finalRngState,
					result->milliseconds
				) < 0) {
			ok = false;
			break;
		}
		wroteRows = true;
	}
	if (wroteRows && fflush(run->csv) != 0) {
		ok = false;
	}
	if (!ok) {
		failOutput(run, "the results CSV");
	}
}

// Called with run->lock held.
static void failOutput(Run *run, const char *what) {
	log_message("cannot write %s", what);
	run->outputFailed = true;
	atomic_store_explicit(&run->failed, true, memory_order_relaxed);
}

// Posts a snapshot to the viewer window (Windows only: the macOS runner has no viewer).
static void reportProgress(Run *run) {
#ifdef _WIN32
	if (run->progressWindow &&
			!PostMessageW(
				(HWND)run->progressWindow, // NOLINT(performance-no-int-to-ptr): cross-process HWND.
				HARNESS_PROGRESS_MESSAGE,
				(WPARAM)((uint64_t)run->homeWins << 32 | run->completed),
				(LPARAM)((uint64_t)run->draws << 32 | run->total)
			)) {
		log_message("cannot report simulation progress (Windows error %lu)", GetLastError());
		run->progressWindow = 0;
	}
#else
	(void)run;
#endif
}

// True once the job's stop file exists while seeds are left to start.
static bool stopRequested(Run *run) {
	if (!run->job->stopFile[0] ||
			atomic_load_explicit(&run->next, memory_order_relaxed) >= run->total) {
		return false;
	}
	FILE *file = platform_openFile(run->job->stopFile, "rb");
	if (!file) {
		return false;
	}
	(void)fclose(file);
	return true;
}

static void worker(void *parameter) {
	const Worker *self = parameter;
	Run *run = self->run;
	game_initThread();
	if (!match_inputStream(self->index)) {
		log_message("worker %u: cannot create a stream", self->index);
		atomic_store_explicit(&run->failed, true, memory_order_relaxed);
		return;
	}
	ByteBuffer input = {0}; // a private copy: each worker patches its own seed into it
	SeedBuilder *builder = run->seedBuilder;
	bool readsStatistics = run->rows || run->details;
	MatchStats *stats = readsStatistics ? malloc(sizeof *stats) : NULL;
	if ((!builder && !buffer_append(&input, run->input->data, run->input->length)) ||
			(readsStatistics && !stats)) {
		log_message("worker %u: out of memory copying the input", self->index);
		atomic_store_explicit(&run->failed, true, memory_order_relaxed);
		buffer_free(&input);
		free(stats);
		return;
	}
	while (!atomic_load_explicit(&run->failed, memory_order_relaxed) &&
				 !atomic_load_explicit(&run->stopped, memory_order_relaxed)) {
		if (stopRequested(run)) {
			log_message("worker %u: the stop file exists; starting no more seeds", self->index);
			atomic_store_explicit(&run->stopped, true, memory_order_relaxed);
			break;
		}
		size_t index = atomic_fetch_add_explicit(&run->next, 1, memory_order_relaxed);
		if (index >= run->total) {
			break;
		}
		SeedResult result = {0};
		SeedRows rows = {0};
		result.ok =
			playSeed(run, self->index, &input, builder, index, stats, &result, run->rows ? &rows : NULL);
		if (!finishSeed(run, index, &result, &rows)) {
			break;
		}
	}
	buffer_free(&input);
	free(stats);
}

// Keeps a played seed's result and rows, writes the rows now ready and updates the progress. False
// if the seed's failure stops the job.
static bool finishSeed(Run *run, size_t index, SeedResult *result, SeedRows *rows) {
	result->done = true;
	result->skipped = !result->ok && run->job->skipSeedErrors && result->stage == STAGE_REBUILD;
	bool stops = !result->ok && !result->skipped;
	if (!result->ok) {
		freeRows(rows);
	}
	platform_lock(&run->lock);
	run->results[index] = *result;
	if (run->rows) {
		run->rows[index] = *rows;
	}
	if (stops) {
		noteFailure(run, index, result->stage);
	}
	run->skipped += result->skipped;
	writeReadyRows(run);
	run->completed += result->ok;
	run->homeWins += result->ok && result->match.homeGoals > result->match.awayGoals;
	run->draws += result->ok && result->match.homeGoals == result->match.awayGoals;
	reportProgress(run);
	platform_unlock(&run->lock);
	if (stops) {
		atomic_store_explicit(&run->failed, true, memory_order_relaxed);
	}
	return !stops;
}

// Runs worker 0 on this thread and the others on new threads; returns once all have finished,
// with the number of workers that ran.
static unsigned runWorkers(Run *run, unsigned count) {
	Worker workers[HARNESS_MAX_THREADS];
	PlatformThread threads[HARNESS_MAX_THREADS];
	unsigned started = 0;
	for (unsigned i = 1; i < count; ++i) {
		workers[i] = (Worker){run, i};
		if (!platform_startThread(&threads[started], worker, &workers[i])) {
			log_message("cannot start worker %u; continuing with %u", i, started + 1);
			break;
		}
		++started;
	}
	workers[0] = (Worker){run, 0};
	worker(&workers[0]);
	platform_joinThreads(threads, started);
	return started + 1;
}

// A file next to the output (see matchStats_siblingPath), with its header written; NULL if not.
static FILE *openSibling(const wchar_t *output, const wchar_t *suffix, const char *header) {
	wchar_t path[HARNESS_PATH_LENGTH];
	FILE *file = matchStats_siblingPath(output, suffix, path, HARNESS_PATH_LENGTH)
								 ? platform_openFile(path, "wb")
								 : NULL;
	if (file && fputs(header, file) < 0) {
		(void)fclose(file);
		return NULL;
	}
	return file;
}

static bool openRun(const Job *job, const ByteBuffer *input, Run *run) {
	*run = (Run){.job = job,
		.input = input,
		.total = job->keepSeed ? 1 : job->seedCount,
		.progressWindow = job->progressWindow};
	atomic_init(&run->next, 0);
	atomic_init(&run->failed, false);
	atomic_init(&run->stopped, false);
	run->frequency = platform_tickFrequency();
	if (!match_seedOffset(input->data, input->length, &run->seedOffset)) {
		log_message("the input is not a serialised GAME_MATCH");
		return false;
	}
	run->fileSeed = memory_loadU32(input->data + run->seedOffset);
	run->results = calloc(run->total, sizeof *run->results);
	run->rows = job->perMatch ? calloc(run->total, sizeof *run->rows) : NULL;
	run->details = job->detailed ? calloc(1, sizeof *run->details) : NULL;
	run->csv = platform_openFile(job->output, "wb");
	if (!run->results || !run->csv || (job->perMatch && !run->rows) ||
			(job->detailed && !run->details)) {
		log_message("out of memory, or cannot open the output file");
		free(run->results);
		free(run->rows);
		free(run->details);
		if (run->csv) {
			(void)fclose(run->csv);
		}
		return false;
	}
	platform_initLock(&run->lock);
	if (fprintf(run->csv, "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n") < 0) {
		log_message("cannot write the results CSV header");
		(void)closeRun(run);
		return false;
	}
	if (job->perMatch) {
		run->players = openSibling(job->output, L"-match-players.csv", matchStats_playerRowsHeader);
		run->teams = openSibling(job->output, L"-match-teams.csv", matchStats_teamRowsHeader);
	}
	if (job->skipSeedErrors) {
		run->failedSeeds = openSibling(job->output, L"-failed.csv", "index,seed,stage\n");
	}
	if ((job->perMatch && (!run->players || !run->teams)) ||
			(job->skipSeedErrors && !run->failedSeeds)) {
		log_message("cannot open the per-match or failed seeds' files next to the output");
		(void)closeRun(run);
		return false;
	}
	return true;
}

static bool closeFile(FILE *file) {
	if (!file) {
		return true;
	}
	bool ok = !ferror(file);
	return fclose(file) == 0 && ok;
}

static bool closeRun(Run *run) {
	bool ok = closeFile(run->csv);
	if (!ok) {
		log_message("cannot finish writing the results CSV");
	}
	// Each file is closed whether or not another failed, so that none stays open in the game.
	bool players = closeFile(run->players);
	bool teams = closeFile(run->teams);
	bool failedSeeds = closeFile(run->failedSeeds);
	if (!players || !teams || !failedSeeds) {
		log_message("cannot finish writing the per-match rows or the failed seeds");
		ok = false;
	}
	platform_destroyLock(&run->lock);
	if (run->details) {
		matchStats_free(run->details);
	}
	for (size_t i = 0; run->rows && i < run->total; ++i) {
		freeRows(&run->rows[i]);
	}
	free(run->rows);
	free(run->details);
	free(run->results);
	return ok;
}

static const char *stageName(uint8_t stage) {
	switch (stage) {
		case STAGE_REBUILD: return "rebuild";
		case STAGE_DESERIALIZE: return "deserialize";
		case STAGE_STATS: return "stats";
		default: return "-";
	}
}

// The result line: "ok: N/N runs", with what else happened, and the report (docs/TECHNICAL.md).
static void formatSummary(const RunSummary *summary, char *out, size_t size) {
	int length = snprintf(
		out, size, "%s: %zu/%zu runs", summary->ok ? "ok" : "error", summary->written, summary->total
	);
	if (summary->skipped && length >= 0 && (size_t)length < size) {
		length += snprintf(
			out + length, size - (size_t)length, ", %zu skipped (rebuild failed)", summary->skipped
		);
	}
	if (length >= 0 && (size_t)length < size) {
		length += snprintf(
			out + length,
			size - (size_t)length,
			"%s%s%s",
			summary->stopped ? ", stopped by the stop file" : "",
			summary->rebuilt ? ", each seed rebuilt by the game" : "",
			summary->seeLog ? "; see harness.log" : ""
		);
	}
	if (!summary->report || length < 0 || (size_t)length >= size) {
		return;
	}
	char edits[24] = "-";
	char failedIndex[24] = "-";
	if (summary->hasEdits) {
		(void)snprintf(edits, sizeof edits, "%zu", summary->edits);
	}
	if (summary->hasFailure) {
		(void)snprintf(
			failedIndex, sizeof failedIndex, "%llu", (unsigned long long)summary->failedIndex
		);
	}
	char dirty[4] = "-";
	if (summary->dirty >= 0) {
		(void)snprintf(dirty, sizeof dirty, "%d", summary->dirty);
	}
	(void)snprintf(
		out + length,
		size - (size_t)length,
		"; report protocol=%s commit=%s dirty=%s build=%u slice=%s translated=%d pid=%lu loaded=%lld "
		"jobs=%llu threads=%u edits=%s elapsed_ms=%llu written=%zu skipped=%zu stopped=%d "
		"failed_index=%s failed_stage=%s output_failed=%d",
		summary->protocol,
		summary->commit,
		dirty,
		(unsigned)summary->build,
		summary->slice,
		summary->translated,
		summary->pid,
		(long long)summary->loaded,
		(unsigned long long)summary->jobs,
		summary->threads,
		edits,
		(unsigned long long)summary->elapsedMs,
		summary->written,
		summary->skipped,
		summary->stopped,
		failedIndex,
		summary->hasFailure ? stageName(summary->failedStage) : "-",
		summary->outputFailed
	);
}
