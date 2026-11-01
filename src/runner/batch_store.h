// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A batch's folder (see docs/CLI.md "Batches") and the state its files hold: the frozen plan and
// inputs, the committed chunks, the attempts at the others with their parts, and the files made
// from them (index.csv and batch.json) and events.log. There is no other state. A chunk is
// committed by renaming a finished folder into place, so a process stopped at any point leaves
// either the old state or the new one, and every index ends up in exactly one committed chunk.
// Only the process that holds batch.lock changes a batch; reading it needs no lock.
#pragma once

#include "batch_plan.h"
#include "batch_verify.h"
#include "buffer.h"
#include "client.h"
#include "game_logs.h"
#include "host_info.h"
#include "platform.h"
#include "protocol.h"
#include "run_job.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>


enum {
	BATCH_SESSION_LENGTH = 16,  // a session's ID ("s0001") and its NUL
	BATCH_SESSIONS_LENGTH = 96, // a chunk's sessions ("s0001;s0002") and its NUL
};

// The steps of a change to a batch's files, each reported to BatchHost.reached once done.
typedef enum BatchStep {
	BATCH_STEP_ATTEMPT_STARTED, // an attempt's folder and attempt.json are written
	BATCH_STEP_PART_WRITTEN,    // an attempt was evaluated: its part.json is written
	BATCH_STEP_TIDIED,          // a folder left by a stopped process is removed
	BATCH_STEP_MERGED,          // a chunk's parts are merged into chunk=<first>.tmp
	BATCH_STEP_COMPRESSED,      // one of its files is compressed
	BATCH_STEP_DESCRIBED,       // its chunk.json is written
	BATCH_STEP_SYNCED,          // its files and folder are written through to the disk
	BATCH_STEP_RENAMED,         // it is renamed to chunk=<first>: committed
	BATCH_STEP_ATTEMPT_REMOVED, // one of the committed chunk's attempt folders is removed
	BATCH_STEP_INDEXED,         // index.csv has the chunk
	BATCH_STEP_LOGGED,          // events.log has it
	BATCH_STEP_SUMMARISED,      // batch.json is rewritten
	BATCH_STEP_FIXTURE_BUILT,   // a season's fixture, or a home intermediate, is kept in inputs/
	BATCH_STEP_FIXTURES_FROZEN, // inputs/fixtures.txt freezes a season's fixtures
} BatchStep;

// Where the computer draws its power from.
typedef enum BatchPower {
	BATCH_POWER_UNKNOWN,
	BATCH_POWER_AC,
	BATCH_POWER_BATTERY,
} BatchPower;

// What the batch code needs from the computer and the game, so that tests can stand in for them.
// batch run uses the members from `submit` on (macOS only; NULL on Windows).
typedef struct BatchHost {
	void *context;
	// Replaces the file at `path` with `path`.gz (gzip -n -6), checked (gzip -t). Prints why and
	// returns false on failure.
	bool (*compress)(void *context, const wchar_t *path);
	// The decompressed bytes of a .gz file as a stream, with what closing it needs in `*handle`;
	// NULL (printed why) on failure.
	FILE *(*openDecompressed)(void *context, const wchar_t *path, void **handle);
	// Closes such a stream: true if all of it decompressed.
	bool (*closeDecompressed)(void *context, FILE *file, void *handle);
	// Write a file or a folder's entries through to the disk (platform_syncFile and
	// platform_syncDirectory).
	bool (*syncFile)(void *context, FILE *file);
	bool (*syncDirectory)(void *context, const wchar_t *path);
	bool (*freeBytes)(void *context, const wchar_t *path, uint64_t *bytes);
	time_t (*utcNow)(void *context);
	// Called after each step of a change; false stops there, as if the process had been killed
	// (tests). NULL: never.
	bool (*reached)(void *context, BatchStep step);
	// The game and its harness: client_submitWatched, macosGame_checkSave, the running game, its
	// process and slice (BATCH_SLICE_*), whether it serves this harness, quitting it and its memory.
	ClientOutcome (*submit)(
		void *context,
		const char *job,
		unsigned timeoutMs,
		ClientWatch *watch,
		char *result,
		size_t size
	);
	int (*ensureSave)(
		void *context,
		const SessionNeed *need,
		char *date,
		size_t size,
		SessionOutcome *outcome
	);
	uint32_t (*findGame)(void *context);
	bool (*gameRunning)(void *context, uint32_t pid);
	uint32_t (*gameSlice)(void *context, uint32_t pid);
	bool (*harnessListening)(void *context);
	bool (*stopGame)(void *context, uint32_t pid, unsigned graceS);
	uint64_t (*residentBytes)(void *context, uint32_t pid);
	// The computer: its power, keeping it awake while this process runs (on AC power, and with
	// `onBattery` on battery power too), notifications, the signals that stop a batch (the first
	// creates `stop`, the second `stopNow`, the third exits with 4) and how many have come, clocks in
	// milliseconds (the uptime clock leaves out sleep), and sleeping.
	BatchPower (*powerSource)(void *context);
	bool (*keepAwake)(void *context, bool onBattery);
	void (*notify)(void *context, const char *text);
	bool (*watchSignals)(void *context, const wchar_t *stop, const wchar_t *stopNow);
	unsigned (*signals)(void *context);
	uint64_t (*uptimeMs)(void *context);
	uint64_t (*monotonicMs)(void *context);
	void (*sleepMs)(void *context, unsigned milliseconds);
	// FM24's file-list error logs, in the Logs folder of the data folder its save is looked up in:
	// empties the newest and deletes the others (gameLogs_trim), adding to `out`. NULL: nothing to
	// trim.
	void (*trimGameLogs)(void *context, GameLogsTrim *out);
} BatchHost;

// The real host: gzip on macOS (none on Windows), the platform's syncs and clocks.
void batchHost_real(BatchHost *host);

typedef enum BatchStatus {
	BATCH_STATUS_OK,
	BATCH_STATUS_FAILED, // printed why
	BATCH_STATUS_KILLED, // BatchHost.reached stopped it
} BatchStatus;

// How an attempt ended (its part.json's "outcome").
typedef enum BatchPartOutcome {
	BATCH_PART_OK,        // the job finished every index it was given
	BATCH_PART_STOPPED,   // the stop file stopped it
	BATCH_PART_CRASHED,   // the game went, or the job was never seen to end
	BATCH_PART_ABANDONED, // the batch stopped waiting for it (a hang)
	BATCH_PART_ADOPTED,   // a later run found it finished
	BATCH_PART_ERROR,     // it failed
	BATCH_PART_OUTCOMES,
} BatchPartOutcome;

// An attempt at a chunk (attempt.json) and, once evaluated, its part (part.json): the verified
// prefix of what it was given.
typedef struct BatchPart {
	uint64_t first; // the first index it was given
	uint64_t count; // how many
	time_t startedAt;
	uint64_t verified; // the indices from `first` its files cover
	uint64_t poison;
	time_t finishedAt;
	uint64_t wallMs;
	uint64_t sleptMs;
	uint64_t rows[BATCH_ROLES];
	uint32_t attempt; // k of its folder, a<k>
	uint32_t threads;
	BatchPartOutcome outcome;
	bool evaluated; // it has part.json
	bool hasPoison; // `poison` is an index that crashes the game, excluded from the chunk
	char session[BATCH_SESSION_LENGTH];
	char clientOutcome[24];                 // "replied", "lost", ...; "" if unknown
	char resultLine[HARNESS_RESULT_LENGTH]; // "" if none
} BatchPart;

// A chunk that is not committed, with its attempts in order.
typedef struct BatchChunk {
	size_t arm;
	uint64_t chunk;
	uint64_t first;
	uint64_t last;
	BatchPart *parts;
	size_t partCount;
	size_t capacity;
} BatchChunk;

// A chunk's parts in order from its first index: each starts where the one before it ends (after
// its poison index, if any). The work left is [cursor, last].
typedef struct BatchChain {
	uint64_t cursor; // last + 1 once nothing is left
	size_t *parts;   // indices into the chunk's parts, of those that advanced
	size_t count;
	uint64_t *excluded; // the poison indices, ascending
	size_t excludedCount;
	uint64_t *stops; // every cursor the chain passed, the last one included
	size_t stopCount;
} BatchChain;

// A committed chunk, as its chunk.json describes it.
typedef struct BatchCommitted {
	size_t arm;
	uint64_t chunk;
	uint64_t first;
	uint64_t count;
	bool readable; // its chunk.json could be read; if not, only the above is known
	uint64_t rows[BATCH_ROLES];
	uint64_t excluded;
	ResultsSummary summary;
	uint32_t attempts; // every attempt at it
	uint32_t outcomes[BATCH_PART_OUTCOMES];
	char sessions[BATCH_SESSIONS_LENGTH];
	uint64_t wallMs; // of every attempt
	uint64_t sleptMs;
	time_t committedAt;
	char sha256[BATCH_ROLES][SHA256_HEX_LENGTH]; // each file's, uncompressed ("" if none)
	uint64_t rawBytes;                           // its files' bytes, uncompressed
	int canaryMatch; // chunk 0 against the canary reference: 1, 0, or -1 if not compared
} BatchCommitted;

// A game session of a batch (sessions/<id>/session.json): one process of the game, warmed up and
// checked against the batch's canary before its parts.
typedef struct BatchSession {
	char id[BATCH_SESSION_LENGTH];
	time_t startedAt;
	char reason[64];
	uint32_t pid;
	bool launchedByBatch;
	bool hasHarness;      // a report gave the harness's identity:
	RunReport harness;    // its protocol, commit, build, slice, Rosetta flag and load time
	bool identityChanged; // not the first session's (--allow-harness-change)
	char save[BATCH_TEXT_LENGTH];
	char gameDate[BATCH_TEXT_LENGTH];
	bool warmedUp;
	char canary[16];      // "off", "reference", "match", "mismatch"; "" before it ran
	bool nonrepeatable;   // a canary mismatched and on_canary_mismatch=continue went on
	uint64_t jobsAtStart; // jobs the harness had received before the session's first
	uint64_t foreignJobs; // jobs another runner ran meanwhile
} BatchSession;

// A folder a stopped process left, which batch_recover removes.
typedef struct BatchLeftover {
	wchar_t path[HARNESS_PATH_LENGTH];
	bool chunkFolder; // an unfinished chunk folder; else an attempt's
} BatchLeftover;

typedef struct Batch {
	wchar_t directory[HARNESS_PATH_LENGTH];
	char directoryUtf8[BATCH_PATH_LENGTH];
	const BatchHost *host;
	HostInfo hostInfo; // for batch.json
	BatchPlan plan;
	ByteBuffer planText;
	uint32_t *list;            // a list's seeds
	BatchCommitted *committed; // ordered by arm, then first index
	size_t committedCount;
	size_t committedCapacity;
	BatchChunk *pending; // chunks with attempts, ordered as `committed`
	size_t pendingCount;
	size_t pendingCapacity;
	BatchLeftover *leftovers;
	size_t leftoverCount;
	size_t leftoverCapacity;
	BatchSession *sessions; // those with a readable session.json, by number
	size_t sessionCount;
	size_t sessionCapacity;
	uint32_t lastSession; // the highest session number with a folder, 0 for none
	char problem[512];    // why a committed chunk could not be read ("" if none)
	// A season: whether inputs/fixtures.txt has frozen its fixtures (and its hash), and how many are
	// built.
	bool fixturesFrozen;
	char fixturesSha256[SHA256_HEX_LENGTH];
	size_t fixturesBuilt;
} Batch;

typedef enum BatchOpenResult {
	BATCH_OPENED,
	BATCH_OPEN_FAILED,  // printed why
	BATCH_OPEN_CHANGED, // plan.txt, an input or a chunk does not match the frozen plan (exit 2)
} BatchOpenResult;

// What an attempt starts with.
typedef struct BatchAttemptFacts {
	uint32_t threads;
	const char *session;
	time_t startedAt;
	uint64_t maxCount; // the most indices to give it; 0 for all that are left
} BatchAttemptFacts;

// An attempt started: what to submit.
typedef struct BatchAttempt {
	uint32_t attempt;
	uint64_t first;
	uint64_t count;
	BatchPartPaths paths;
	char job[HARNESS_JOB_LENGTH];
} BatchAttempt;

// How an attempt ended, for its part.json.
typedef struct BatchPartFacts {
	BatchPartOutcome outcome;
	const char *clientOutcome; // NULL if unknown
	const char *resultLine;    // NULL or "" for none
	time_t finishedAt;
	uint64_t wallMs;
	uint64_t sleptMs;
	// An index that crashed the game before: it is excluded as a poison seed if the attempt
	// crashed there too (outcome crashed or abandoned, not an attempt that played nothing).
	bool hasPoison;
	uint64_t poison;
	bool discard; // its rows do not count (a harness the batch does not accept): verified is 0
} BatchPartFacts;

// What a chunk.json records of the computer when the chunk commits.
typedef struct BatchCommitFacts {
	const char *power;     // "ac" or "battery"; NULL if unknown
	uint64_t gameRssBytes; // 0 if unknown
	int canaryMatch;       // chunk 0 against the canary reference: 1, 0, or -1 if not compared
	// If given, compares the merged chunk in `folder` (uncompressed) with the canary reference
	// instead of `canaryMatch`: 1, 0 or -1.
	int (*checkCanary)(void *context, const wchar_t *folder);
	void *canaryContext;
} BatchCommitFacts;

// What batch_recover did.
typedef struct BatchRecovery {
	size_t removed;   // leftover folders
	size_t evaluated; // attempts given their part.json
	size_t shortened; // parts whose files hold fewer rows than their part.json, shortened
	size_t stale;     // parts off their chunk's chain, removed
	size_t committed; // chunks with no work left, committed
	size_t relogged;  // committed chunks events.log and index.csv lacked
} BatchRecovery;

// batch.json's "outcome".
typedef struct BatchOutcome {
	const char *state;  // "created", "running", "stopped" or "complete"
	const char *reason; // NULL for none
	int exitCode;       // -1 for none
	time_t at;
} BatchOutcome;

// Progress, from the committed chunks and the parts of the others.
typedef struct BatchProgress {
	uint64_t planned; // indices of every arm
	uint64_t done;    // in committed chunks
	uint64_t inParts; // covered by the parts of chunks not yet committed
	uint64_t chunks;
	uint64_t chunksDone;
	size_t rateChunks; // the latest committed chunks the rate is over (up to 10)
	double rate;       // indices per second awake over them; 0 if unknown
	bool hasEta;
	time_t eta;
	uint32_t attempts;
	uint32_t outcomes[BATCH_PART_OUTCOMES];
	uint64_t excluded;
	uint64_t sleptMs;
} BatchProgress;

// A season's blocks: block c is chunk c of every fixture, seasons first + c * chunk on.
typedef struct BatchSeasons {
	uint64_t blocks;
	uint64_t blocksComplete; // every fixture's chunk committed
	uint64_t seasons;        // planned
	uint64_t seasonsComplete;
	uint64_t completeFirst; // the seasons of the complete blocks from the first (count 0: none)
	uint64_t completeCount;
	uint64_t matchesMissing; // failed and excluded seeds in complete blocks, each costing its season
	bool hasCurrent;         // the first block not complete, and its fixtures committed
	uint64_t current;
	size_t currentDone;
} BatchSeasons;

// An arm's committed chunks, added up.
typedef struct BatchArmTotals {
	uint64_t chunks;
	uint64_t covered; // indices
	uint64_t rows[BATCH_ROLES];
	uint64_t excluded;
	ResultsSummary summary;
} BatchArmTotals;

// The names part.json and chunk.json give outcomes.
const char *batch_outcomeName(BatchPartOutcome outcome);

// Opens the batch in `directory` (absolute): reads plan.txt and checks it and the inputs against
// their hashes, then reads the committed chunks and the attempts.
BatchOpenResult batch_open(Batch *batch, const wchar_t *directory, const BatchHost *host);
void batch_close(Batch *batch);

// The first chunk of the plan's order without chunk.json; false if every chunk is committed.
bool batch_nextChunk(const Batch *batch, size_t *arm, uint64_t *chunk);
const BatchCommitted *batch_committed(const Batch *batch, size_t arm, uint64_t chunk);
// The attempts at a chunk; NULL if it has none (or is committed).
const BatchChunk *batch_pending(const Batch *batch, size_t arm, uint64_t chunk);
// The chain of a chunk's evaluated parts. False if memory runs out.
bool batchChain_compute(const BatchChunk *chunk, BatchChain *chain);
void batchChain_free(BatchChain *chain);

// Starts the next attempt at a chunk, from its chain's cursor: makes its folder and writes its
// attempt.json, written through to the disk, and the job to submit. Every earlier attempt must
// have its part.json.
BatchStatus batch_beginAttempt(
	Batch *batch,
	size_t arm,
	uint64_t chunk,
	const BatchAttemptFacts *facts,
	BatchAttempt *out
);
// Evaluates an attempt once its job has ended: verifies its files and writes its part.json.
BatchStatus batch_finishAttempt(
	Batch *batch,
	size_t arm,
	uint64_t chunk,
	uint32_t attempt,
	const BatchPartFacts *facts
);
// Commits a chunk whose chain covers it: merges its parts, compresses, describes, writes through,
// renames into place, then removes its attempts and adds it to index.csv, events.log and
// batch.json (as running).
BatchStatus batch_commit(Batch *batch, size_t arm, uint64_t chunk, const BatchCommitFacts *facts);
// Brings the files back to a consistent state after an interruption, as batch run does before
// anything else, with no job still writing: removes the leftover folders, evaluates each attempt
// without part.json as `unevaluated`, shortens parts whose files lost rows (to a power cut),
// removes stale parts, rewrites index.csv (logging committed chunks it lacked) and commits every
// chunk with no work left but an arm's chunk 0 when the plan has canaries, which batch run
// commits itself to compare it with the reference.
BatchStatus batch_recover(Batch *batch, BatchPartOutcome unevaluated, BatchRecovery *out);

// Rewrites batch.json from the files, with `outcome`.
BatchStatus batch_writeManifest(Batch *batch, const BatchOutcome *outcome);
// Rewrites index.csv from the committed chunks.
bool batch_writeIndex(Batch *batch);
// Appends a line to events.log: the UTC time, then the text.
void batch_logEvent(Batch *batch, const char *format, ...) PLATFORM_PRINTF(2, 3);
// batch.json's outcome; false (with "" and -1) if it cannot be read.
bool batch_readOutcome(
	const Batch *batch,
	char *state,
	size_t stateSize,
	char *reason,
	size_t reasonSize,
	int *exitCode,
	time_t *at
);

void batch_progress(const Batch *batch, time_t now, BatchProgress *out);
void batch_armTotals(const Batch *batch, size_t arm, BatchArmTotals *out);
void batch_seasons(const Batch *batch, BatchSeasons *out);
// Whether block `chunk` of a season has every fixture's chunk committed.
bool batch_blockComplete(const Batch *batch, uint64_t chunk);
// "chunk <arm>/<first> ok 1000 in 555.0 s (1.80/s); 74,000/2,000,000 (3.7%); 1.79/s over 10
// chunks; ETA 2026-11-07 14:02 UTC" for a committed chunk.
void batch_progressLine(
	const Batch *batch,
	const BatchCommitted *chunk,
	time_t now,
	char *out,
	size_t size
);
// A number with thousands separators ("74,000").
void batch_formatCount(uint64_t value, char *out, size_t size);

// Starts the batch's next session: makes sessions/s<n>/ and writes its ID ("s0001") to `id`.
bool batch_startSession(Batch *batch, char id[BATCH_SESSION_LENGTH]);
// Writes a session's session.json and keeps it for batch.json.
bool batch_saveSession(Batch *batch, const BatchSession *session);
// The first session with a harness identity, which the batch pins; NULL if none has one.
const BatchSession *batch_pinnedSession(const Batch *batch);
// The path of `name` in session `id`'s folder.
bool batch_sessionPath(const Batch *batch, const char *id, const char *name, wchar_t *out);
// True if suspect.csv lists a chunk for `reason`, reading past lines a power cut left as zeros.
bool batch_isSuspect(const Batch *batch, size_t arm, uint64_t first, const char *reason);
// Adds a chunk to suspect.csv ("arm,chunk,session,reason"), which merges leave out, written
// through to the disk.
bool batch_addSuspect(
	Batch *batch,
	size_t arm,
	uint64_t first,
	const char *session,
	const char *reason
);

// Removes a folder and everything in it. Prints why and returns false on failure.
bool batch_removeFolder(const wchar_t *path);
// Writes a file in one step, written through to the disk (with the host's syncs): a temporary file
// beside it, renamed over it. False on failure, leaving any earlier file as it was.
bool batch_writeFile(const BatchHost *host, const wchar_t *path, const void *bytes, size_t size);

// A season's fixture is built once its inputs/<id>.json (.json.gz with compress=gzip) is there.
bool batch_fixtureBuilt(const Batch *batch, size_t arm);
// Keeps what a replacement built into <stem>.tmp.json (`stem` in the batch's folder, as
// "inputs/f0001"): its encoding `bin` as <stem>.bin, then the JSON, compressed if `compress`, as
// <stem>.json(.gz), each written through to the disk, the JSON last.
BatchStatus batch_keepBuilt(Batch *batch, const char *stem, bool compress, const ByteBuffer *bin);
// Freezes a season's fixtures, every one built: their hashes (of the JSONs uncompressed) and team
// overrides into inputs/fixtures.txt, written through to the disk, then removes inputs/build/.
BatchStatus batch_freezeFixtures(Batch *batch);
// Checks a season's fixtures' files against inputs/fixtures.txt, decompressing the JSONs. Prints
// each problem to `report`; true if there are none.
bool batch_verifyFixtures(Batch *batch, FILE *report);

// Checks a committed chunk's files against its chunk.json, and with `deep` decompresses them and
// verifies their rows. Prints each problem to `report`; true if there are none.
bool batch_verifyChunk(Batch *batch, const BatchCommitted *chunk, bool deep, FILE *report);
