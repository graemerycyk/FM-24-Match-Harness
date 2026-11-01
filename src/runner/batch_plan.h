// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A batch's plan, "fm24-batch-plan/1" (see docs/CLI.md): its arms, or a season's teams and the
// fixtures they make, its seed set and settings, the frozen plan.txt that `fm24ctl batch create`
// writes, the seeds of each index, the chunks and the order they run in, and the job that plays
// part of a chunk. Pure: nothing here reads a file.
#pragma once

#include "buffer.h"
#include "protocol.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	BATCH_MAX_ARMS = 16,
	BATCH_MAX_TEAMS = 40,                                         // a season's teams
	BATCH_MAX_FIXTURES = BATCH_MAX_TEAMS * (BATCH_MAX_TEAMS - 1), // and its fixtures, its arms
	BATCH_ARM_LENGTH = 33,                       // an arm's ID, [A-Za-z0-9_-]{1,32}, and its NUL
	BATCH_NAME_LENGTH = 65,                      // a batch's name, [A-Za-z0-9_.-]{1,64}, and its NUL
	BATCH_PATH_LENGTH = 3 * HARNESS_PATH_LENGTH, // a UTF-8 path and its NUL
	BATCH_TEXT_LENGTH = 256,                     // a save's name or a game date, and its NUL
	BATCH_CHUNK_LIMIT = 1000000,                 // the most matches a chunk may have
	BATCH_CHUNK_ID_LENGTH = 11,                  // a chunk's first index as "%010llu", and its NUL
	BATCH_SEED_LENGTH = 11,                      // the most bytes a listed seed takes in a job
};

typedef enum BatchSeedKind {
	BATCH_SEEDS_RANGE,   // range:<first>-<last>: index i plays seed i
	BATCH_SEEDS_SCATTER, // scatter:<key>:<from>-<to>: index i plays batchSeeds_scatter(key, i)
	BATCH_SEEDS_LIST,    // list:<file>: index i plays the file's line i
	// season:<key>:<first>-<last>: index (season) s of fixture k of F plays
	// batchSeeds_scatter(key, s * F + k)
	BATCH_SEEDS_SEASON,
} BatchSeedKind;

// A seed set: each index from `first` to `last` plays one seed, and no two play the same.
typedef struct BatchSeeds {
	BatchSeedKind kind;
	uint64_t key; // scatter and season
	uint64_t first;
	uint64_t last;
	const uint32_t *list; // list: the file's seeds (first 0, last count - 1); not owned
	uint64_t stride;      // season: the fixtures, F
	uint64_t offset;      // season: the fixture's number, k (see batchPlan_armSeeds)
} BatchSeeds;

// The values of the settings that name a choice, in the order of their names.
enum { BATCH_SEED_ERRORS_STOP, BATCH_SEED_ERRORS_SKIP };
enum { BATCH_ORDER_INTERLEAVE, BATCH_ORDER_SEQUENTIAL };
enum { BATCH_COMPRESS_NONE, BATCH_COMPRESS_GZIP };
enum { BATCH_MISMATCH_STOP, BATCH_MISMATCH_CONTINUE };
enum { BATCH_SLICE_ARM64, BATCH_SLICE_X86_64 };
enum { BATCH_MODE_MATCH, BATCH_MODE_SEASON };

typedef struct BatchArm {
	char id[BATCH_ARM_LENGTH];
	char path[BATCH_PATH_LENGTH]; // its fixture, as written in the plan; absolute once frozen
	// Frozen: the fixture's and its encoding's hashes, and its team overrides (0 for none).
	char jsonSha256[SHA256_HEX_LENGTH];
	char binSha256[SHA256_HEX_LENGTH];
	uint32_t teams[2];
	uint32_t pair[2]; // a season's fixture: its home and away teams' unique IDs (0 for an arm)
} BatchArm;

// How many seeds of an arm may fail to rebuild: a count, or thousandths of a percent of the indices
// done.
typedef struct BatchErrorLimit {
	bool percent;
	uint64_t value;
} BatchErrorLimit;

typedef struct BatchPlan {
	char name[BATCH_NAME_LENGTH];
	uint32_t mode; // BATCH_MODE_*
	// The arms (malloc'ed; see batchPlan_free): a batch's arms, or a season's fixtures.
	BatchArm *arms;
	size_t armCount;
	// A season: its teams' unique IDs in the plan's order, and the fixture they are built from, its
	// path as written (absolute once frozen) and, frozen, its hashes and team overrides.
	uint32_t teams[BATCH_MAX_TEAMS];
	size_t teamCount;
	BatchArm base;
	BatchSeeds seeds;
	char listPath[BATCH_PATH_LENGTH]; // list: the file, as written; absolute once frozen
	// The settings (see docs/CLI.md); flags are 0 or 1, and choices the constants above.
	uint32_t chunk;
	uint32_t threads;
	uint32_t perMatch;
	uint32_t keepBuild;
	uint32_t humanControl;
	uint32_t seedErrors;
	BatchErrorLimit maxSeedErrors;
	uint32_t order;
	uint32_t compress;
	uint32_t warmup;
	uint32_t canary;
	uint32_t canaryEvery;
	uint32_t onCanaryMismatch;
	uint32_t slice;
	uint32_t launch;
	uint32_t anySave;
	uint32_t killGame;
	uint32_t hangMinutes;
	uint32_t queueWaitMinutes;
	uint32_t busyWaitMinutes;
	uint32_t maxCrashesPerHour;
	uint32_t maxPoison;
	uint32_t pauseOnBattery;
	uint32_t minFreeGb;
	uint32_t notify;
	uint32_t trimGameLogs;
	char save[BATCH_PATH_LENGTH]; // a save's name or a .fm path; "" for the arms' save
	// Frozen: what `create` found in the fixtures, and the list's hash.
	char saveName[BATCH_TEXT_LENGTH]; // the save the batch needs; "" for none
	char gameDate[BATCH_TEXT_LENGTH]; // the fixtures' in-game date; "" if unknown
	uint32_t gameBuild;
	char listSha256[SHA256_HEX_LENGTH];
	// The SHA-256 of a frozen plan's text before its last line, which holds it.
	char planSha256[SHA256_HEX_LENGTH];
} BatchPlan;

// The defaults that depend on the computer: workers per job, and whether gzip is there.
typedef struct BatchDefaults {
	uint32_t threads;
	uint32_t compress;
} BatchDefaults;

// The paths of a part's job (UTF-8): where the harness reads the arm's encoded fixture, writes the
// results (and their siblings) and looks for the stop file.
typedef struct BatchPartPaths {
	char input[BATCH_PATH_LENGTH];    // <dir>/inputs/<arm>.bin
	char output[BATCH_PATH_LENGTH];   // <dir>/attempts/<arm>/<chunk>/a<attempt>/results.csv
	char stopFile[BATCH_PATH_LENGTH]; // <dir>/control/stop-now
} BatchPartPaths;

// The seed of `index` (from seeds->first to seeds->last).
uint32_t batchSeeds_at(const BatchSeeds *seeds, uint64_t index);
// The scatter seed set's generator, a bijection on 32-bit indices: adjacent keys give unrelated
// seeds, so batches with the same key and disjoint index ranges never share one.
uint32_t batchSeeds_scatter(uint64_t key, uint32_t index);
uint64_t batchSeeds_count(const BatchSeeds *seeds);
// Reads a seed list: one decimal 32-bit seed per line. False (with why in `error`) if a line is not
// one, there are none or a seed is there twice; else `*seeds` is malloc'ed.
bool batchSeeds_readList(
	const char *text,
	size_t length,
	uint32_t **seeds,
	size_t *count,
	char *error,
	size_t errorSize
);
// The seeds= text of indices [first, last]: "a" or "a-b" for a range, else the seeds in index
// order, separated by commas. False if it does not fit in `size`.
bool batchSeeds_format(
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t last,
	char *out,
	size_t size
);
// The SHA-256 of the seeds of indices [first, last], one decimal seed per line.
void batchSeeds_hash(
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t last,
	char hex[SHA256_HEX_LENGTH]
);

// Reads a plan's text: a plan to create from or, with `frozen`, a batch's plan.txt, which also has
// what create added and ends with the hash of the lines before it. Settings not given take their
// defaults (a season's own for chunk, canary and max_seed_errors); a list's seeds are not read
// (seeds.list stays NULL); a season's fixtures are made from its teams, without their hashes and
// overrides. False with the line and why in `error`, the plan freed. Free a plan read with
// batchPlan_free before reading another into it.
bool batchPlan_parse(
	const char *text,
	size_t length,
	bool frozen,
	const BatchDefaults *defaults,
	BatchPlan *plan,
	char *error,
	size_t errorSize
);
void batchPlan_free(BatchPlan *plan);
// Whether `text` looks like a frozen plan.txt, ending with the line of its hash (which
// batchPlan_parse checks).
bool batchPlan_isFrozen(const char *text, size_t length);
// Appends the frozen plan.txt: every setting spelled out and what create read, then the line with
// the hash of the lines before it, which also goes into plan->planSha256. False if memory runs out.
bool batchPlan_format(BatchPlan *plan, ByteBuffer *out);
// The setting's text in a plan ("1%", "250").
void batchPlan_formatLimit(const BatchErrorLimit *limit, char *out, size_t size);
// Reads "json_sha256=<hex> bin_sha256=<hex> home_team=<n|-> away_team=<n|->" (modifying `text`)
// into `arm`, as a frozen plan's fixture lines have them after the arm's ID; false if it is not
// that.
bool batchPlan_readHashes(char *text, BatchArm *arm);
// Writes them so.
void batchPlan_formatHashes(const BatchArm *arm, char *out, size_t size);

// The seeds of an arm: the plan's, or a season fixture's own.
BatchSeeds batchPlan_armSeeds(const BatchPlan *plan, size_t arm);
// Whether a session warms an arm up and checks it with the canary: every arm, or a season's check
// fixtures (batchSeason_isCheckFixture).
bool batchPlan_isCheckArm(const BatchPlan *plan, size_t arm);

// Chunk k of an arm covers indices [first + k * chunk, first + (k + 1) * chunk - 1], the last one
// ending at the seed set's last index.
uint64_t batchPlan_chunksPerArm(const BatchPlan *plan);
void batchPlan_chunkRange(const BatchPlan *plan, uint64_t chunk, uint64_t *first, uint64_t *last);
// The chunk whose first index is `first`; false if none starts there.
bool batchPlan_chunkAt(const BatchPlan *plan, uint64_t first, uint64_t *chunk);
// The chunk at `position` (0 to arms * chunks - 1) of the plan's order: interleave runs chunk c of
// every arm before chunk c + 1, sequential every chunk of an arm before the next arm.
void batchPlan_scheduled(const BatchPlan *plan, uint64_t position, size_t *arm, uint64_t *chunk);
// A chunk's ID, its first index as ten digits.
void batchPlan_chunkId(uint64_t first, char id[BATCH_CHUNK_ID_LENGTH]);

// The paths of attempt `attempt` of the chunk starting at `first`, in the batch in `directory`
// (absolute, UTF-8). False if one does not fit.
bool batchPlan_partPaths(
	const char *directory,
	const char *arm,
	uint64_t first,
	uint32_t attempt,
	BatchPartPaths *out
);
// The run job of a part of arm `arm`: indices from `indexBase` on, playing `seeds` (see
// batchSeeds_format) with `threads` workers, report=1 and the plan's other settings. False if it
// does not fit in `size` or in a job (see runJob_format).
bool batchPlan_formatJob(
	const BatchPlan *plan,
	size_t arm,
	const BatchPartPaths *paths,
	const char *seeds,
	uint64_t indexBase,
	uint32_t threads,
	char *out,
	size_t size
);
// The length of the longest job a part of `chunk` matches can need in the batch in `directory`:
// the longest paths and attempt number, every seed of 10 digits and 16 workers. SIZE_MAX if its
// paths do not fit.
size_t batchPlan_longestJob(const BatchPlan *plan, const char *directory, uint32_t chunk);
// The largest chunk whose longest job fits (0 if none does).
uint32_t batchPlan_largestChunk(const BatchPlan *plan, const char *directory);
