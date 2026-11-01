// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_store.h"

#include "batch_season.h"
#include "build_info.h"
#include "files.h"
#include "json_reader.h"
#include "json_writer.h"
#include "manifest.h"
#include "match_config.h"
#include "run_job.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


enum {
	RELATIVE_LENGTH = 256, // a path inside the batch's folder
	NAME_LENGTH = 256,     // the longest folder entry the batch reads
	KEY_LENGTH = 96,       // a JSON path
	RATE_CHUNKS = 10,      // the latest committed chunks the rate is over
	TIME_LENGTH = 32,
	// A suspect.csv line, and its header.
	SUSPECT_LENGTH = 32 + BATCH_ARM_LENGTH + BATCH_CHUNK_ID_LENGTH + BATCH_SESSIONS_LENGTH + 64,
};

static const char *const outcomeNames[BATCH_PART_OUTCOMES] = {
	"ok",
	"stopped",
	"crashed",
	"abandoned",
	"adopted",
	"error",
};
static const char *const roleNames[BATCH_ROLES] = {
	"results",
	"match_players",
	"match_teams",
	"failed",
};
// What the harness writes for an attempt's job, and the files of a chunk before compression.
static const char *const attemptFiles[BATCH_ROLES] = {
	"results.csv",
	"results-match-players.csv",
	"results-match-teams.csv",
	"results-failed.csv",
};
static const char *const chunkFiles[BATCH_ROLES] = {
	"results.csv",
	"match-players.csv",
	"match-teams.csv",
	"failed.csv",
};
static const char attemptFormat[] = "fm24-batch-attempt/1";
static const char partFormat[] = "fm24-batch-part/1";
static const char chunkFormat[] = "fm24-chunk-manifest/1";
static const char sessionFormat[] = "fm24-batch-session/1";
static const char suspectHeader[] = "arm,chunk,session,reason\n";
static const char indexHeader[] =
	"arm,first_index,count,results,failed,excluded,player_rows,team_rows,home,draw,away,home_goals,"
	"away_goals,unfinished,sessions,attempts,wall_ms,matches_per_s,committed_at,results_sha256,"
	"players_sha256,teams_sha256,path\n";

typedef struct Entry {
	wchar_t name[NAME_LENGTH];
	bool isDirectory;
} Entry;

typedef struct Listing {
	Entry *entries;
	size_t count;
	size_t capacity;
	bool failed;
} Listing;

// One of a chunk's files while it commits.
typedef struct ChunkFile {
	bool present;
	char name[32]; // as stored: "results.csv.gz", or without ".gz"
	uint64_t rows;
	uint64_t rawBytes;
	char rawSha256[SHA256_HEX_LENGTH];
	char contentSha256[SHA256_HEX_LENGTH];
	uint64_t bytes;
	char sha256[SHA256_HEX_LENGTH];
} ChunkFile;

// A commit under way.
typedef struct Commit {
	Batch *batch;
	BatchChunk *pending;
	BatchChain chain;
	const BatchCommitFacts *facts;
	wchar_t armFolder[HARNESS_PATH_LENGTH];
	wchar_t temporary[HARNESS_PATH_LENGTH];
	wchar_t folder[HARNESS_PATH_LENGTH];
	ChunkFile files[BATCH_ROLES];
	ResultsSummary summary;
	int canaryMatch; // against the canary reference: 1, 0, or -1 if not compared
	time_t committedAt;
	ByteBuffer manifest; // chunk.json
} Commit;

// A session's chunks, for batch.json, and its record if it has one.
typedef struct SessionCount {
	char id[BATCH_SESSION_LENGTH];
	uint64_t chunks;
	const BatchSession *record;
} SessionCount;

static bool reached(const Batch *batch, BatchStep step);
static BatchStatus failed(const char *format, ...) PLATFORM_PRINTF(1, 2);
static bool pathOf(const Batch *batch, wchar_t *out, const char *format, ...) PLATFORM_PRINTF(3, 4);
static bool joinName(wchar_t *out, const wchar_t *folder, const char *name);
static bool joinWide(wchar_t *out, const wchar_t *folder, const wchar_t *name);
static void folderOf(const wchar_t *path, wchar_t *out);
static bool listEntry(void *context, const wchar_t *name, bool isDirectory);
static bool listFolder(const wchar_t *path, Listing *listing);
static bool removeTree(const wchar_t *path);
static bool replaceText(const Batch *batch, const wchar_t *path, const ByteBuffer *text);
static bool hashFile(const wchar_t *path, uint64_t *bytes, char hex[SHA256_HEX_LENGTH]);
static bool hashStream(FILE *file, uint64_t *bytes, char hex[SHA256_HEX_LENGTH]);
static bool readFixtureJson(Batch *batch, size_t arm, ByteBuffer *out);
static bool readJson(const wchar_t *path, JsonFlat *flat);
static bool entryName(const Entry *entry, char *out, size_t size);
static bool readChunkId(const char *text, uint64_t *first, const char **rest);
static size_t armNamed(const Batch *batch, const char *id);
static BatchOpenResult readPlan(Batch *batch);
static BatchOpenResult checkInputs(Batch *batch);
static BatchOpenResult checkSeasonInputs(Batch *batch);
static size_t countBuilt(const Batch *batch);
static BatchOpenResult
checkInput(Batch *batch, const char *name, const char *expected, ByteBuffer *keep);
static BatchOpenResult scanChunks(Batch *batch);
static BatchOpenResult scanArmChunks(Batch *batch, size_t arm, const wchar_t *folder);
static BatchOpenResult
readCommittedFolder(Batch *batch, size_t arm, uint64_t first, const wchar_t *folder);
static bool readCommitted(const Batch *batch, const JsonFlat *flat, BatchCommitted *record);
static void readCommittedAttempts(const JsonFlat *flat, const char *array, BatchCommitted *record);
static void addSession(char *sessions, size_t size, const char *session);
static BatchOpenResult scanAttempts(Batch *batch);
static bool scanChunkAttempts(Batch *batch, size_t arm, uint64_t chunk, const wchar_t *folder);
static bool
readPart(const Batch *batch, const BatchChunk *chunk, const wchar_t *folder, BatchPart *part);
static void readEvaluation(const JsonFlat *flat, BatchPart *part);
static bool addLeftover(Batch *batch, const wchar_t *path, bool chunkFolder);
static bool addCommitted(Batch *batch, const BatchCommitted *record);
static BatchChunk *findPending(const Batch *batch, size_t arm, uint64_t chunk);
static BatchChunk *addPending(Batch *batch, size_t arm, uint64_t chunk);
static void dropPending(Batch *batch, BatchChunk *chunk);
static BatchPart *findPart(const BatchChunk *chunk, uint32_t attempt);
static bool addPart(BatchChunk *chunk, const BatchPart *part);
static int compareParts(const void *a, const void *b);
static int compareCommitted(const void *a, const void *b);
static bool
attemptFolder(const Batch *batch, const BatchChunk *chunk, uint32_t attempt, wchar_t *out);
static bool chunkAttemptsFolder(const Batch *batch, size_t arm, uint64_t first, wchar_t *out);
static bool writeAttemptJson(
	Batch *batch,
	const BatchChunk *chunk,
	const BatchPart *part,
	const char *seeds,
	const char *job
);
static bool evaluatePart(
	Batch *batch,
	const BatchChunk *chunk,
	const BatchPart *part,
	BatchSink *outputs,
	BatchVerified *verified
);
static bool syncPart(Batch *batch, const BatchChunk *chunk, const BatchPart *part);
static bool writePartJson(Batch *batch, const BatchChunk *chunk, const BatchPart *part);
static void writePartMembers(JsonWriter *writer, const BatchPart *part);
static void logPart(Batch *batch, const BatchChunk *chunk, const BatchPart *part);
static BatchStatus mergeParts(Commit *commit);
static BatchStatus checkReference(Commit *commit);
static BatchStatus openSinks(Commit *commit, BatchSink *sinks, FILE **files);
static bool closeSinks(Commit *commit, BatchSink *sinks, FILE **files);
static BatchStatus compressFiles(Commit *commit);
static BatchStatus describeChunk(Commit *commit);
static void writeChunkSeeds(JsonWriter *writer, const Commit *commit);
static void writeChunkParts(JsonWriter *writer, const Commit *commit);
static void writeChunkFiles(JsonWriter *writer, const Commit *commit);
static BatchStatus syncChunk(Commit *commit);
static BatchStatus renameChunk(Commit *commit);
static BatchStatus finishCommit(Commit *commit);
static bool isChainPart(const BatchChain *chain, size_t part);
static BatchStatus removeLeftovers(Batch *batch, BatchRecovery *out);
static BatchStatus removeBuildLeftovers(Batch *batch);
static BatchStatus
evaluateUnfinished(Batch *batch, BatchPartOutcome unevaluated, BatchRecovery *out);
static BatchStatus shortenParts(Batch *batch, BatchRecovery *out);
static BatchStatus removeStale(Batch *batch, BatchRecovery *out);
static BatchStatus relogCommitted(Batch *batch, BatchRecovery *out);
static const BatchCommitted *loggedCommit(const Batch *batch, const char *line, size_t length);
static BatchStatus commitFinished(Batch *batch, BatchRecovery *out);
static bool chainDone(const BatchChunk *chunk, bool *done);
static bool
appendLine(const wchar_t *path, const char *line, bool create, const BatchHost *through);
static bool nextLine(const ByteBuffer *text, size_t *at, const char **line, size_t *length);
static void formatIndexLine(const Batch *batch, const BatchCommitted *chunk, ByteBuffer *out);
static void formatUtc(time_t seconds, char *out, size_t size);
static double chunkRate(const BatchCommitted *chunk);
static void writeArms(JsonWriter *writer, const Batch *batch);
static void writeSeasons(JsonWriter *writer, const Batch *batch);
static void writeSessions(JsonWriter *writer, const Batch *batch);
static bool countSession(SessionCount **sessions, size_t *count, const char *id, size_t length);
static int compareSessionCounts(const void *a, const void *b);
static void writeSessionMembers(JsonWriter *writer, const BatchSession *session);
static void writePinned(JsonWriter *writer, const Batch *batch);
static bool scanSessions(Batch *batch);
static bool readSessionNumber(const char *name, uint32_t *number);
static bool readSession(const JsonFlat *flat, BatchSession *session);
static bool keepSession(Batch *batch, const BatchSession *session);
static void writeProgress(JsonWriter *writer, const BatchProgress *progress);
static void writeOptionalText(JsonWriter *writer, const char *key, const char *text);
static void writeTeam(JsonWriter *writer, const char *key, uint32_t team);
static bool verifyListedFile(
	const JsonFlat *flat,
	const wchar_t *folder,
	const char *name,
	FILE *report,
	const char *label
);
static bool verifyDeep(
	Batch *batch,
	const BatchCommitted *chunk,
	const JsonFlat *flat,
	const wchar_t *folder,
	FILE *report,
	const char *label
);
static int fileIndex(const JsonFlat *flat, const char *name);
static bool readChunkFiles(
	Batch *batch,
	const BatchCommitted *chunk,
	const JsonFlat *flat,
	const wchar_t *folder,
	BatchVerified *verified
);
static bool
sameContents(const JsonFlat *flat, int at, const BatchInput *input, uint64_t rows, bool results);

const char *batch_outcomeName(BatchPartOutcome outcome) {
	return outcome < BATCH_PART_OUTCOMES ? outcomeNames[outcome] : "error";
}

BatchOpenResult batch_open(Batch *batch, const wchar_t *directory, const BatchHost *host) {
	memset(batch, 0, sizeof *batch);
	batch->host = host;
	if (wcslen(directory) >= HARNESS_PATH_LENGTH ||
			!platform_toUtf8(directory, batch->directoryUtf8, sizeof batch->directoryUtf8)) {
		fprintf(stderr, "the batch folder's path is too long or not valid text\n");
		return BATCH_OPEN_FAILED;
	}
	wcscpy(batch->directory, directory);
	hostInfo_read(&batch->hostInfo);
	BatchOpenResult result = readPlan(batch);
	if (result == BATCH_OPENED) {
		result = checkInputs(batch);
	}
	if (result == BATCH_OPENED) {
		result = scanChunks(batch);
	}
	if (result == BATCH_OPENED) {
		result = scanAttempts(batch);
	}
	if (result == BATCH_OPENED && !scanSessions(batch)) {
		result = BATCH_OPEN_FAILED;
	}
	if (result == BATCH_OPENED && batch->plan.mode == BATCH_MODE_SEASON && !batch->fixturesFrozen &&
			(batch->committedCount || batch->pendingCount)) {
		fprintf(
			stderr,
			"%s: chunks have been played, but inputs/fixtures.txt, which froze their fixtures, is "
			"missing\n",
			batch->directoryUtf8
		);
		result = BATCH_OPEN_CHANGED;
	}
	if (result != BATCH_OPENED) {
		batch_close(batch);
	}
	return result;
}

void batch_close(Batch *batch) {
	batchPlan_free(&batch->plan);
	buffer_free(&batch->planText);
	free(batch->list);
	free(batch->committed);
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		free(batch->pending[i].parts);
	}
	free(batch->pending);
	free(batch->leftovers);
	free(batch->sessions);
	batch->list = NULL;
	batch->committed = NULL;
	batch->pending = NULL;
	batch->leftovers = NULL;
	batch->sessions = NULL;
	batch->committedCount = batch->pendingCount = batch->leftoverCount = 0;
	batch->committedCapacity = batch->pendingCapacity = batch->leftoverCapacity = 0;
	batch->sessionCount = batch->sessionCapacity = 0;
}

bool batch_nextChunk(const Batch *batch, size_t *arm, uint64_t *chunk) {
	uint64_t total = batchPlan_chunksPerArm(&batch->plan) * batch->plan.armCount;
	if (batch->committedCount == total) {
		return false;
	}
	for (uint64_t position = 0; position < total; ++position) {
		batchPlan_scheduled(&batch->plan, position, arm, chunk);
		if (!batch_committed(batch, *arm, *chunk)) {
			return true;
		}
	}
	return false;
}

const BatchCommitted *batch_committed(const Batch *batch, size_t arm, uint64_t chunk) {
	size_t low = 0;
	size_t high = batch->committedCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		const BatchCommitted *record = &batch->committed[middle];
		if (record->arm == arm && record->chunk == chunk) {
			return record;
		}
		if (record->arm < arm || (record->arm == arm && record->chunk < chunk)) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return NULL;
}

const BatchChunk *batch_pending(const Batch *batch, size_t arm, uint64_t chunk) {
	return findPending(batch, arm, chunk);
}

bool batchChain_compute(const BatchChunk *chunk, BatchChain *chain) {
	*chain = (BatchChain){.cursor = chunk->first};
	size_t count = chunk->partCount;
	chain->parts = malloc((count + 1) * sizeof *chain->parts);
	chain->excluded = malloc((count + 1) * sizeof *chain->excluded);
	chain->stops = malloc((count + 2) * sizeof *chain->stops);
	if (!chain->parts || !chain->excluded || !chain->stops) {
		batchChain_free(chain);
		return false;
	}
	for (;;) {
		chain->stops[chain->stopCount++] = chain->cursor;
		size_t best = count;
		uint64_t bestAdvance = 0;
		for (size_t i = 0; i < count; ++i) {
			const BatchPart *part = &chunk->parts[i];
			if (!part->evaluated || part->first != chain->cursor) {
				continue;
			}
			uint64_t advance =
				part->verified + (part->hasPoison && part->poison == part->first + part->verified);
			// The furthest, and of those the latest attempt.
			if (advance && (advance > bestAdvance ||
											 (advance == bestAdvance && part->attempt > chunk->parts[best].attempt))) {
				best = i;
				bestAdvance = advance;
			}
		}
		if (!bestAdvance || chain->cursor > chunk->last) {
			return true;
		}
		const BatchPart *part = &chunk->parts[best];
		chain->parts[chain->count++] = best;
		if (bestAdvance > part->verified) {
			chain->excluded[chain->excludedCount++] = part->poison;
		}
		chain->cursor += bestAdvance;
	}
}

void batchChain_free(BatchChain *chain) {
	free(chain->parts);
	free(chain->excluded);
	free(chain->stops);
	*chain = (BatchChain){0};
}

BatchStatus batch_beginAttempt(
	Batch *batch,
	size_t arm,
	uint64_t chunk,
	const BatchAttemptFacts *facts,
	BatchAttempt *out
) {
	if (batch_committed(batch, arm, chunk)) {
		return failed(
			"chunk %llu of arm %s is committed already",
			(unsigned long long)chunk,
			batch->plan.arms[arm].id
		);
	}
	BatchChunk *pending = findPending(batch, arm, chunk);
	pending = pending ? pending : addPending(batch, arm, chunk);
	if (!pending) {
		return failed("out of memory");
	}
	uint32_t attempt = 1;
	for (size_t i = 0; i < pending->partCount; ++i) {
		if (!pending->parts[i].evaluated) {
			return failed("attempt a%u has not been evaluated", pending->parts[i].attempt);
		}
		attempt = pending->parts[i].attempt >= attempt ? pending->parts[i].attempt + 1 : attempt;
	}
	BatchChain chain;
	if (!batchChain_compute(pending, &chain)) {
		return failed("out of memory");
	}
	uint64_t cursor = chain.cursor;
	batchChain_free(&chain);
	if (cursor > pending->last) {
		return failed(
			"chunk %llu of arm %s has no work left", (unsigned long long)chunk, batch->plan.arms[arm].id
		);
	}
	uint64_t count = pending->last - cursor + 1;
	count = facts->maxCount && facts->maxCount < count ? facts->maxCount : count;
	*out = (BatchAttempt){.attempt = attempt, .first = cursor, .count = count};
	BatchPart part = {
		.attempt = attempt,
		.first = cursor,
		.count = count,
		.threads = facts->threads,
		.startedAt = facts->startedAt,
	};
	(void)snprintf(part.session, sizeof part.session, "%s", facts->session ? facts->session : "");
	char seeds[HARNESS_JOB_LENGTH];
	BatchSeeds armSeeds = batchPlan_armSeeds(&batch->plan, arm);
	if (!batchPlan_partPaths(
				batch->directoryUtf8, batch->plan.arms[arm].id, pending->first, attempt, &out->paths
			) ||
			!batchSeeds_format(&armSeeds, cursor, cursor + count - 1, seeds, sizeof seeds) ||
			!batchPlan_formatJob(
				&batch->plan, arm, &out->paths, seeds, cursor, facts->threads, out->job, sizeof out->job
			)) {
		return failed("the job for attempt a%u is too long", attempt);
	}
	if (!writeAttemptJson(batch, pending, &part, seeds, out->job) || !addPart(pending, &part)) {
		return failed("cannot start attempt a%u", attempt);
	}
	return reached(batch, BATCH_STEP_ATTEMPT_STARTED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

BatchStatus batch_finishAttempt(
	Batch *batch,
	size_t arm,
	uint64_t chunk,
	uint32_t attempt,
	const BatchPartFacts *facts
) {
	BatchChunk *pending = findPending(batch, arm, chunk);
	BatchPart *part = pending ? findPart(pending, attempt) : NULL;
	if (!part) {
		return failed(
			"there is no attempt a%u at chunk %llu of arm %s",
			attempt,
			(unsigned long long)chunk,
			batch->plan.arms[arm].id
		);
	}
	BatchVerified verified;
	if (!evaluatePart(batch, pending, part, NULL, &verified)) {
		return failed("cannot read the files of attempt a%u", attempt);
	}
	// The rows part.json vouches for reach the disk before it does.
	if (!syncPart(batch, pending, part)) {
		return failed("cannot write the files of attempt a%u through to the disk", attempt);
	}
	BatchPart evaluated = *part;
	evaluated.evaluated = true;
	evaluated.verified = facts->discard ? 0 : verified.verified;
	if (!facts->discard) {
		memcpy(evaluated.rows, verified.rows, sizeof evaluated.rows);
	}
	evaluated.outcome = facts->outcome;
	evaluated.finishedAt = facts->finishedAt;
	evaluated.wallMs = facts->wallMs;
	evaluated.sleptMs = facts->sleptMs;
	bool crashLike = facts->outcome == BATCH_PART_CRASHED || facts->outcome == BATCH_PART_ABANDONED;
	evaluated.hasPoison = facts->hasPoison && crashLike && !facts->discard &&
												evaluated.verified < evaluated.count &&
												facts->poison == evaluated.first + evaluated.verified;
	evaluated.poison = evaluated.hasPoison ? facts->poison : 0;
	(void)snprintf(
		evaluated.clientOutcome,
		sizeof evaluated.clientOutcome,
		"%s",
		facts->clientOutcome ? facts->clientOutcome : ""
	);
	(void)snprintf(
		evaluated.resultLine,
		sizeof evaluated.resultLine,
		"%s",
		facts->resultLine ? facts->resultLine : ""
	);
	if (!writePartJson(batch, pending, &evaluated)) {
		return failed("cannot write the part.json of attempt a%u", attempt);
	}
	*part = evaluated;
	logPart(batch, pending, part);
	return reached(batch, BATCH_STEP_PART_WRITTEN) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

BatchStatus batch_commit(Batch *batch, size_t arm, uint64_t chunk, const BatchCommitFacts *facts) {
	Commit commit = {.batch = batch, .facts = facts, .pending = findPending(batch, arm, chunk)};
	char id[BATCH_CHUNK_ID_LENGTH];
	const char *armId = batch->plan.arms[arm].id;
	if (!commit.pending || batch_committed(batch, arm, chunk)) {
		return failed("chunk %llu of arm %s has no parts to commit", (unsigned long long)chunk, armId);
	}
	batchPlan_chunkId(commit.pending->first, id);
	if (!batchChain_compute(commit.pending, &commit.chain)) {
		return failed("out of memory");
	}
	BatchStatus status = BATCH_STATUS_OK;
	if (commit.chain.cursor <= commit.pending->last) {
		status = failed("chunk %s/%s has work left", armId, id);
	} else if (!pathOf(batch, commit.armFolder, "chunks/arm=%s", armId) ||
						 !pathOf(batch, commit.temporary, "chunks/arm=%s/chunk=%s.tmp", armId, id) ||
						 !pathOf(batch, commit.folder, "chunks/arm=%s/chunk=%s", armId, id)) {
		status = failed("the batch folder's path is too long");
	}
	BatchStatus (*const steps[])(Commit *) = {
		mergeParts,
		checkReference,
		compressFiles,
		describeChunk,
		syncChunk,
		renameChunk,
		finishCommit,
	};
	for (size_t i = 0; status == BATCH_STATUS_OK && i < sizeof steps / sizeof steps[0]; ++i) {
		status = steps[i](&commit);
		// A failure before the rename leaves no trace; after it, the chunk is committed.
		if (status == BATCH_STATUS_FAILED && steps[i] != finishCommit) {
			(void)removeTree(commit.temporary);
		}
	}
	batchChain_free(&commit.chain);
	buffer_free(&commit.manifest);
	return status;
}

BatchStatus batch_recover(Batch *batch, BatchPartOutcome unevaluated, BatchRecovery *out) {
	*out = (BatchRecovery){0};
	BatchStatus status = removeLeftovers(batch, out);
	status = status == BATCH_STATUS_OK ? evaluateUnfinished(batch, unevaluated, out) : status;
	status = status == BATCH_STATUS_OK ? shortenParts(batch, out) : status;
	status = status == BATCH_STATUS_OK ? removeStale(batch, out) : status;
	status = status == BATCH_STATUS_OK ? relogCommitted(batch, out) : status;
	return status == BATCH_STATUS_OK ? commitFinished(batch, out) : status;
}

BatchStatus batch_writeManifest(Batch *batch, const BatchOutcome *outcome) {
	wchar_t path[HARNESS_PATH_LENGTH];
	BatchProgress progress;
	batch_progress(batch, outcome->at, &progress);
	const BatchPlan *plan = &batch->plan;
	ByteBuffer text = {0};
	JsonWriter writer;
	jsonWriter_start(&writer, &text);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", "fm24-batch-manifest/1");
	jsonWriter_string(&writer, "name", plan->name);
	jsonWriter_string(&writer, "plan_sha256", plan->planSha256);
	jsonWriter_string(&writer, "plan", buffer_string(&batch->planText));
	jsonWriter_beginObject(&writer, "fm24ctl");
	jsonWriter_string(&writer, "protocol", HARNESS_PROTOCOL_TEXT);
	jsonWriter_string(&writer, "commit", buildInfo_commit);
	if (buildInfo_dirty < 0) {
		jsonWriter_null(&writer, "dirty");
	} else {
		jsonWriter_bool(&writer, "dirty", buildInfo_dirty != 0);
	}
	jsonWriter_string(&writer, "slice", BUILD_INFO_SLICE);
	jsonWriter_endObject(&writer);
	jsonWriter_beginObject(&writer, "host");
	writeOptionalText(&writer, "model", batch->hostInfo.model);
	writeOptionalText(&writer, "cpu", batch->hostInfo.cpu);
	jsonWriter_unsigned(&writer, "logical_cpus", batch->hostInfo.logicalCpus);
	jsonWriter_unsigned(&writer, "memory_bytes", batch->hostInfo.memoryBytes);
	writeOptionalText(&writer, "os", batch->hostInfo.os);
	writeOptionalText(&writer, "os_build", batch->hostInfo.osBuild);
	jsonWriter_endObject(&writer);
	jsonWriter_beginObject(&writer, "pinned");
	writePinned(&writer, batch);
	writeOptionalText(&writer, "save", plan->saveName);
	writeOptionalText(&writer, "game_date", plan->gameDate);
	jsonWriter_unsigned(&writer, "game_build", plan->gameBuild);
	jsonWriter_string(&writer, "slice", plan->slice == BATCH_SLICE_X86_64 ? "x86_64" : "arm64");
	jsonWriter_endObject(&writer);
	writeArms(&writer, batch);
	if (plan->mode == BATCH_MODE_SEASON) {
		writeSeasons(&writer, batch);
	}
	writeSessions(&writer, batch);
	writeProgress(&writer, &progress);
	jsonWriter_beginObject(&writer, "incidents");
	jsonWriter_unsigned(&writer, "attempts", progress.attempts);
	jsonWriter_unsigned(
		&writer,
		"retries",
		progress.attempts > progress.chunksDone ? progress.attempts - progress.chunksDone : 0
	);
	for (size_t i = BATCH_PART_STOPPED; i < BATCH_PART_OUTCOMES; ++i) {
		jsonWriter_unsigned(&writer, outcomeNames[i], progress.outcomes[i]);
	}
	jsonWriter_unsigned(&writer, "poison", progress.excluded);
	jsonWriter_unsigned(&writer, "slept_ms", progress.sleptMs);
	jsonWriter_endObject(&writer);
	jsonWriter_beginObject(&writer, "outcome");
	jsonWriter_string(&writer, "state", outcome->state);
	jsonWriter_string(&writer, "reason", outcome->reason);
	if (outcome->exitCode >= 0) {
		jsonWriter_signed(&writer, "exit_code", outcome->exitCode);
	} else {
		jsonWriter_null(&writer, "exit_code");
	}
	jsonWriter_utcTime(&writer, "at", outcome->at);
	jsonWriter_endObject(&writer);
	jsonWriter_endObject(&writer);
	bool ok = jsonWriter_finish(&writer) && buffer_append(&text, "\n", 1) &&
						pathOf(batch, path, "batch.json") && replaceText(batch, path, &text);
	buffer_free(&text);
	if (!ok) {
		return failed("cannot write batch.json");
	}
	return reached(batch, BATCH_STEP_SUMMARISED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

bool batch_writeIndex(Batch *batch) {
	ByteBuffer text = {0};
	bool ok = buffer_append(&text, indexHeader, sizeof indexHeader - 1);
	uint64_t total = batchPlan_chunksPerArm(&batch->plan) * batch->plan.armCount;
	for (uint64_t position = 0; ok && position < total && batch->committedCount; ++position) {
		size_t arm = 0;
		uint64_t chunk = 0;
		batchPlan_scheduled(&batch->plan, position, &arm, &chunk);
		const BatchCommitted *record = batch_committed(batch, arm, chunk);
		if (record) {
			size_t before = text.length;
			formatIndexLine(batch, record, &text);
			ok = text.length > before;
		}
	}
	wchar_t path[HARNESS_PATH_LENGTH];
	ok = ok && pathOf(batch, path, "index.csv") && replaceText(batch, path, &text);
	buffer_free(&text);
	return ok;
}

void batch_logEvent(Batch *batch, const char *format, ...) {
	char line[1024];
	formatUtc(batch->host->utcNow(batch->host->context), line, TIME_LENGTH);
	size_t length = strlen(line);
	line[length++] = ' ';
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(line + length, sizeof line - length - 1, format, arguments);
	va_end(arguments);
	length = written < 0 ? length : length + (size_t)written;
	length = length < sizeof line - 2 ? length : sizeof line - 2;
	line[length++] = '\n';
	line[length] = 0;
	wchar_t path[HARNESS_PATH_LENGTH];
	if (!pathOf(batch, path, "events.log") || !appendLine(path, line, true, NULL)) {
		fprintf(stderr, "cannot add to events.log: %s", line);
	}
}

bool batch_readOutcome(
	const Batch *batch,
	char *state,
	size_t stateSize,
	char *reason,
	size_t reasonSize,
	int *exitCode,
	time_t *at
) {
	wchar_t path[HARNESS_PATH_LENGTH];
	JsonFlat flat = {0};
	uint64_t code = 0;
	bool ok = pathOf(batch, path, "batch.json") && readJson(path, &flat) &&
						jsonFlat_string(&flat, "outcome.state", state, stateSize);
	if (!ok) {
		state[0] = 0;
	}
	if (!ok || !jsonFlat_string(&flat, "outcome.reason", reason, reasonSize)) {
		reason[0] = 0;
	}
	*exitCode =
		ok && jsonFlat_unsigned(&flat, "outcome.exit_code", &code) && code < 256 ? (int)code : -1;
	if (!ok || !jsonFlat_time(&flat, "outcome.at", at)) {
		*at = 0;
	}
	jsonFlat_free(&flat);
	return ok;
}

void batch_progress(const Batch *batch, time_t now, BatchProgress *out) {
	*out = (BatchProgress){0};
	const BatchPlan *plan = &batch->plan;
	out->planned = batchSeeds_count(&plan->seeds) * plan->armCount;
	out->chunks = batchPlan_chunksPerArm(plan) * plan->armCount;
	out->chunksDone = batch->committedCount;
	// The latest committed chunks, by when they committed (then by index).
	const BatchCommitted *latest[RATE_CHUNKS] = {0};
	for (size_t i = 0; i < batch->committedCount; ++i) {
		const BatchCommitted *chunk = &batch->committed[i];
		out->done += chunk->count;
		out->attempts += chunk->attempts;
		out->excluded += chunk->excluded;
		out->sleptMs += chunk->sleptMs;
		for (size_t o = 0; o < BATCH_PART_OUTCOMES; ++o) {
			out->outcomes[o] += chunk->outcomes[o];
		}
		const BatchCommitted *candidate = chunk;
		for (size_t r = 0; r < RATE_CHUNKS && candidate; ++r) {
			if (!latest[r] || candidate->committedAt >= latest[r]->committedAt) {
				const BatchCommitted *displaced = latest[r];
				latest[r] = candidate;
				candidate = displaced;
			}
		}
	}
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		const BatchChunk *chunk = &batch->pending[i];
		BatchChain chain;
		if (batchChain_compute(chunk, &chain)) {
			out->inParts += chain.cursor - chunk->first;
			batchChain_free(&chain);
		}
		for (size_t p = 0; p < chunk->partCount; ++p) {
			const BatchPart *part = &chunk->parts[p];
			++out->attempts;
			out->sleptMs += part->sleptMs;
			out->outcomes[part->outcome] += part->evaluated;
			out->excluded += part->hasPoison;
		}
	}
	uint64_t indices = 0;
	uint64_t awakeMs = 0;
	for (size_t r = 0; r < RATE_CHUNKS && latest[r]; ++r) {
		indices += latest[r]->count;
		awakeMs += latest[r]->wallMs > latest[r]->sleptMs ? latest[r]->wallMs - latest[r]->sleptMs : 0;
		out->rateChunks = r + 1;
	}
	if (awakeMs) {
		out->rate = (double)indices * 1000.0 / (double)awakeMs;
		double left = (double)(out->planned - out->done - out->inParts);
		out->hasEta = true;
		out->eta = now + (time_t)(left / out->rate + 0.5);
	}
}

void batch_progressLine(
	const Batch *batch,
	const BatchCommitted *chunk,
	time_t now,
	char *out,
	size_t size
) {
	BatchProgress progress;
	batch_progress(batch, now, &progress);
	char id[BATCH_CHUNK_ID_LENGTH];
	char done[32];
	char planned[32];
	char rate[64] = "rate unknown";
	char eta[48] = "";
	batchPlan_chunkId(chunk->first, id);
	batch_formatCount(progress.done, done, sizeof done);
	batch_formatCount(progress.planned, planned, sizeof planned);
	if (progress.rate > 0) {
		(void)snprintf(
			rate,
			sizeof rate,
			"%.2f/s over %zu chunk%s",
			progress.rate,
			progress.rateChunks,
			progress.rateChunks == 1 ? "" : "s"
		);
	}
	if (progress.hasEta) {
		char at[TIME_LENGTH];
		formatUtc(progress.eta, at, sizeof at);
		(void)snprintf(eta, sizeof eta, "; ETA %.10s %.5s UTC", at, at + 11);
	}
	uint64_t awakeMs = chunk->wallMs > chunk->sleptMs ? chunk->wallMs - chunk->sleptMs : 0;
	char attempts[32] = "";
	if (chunk->attempts > 1) {
		(void)snprintf(attempts, sizeof attempts, ", %u attempts", chunk->attempts);
	}
	(void)snprintf(
		out,
		size,
		"chunk %s/%s ok %llu in %.1f s (%.2f/s%s); %s/%s (%.1f%%); %s%s",
		batch->plan.arms[chunk->arm].id,
		id,
		(unsigned long long)chunk->count,
		(double)awakeMs / 1000.0,
		chunkRate(chunk),
		attempts,
		done,
		planned,
		progress.planned ? 100.0 * (double)progress.done / (double)progress.planned : 0.0,
		rate,
		eta
	);
}

void batch_armTotals(const Batch *batch, size_t arm, BatchArmTotals *out) {
	*out = (BatchArmTotals){0};
	// The committed chunks are ordered by arm: the arm's start from the first not before it.
	size_t low = 0;
	size_t high = batch->committedCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (batch->committed[middle].arm < arm) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	for (size_t i = low; i < batch->committedCount && batch->committed[i].arm == arm; ++i) {
		const BatchCommitted *chunk = &batch->committed[i];
		++out->chunks;
		out->covered += chunk->count;
		out->excluded += chunk->excluded;
		for (size_t role = 0; role < BATCH_ROLES; ++role) {
			out->rows[role] += chunk->rows[role];
		}
		out->summary.matches += chunk->summary.matches;
		out->summary.home += chunk->summary.home;
		out->summary.draw += chunk->summary.draw;
		out->summary.away += chunk->summary.away;
		out->summary.homeGoals += chunk->summary.homeGoals;
		out->summary.awayGoals += chunk->summary.awayGoals;
		out->summary.unfinished += chunk->summary.unfinished;
	}
}

void batch_seasons(const Batch *batch, BatchSeasons *out) {
	const BatchPlan *plan = &batch->plan;
	*out = (BatchSeasons){
		.blocks = batchPlan_chunksPerArm(plan), .seasons = batchSeeds_count(&plan->seeds)};
	bool leading = true;
	for (uint64_t block = 0; block < out->blocks; ++block) {
		size_t done = 0;
		uint64_t missing = 0;
		for (size_t arm = 0; arm < plan->armCount; ++arm) {
			const BatchCommitted *chunk = batch_committed(batch, arm, block);
			done += chunk != NULL;
			missing += chunk ? chunk->rows[BATCH_FAILED] + chunk->excluded : 0;
		}
		uint64_t first = 0;
		uint64_t last = 0;
		batchPlan_chunkRange(plan, block, &first, &last);
		if (done < plan->armCount) {
			if (!out->hasCurrent) {
				out->hasCurrent = true;
				out->current = block;
				out->currentDone = done;
			}
			leading = false;
			continue;
		}
		++out->blocksComplete;
		out->seasonsComplete += last - first + 1;
		out->matchesMissing += missing;
		if (leading) {
			out->completeFirst = out->completeCount ? out->completeFirst : first;
			out->completeCount += last - first + 1;
		}
	}
}

bool batch_blockComplete(const Batch *batch, uint64_t chunk) {
	for (size_t arm = 0; arm < batch->plan.armCount; ++arm) {
		if (!batch_committed(batch, arm, chunk)) {
			return false;
		}
	}
	return true;
}

bool batch_fixtureBuilt(const Batch *batch, size_t arm) {
	wchar_t path[HARNESS_PATH_LENGTH];
	bool gzip = batch->plan.compress == BATCH_COMPRESS_GZIP;
	FILE *file = pathOf(batch, path, "inputs/%s.json%s", batch->plan.arms[arm].id, gzip ? ".gz" : "")
								 ? platform_openFile(path, "rb")
								 : NULL;
	if (file) {
		(void)fclose(file);
	}
	return file != NULL;
}

BatchStatus batch_keepBuilt(Batch *batch, const char *stem, bool compress, const ByteBuffer *bin) {
	const BatchHost *host = batch->host;
	wchar_t path[HARNESS_PATH_LENGTH];
	wchar_t temporary[HARNESS_PATH_LENGTH];
	wchar_t folder[HARNESS_PATH_LENGTH];
	if (!pathOf(batch, path, "%s.bin", stem) ||
			!batch_writeFile(host, path, bin->data, bin->length) ||
			!pathOf(batch, temporary, "%s.tmp.json", stem) ||
			(compress && !host->compress(host->context, temporary)) ||
			!pathOf(batch, temporary, "%s.tmp.json%s", stem, compress ? ".gz" : "") ||
			!pathOf(batch, path, "%s.json%s", stem, compress ? ".gz" : "")) {
		return failed("cannot keep %s", stem);
	}
	FILE *file = platform_openFile(temporary, "ab");
	bool ok = file && host->syncFile(host->context, file);
	ok = file && fclose(file) == 0 && ok;
	folderOf(path, folder);
	if (!ok || !platform_replaceFile(temporary, path) ||
			!host->syncDirectory(host->context, folder)) {
		return failed("cannot keep %s", stem);
	}
	return reached(batch, BATCH_STEP_FIXTURE_BUILT) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

BatchStatus batch_freezeFixtures(Batch *batch) {
	BatchPlan *plan = &batch->plan;
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		BatchArm *fixture = &plan->arms[arm];
		wchar_t path[HARNESS_PATH_LENGTH];
		ByteBuffer json = {0};
		ConfigHeader header;
		uint64_t bytes = 0;
		bool ok = pathOf(batch, path, "inputs/%s.bin", fixture->id) &&
							hashFile(path, &bytes, fixture->binSha256) && readFixtureJson(batch, arm, &json) &&
							matchConfig_readHeader(buffer_string(&json), json.length, &header);
		if (ok) {
			sha256_hexOf(json.data, json.length, fixture->jsonSha256);
			fixture->teams[0] = header.teams[0];
			fixture->teams[1] = header.teams[1];
		}
		buffer_free(&json);
		if (!ok) {
			return failed("cannot read the files of fixture %s", fixture->id);
		}
	}
	ByteBuffer text = {0};
	wchar_t path[HARNESS_PATH_LENGTH];
	bool ok = batchSeason_formatFixtures(plan, &text, batch->fixturesSha256) &&
						pathOf(batch, path, "inputs/fixtures.txt") &&
						batch_writeFile(batch->host, path, text.data, text.length);
	buffer_free(&text);
	if (!ok) {
		return failed("cannot write inputs/fixtures.txt");
	}
	batch->fixturesFrozen = true;
	batch->fixturesBuilt = plan->armCount;
	if (!reached(batch, BATCH_STEP_FIXTURES_FROZEN)) {
		return BATCH_STATUS_KILLED;
	}
	return removeBuildLeftovers(batch);
}

bool batch_verifyFixtures(Batch *batch, FILE *report) {
	const BatchPlan *plan = &batch->plan;
	bool gzip = plan->compress == BATCH_COMPRESS_GZIP;
	size_t problems = 0;
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		const BatchArm *fixture = &plan->arms[arm];
		wchar_t path[HARNESS_PATH_LENGTH];
		char hash[SHA256_HEX_LENGTH] = "";
		uint64_t bytes = 0;
		bool bin = pathOf(batch, path, "inputs/%s.bin", fixture->id) && hashFile(path, &bytes, hash) &&
							 strcmp(hash, fixture->binSha256) == 0;
		ByteBuffer json = {0};
		bool read = readFixtureJson(batch, arm, &json);
		if (read) {
			sha256_hexOf(json.data, json.length, hash);
		}
		buffer_free(&json);
		bool same = read && strcmp(hash, fixture->jsonSha256) == 0;
		if (!bin || !same) {
			fprintf(
				report,
				"fixture %s: inputs/%s%s does not match inputs/fixtures.txt\n",
				fixture->id,
				fixture->id,
				!bin   ? ".bin"
				: gzip ? ".json.gz"
							 : ".json"
			);
			++problems;
		}
	}
	return !problems;
}

// A season's fixture JSON, decompressed if it is kept compressed.
static bool readFixtureJson(Batch *batch, size_t arm, ByteBuffer *out) {
	const BatchHost *host = batch->host;
	bool gzip = batch->plan.compress == BATCH_COMPRESS_GZIP;
	wchar_t path[HARNESS_PATH_LENGTH];
	if (!pathOf(batch, path, "inputs/%s.json%s", batch->plan.arms[arm].id, gzip ? ".gz" : "")) {
		return false;
	}
	if (!gzip) {
		return files_read(path, out);
	}
	void *handle = NULL;
	FILE *file = host->openDecompressed(host->context, path, &handle);
	if (!file) {
		return false;
	}
	uint8_t block[1 << 14];
	bool ok = true;
	for (size_t got = 1; ok && got;) {
		got = fread(block, 1, sizeof block, file);
		ok = !ferror(file) && buffer_append(out, block, got);
	}
	return host->closeDecompressed(host->context, file, handle) && ok;
}

bool batch_removeFolder(const wchar_t *path) {
	return removeTree(path);
}

void batch_formatCount(uint64_t value, char *out, size_t size) {
	char digits[24];
	int length = snprintf(digits, sizeof digits, "%llu", (unsigned long long)value);
	size_t written = 0;
	for (int i = 0; i < length && written + 2 < size; ++i) {
		if (i && (length - i) % 3 == 0) {
			out[written++] = ',';
		}
		out[written++] = digits[i];
	}
	out[written] = 0;
}

bool batch_verifyChunk(Batch *batch, const BatchCommitted *chunk, bool deep, FILE *report) {
	const BatchPlan *plan = &batch->plan;
	char id[BATCH_CHUNK_ID_LENGTH];
	char label[BATCH_ARM_LENGTH + BATCH_CHUNK_ID_LENGTH + 8];
	batchPlan_chunkId(chunk->first, id);
	(void)snprintf(label, sizeof label, "%s/%s", plan->arms[chunk->arm].id, id);
	wchar_t folder[HARNESS_PATH_LENGTH];
	wchar_t path[HARNESS_PATH_LENGTH];
	JsonFlat flat = {0};
	if (!chunk->readable ||
			!pathOf(batch, folder, "chunks/arm=%s/chunk=%s", plan->arms[chunk->arm].id, id) ||
			!joinName(path, folder, "chunk.json") || !readJson(path, &flat)) {
		fprintf(report, "chunk %s: its chunk.json cannot be read\n", label);
		jsonFlat_free(&flat);
		return false;
	}
	bool expected[BATCH_ROLES] = {
		true, plan->perMatch != 0, plan->perMatch != 0, plan->seedErrors == BATCH_SEED_ERRORS_SKIP};
	bool ok = true;
	size_t listed = 0;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		char name[32];
		(void)snprintf(
			name,
			sizeof name,
			"%s%s",
			chunkFiles[role],
			plan->compress == BATCH_COMPRESS_GZIP ? ".gz" : ""
		);
		listed += expected[role];
		if (expected[role]) {
			ok = verifyListedFile(&flat, folder, name, report, label) && ok;
		} else if (fileIndex(&flat, name) >= 0) {
			fprintf(report, "chunk %s: lists %s, which this plan does not write\n", label, name);
			ok = false;
		}
	}
	Listing entries;
	if (listFolder(folder, &entries)) {
		if (entries.count != listed + 1) {
			fprintf(report, "chunk %s: has %zu files, not %zu\n", label, entries.count, listed + 1);
			ok = false;
		}
		free(entries.entries);
	}
	if (chunk->rows[BATCH_RESULTS] + chunk->rows[BATCH_FAILED] + chunk->excluded != chunk->count) {
		fprintf(
			report,
			"chunk %s: its rows and exclusions do not add up to its %llu indices\n",
			label,
			(unsigned long long)chunk->count
		);
		ok = false;
	}
	if (ok && deep) {
		ok = verifyDeep(batch, chunk, &flat, folder, report, label);
	}
	jsonFlat_free(&flat);
	return ok;
}

static bool reached(const Batch *batch, BatchStep step) {
	return !batch->host->reached || batch->host->reached(batch->host->context, step);
}

static BatchStatus failed(const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
	fputc('\n', stderr);
	return BATCH_STATUS_FAILED;
}

// The batch's folder, then a path inside it, ASCII with '/' between its parts.
static bool pathOf(const Batch *batch, wchar_t *out, const char *format, ...) {
	char relative[RELATIVE_LENGTH];
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(relative, sizeof relative, format, arguments);
	va_end(arguments);
	size_t base = wcslen(batch->directory);
	if (written <= 0 || (size_t)written >= sizeof relative ||
			base + 1 + (size_t)written >= HARNESS_PATH_LENGTH) {
		return false;
	}
	wmemcpy(out, batch->directory, base);
	out[base] = PLATFORM_PATH_SEPARATOR;
	for (int i = 0; i <= written; ++i) {
		char c = relative[i];
		out[base + 1 + (size_t)i] = c == '/' ? PLATFORM_PATH_SEPARATOR : (wchar_t)(unsigned char)c;
	}
	return true;
}

// `folder`, the separator and an ASCII `name`.
static bool joinName(wchar_t *out, const wchar_t *folder, const char *name) {
	wchar_t wide[NAME_LENGTH];
	size_t length = strlen(name);
	if (length >= NAME_LENGTH) {
		return false;
	}
	for (size_t i = 0; i <= length; ++i) {
		wide[i] = (wchar_t)(unsigned char)name[i];
	}
	return joinWide(out, folder, wide);
}

// Not with swprintf, which fails on macOS for characters the C locale cannot encode.
static bool joinWide(wchar_t *out, const wchar_t *folder, const wchar_t *name) {
	size_t base = wcslen(folder);
	size_t length = wcslen(name);
	if (base + 1 + length >= HARNESS_PATH_LENGTH) {
		return false;
	}
	wmemcpy(out, folder, base);
	out[base] = PLATFORM_PATH_SEPARATOR;
	wmemcpy(out + base + 1, name, length + 1);
	return true;
}

static void folderOf(const wchar_t *path, wchar_t *out) {
	const wchar_t *name = wcsrchr(path, PLATFORM_PATH_SEPARATOR);
	size_t length = name ? (size_t)(name - path) : 0;
	length = name == path ? 1 : length;
	wmemcpy(out, path, length);
	out[length] = 0;
}

static bool listEntry(void *context, const wchar_t *name, bool isDirectory) {
	Listing *listing = context;
	if (wcslen(name) >= NAME_LENGTH) {
		return true; // not one of the batch's
	}
	if (listing->count == listing->capacity) {
		size_t capacity = listing->capacity ? listing->capacity * 2 : 16;
		Entry *entries = realloc(listing->entries, capacity * sizeof *entries);
		if (!entries) {
			listing->failed = true;
			return false;
		}
		listing->entries = entries;
		listing->capacity = capacity;
	}
	Entry *entry = &listing->entries[listing->count++];
	wcscpy(entry->name, name);
	entry->isDirectory = isDirectory;
	return true;
}

static bool listFolder(const wchar_t *path, Listing *listing) {
	*listing = (Listing){0};
	bool ok = platform_listDirectory(path, listEntry, listing) && !listing->failed;
	if (!ok) {
		free(listing->entries);
		*listing = (Listing){0};
	}
	return ok;
}

static bool removeTree(const wchar_t *path) {
	Listing listing;
	if (!listFolder(path, &listing)) {
		return false;
	}
	bool ok = true;
	for (size_t i = 0; i < listing.count; ++i) {
		wchar_t child[HARNESS_PATH_LENGTH];
		const Entry *entry = &listing.entries[i];
		ok = joinWide(child, path, entry->name) &&
				 (entry->isDirectory ? removeTree(child) : platform_removeFile(child)) && ok;
	}
	free(listing.entries);
	return ok && platform_removeDirectory(path);
}

static bool replaceText(const Batch *batch, const wchar_t *path, const ByteBuffer *text) {
	return batch_writeFile(batch->host, path, text->data, text->length);
}

bool batch_writeFile(const BatchHost *host, const wchar_t *path, const void *bytes, size_t size) {
	wchar_t temporary[HARNESS_PATH_LENGTH];
	wchar_t folder[HARNESS_PATH_LENGTH];
	size_t length = wcslen(path);
	if (length + 4 >= HARNESS_PATH_LENGTH) {
		return false;
	}
	wmemcpy(temporary, path, length);
	wmemcpy(temporary + length, L".tmp", 5);
	folderOf(path, folder);
	FILE *file = platform_openFile(temporary, "wb");
	bool ok = file && fwrite(bytes, 1, size, file) == size && host->syncFile(host->context, file);
	ok = file && fclose(file) == 0 && ok;
	ok = ok && platform_replaceFile(temporary, path) && host->syncDirectory(host->context, folder);
	if (!ok) {
		(void)platform_removeFile(temporary);
	}
	return ok;
}

static bool hashFile(const wchar_t *path, uint64_t *bytes, char hex[SHA256_HEX_LENGTH]) {
	FILE *file = platform_openFile(path, "rb");
	if (!file) {
		return false;
	}
	bool ok = hashStream(file, bytes, hex);
	return fclose(file) == 0 && ok;
}

// A stream's SHA-256 and length, read to its end.
static bool hashStream(FILE *file, uint64_t *bytes, char hex[SHA256_HEX_LENGTH]) {
	static uint8_t block[1 << 16];
	Sha256 hash;
	sha256_start(&hash);
	*bytes = 0;
	size_t got = 0;
	while ((got = fread(block, 1, sizeof block, file)) > 0) {
		sha256_add(&hash, block, got);
		*bytes += got;
	}
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_finish(&hash, digest);
	sha256_hex(digest, hex);
	return !ferror(file);
}

static bool readJson(const wchar_t *path, JsonFlat *flat) {
	ByteBuffer text = {0};
	bool ok = files_read(path, &text) && jsonFlat_read(flat, buffer_string(&text), text.length);
	buffer_free(&text);
	return ok;
}

static bool entryName(const Entry *entry, char *out, size_t size) {
	return platform_toUtf8(entry->name, out, size);
}

// A chunk's ID, ten digits, at the start of `text`; `rest` is what follows.
static bool readChunkId(const char *text, uint64_t *first, const char **rest) {
	*first = 0;
	for (size_t i = 0; i < BATCH_CHUNK_ID_LENGTH - 1; ++i) {
		if (text[i] < '0' || text[i] > '9') {
			return false;
		}
		*first = *first * 10 + (uint64_t)(text[i] - '0');
	}
	*rest = text + BATCH_CHUNK_ID_LENGTH - 1;
	return true;
}

// The arm with that ID, or the arm count if none.
static size_t armNamed(const Batch *batch, const char *id) {
	size_t arm = 0;
	while (arm < batch->plan.armCount && strcmp(batch->plan.arms[arm].id, id) != 0) {
		++arm;
	}
	return arm;
}

static BatchOpenResult readPlan(Batch *batch) {
	wchar_t path[HARNESS_PATH_LENGTH];
	char error[512];
	BatchDefaults defaults = {1, BATCH_COMPRESS_NONE};
	if (!pathOf(batch, path, "plan.txt") || !files_read(path, &batch->planText) ||
			!batchPlan_isFrozen(buffer_string(&batch->planText), batch->planText.length)) {
		fprintf(
			stderr,
			"%s is not a batch: it has no plan.txt that batch create froze\n",
			batch->directoryUtf8
		);
		return BATCH_OPEN_FAILED;
	}
	if (!batchPlan_parse(
				buffer_string(&batch->planText),
				batch->planText.length,
				true,
				&defaults,
				&batch->plan,
				error,
				sizeof error
			)) {
		fprintf(stderr, "%s: plan.txt: %s\n", batch->directoryUtf8, error);
		return BATCH_OPEN_CHANGED;
	}
	return BATCH_OPENED;
}

// The fixtures, their encodings and a list's seeds, against the hashes plan.txt froze.
static BatchOpenResult checkInputs(Batch *batch) {
	BatchPlan *plan = &batch->plan;
	char name[BATCH_ARM_LENGTH + 16];
	if (plan->mode == BATCH_MODE_SEASON) {
		return checkSeasonInputs(batch);
	}
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		const BatchArm *entry = &plan->arms[arm];
		(void)snprintf(name, sizeof name, "%s.json", entry->id);
		BatchOpenResult result = checkInput(batch, name, entry->jsonSha256, NULL);
		(void)snprintf(name, sizeof name, "%s.bin", entry->id);
		result = result == BATCH_OPENED ? checkInput(batch, name, entry->binSha256, NULL) : result;
		if (result != BATCH_OPENED) {
			return result;
		}
	}
	if (plan->seeds.kind != BATCH_SEEDS_LIST) {
		return BATCH_OPENED;
	}
	ByteBuffer text = {0};
	BatchOpenResult result = checkInput(batch, "seeds.txt", plan->listSha256, &text);
	size_t count = 0;
	char error[256];
	if (result == BATCH_OPENED &&
			!batchSeeds_readList(
				buffer_string(&text), text.length, &batch->list, &count, error, sizeof error
			)) {
		fprintf(stderr, "%s: inputs/seeds.txt: %s\n", batch->directoryUtf8, error);
		result = BATCH_OPEN_CHANGED;
	}
	buffer_free(&text);
	plan->seeds.list = batch->list;
	plan->seeds.first = 0;
	plan->seeds.last = count ? count - 1 : 0;
	return result;
}

// A season's base against plan.txt, then its fixtures, once inputs/fixtures.txt has frozen them:
// that file against the plan, and each fixture's encoding against it (verify checks the JSONs).
static BatchOpenResult checkSeasonInputs(Batch *batch) {
	BatchPlan *plan = &batch->plan;
	BatchOpenResult result = checkInput(batch, "base.json", plan->base.jsonSha256, NULL);
	result =
		result == BATCH_OPENED ? checkInput(batch, "base.bin", plan->base.binSha256, NULL) : result;
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer text = {0};
	if (result != BATCH_OPENED || !pathOf(batch, path, "inputs/fixtures.txt") ||
			!files_read(path, &text)) {
		batch->fixturesBuilt = result == BATCH_OPENED ? countBuilt(batch) : 0;
		return result;
	}
	char error[256];
	bool read = batchSeason_readFixtures(
		buffer_string(&text), text.length, plan, batch->fixturesSha256, error, sizeof error
	);
	buffer_free(&text);
	if (!read) {
		fprintf(stderr, "%s: inputs/fixtures.txt: %s\n", batch->directoryUtf8, error);
		return BATCH_OPEN_CHANGED;
	}
	batch->fixturesFrozen = true;
	batch->fixturesBuilt = plan->armCount;
	for (size_t arm = 0; result == BATCH_OPENED && arm < plan->armCount; ++arm) {
		char name[BATCH_ARM_LENGTH + 16];
		(void)snprintf(name, sizeof name, "%s.bin", plan->arms[arm].id);
		result = checkInput(batch, name, plan->arms[arm].binSha256, NULL);
	}
	return result;
}

// The fixtures in inputs/ whose JSON is there: inputs/f<number>.json(.gz).
static size_t countBuilt(const Batch *batch) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	Listing entries;
	if (!pathOf(batch, folder, "inputs") || !listFolder(folder, &entries)) {
		return 0;
	}
	const char *suffix = batch->plan.compress == BATCH_COMPRESS_GZIP ? ".json.gz" : ".json";
	size_t built = 0;
	for (size_t i = 0; i < entries.count; ++i) {
		char name[NAME_LENGTH * 4];
		size_t number = 0;
		if (entries.entries[i].isDirectory || !entryName(&entries.entries[i], name, sizeof name) ||
				name[0] != 'f' || strspn(name + 1, "0123456789") != 4 || strcmp(name + 5, suffix) != 0) {
			continue;
		}
		for (size_t digit = 1; digit < 5; ++digit) {
			number = number * 10 + (size_t)(name[digit] - '0');
		}
		built += number >= 1 && number <= batch->plan.armCount;
	}
	free(entries.entries);
	return built;
}

static BatchOpenResult
checkInput(Batch *batch, const char *name, const char *expected, ByteBuffer *keep) {
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer bytes = {0};
	char actual[SHA256_HEX_LENGTH] = "";
	bool read = pathOf(batch, path, "inputs/%s", name) && files_read(path, &bytes);
	if (read) {
		sha256_hexOf(buffer_string(&bytes), bytes.length, actual);
	}
	if (keep && read) {
		*keep = bytes;
	} else {
		buffer_free(&bytes);
	}
	if (!read || strcmp(actual, expected) != 0) {
		fprintf(
			stderr,
			"%s: inputs/%s %s since the batch was created\n",
			batch->directoryUtf8,
			name,
			read ? "has been changed" : "is missing"
		);
		return BATCH_OPEN_CHANGED;
	}
	return BATCH_OPENED;
}

static BatchOpenResult scanChunks(Batch *batch) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	Listing arms;
	if (!pathOf(batch, folder, "chunks") || !listFolder(folder, &arms)) {
		return BATCH_OPEN_FAILED;
	}
	BatchOpenResult result = BATCH_OPENED;
	for (size_t i = 0; result == BATCH_OPENED && i < arms.count; ++i) {
		char name[NAME_LENGTH * 4];
		wchar_t armFolder[HARNESS_PATH_LENGTH];
		if (!arms.entries[i].isDirectory || !entryName(&arms.entries[i], name, sizeof name) ||
				strncmp(name, "arm=", 4) != 0) {
			continue;
		}
		size_t arm = armNamed(batch, name + 4);
		if (arm < batch->plan.armCount && joinWide(armFolder, folder, arms.entries[i].name)) {
			result = scanArmChunks(batch, arm, armFolder);
		}
	}
	free(arms.entries);
	if (batch->committedCount) {
		qsort(batch->committed, batch->committedCount, sizeof *batch->committed, compareCommitted);
	}
	return result;
}

static BatchOpenResult scanArmChunks(Batch *batch, size_t arm, const wchar_t *folder) {
	Listing chunks;
	if (!listFolder(folder, &chunks)) {
		return BATCH_OPEN_FAILED;
	}
	BatchOpenResult result = BATCH_OPENED;
	for (size_t i = 0; result == BATCH_OPENED && i < chunks.count; ++i) {
		char name[NAME_LENGTH * 4];
		wchar_t path[HARNESS_PATH_LENGTH];
		uint64_t first = 0;
		const char *rest = NULL;
		if (!chunks.entries[i].isDirectory || !entryName(&chunks.entries[i], name, sizeof name) ||
				strncmp(name, "chunk=", 6) != 0 || !readChunkId(name + 6, &first, &rest) ||
				!joinWide(path, folder, chunks.entries[i].name)) {
			continue;
		}
		if (strcmp(rest, ".tmp") == 0) {
			result = addLeftover(batch, path, true) ? BATCH_OPENED : BATCH_OPEN_FAILED;
		} else if (!*rest) {
			result = readCommittedFolder(batch, arm, first, path);
		}
	}
	free(chunks.entries);
	return result;
}

// A chunk=<first> folder: committed if it has chunk.json, else left by a stopped process.
static BatchOpenResult
readCommittedFolder(Batch *batch, size_t arm, uint64_t first, const wchar_t *folder) {
	const BatchPlan *plan = &batch->plan;
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer text = {0};
	uint64_t chunk = 0;
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(first, id);
	if (!batchPlan_chunkAt(plan, first, &chunk)) {
		fprintf(
			stderr,
			"%s: chunks/arm=%s/chunk=%s is not a chunk of this plan\n",
			batch->directoryUtf8,
			plan->arms[arm].id,
			id
		);
		return BATCH_OPEN_CHANGED;
	}
	if (!joinName(path, folder, "chunk.json") || !files_read(path, &text)) {
		buffer_free(&text);
		return addLeftover(batch, folder, true) ? BATCH_OPENED : BATCH_OPEN_FAILED;
	}
	JsonFlat flat = {0};
	uint64_t last = 0;
	BatchCommitted record = {.arm = arm, .chunk = chunk, .first = first};
	batchPlan_chunkRange(plan, chunk, &first, &last);
	record.count = last - first + 1;
	char planHash[SHA256_HEX_LENGTH] = "";
	record.readable =
		jsonFlat_read(&flat, buffer_string(&text), text.length) && readCommitted(batch, &flat, &record);
	(void)jsonFlat_string(&flat, "plan_sha256", planHash, sizeof planHash);
	jsonFlat_free(&flat);
	buffer_free(&text);
	if (record.readable && strcmp(planHash, plan->planSha256) != 0) {
		fprintf(
			stderr,
			"%s: chunk %s/%s was committed under another plan\n",
			batch->directoryUtf8,
			plan->arms[arm].id,
			id
		);
		return BATCH_OPEN_CHANGED;
	}
	if (!record.readable && !batch->problem[0]) {
		(void)snprintf(
			batch->problem,
			sizeof batch->problem,
			"the chunk.json of %s/%s cannot be read",
			plan->arms[arm].id,
			id
		);
	}
	return addCommitted(batch, &record) ? BATCH_OPENED : BATCH_OPEN_FAILED;
}

// A committed chunk's record from its chunk.json, which must be of this chunk.
static bool readCommitted(const Batch *batch, const JsonFlat *flat, BatchCommitted *record) {
	char format[32];
	char arm[BATCH_ARM_LENGTH];
	uint64_t first = 0;
	uint64_t count = 0;
	bool ok = jsonFlat_string(flat, "format", format, sizeof format) &&
						strcmp(format, chunkFormat) == 0 && jsonFlat_string(flat, "arm", arm, sizeof arm) &&
						strcmp(arm, batch->plan.arms[record->arm].id) == 0 &&
						jsonFlat_unsigned(flat, "seeds.first_index", &first) && first == record->first &&
						jsonFlat_unsigned(flat, "seeds.count", &count) && count == record->count &&
						jsonFlat_time(flat, "committed_at", &record->committedAt);
	static const char *const rowKeys[BATCH_ROLES] = {
		"rows.results", "rows.match_players", "rows.match_teams", "rows.failed"};
	for (size_t role = 0; ok && role < BATCH_ROLES; ++role) {
		ok = jsonFlat_unsigned(flat, rowKeys[role], &record->rows[role]);
	}
	static const char *const summaryKeys[] = {"summary.home",
		"summary.draw",
		"summary.away",
		"summary.home_goals",
		"summary.away_goals",
		"summary.unfinished"};
	uint64_t *summary[] = {&record->summary.home,
		&record->summary.draw,
		&record->summary.away,
		&record->summary.homeGoals,
		&record->summary.awayGoals,
		&record->summary.unfinished};
	for (size_t i = 0; ok && i < sizeof summaryKeys / sizeof summaryKeys[0]; ++i) {
		ok = jsonFlat_unsigned(flat, summaryKeys[i], summary[i]);
	}
	record->summary.matches = record->rows[BATCH_RESULTS];
	bool match = false;
	record->canaryMatch = jsonFlat_bool(flat, "checks.canary_reference_match", &match) ? match : -1;
	for (record->excluded = 0;; ++record->excluded) {
		char key[KEY_LENGTH];
		uint64_t index = 0;
		(void)snprintf(key, sizeof key, "excluded.%llu.index", (unsigned long long)record->excluded);
		if (!jsonFlat_unsigned(flat, key, &index)) {
			break;
		}
	}
	readCommittedAttempts(flat, "parts", record);
	readCommittedAttempts(flat, "unused_attempts", record);
	for (size_t i = 0; ok; ++i) {
		char key[KEY_LENGTH];
		char name[32];
		(void)snprintf(key, sizeof key, "files.%zu.name", i);
		if (!jsonFlat_string(flat, key, name, sizeof name)) {
			break;
		}
		for (size_t role = 0; role < BATCH_ROLES; ++role) {
			if (strncmp(name, chunkFiles[role], strlen(chunkFiles[role])) == 0) {
				(void)snprintf(key, sizeof key, "files.%zu.raw_sha256", i);
				ok = jsonFlat_string(flat, key, record->sha256[role], sizeof record->sha256[role]);
			}
		}
		uint64_t rawBytes = 0;
		(void)snprintf(key, sizeof key, "files.%zu.raw_bytes", i);
		record->rawBytes += jsonFlat_unsigned(flat, key, &rawBytes) ? rawBytes : 0;
	}
	return ok && record->attempts > 0;
}

// A chunk.json's attempts: its parts, then those that did not advance.
static void readCommittedAttempts(const JsonFlat *flat, const char *array, BatchCommitted *record) {
	for (size_t i = 0;; ++i) {
		char key[KEY_LENGTH];
		char text[BATCH_SESSION_LENGTH];
		uint64_t value = 0;
		(void)snprintf(key, sizeof key, "%s.%zu.attempt", array, i);
		if (!jsonFlat_unsigned(flat, key, &value)) {
			return;
		}
		++record->attempts;
		(void)snprintf(key, sizeof key, "%s.%zu.outcome", array, i);
		size_t outcome = 0;
		if (jsonFlat_string(flat, key, text, sizeof text)) {
			while (outcome < BATCH_PART_OUTCOMES && strcmp(outcomeNames[outcome], text) != 0) {
				++outcome;
			}
		}
		record->outcomes[outcome < BATCH_PART_OUTCOMES ? outcome : BATCH_PART_ERROR] += 1;
		(void)snprintf(key, sizeof key, "%s.%zu.wall_ms", array, i);
		record->wallMs += jsonFlat_unsigned(flat, key, &value) ? value : 0;
		(void)snprintf(key, sizeof key, "%s.%zu.slept_ms", array, i);
		record->sleptMs += jsonFlat_unsigned(flat, key, &value) ? value : 0;
		(void)snprintf(key, sizeof key, "%s.%zu.session", array, i);
		if (jsonFlat_string(flat, key, text, sizeof text) && text[0]) {
			addSession(record->sessions, sizeof record->sessions, text);
		}
	}
}

// Adds a session to a ';'-separated list, once.
static void addSession(char *sessions, size_t size, const char *session) {
	size_t length = strlen(session);
	for (const char *at = sessions; *at;) {
		size_t part = strcspn(at, ";");
		if (part == length && strncmp(at, session, length) == 0) {
			return;
		}
		at += part + (at[part] == ';');
	}
	size_t used = strlen(sessions);
	(void)snprintf(sessions + used, size - used, "%s%s", used ? ";" : "", session);
}

static BatchOpenResult scanAttempts(Batch *batch) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	Listing arms;
	if (!pathOf(batch, folder, "attempts") || !listFolder(folder, &arms)) {
		return BATCH_OPEN_FAILED;
	}
	bool ok = true;
	for (size_t i = 0; ok && i < arms.count; ++i) {
		char name[NAME_LENGTH * 4];
		wchar_t armFolder[HARNESS_PATH_LENGTH];
		Listing chunks = {0};
		size_t arm = arms.entries[i].isDirectory && entryName(&arms.entries[i], name, sizeof name)
									 ? armNamed(batch, name)
									 : batch->plan.armCount;
		if (arm == batch->plan.armCount || !joinWide(armFolder, folder, arms.entries[i].name) ||
				!listFolder(armFolder, &chunks)) {
			continue;
		}
		for (size_t c = 0; ok && c < chunks.count; ++c) {
			wchar_t chunkFolder[HARNESS_PATH_LENGTH];
			uint64_t first = 0;
			uint64_t chunk = 0;
			const char *rest = NULL;
			if (!chunks.entries[c].isDirectory || !entryName(&chunks.entries[c], name, sizeof name) ||
					!readChunkId(name, &first, &rest) || *rest ||
					!batchPlan_chunkAt(&batch->plan, first, &chunk) ||
					!joinWide(chunkFolder, armFolder, chunks.entries[c].name)) {
				continue;
			}
			ok = batch_committed(batch, arm, chunk) ? addLeftover(batch, chunkFolder, false)
																							: scanChunkAttempts(batch, arm, chunk, chunkFolder);
		}
		free(chunks.entries);
	}
	free(arms.entries);
	return ok ? BATCH_OPENED : BATCH_OPEN_FAILED;
}

// The a<k> folders of a chunk that is not committed. Those without a readable attempt.json never
// had a job submitted; so does the chunk's folder if none has one.
static bool scanChunkAttempts(Batch *batch, size_t arm, uint64_t chunk, const wchar_t *folder) {
	Listing attempts;
	if (!listFolder(folder, &attempts)) {
		return false;
	}
	BatchChunk *pending = addPending(batch, arm, chunk);
	size_t leftovers = batch->leftoverCount;
	bool ok = pending != NULL;
	for (size_t i = 0; ok && i < attempts.count; ++i) {
		char name[NAME_LENGTH * 4];
		wchar_t path[HARNESS_PATH_LENGTH];
		uint64_t number = 0;
		BatchPart part = {0};
		if (!attempts.entries[i].isDirectory || !entryName(&attempts.entries[i], name, sizeof name) ||
				name[0] != 'a' || name[1] < '1' || name[1] > '9' ||
				strspn(name + 1, "0123456789") != strlen(name + 1) || strlen(name) > 10 ||
				!joinWide(path, folder, attempts.entries[i].name)) {
			continue;
		}
		for (const char *digit = name + 1; *digit; ++digit) {
			number = number * 10 + (uint64_t)(*digit - '0');
		}
		part.attempt = number <= UINT32_MAX ? (uint32_t)number : 0;
		ok = part.attempt && readPart(batch, pending, path, &part) ? addPart(pending, &part)
																															 : addLeftover(batch, path, false);
	}
	free(attempts.entries);
	if (ok && pending->partCount) {
		qsort(pending->parts, pending->partCount, sizeof *pending->parts, compareParts);
	} else if (ok) {
		batch->leftoverCount = leftovers;
		dropPending(batch, pending);
		ok = addLeftover(batch, folder, false);
	}
	return ok;
}

// An attempt from its attempt.json and, if it has a readable one, its part.json.
static bool
readPart(const Batch *batch, const BatchChunk *chunk, const wchar_t *folder, BatchPart *part) {
	wchar_t path[HARNESS_PATH_LENGTH];
	JsonFlat flat = {0};
	char format[32];
	char arm[BATCH_ARM_LENGTH];
	uint64_t values[4] = {0}; // attempt, chunk, first index, count
	static const char *const keys[] = {"attempt", "chunk", "first_index", "count"};
	uint64_t threads = 0;
	bool ok = joinName(path, folder, "attempt.json") && readJson(path, &flat) &&
						jsonFlat_string(&flat, "format", format, sizeof format) &&
						strcmp(format, attemptFormat) == 0 && jsonFlat_string(&flat, "arm", arm, sizeof arm) &&
						strcmp(arm, batch->plan.arms[chunk->arm].id) == 0 &&
						jsonFlat_unsigned(&flat, "threads", &threads) && threads >= 1 &&
						threads <= HARNESS_MAX_THREADS &&
						jsonFlat_string(&flat, "session", part->session, sizeof part->session) &&
						jsonFlat_time(&flat, "started_at", &part->startedAt);
	for (size_t i = 0; ok && i < 4; ++i) {
		ok = jsonFlat_unsigned(&flat, keys[i], &values[i]);
	}
	ok = ok && values[0] == part->attempt && values[1] == chunk->first && values[2] >= chunk->first &&
			 values[2] <= chunk->last && values[3] >= 1 && values[3] - 1 <= chunk->last - values[2];
	part->first = values[2];
	part->count = values[3];
	part->threads = (uint32_t)threads;
	if (ok && joinName(path, folder, "part.json") && readJson(path, &flat)) {
		readEvaluation(&flat, part);
	}
	jsonFlat_free(&flat);
	return ok;
}

// part.json, if it is of this attempt and reads; else the attempt stays to be evaluated.
static void readEvaluation(const JsonFlat *flat, BatchPart *part) {
	char format[32];
	char outcome[16];
	uint64_t values[4] = {0}; // attempt, first index, count, verified
	static const char *const keys[] = {"attempt", "first_index", "count", "verified"};
	bool ok = jsonFlat_string(flat, "format", format, sizeof format) &&
						strcmp(format, partFormat) == 0 &&
						jsonFlat_string(flat, "outcome", outcome, sizeof outcome) &&
						jsonFlat_time(flat, "finished_at", &part->finishedAt) &&
						jsonFlat_unsigned(flat, "wall_ms", &part->wallMs) &&
						jsonFlat_unsigned(flat, "slept_ms", &part->sleptMs);
	for (size_t i = 0; ok && i < 4; ++i) {
		ok = jsonFlat_unsigned(flat, keys[i], &values[i]);
	}
	ok = ok && values[0] == part->attempt && values[1] == part->first && values[2] == part->count &&
			 values[3] <= part->count;
	for (size_t role = 0; ok && role < BATCH_ROLES; ++role) {
		char key[KEY_LENGTH];
		(void)snprintf(key, sizeof key, "rows.%s", roleNames[role]);
		ok = jsonFlat_unsigned(flat, key, &part->rows[role]);
	}
	size_t named = 0;
	while (ok && named < BATCH_PART_OUTCOMES && strcmp(outcomeNames[named], outcome) != 0) {
		++named;
	}
	part->hasPoison = ok && jsonFlat_unsigned(flat, "poison", &part->poison);
	ok = ok && named < BATCH_PART_OUTCOMES &&
			 (part->hasPoison ? part->poison == part->first + values[3] && values[3] < part->count
												: jsonFlat_isNull(flat, "poison"));
	if (!ok) {
		part->hasPoison = false;
		part->poison = 0;
		return;
	}
	(void)jsonFlat_string(flat, "client_outcome", part->clientOutcome, sizeof part->clientOutcome);
	(void)jsonFlat_string(flat, "result_line", part->resultLine, sizeof part->resultLine);
	part->outcome = (BatchPartOutcome)named;
	part->verified = values[3];
	part->evaluated = true;
}

static bool addLeftover(Batch *batch, const wchar_t *path, bool chunkFolder) {
	if (batch->leftoverCount == batch->leftoverCapacity) {
		size_t capacity = batch->leftoverCapacity ? batch->leftoverCapacity * 2 : 4;
		BatchLeftover *leftovers = realloc(batch->leftovers, capacity * sizeof *leftovers);
		if (!leftovers) {
			return false;
		}
		batch->leftovers = leftovers;
		batch->leftoverCapacity = capacity;
	}
	BatchLeftover *leftover = &batch->leftovers[batch->leftoverCount++];
	wcscpy(leftover->path, path);
	leftover->chunkFolder = chunkFolder;
	return true;
}

// Adds a committed chunk, keeping the list ordered.
static bool addCommitted(Batch *batch, const BatchCommitted *record) {
	if (batch->committedCount == batch->committedCapacity) {
		size_t capacity = batch->committedCapacity ? batch->committedCapacity * 2 : 64;
		BatchCommitted *committed = realloc(batch->committed, capacity * sizeof *committed);
		if (!committed) {
			return false;
		}
		batch->committed = committed;
		batch->committedCapacity = capacity;
	}
	size_t at = batch->committedCount;
	while (at && compareCommitted(&batch->committed[at - 1], record) > 0) {
		--at;
	}
	memmove(
		&batch->committed[at + 1], &batch->committed[at], (batch->committedCount - at) * sizeof *record
	);
	batch->committed[at] = *record;
	++batch->committedCount;
	return true;
}

static BatchChunk *findPending(const Batch *batch, size_t arm, uint64_t chunk) {
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		if (batch->pending[i].arm == arm && batch->pending[i].chunk == chunk) {
			return &batch->pending[i];
		}
	}
	return NULL;
}

static BatchChunk *addPending(Batch *batch, size_t arm, uint64_t chunk) {
	if (batch->pendingCount == batch->pendingCapacity) {
		size_t capacity = batch->pendingCapacity ? batch->pendingCapacity * 2 : 4;
		BatchChunk *pending = realloc(batch->pending, capacity * sizeof *pending);
		if (!pending) {
			return NULL;
		}
		batch->pending = pending;
		batch->pendingCapacity = capacity;
	}
	BatchChunk *added = &batch->pending[batch->pendingCount++];
	*added = (BatchChunk){.arm = arm, .chunk = chunk};
	batchPlan_chunkRange(&batch->plan, chunk, &added->first, &added->last);
	return added;
}

static void dropPending(Batch *batch, BatchChunk *chunk) {
	free(chunk->parts);
	size_t at = (size_t)(chunk - batch->pending);
	memmove(chunk, chunk + 1, (batch->pendingCount - at - 1) * sizeof *chunk);
	--batch->pendingCount;
}

static BatchPart *findPart(const BatchChunk *chunk, uint32_t attempt) {
	for (size_t i = 0; i < chunk->partCount; ++i) {
		if (chunk->parts[i].attempt == attempt) {
			return &chunk->parts[i];
		}
	}
	return NULL;
}

static bool addPart(BatchChunk *chunk, const BatchPart *part) {
	if (chunk->partCount == chunk->capacity) {
		size_t capacity = chunk->capacity ? chunk->capacity * 2 : 4;
		BatchPart *parts = realloc(chunk->parts, capacity * sizeof *parts);
		if (!parts) {
			return false;
		}
		chunk->parts = parts;
		chunk->capacity = capacity;
	}
	chunk->parts[chunk->partCount++] = *part;
	return true;
}

static int compareParts(const void *a, const void *b) {
	uint32_t left = ((const BatchPart *)a)->attempt;
	uint32_t right = ((const BatchPart *)b)->attempt;
	return (left > right) - (left < right);
}

static int compareCommitted(const void *a, const void *b) {
	const BatchCommitted *left = a;
	const BatchCommitted *right = b;
	if (left->arm != right->arm) {
		return left->arm < right->arm ? -1 : 1;
	}
	return (left->chunk > right->chunk) - (left->chunk < right->chunk);
}

static bool
attemptFolder(const Batch *batch, const BatchChunk *chunk, uint32_t attempt, wchar_t *out) {
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(chunk->first, id);
	return pathOf(batch, out, "attempts/%s/%s/a%u", batch->plan.arms[chunk->arm].id, id, attempt);
}

static bool chunkAttemptsFolder(const Batch *batch, size_t arm, uint64_t first, wchar_t *out) {
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(first, id);
	return pathOf(batch, out, "attempts/%s/%s", batch->plan.arms[arm].id, id);
}

// Makes the attempt's folder and writes its attempt.json, both written through to the disk.
static bool writeAttemptJson(
	Batch *batch,
	const BatchChunk *chunk,
	const BatchPart *part,
	const char *seeds,
	const char *job
) {
	wchar_t armFolder[HARNESS_PATH_LENGTH];
	wchar_t chunkFolder[HARNESS_PATH_LENGTH];
	wchar_t folder[HARNESS_PATH_LENGTH];
	wchar_t path[HARNESS_PATH_LENGTH];
	wchar_t attempts[HARNESS_PATH_LENGTH];
	char seedsSha256[SHA256_HEX_LENGTH];
	char jobSha256[SHA256_HEX_LENGTH];
	sha256_hexOf(seeds, strlen(seeds), seedsSha256);
	sha256_hexOf(job, strlen(job), jobSha256);
	ByteBuffer text = {0};
	JsonWriter writer;
	jsonWriter_start(&writer, &text);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", attemptFormat);
	jsonWriter_string(&writer, "batch", batch->plan.name);
	jsonWriter_string(&writer, "arm", batch->plan.arms[chunk->arm].id);
	jsonWriter_unsigned(&writer, "chunk", chunk->first);
	jsonWriter_unsigned(&writer, "attempt", part->attempt);
	jsonWriter_unsigned(&writer, "first_index", part->first);
	jsonWriter_unsigned(&writer, "count", part->count);
	jsonWriter_unsigned(&writer, "threads", part->threads);
	jsonWriter_string(&writer, "session", part->session);
	jsonWriter_utcTime(&writer, "started_at", part->startedAt);
	jsonWriter_string(&writer, "seeds_sha256", seedsSha256);
	jsonWriter_string(&writer, "job_sha256", jobSha256);
	jsonWriter_endObject(&writer);
	bool ok = jsonWriter_finish(&writer) && buffer_append(&text, "\n", 1) &&
						pathOf(batch, attempts, "attempts") &&
						pathOf(batch, armFolder, "attempts/%s", batch->plan.arms[chunk->arm].id) &&
						chunkAttemptsFolder(batch, chunk->arm, chunk->first, chunkFolder) &&
						attemptFolder(batch, chunk, part->attempt, folder) &&
						joinName(path, folder, "attempt.json") && platform_makeDirectory(attempts) &&
						platform_makeDirectory(armFolder) && platform_makeDirectory(chunkFolder) &&
						platform_makeDirectory(folder) && replaceText(batch, path, &text) &&
						batch->host->syncDirectory(batch->host->context, chunkFolder);
	buffer_free(&text);
	return ok;
}

// Verifies an attempt's files, merging the verified rows into `outputs` if given.
static bool evaluatePart(
	Batch *batch,
	const BatchChunk *chunk,
	const BatchPart *part,
	BatchSink *outputs,
	BatchVerified *verified
) {
	const BatchPlan *plan = &batch->plan;
	wchar_t folder[HARNESS_PATH_LENGTH];
	BatchSeeds seeds = batchPlan_armSeeds(plan, chunk->arm);
	BatchVerify request = {
		.seeds = &seeds,
		.first = part->first,
		.count = outputs ? part->verified : part->count,
		.perMatch = plan->perMatch != 0,
		.failed = plan->seedErrors == BATCH_SEED_ERRORS_SKIP,
		.outputs = outputs,
	};
	bool expected[BATCH_ROLES] = {true, request.perMatch, request.perMatch, request.failed};
	bool ok = attemptFolder(batch, chunk, part->attempt, folder);
	for (size_t role = 0; ok && role < BATCH_ROLES; ++role) {
		wchar_t path[HARNESS_PATH_LENGTH];
		if (expected[role] && joinName(path, folder, attemptFiles[role])) {
			request.inputs[role] = platform_openFile(path, "rb");
		}
	}
	ok = ok && batchVerify_run(&request, verified);
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		if (request.inputs[role]) {
			(void)fclose(request.inputs[role]);
		}
	}
	return ok;
}

// Writes an attempt's files, which the harness only flushed, and its folder through to the disk,
// so that a power cut cannot keep a part.json whose rows it lost.
static bool syncPart(Batch *batch, const BatchChunk *chunk, const BatchPart *part) {
	const BatchHost *host = batch->host;
	wchar_t folder[HARNESS_PATH_LENGTH];
	if (!attemptFolder(batch, chunk, part->attempt, folder)) {
		return false;
	}
	bool ok = true;
	for (size_t role = 0; ok && role < BATCH_ROLES; ++role) {
		wchar_t path[HARNESS_PATH_LENGTH];
		// "r+b" neither creates a file the job did not write nor changes one.
		FILE *file = joinName(path, folder, attemptFiles[role]) ? platform_openFile(path, "r+b") : NULL;
		if (file) {
			ok = host->syncFile(host->context, file);
			ok = fclose(file) == 0 && ok;
		}
	}
	return ok && host->syncDirectory(host->context, folder);
}

static bool writePartJson(Batch *batch, const BatchChunk *chunk, const BatchPart *part) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer text = {0};
	JsonWriter writer;
	jsonWriter_start(&writer, &text);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", partFormat);
	jsonWriter_string(&writer, "arm", batch->plan.arms[chunk->arm].id);
	jsonWriter_unsigned(&writer, "chunk", chunk->first);
	writePartMembers(&writer, part);
	jsonWriter_beginObject(&writer, "rows");
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		jsonWriter_unsigned(&writer, roleNames[role], part->rows[role]);
	}
	jsonWriter_endObject(&writer);
	writeOptionalText(&writer, "result_line", part->resultLine);
	jsonWriter_endObject(&writer);
	bool ok = jsonWriter_finish(&writer) && buffer_append(&text, "\n", 1) &&
						attemptFolder(batch, chunk, part->attempt, folder) &&
						joinName(path, folder, "part.json") && replaceText(batch, path, &text);
	buffer_free(&text);
	return ok;
}

// An evaluated attempt's members, as part.json and chunk.json have them.
static void writePartMembers(JsonWriter *writer, const BatchPart *part) {
	jsonWriter_unsigned(writer, "attempt", part->attempt);
	jsonWriter_unsigned(writer, "first_index", part->first);
	jsonWriter_unsigned(writer, "count", part->count);
	jsonWriter_unsigned(writer, "verified", part->verified);
	jsonWriter_string(writer, "outcome", batch_outcomeName(part->outcome));
	if (part->hasPoison) {
		jsonWriter_unsigned(writer, "poison", part->poison);
	} else {
		jsonWriter_null(writer, "poison");
	}
	jsonWriter_string(writer, "session", part->session);
	jsonWriter_unsigned(writer, "threads", part->threads);
	jsonWriter_utcTime(writer, "started_at", part->startedAt);
	jsonWriter_utcTime(writer, "finished_at", part->finishedAt);
	jsonWriter_unsigned(writer, "wall_ms", part->wallMs);
	jsonWriter_unsigned(writer, "slept_ms", part->sleptMs);
	writeOptionalText(writer, "client_outcome", part->clientOutcome);
}

static void logPart(Batch *batch, const BatchChunk *chunk, const BatchPart *part) {
	char id[BATCH_CHUNK_ID_LENGTH];
	const char *arm = batch->plan.arms[chunk->arm].id;
	batchPlan_chunkId(chunk->first, id);
	batch_logEvent(
		batch,
		"part_finished arm=%s chunk=%s attempt=%u first=%llu count=%llu verified=%llu outcome=%s",
		arm,
		id,
		part->attempt,
		(unsigned long long)part->first,
		(unsigned long long)part->count,
		(unsigned long long)part->verified,
		batch_outcomeName(part->outcome)
	);
	if (part->rows[BATCH_FAILED]) {
		batch_logEvent(
			batch,
			"seed_errors arm=%s chunk=%s attempt=%u count=%llu",
			arm,
			id,
			part->attempt,
			(unsigned long long)part->rows[BATCH_FAILED]
		);
	}
	if (part->hasPoison) {
		BatchSeeds seeds = batchPlan_armSeeds(&batch->plan, chunk->arm);
		batch_logEvent(
			batch,
			"poison arm=%s chunk=%s index=%llu seed=%u",
			arm,
			id,
			(unsigned long long)part->poison,
			batchSeeds_at(&seeds, part->poison)
		);
	}
}

// Commit step 1: the chain's parts, each up to its verified prefix, into chunk=<first>.tmp, with
// each result row given its index.
static BatchStatus mergeParts(Commit *commit) {
	Batch *batch = commit->batch;
	wchar_t chunks[HARNESS_PATH_LENGTH];
	BatchSink sinks[BATCH_ROLES];
	FILE *files[BATCH_ROLES] = {0};
	if (!pathOf(batch, chunks, "chunks") || !platform_makeDirectory(chunks) ||
			!platform_makeDirectory(commit->armFolder) || !platform_makeDirectory(commit->temporary)) {
		return failed("cannot make the chunk's folder");
	}
	BatchStatus status = openSinks(commit, sinks, files);
	for (size_t i = 0; status == BATCH_STATUS_OK && i < commit->chain.count; ++i) {
		const BatchPart *part = &commit->pending->parts[commit->chain.parts[i]];
		BatchVerified verified;
		if (!evaluatePart(batch, commit->pending, part, sinks, &verified) ||
				verified.verified != part->verified) {
			status = failed("attempt a%u no longer matches its part.json", part->attempt);
			break;
		}
		commit->summary.matches += verified.summary.matches;
		commit->summary.home += verified.summary.home;
		commit->summary.draw += verified.summary.draw;
		commit->summary.away += verified.summary.away;
		commit->summary.homeGoals += verified.summary.homeGoals;
		commit->summary.awayGoals += verified.summary.awayGoals;
		commit->summary.unfinished += verified.summary.unfinished;
	}
	if (!closeSinks(commit, sinks, files) && status == BATCH_STATUS_OK) {
		status = failed("cannot write the chunk's files");
	}
	if (status != BATCH_STATUS_OK) {
		return status;
	}
	return reached(batch, BATCH_STEP_MERGED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

static BatchStatus openSinks(Commit *commit, BatchSink *sinks, FILE **files) {
	const BatchPlan *plan = &commit->batch->plan;
	bool expected[BATCH_ROLES] = {
		true, plan->perMatch != 0, plan->perMatch != 0, plan->seedErrors == BATCH_SEED_ERRORS_SKIP};
	const char *headers[BATCH_ROLES] = {batchVerify_chunkResultsHeader,
		batchVerify_playersHeader,
		batchVerify_teamsHeader,
		batchVerify_failedHeader};
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		wchar_t path[HARNESS_PATH_LENGTH];
		commit->files[role].present = expected[role];
		// A file the plan does not write still gets a sink, which writes nothing.
		files[role] = expected[role] && joinName(path, commit->temporary, chunkFiles[role])
										? platform_openFile(path, "wb")
										: NULL;
		batchSink_start(&sinks[role], files[role], headers[role], role == BATCH_RESULTS);
		if (expected[role] && !files[role]) {
			return failed("cannot write the chunk's %s", chunkFiles[role]);
		}
	}
	return BATCH_STATUS_OK;
}

static bool closeSinks(Commit *commit, BatchSink *sinks, FILE **files) {
	bool ok = true;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		ChunkFile *file = &commit->files[role];
		if (!files[role]) {
			continue;
		}
		ok = !sinks[role].failed && !ferror(files[role]) && ok;
		ok = fclose(files[role]) == 0 && ok;
		file->rows = sinks[role].rows;
		file->rawBytes = sinks[role].bytes;
		batchSink_finish(&sinks[role], file->rawSha256, file->contentSha256);
	}
	return ok;
}

// Commit step 1, continued: chunk 0's first rows against the canary reference, while uncompressed.
static BatchStatus checkReference(Commit *commit) {
	const BatchCommitFacts *facts = commit->facts;
	commit->canaryMatch = facts ? facts->canaryMatch : -1;
	if (facts && facts->checkCanary) {
		commit->canaryMatch = facts->checkCanary(facts->canaryContext, commit->temporary);
	}
	return BATCH_STATUS_OK;
}

// Commit step 2: compresses each file (gzip), noting the stored files' sizes and hashes.
static BatchStatus compressFiles(Commit *commit) {
	Batch *batch = commit->batch;
	bool gzip = batch->plan.compress == BATCH_COMPRESS_GZIP;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		ChunkFile *file = &commit->files[role];
		wchar_t path[HARNESS_PATH_LENGTH];
		if (!file->present) {
			continue;
		}
		(void)snprintf(file->name, sizeof file->name, "%s%s", chunkFiles[role], gzip ? ".gz" : "");
		if (!joinName(path, commit->temporary, chunkFiles[role]) ||
				(gzip && !batch->host->compress(batch->host->context, path)) ||
				!joinName(path, commit->temporary, file->name) ||
				!hashFile(path, &file->bytes, file->sha256)) {
			return failed("cannot compress the chunk's %s", chunkFiles[role]);
		}
		if (gzip && !reached(batch, BATCH_STEP_COMPRESSED)) {
			return BATCH_STATUS_KILLED;
		}
	}
	return BATCH_STATUS_OK;
}

// Commit step 3: chunk.json, which also gives the committed chunk's record.
static BatchStatus describeChunk(Commit *commit) {
	Batch *batch = commit->batch;
	const BatchPlan *plan = &batch->plan;
	const BatchArm *arm = &plan->arms[commit->pending->arm];
	const BatchCommitFacts *facts = commit->facts;
	commit->committedAt = batch->host->utcNow(batch->host->context);
	uint64_t freeBytes = 0;
	bool hasFree = batch->host->freeBytes(batch->host->context, batch->directory, &freeBytes);
	JsonWriter writer;
	jsonWriter_start(&writer, &commit->manifest);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", chunkFormat);
	jsonWriter_string(&writer, "batch", plan->name);
	jsonWriter_string(&writer, "plan_sha256", plan->planSha256);
	jsonWriter_string(&writer, "arm", arm->id);
	jsonWriter_string(&writer, "fixture_sha256", arm->jsonSha256);
	jsonWriter_string(&writer, "input_sha256", arm->binSha256);
	writeTeam(&writer, "home_team", arm->teams[0]);
	writeTeam(&writer, "away_team", arm->teams[1]);
	if (plan->mode == BATCH_MODE_SEASON) {
		jsonWriter_string(&writer, "fixtures_sha256", batch->fixturesSha256);
		jsonWriter_beginObject(&writer, "season_fixture");
		jsonWriter_unsigned(&writer, "number", commit->pending->arm + 1);
		jsonWriter_unsigned(&writer, "fixtures", plan->armCount);
		jsonWriter_unsigned(&writer, "home_team_uid", arm->pair[0]);
		jsonWriter_unsigned(&writer, "away_team_uid", arm->pair[1]);
		jsonWriter_endObject(&writer);
	}
	writeChunkSeeds(&writer, commit);
	writeChunkParts(&writer, commit);
	jsonWriter_beginObject(&writer, "rows");
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		jsonWriter_unsigned(&writer, roleNames[role], commit->files[role].rows);
	}
	jsonWriter_endObject(&writer);
	writeChunkFiles(&writer, commit);
	jsonWriter_beginObject(&writer, "summary");
	jsonWriter_unsigned(&writer, "matches", commit->summary.matches);
	jsonWriter_unsigned(&writer, "home", commit->summary.home);
	jsonWriter_unsigned(&writer, "draw", commit->summary.draw);
	jsonWriter_unsigned(&writer, "away", commit->summary.away);
	jsonWriter_unsigned(&writer, "home_goals", commit->summary.homeGoals);
	jsonWriter_unsigned(&writer, "away_goals", commit->summary.awayGoals);
	jsonWriter_unsigned(&writer, "unfinished", commit->summary.unfinished);
	jsonWriter_endObject(&writer);
	jsonWriter_beginObject(&writer, "host");
	writeOptionalText(&writer, "power", facts ? facts->power : NULL);
	if (hasFree) {
		jsonWriter_unsigned(&writer, "free_bytes", freeBytes);
	} else {
		jsonWriter_null(&writer, "free_bytes");
	}
	if (facts && facts->gameRssBytes) {
		jsonWriter_unsigned(&writer, "game_rss_bytes", facts->gameRssBytes);
	} else {
		jsonWriter_null(&writer, "game_rss_bytes");
	}
	jsonWriter_endObject(&writer);
	jsonWriter_beginObject(&writer, "checks");
	jsonWriter_bool(&writer, "verified", true);
	if (commit->canaryMatch >= 0) {
		jsonWriter_bool(&writer, "canary_reference_match", commit->canaryMatch != 0);
	} else {
		jsonWriter_null(&writer, "canary_reference_match");
	}
	jsonWriter_endObject(&writer);
	jsonWriter_utcTime(&writer, "committed_at", commit->committedAt);
	jsonWriter_endObject(&writer);
	wchar_t path[HARNESS_PATH_LENGTH];
	bool ok = jsonWriter_finish(&writer) && buffer_append(&commit->manifest, "\n", 1) &&
						joinName(path, commit->temporary, "chunk.json") &&
						files_write(path, commit->manifest.data, commit->manifest.length);
	if (!ok) {
		return failed("cannot write the chunk's chunk.json");
	}
	return reached(batch, BATCH_STEP_DESCRIBED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

static void writeChunkSeeds(JsonWriter *writer, const Commit *commit) {
	const BatchPlan *plan = &commit->batch->plan;
	const BatchChunk *chunk = commit->pending;
	char spec[BATCH_PATH_LENGTH + 32];
	char hash[SHA256_HEX_LENGTH];
	BatchSeeds seeds = batchPlan_armSeeds(plan, chunk->arm);
	switch (plan->seeds.kind) {
		case BATCH_SEEDS_SCATTER:
			(void)snprintf(
				spec,
				sizeof spec,
				"scatter:%llu:%llu-%llu",
				(unsigned long long)plan->seeds.key,
				(unsigned long long)plan->seeds.first,
				(unsigned long long)plan->seeds.last
			);
			break;
		case BATCH_SEEDS_LIST: (void)snprintf(spec, sizeof spec, "list:%s", plan->listPath); break;
		case BATCH_SEEDS_SEASON:
			(void)snprintf(
				spec,
				sizeof spec,
				"season:%llu:%llu:%llu:%llu-%llu",
				(unsigned long long)seeds.key,
				(unsigned long long)seeds.stride,
				(unsigned long long)seeds.offset,
				(unsigned long long)seeds.first,
				(unsigned long long)seeds.last
			);
			break;
		default:
			(void)snprintf(
				spec,
				sizeof spec,
				"range:%llu-%llu",
				(unsigned long long)plan->seeds.first,
				(unsigned long long)plan->seeds.last
			);
			break;
	}
	batchSeeds_hash(&seeds, chunk->first, chunk->last, hash);
	jsonWriter_beginObject(writer, "seeds");
	jsonWriter_string(writer, "spec", spec);
	jsonWriter_unsigned(writer, "first_index", chunk->first);
	jsonWriter_unsigned(writer, "last_index", chunk->last);
	jsonWriter_unsigned(writer, "count", chunk->last - chunk->first + 1);
	jsonWriter_string(writer, "seeds_sha256", hash);
	jsonWriter_endObject(writer);
	jsonWriter_beginArray(writer, "excluded");
	for (size_t i = 0; i < commit->chain.excludedCount; ++i) {
		uint64_t index = commit->chain.excluded[i];
		jsonWriter_beginObject(writer, NULL);
		jsonWriter_unsigned(writer, "index", index);
		jsonWriter_unsigned(writer, "seed", batchSeeds_at(&seeds, index));
		jsonWriter_string(writer, "reason", "poison");
		jsonWriter_endObject(writer);
	}
	jsonWriter_endArray(writer);
}

// The chain's parts, each with its harness report, then the attempts that did not advance it.
static void writeChunkParts(JsonWriter *writer, const Commit *commit) {
	const BatchChunk *chunk = commit->pending;
	for (int pass = 0; pass < 2; ++pass) {
		jsonWriter_beginArray(writer, pass ? "unused_attempts" : "parts");
		size_t count = pass ? chunk->partCount : commit->chain.count;
		for (size_t i = 0; i < count; ++i) {
			if (pass && isChainPart(&commit->chain, i)) {
				continue;
			}
			const BatchPart *part = &chunk->parts[pass ? i : commit->chain.parts[i]];
			RunReport report;
			jsonWriter_beginObject(writer, NULL);
			writePartMembers(writer, part);
			manifest_writeHarness(writer, runReport_parse(part->resultLine, &report) ? &report : NULL);
			jsonWriter_endObject(writer);
		}
		jsonWriter_endArray(writer);
	}
}

static void writeChunkFiles(JsonWriter *writer, const Commit *commit) {
	jsonWriter_beginArray(writer, "files");
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		const ChunkFile *file = &commit->files[role];
		if (!file->present) {
			continue;
		}
		jsonWriter_beginObject(writer, NULL);
		jsonWriter_string(writer, "name", file->name);
		jsonWriter_unsigned(writer, "rows", file->rows);
		jsonWriter_unsigned(writer, "raw_bytes", file->rawBytes);
		jsonWriter_string(writer, "raw_sha256", file->rawSha256);
		if (role == BATCH_RESULTS) {
			jsonWriter_string(writer, "content_sha256", file->contentSha256);
		}
		jsonWriter_unsigned(writer, "bytes", file->bytes);
		jsonWriter_string(writer, "sha256", file->sha256);
		jsonWriter_endObject(writer);
	}
	jsonWriter_endArray(writer);
}

// Commit step 4: every file, then the folder, written through to the disk.
static BatchStatus syncChunk(Commit *commit) {
	Batch *batch = commit->batch;
	const BatchHost *host = batch->host;
	bool ok = true;
	for (size_t role = 0; ok && role <= BATCH_ROLES; ++role) {
		wchar_t path[HARNESS_PATH_LENGTH];
		if (role < BATCH_ROLES && !commit->files[role].present) {
			continue;
		}
		ok = joinName(
			path, commit->temporary, role < BATCH_ROLES ? commit->files[role].name : "chunk.json"
		);
		FILE *file = ok ? platform_openFile(path, "ab") : NULL;
		ok = file && host->syncFile(host->context, file);
		ok = file && fclose(file) == 0 && ok;
	}
	if (!ok || !host->syncDirectory(host->context, commit->temporary)) {
		return failed("cannot write the chunk's files to the disk");
	}
	return reached(batch, BATCH_STEP_SYNCED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

// Commit step 5: the rename that commits the chunk, written through to the disk.
static BatchStatus renameChunk(Commit *commit) {
	Batch *batch = commit->batch;
	BatchCommitted record = {
		.arm = commit->pending->arm,
		.chunk = commit->pending->chunk,
		.first = commit->pending->first,
		.count = commit->pending->last - commit->pending->first + 1,
	};
	JsonFlat flat = {0};
	record.readable =
		jsonFlat_read(&flat, buffer_string(&commit->manifest), commit->manifest.length) &&
		readCommitted(batch, &flat, &record);
	jsonFlat_free(&flat);
	if (!record.readable) {
		return failed("the chunk's chunk.json does not read back");
	}
	if (!platform_renameFolder(commit->temporary, commit->folder) ||
			!batch->host->syncDirectory(batch->host->context, commit->armFolder)) {
		return failed("cannot commit the chunk");
	}
	if (!addCommitted(batch, &record)) {
		return failed("out of memory");
	}
	return reached(batch, BATCH_STEP_RENAMED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

// Commit step 6, once committed: removes the attempts, then adds the chunk to index.csv,
// events.log and batch.json.
static BatchStatus finishCommit(Commit *commit) {
	Batch *batch = commit->batch;
	BatchChunk *pending = commit->pending;
	size_t arm = pending->arm;
	uint64_t chunk = pending->chunk;
	uint64_t first = pending->first;
	wchar_t folder[HARNESS_PATH_LENGTH];
	for (size_t i = 0; i < pending->partCount; ++i) {
		if (!attemptFolder(batch, pending, pending->parts[i].attempt, folder) || !removeTree(folder)) {
			return failed("cannot remove the attempts of a committed chunk");
		}
		if (!reached(batch, BATCH_STEP_ATTEMPT_REMOVED)) {
			return BATCH_STATUS_KILLED;
		}
	}
	dropPending(batch, pending);
	commit->pending = NULL;
	if (!chunkAttemptsFolder(batch, arm, first, folder) || !removeTree(folder)) {
		return failed("cannot remove the attempts of a committed chunk");
	}
	const BatchCommitted *record = batch_committed(batch, arm, chunk);
	ByteBuffer line = {0};
	wchar_t path[HARNESS_PATH_LENGTH];
	formatIndexLine(batch, record, &line);
	bool indexed = line.length && pathOf(batch, path, "index.csv") &&
								 appendLine(path, buffer_string(&line), false, NULL);
	buffer_free(&line);
	if (!indexed && !batch_writeIndex(batch)) {
		return failed("cannot add the chunk to index.csv");
	}
	if (!reached(batch, BATCH_STEP_INDEXED)) {
		return BATCH_STATUS_KILLED;
	}
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(first, id);
	batch_logEvent(
		batch,
		"chunk_committed arm=%s chunk=%s results=%llu failed=%llu excluded=%llu attempts=%u",
		batch->plan.arms[arm].id,
		id,
		(unsigned long long)record->rows[BATCH_RESULTS],
		(unsigned long long)record->rows[BATCH_FAILED],
		(unsigned long long)record->excluded,
		record->attempts
	);
	if (!reached(batch, BATCH_STEP_LOGGED)) {
		return BATCH_STATUS_KILLED;
	}
	BatchOutcome outcome = {"running", NULL, -1, batch->host->utcNow(batch->host->context)};
	return batch_writeManifest(batch, &outcome);
}

static bool isChainPart(const BatchChain *chain, size_t part) {
	for (size_t i = 0; i < chain->count; ++i) {
		if (chain->parts[i] == part) {
			return true;
		}
	}
	return false;
}

static BatchStatus removeLeftovers(Batch *batch, BatchRecovery *out) {
	while (batch->leftoverCount) {
		const BatchLeftover *leftover = &batch->leftovers[batch->leftoverCount - 1];
		if (!removeTree(leftover->path)) {
			return failed("cannot remove a folder a stopped batch left");
		}
		--batch->leftoverCount;
		++out->removed;
		if (!reached(batch, BATCH_STEP_TIDIED)) {
			return BATCH_STATUS_KILLED;
		}
	}
	wchar_t path[HARNESS_PATH_LENGTH];
	static const char *const temporaries[] = {"batch.json.tmp", "index.csv.tmp"};
	for (size_t i = 0; i < sizeof temporaries / sizeof temporaries[0]; ++i) {
		if (pathOf(batch, path, "%s", temporaries[i])) {
			(void)platform_removeFile(path);
		}
	}
	return batch->plan.mode == BATCH_MODE_SEASON ? removeBuildLeftovers(batch) : BATCH_STATUS_OK;
}

// What a stopped build of a season's fixtures left: its temporary files in inputs/ and
// inputs/build/, or all of inputs/build/ once the fixtures are frozen.
static BatchStatus removeBuildLeftovers(Batch *batch) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	bool ok = pathOf(batch, folder, "inputs/build");
	if (ok && batch->fixturesFrozen) {
		Listing listing;
		if (listFolder(folder, &listing)) {
			free(listing.entries);
			ok = removeTree(folder);
		}
		return ok ? BATCH_STATUS_OK : failed("cannot remove inputs/build");
	}
	static const char *const folders[] = {"inputs", "inputs/build"};
	for (size_t f = 0; ok && f < sizeof folders / sizeof folders[0]; ++f) {
		Listing listing;
		if (!pathOf(batch, folder, "%s", folders[f]) || !listFolder(folder, &listing)) {
			continue;
		}
		for (size_t i = 0; ok && i < listing.count; ++i) {
			wchar_t path[HARNESS_PATH_LENGTH];
			if (!listing.entries[i].isDirectory && wcsstr(listing.entries[i].name, L".tmp") &&
					joinWide(path, folder, listing.entries[i].name)) {
				ok = platform_removeFile(path);
			}
		}
		free(listing.entries);
	}
	return ok ? BATCH_STATUS_OK : failed("cannot remove the temporary files of a stopped build");
}

static BatchStatus
evaluateUnfinished(Batch *batch, BatchPartOutcome unevaluated, BatchRecovery *out) {
	BatchPartFacts facts = {
		.outcome = unevaluated,
		.finishedAt = batch->host->utcNow(batch->host->context),
	};
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		BatchChunk *chunk = &batch->pending[i];
		for (size_t p = 0; p < chunk->partCount; ++p) {
			if (chunk->parts[p].evaluated) {
				continue;
			}
			BatchStatus status =
				batch_finishAttempt(batch, chunk->arm, chunk->chunk, chunk->parts[p].attempt, &facts);
			if (status != BATCH_STATUS_OK) {
				return status;
			}
			++out->evaluated;
		}
	}
	return BATCH_STATUS_OK;
}

// Parts whose files now hold fewer rows than their part.json vouches for, as a disk that lost
// what it was told was written would leave them: each is shortened to what its files hold, so
// that the chain stops there and the rest is played again.
static BatchStatus shortenParts(Batch *batch, BatchRecovery *out) {
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		BatchChunk *chunk = &batch->pending[i];
		for (size_t p = 0; p < chunk->partCount; ++p) {
			BatchPart *part = &chunk->parts[p];
			BatchVerified verified;
			if (!part->evaluated || !part->verified) {
				continue;
			}
			if (!evaluatePart(batch, chunk, part, NULL, &verified)) {
				return failed("cannot read the files of attempt a%u", part->attempt);
			}
			if (verified.verified >= part->verified) {
				continue;
			}
			char id[BATCH_CHUNK_ID_LENGTH];
			batchPlan_chunkId(chunk->first, id);
			batch_logEvent(
				batch,
				"part_shortened arm=%s chunk=%s attempt=%u verified=%llu was=%llu",
				batch->plan.arms[chunk->arm].id,
				id,
				part->attempt,
				(unsigned long long)verified.verified,
				(unsigned long long)part->verified
			);
			BatchPart shortened = *part;
			shortened.verified = verified.verified;
			memcpy(shortened.rows, verified.rows, sizeof shortened.rows);
			shortened.hasPoison = false;
			shortened.poison = 0;
			if (!writePartJson(batch, chunk, &shortened)) {
				return failed("cannot write the part.json of attempt a%u", part->attempt);
			}
			*part = shortened;
			++out->shortened;
			if (!reached(batch, BATCH_STEP_PART_WRITTEN)) {
				return BATCH_STATUS_KILLED;
			}
		}
	}
	return BATCH_STATUS_OK;
}

// Parts that do not start where the chain stops: no later attempt can use them.
static BatchStatus removeStale(Batch *batch, BatchRecovery *out) {
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		BatchChunk *chunk = &batch->pending[i];
		BatchChain chain;
		if (!batchChain_compute(chunk, &chain)) {
			return failed("out of memory");
		}
		for (size_t p = chunk->partCount; p-- > 0;) {
			bool reachable = false;
			for (size_t s = 0; s < chain.stopCount; ++s) {
				reachable = reachable || chain.stops[s] == chunk->parts[p].first;
			}
			wchar_t folder[HARNESS_PATH_LENGTH];
			if (reachable) {
				continue;
			}
			if (!attemptFolder(batch, chunk, chunk->parts[p].attempt, folder) || !removeTree(folder)) {
				batchChain_free(&chain);
				return failed("cannot remove a stale part");
			}
			memmove(
				&chunk->parts[p], &chunk->parts[p + 1], (chunk->partCount - p - 1) * sizeof *chunk->parts
			);
			--chunk->partCount;
			++out->stale;
			if (!reached(batch, BATCH_STEP_TIDIED)) {
				batchChain_free(&chain);
				return BATCH_STATUS_KILLED;
			}
		}
		batchChain_free(&chain);
	}
	return BATCH_STATUS_OK;
}

// Logs the committed chunks that events.log lacks (their process stopped before it could), then
// rewrites index.csv. The log is read by its length, past any line a power cut left as zeros.
static BatchStatus relogCommitted(Batch *batch, BatchRecovery *out) {
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer log = {0};
	bool *logged = calloc(batch->committedCount + 1, sizeof *logged);
	if (!logged) {
		return failed("out of memory");
	}
	if (pathOf(batch, path, "events.log")) {
		(void)files_read(path, &log);
	}
	const char *line = NULL;
	size_t length = 0;
	for (size_t at = 0; nextLine(&log, &at, &line, &length);) {
		const BatchCommitted *record = loggedCommit(batch, line, length);
		if (record) {
			logged[record - batch->committed] = true;
		}
	}
	buffer_free(&log);
	for (size_t i = 0; i < batch->committedCount; ++i) {
		const BatchCommitted *record = &batch->committed[i];
		char id[BATCH_CHUNK_ID_LENGTH];
		if (logged[i]) {
			continue;
		}
		batchPlan_chunkId(record->first, id);
		batch_logEvent(
			batch,
			"chunk_committed arm=%s chunk=%s results=%llu failed=%llu excluded=%llu attempts=%u "
			"recovered=1",
			batch->plan.arms[record->arm].id,
			id,
			(unsigned long long)record->rows[BATCH_RESULTS],
			(unsigned long long)record->rows[BATCH_FAILED],
			(unsigned long long)record->excluded,
			record->attempts
		);
		++out->relogged;
	}
	free(logged);
	if (!batch_writeIndex(batch)) {
		return failed("cannot write index.csv");
	}
	return reached(batch, BATCH_STEP_INDEXED) ? BATCH_STATUS_OK : BATCH_STATUS_KILLED;
}

// The committed chunk an events.log line logs as committed (" chunk_committed arm=<arm>
// chunk=<id> ..."), or NULL.
static const BatchCommitted *loggedCommit(const Batch *batch, const char *line, size_t length) {
	static const char event[] = " chunk_committed arm=";
	static const char chunkKey[] = " chunk=";
	char text[1024]; // longer than batch_logEvent's lines
	if (length >= sizeof text) {
		return NULL;
	}
	memcpy(text, line, length);
	text[length] = 0;
	const char *arm = strstr(text, event);
	if (!arm) {
		return NULL;
	}
	arm += sizeof event - 1;
	size_t armLength = strcspn(arm, " ");
	char id[BATCH_ARM_LENGTH];
	uint64_t first = 0;
	uint64_t chunk = 0;
	const char *rest = NULL;
	if (armLength == 0 || armLength >= sizeof id ||
			strncmp(arm + armLength, chunkKey, sizeof chunkKey - 1) != 0 ||
			!readChunkId(arm + armLength + sizeof chunkKey - 1, &first, &rest) || *rest != ' ') {
		return NULL;
	}
	memcpy(id, arm, armLength);
	id[armLength] = 0;
	size_t index = armNamed(batch, id);
	return index < batch->plan.armCount && batchPlan_chunkAt(&batch->plan, first, &chunk)
					 ? batch_committed(batch, index, chunk)
					 : NULL;
}

// Commits, in the plan's order, every chunk whose parts cover it, but an arm's chunk 0 when the
// plan has canaries: batch run commits that itself, comparing it with the canary reference.
static BatchStatus commitFinished(Batch *batch, BatchRecovery *out) {
	uint64_t total = batchPlan_chunksPerArm(&batch->plan) * batch->plan.armCount;
	BatchCommitFacts facts = {.canaryMatch = -1};
	for (uint64_t position = 0; position < total && batch->pendingCount; ++position) {
		size_t arm = 0;
		uint64_t chunk = 0;
		bool done = false;
		batchPlan_scheduled(&batch->plan, position, &arm, &chunk);
		const BatchChunk *pending = findPending(batch, arm, chunk);
		if (!pending || (chunk == 0 && batch->plan.canary)) {
			continue;
		}
		if (!chainDone(pending, &done)) {
			return failed("out of memory");
		}
		BatchStatus status = done ? batch_commit(batch, arm, chunk, &facts) : BATCH_STATUS_OK;
		if (status != BATCH_STATUS_OK) {
			return status;
		}
		out->committed += done;
	}
	return BATCH_STATUS_OK;
}

static bool chainDone(const BatchChunk *chunk, bool *done) {
	BatchChain chain;
	if (!batchChain_compute(chunk, &chain)) {
		return false;
	}
	*done = chain.cursor > chunk->last;
	batchChain_free(&chain);
	return true;
}

// Appends a line to a text file, on a line of its own even if the file ends in a torn one; with
// `through`, written through to the disk with that host's syncs, as is the folder of a file it
// creates. False if it cannot, or the file is missing and not to be created.
static bool
appendLine(const wchar_t *path, const char *line, bool create, const BatchHost *through) {
	FILE *file = platform_openFile(path, "rb");
	bool newline = false;
	bool created = file == NULL;
	if (file) {
		newline = fseek(file, -1, SEEK_END) == 0 && fgetc(file) != '\n';
		(void)fclose(file);
	} else if (!create) {
		return false;
	}
	file = platform_openFile(path, "ab");
	if (!file) {
		return false;
	}
	bool ok = (!newline || fputc('\n', file) != EOF) && fputs(line, file) >= 0;
	ok = ok && (!through || through->syncFile(through->context, file));
	ok = fclose(file) == 0 && ok;
	if (ok && through && created) {
		wchar_t folder[HARNESS_PATH_LENGTH];
		folderOf(path, folder);
		ok = through->syncDirectory(through->context, folder);
	}
	return ok;
}

// The next line of `text` from `*at`, without its '\n'; false at the end. A line holding a NUL
// byte, as a power cut can leave the end of a file it cut short, is passed over.
static bool nextLine(const ByteBuffer *text, size_t *at, const char **line, size_t *length) {
	while (*at < text->length) {
		const char *start = (const char *)text->data + *at;
		size_t left = text->length - *at;
		const char *end = memchr(start, '\n', left);
		size_t size = end ? (size_t)(end - start) : left;
		*at += size + (end != NULL);
		if (!memchr(start, 0, size)) {
			*line = start;
			*length = size;
			return true;
		}
	}
	return false;
}

static void formatIndexLine(const Batch *batch, const BatchCommitted *chunk, ByteBuffer *out) {
	char id[BATCH_CHUNK_ID_LENGTH];
	char committed[TIME_LENGTH];
	char rate[32] = "";
	char line[1024];
	batchPlan_chunkId(chunk->first, id);
	formatUtc(chunk->committedAt, committed, sizeof committed);
	double perSecond = chunkRate(chunk);
	if (perSecond > 0) {
		(void)snprintf(rate, sizeof rate, "%.2f", perSecond);
	}
	const char *arm = batch->plan.arms[chunk->arm].id;
	int written = snprintf(
		line,
		sizeof line,
		"%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s,%u,%llu,%s,%s,%s,%s,%s,"
		"chunks/arm=%s/chunk=%s\n",
		arm,
		(unsigned long long)chunk->first,
		(unsigned long long)chunk->count,
		(unsigned long long)chunk->rows[BATCH_RESULTS],
		(unsigned long long)chunk->rows[BATCH_FAILED],
		(unsigned long long)chunk->excluded,
		(unsigned long long)chunk->rows[BATCH_PLAYERS],
		(unsigned long long)chunk->rows[BATCH_TEAMS],
		(unsigned long long)chunk->summary.home,
		(unsigned long long)chunk->summary.draw,
		(unsigned long long)chunk->summary.away,
		(unsigned long long)chunk->summary.homeGoals,
		(unsigned long long)chunk->summary.awayGoals,
		(unsigned long long)chunk->summary.unfinished,
		chunk->sessions,
		chunk->attempts,
		(unsigned long long)chunk->wallMs,
		rate,
		committed,
		chunk->sha256[BATCH_RESULTS],
		chunk->sha256[BATCH_PLAYERS],
		chunk->sha256[BATCH_TEAMS],
		arm,
		id
	);
	if (written > 0 && (size_t)written < sizeof line) {
		(void)buffer_append(out, line, (size_t)written);
	}
}

static void formatUtc(time_t seconds, char *out, size_t size) {
	struct tm utc;
	if (!platform_utcTime(seconds, &utc)) {
		(void)snprintf(out, size, "unknown");
		return;
	}
	(void)snprintf(
		out,
		size,
		"%04d-%02d-%02dT%02d:%02d:%02dZ",
		utc.tm_year + 1900,
		utc.tm_mon + 1,
		utc.tm_mday,
		utc.tm_hour,
		utc.tm_min,
		utc.tm_sec
	);
}

// A committed chunk's indices per second awake; 0 if unknown.
static double chunkRate(const BatchCommitted *chunk) {
	uint64_t awakeMs = chunk->wallMs > chunk->sleptMs ? chunk->wallMs - chunk->sleptMs : 0;
	return awakeMs ? (double)chunk->count * 1000.0 / (double)awakeMs : 0;
}

static void writeArms(JsonWriter *writer, const Batch *batch) {
	const BatchPlan *plan = &batch->plan;
	jsonWriter_beginArray(writer, "arms");
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		const BatchArm *entry = &plan->arms[arm];
		BatchArmTotals total;
		batch_armTotals(batch, arm, &total);
		jsonWriter_beginObject(writer, NULL);
		jsonWriter_string(writer, "id", entry->id);
		jsonWriter_beginObject(writer, "fixture");
		jsonWriter_string(writer, "path", entry->path);
		jsonWriter_string(writer, "sha256", entry->jsonSha256);
		jsonWriter_string(writer, "input_sha256", entry->binSha256);
		writeTeam(writer, "home_team", entry->teams[0]);
		writeTeam(writer, "away_team", entry->teams[1]);
		jsonWriter_endObject(writer);
		jsonWriter_unsigned(writer, "chunks_done", total.chunks);
		jsonWriter_unsigned(writer, "chunks_total", batchPlan_chunksPerArm(plan));
		jsonWriter_unsigned(writer, "covered", total.covered);
		jsonWriter_unsigned(writer, "matches", total.rows[BATCH_RESULTS]);
		jsonWriter_unsigned(writer, "skipped", total.rows[BATCH_FAILED]);
		jsonWriter_unsigned(writer, "excluded", total.excluded);
		jsonWriter_unsigned(writer, "home", total.summary.home);
		jsonWriter_unsigned(writer, "draw", total.summary.draw);
		jsonWriter_unsigned(writer, "away", total.summary.away);
		jsonWriter_unsigned(writer, "home_goals", total.summary.homeGoals);
		jsonWriter_unsigned(writer, "away_goals", total.summary.awayGoals);
		jsonWriter_unsigned(writer, "unfinished", total.summary.unfinished);
		jsonWriter_endObject(writer);
	}
	jsonWriter_endArray(writer);
}

// A season's fixtures and the seasons its complete blocks hold.
static void writeSeasons(JsonWriter *writer, const Batch *batch) {
	const BatchPlan *plan = &batch->plan;
	BatchSeasons seasons;
	batch_seasons(batch, &seasons);
	jsonWriter_beginObject(writer, "seasons");
	jsonWriter_unsigned(writer, "teams", plan->teamCount);
	jsonWriter_unsigned(writer, "fixtures", plan->armCount);
	jsonWriter_unsigned(writer, "first", plan->seeds.first);
	jsonWriter_unsigned(writer, "last", plan->seeds.last);
	jsonWriter_unsigned(writer, "block", plan->chunk);
	jsonWriter_unsigned(writer, "fixtures_built", batch->fixturesBuilt);
	jsonWriter_bool(writer, "frozen", batch->fixturesFrozen);
	jsonWriter_unsigned(writer, "blocks_complete", seasons.blocksComplete);
	jsonWriter_unsigned(writer, "seasons_complete", seasons.seasonsComplete);
	jsonWriter_unsigned(writer, "matches_missing", seasons.matchesMissing);
	jsonWriter_endObject(writer);
}

// Each session with its record, and those the committed chunks name, with how many chunks each
// worked on.
static void writeSessions(JsonWriter *writer, const Batch *batch) {
	SessionCount *sessions = NULL;
	size_t count = 0;
	for (size_t i = 0; i < batch->sessionCount; ++i) {
		const char *id = batch->sessions[i].id;
		if (countSession(&sessions, &count, id, strlen(id))) {
			sessions[count - 1].record = &batch->sessions[i];
		}
	}
	for (size_t i = 0; i < batch->committedCount; ++i) {
		for (const char *at = batch->committed[i].sessions; *at;) {
			size_t length = strcspn(at, ";");
			size_t s = 0;
			while (s < count &&
						 (strlen(sessions[s].id) != length || strncmp(sessions[s].id, at, length) != 0)) {
				++s;
			}
			if (s < count || countSession(&sessions, &count, at, length)) {
				++sessions[s].chunks;
			}
			at += length + (at[length] == ';');
		}
	}
	if (count) {
		qsort(sessions, count, sizeof *sessions, compareSessionCounts);
	}
	jsonWriter_beginArray(writer, "sessions");
	for (size_t s = 0; s < count; ++s) {
		jsonWriter_beginObject(writer, NULL);
		jsonWriter_string(writer, "id", sessions[s].id);
		if (sessions[s].record) {
			writeSessionMembers(writer, sessions[s].record);
		}
		jsonWriter_unsigned(writer, "chunks", sessions[s].chunks);
		jsonWriter_endObject(writer);
	}
	jsonWriter_endArray(writer);
	free(sessions);
}

// Adds a session of `length` characters to the list; false if it does not fit or memory runs out.
static bool countSession(SessionCount **sessions, size_t *count, const char *id, size_t length) {
	if (length >= BATCH_SESSION_LENGTH) {
		return false;
	}
	SessionCount *grown = realloc(*sessions, (*count + 1) * sizeof **sessions);
	if (!grown) {
		return false;
	}
	*sessions = grown;
	grown[*count] = (SessionCount){0};
	memcpy(grown[*count].id, id, length);
	++*count;
	return true;
}

static int compareSessionCounts(const void *a, const void *b) {
	return strcmp(((const SessionCount *)a)->id, ((const SessionCount *)b)->id);
}

// A session.json's members after its ID, as batch.json's sessions have them too.
static void writeSessionMembers(JsonWriter *writer, const BatchSession *session) {
	const RunReport *harness = session->hasHarness ? &session->harness : NULL;
	jsonWriter_utcTime(writer, "started_at", session->startedAt);
	writeOptionalText(writer, "reason", session->reason);
	if (session->pid) {
		jsonWriter_unsigned(writer, "pid", session->pid);
	} else {
		jsonWriter_null(writer, "pid");
	}
	if (harness && harness->loaded > 0) {
		jsonWriter_utcTime(writer, "loaded", (time_t)harness->loaded);
	} else {
		jsonWriter_null(writer, "loaded");
	}
	jsonWriter_bool(writer, "launched_by_batch", session->launchedByBatch);
	writeOptionalText(writer, "slice", harness ? harness->slice : NULL);
	if (harness) {
		jsonWriter_bool(writer, "translated", harness->translated);
		jsonWriter_beginObject(writer, "harness");
		jsonWriter_string(writer, "protocol", harness->protocol);
		jsonWriter_string(writer, "commit", harness->commit);
		if (harness->dirty < 0) {
			jsonWriter_null(writer, "dirty");
		} else {
			jsonWriter_bool(writer, "dirty", harness->dirty != 0);
		}
		jsonWriter_unsigned(writer, "build", harness->build);
		jsonWriter_endObject(writer);
	} else {
		jsonWriter_null(writer, "translated");
		jsonWriter_null(writer, "harness");
	}
	jsonWriter_bool(writer, "identity_changed", session->identityChanged);
	writeOptionalText(writer, "save", session->save);
	writeOptionalText(writer, "game_date", session->gameDate);
	jsonWriter_bool(writer, "warmup", session->warmedUp);
	writeOptionalText(writer, "canary", session->canary);
	jsonWriter_bool(writer, "nonrepeatable", session->nonrepeatable);
	jsonWriter_unsigned(writer, "jobs_at_start", session->jobsAtStart);
	jsonWriter_unsigned(writer, "foreign_jobs", session->foreignJobs);
}

// The harness the first session found, which later sessions must match.
static void writePinned(JsonWriter *writer, const Batch *batch) {
	const BatchSession *pinned = batch_pinnedSession(batch);
	if (!pinned) {
		jsonWriter_null(writer, "harness");
		return;
	}
	const RunReport *harness = &pinned->harness;
	jsonWriter_beginObject(writer, "harness");
	jsonWriter_string(writer, "session", pinned->id);
	jsonWriter_string(writer, "protocol", harness->protocol);
	jsonWriter_string(writer, "commit", harness->commit);
	if (harness->dirty < 0) {
		jsonWriter_null(writer, "dirty");
	} else {
		jsonWriter_bool(writer, "dirty", harness->dirty != 0);
	}
	jsonWriter_unsigned(writer, "build", harness->build);
	jsonWriter_string(writer, "slice", harness->slice);
	jsonWriter_endObject(writer);
}

bool batch_startSession(Batch *batch, char id[BATCH_SESSION_LENGTH]) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	uint32_t number = batch->lastSession + 1;
	(void)snprintf(id, BATCH_SESSION_LENGTH, "s%04u", number);
	bool ok = number && pathOf(batch, folder, "sessions") && platform_makeDirectory(folder) &&
						pathOf(batch, folder, "sessions/%s", id) && platform_makeDirectory(folder);
	if (ok) {
		batch->lastSession = number;
	}
	return ok;
}

bool batch_saveSession(Batch *batch, const BatchSession *session) {
	ByteBuffer text = {0};
	JsonWriter writer;
	jsonWriter_start(&writer, &text);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", sessionFormat);
	jsonWriter_string(&writer, "id", session->id);
	writeSessionMembers(&writer, session);
	jsonWriter_endObject(&writer);
	wchar_t path[HARNESS_PATH_LENGTH];
	bool ok = jsonWriter_finish(&writer) && buffer_append(&text, "\n", 1) &&
						batch_sessionPath(batch, session->id, "session.json", path) &&
						replaceText(batch, path, &text) && keepSession(batch, session);
	buffer_free(&text);
	return ok;
}

const BatchSession *batch_pinnedSession(const Batch *batch) {
	for (size_t i = 0; i < batch->sessionCount; ++i) {
		if (batch->sessions[i].hasHarness) {
			return &batch->sessions[i];
		}
	}
	return NULL;
}

bool batch_sessionPath(const Batch *batch, const char *id, const char *name, wchar_t *out) {
	return pathOf(batch, out, "sessions/%s/%s", id, name);
}

bool batch_isSuspect(const Batch *batch, size_t arm, uint64_t first, const char *reason) {
	wchar_t path[HARNESS_PATH_LENGTH];
	char start[BATCH_ARM_LENGTH + BATCH_CHUNK_ID_LENGTH + 4];
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkId(first, id);
	(void)snprintf(start, sizeof start, "%s,%s,", batch->plan.arms[arm].id, id);
	size_t startLength = strlen(start);
	size_t reasonLength = strlen(reason);
	ByteBuffer text = {0};
	bool found = false;
	if (pathOf(batch, path, "suspect.csv") && files_read(path, &text)) {
		const char *line = NULL;
		size_t length = 0;
		for (size_t at = 0; !found && nextLine(&text, &at, &line, &length);) {
			found = length > startLength + reasonLength && memcmp(line, start, startLength) == 0 &&
							line[length - reasonLength - 1] == ',' &&
							memcmp(line + length - reasonLength, reason, reasonLength) == 0;
		}
	}
	buffer_free(&text);
	return found;
}

// Written through to the disk, with the header if the file is new: run acts on a chunk 0 that
// differs from the reference until suspect.csv lists it.
bool batch_addSuspect(
	Batch *batch,
	size_t arm,
	uint64_t first,
	const char *session,
	const char *reason
) {
	wchar_t path[HARNESS_PATH_LENGTH];
	char line[SUSPECT_LENGTH];
	char id[BATCH_CHUNK_ID_LENGTH];
	if (!pathOf(batch, path, "suspect.csv")) {
		return false;
	}
	FILE *file = platform_openFile(path, "rb");
	if (file) {
		(void)fclose(file);
	}
	batchPlan_chunkId(first, id);
	(void)snprintf(
		line,
		sizeof line,
		"%s%s,%s,%s,%s\n",
		file ? "" : suspectHeader,
		batch->plan.arms[arm].id,
		id,
		session,
		reason
	);
	return appendLine(path, line, true, batch->host);
}

// The sessions/s<n> folders, and the session.json of each that has a readable one. A batch that
// has not started a session has no sessions folder.
static bool scanSessions(Batch *batch) {
	wchar_t folder[HARNESS_PATH_LENGTH];
	Listing top;
	if (!pathOf(batch, folder, "sessions") || !listFolder(batch->directory, &top)) {
		return false;
	}
	bool found = false;
	for (size_t i = 0; i < top.count; ++i) {
		found = found || (top.entries[i].isDirectory && wcscmp(top.entries[i].name, L"sessions") == 0);
	}
	free(top.entries);
	Listing entries;
	if (!found) {
		return true;
	}
	if (!listFolder(folder, &entries)) {
		return false;
	}
	bool ok = true;
	for (size_t i = 0; ok && i < entries.count; ++i) {
		char name[NAME_LENGTH * 4];
		uint32_t number = 0;
		wchar_t session[HARNESS_PATH_LENGTH];
		wchar_t path[HARNESS_PATH_LENGTH];
		JsonFlat flat = {0};
		BatchSession record = {0};
		if (!entries.entries[i].isDirectory || !entryName(&entries.entries[i], name, sizeof name) ||
				!readSessionNumber(name, &number)) {
			continue;
		}
		batch->lastSession = number > batch->lastSession ? number : batch->lastSession;
		if (joinWide(session, folder, entries.entries[i].name) &&
				joinName(path, session, "session.json") && readJson(path, &flat) &&
				readSession(&flat, &record) && strcmp(record.id, name) == 0) {
			ok = keepSession(batch, &record);
		}
		jsonFlat_free(&flat);
	}
	free(entries.entries);
	return ok;
}

// "s" and a session's number, 1 or more without a leading zero beyond four digits.
static bool readSessionNumber(const char *name, uint32_t *number) {
	size_t digits = strspn(name + 1, "0123456789");
	if (name[0] != 's' || digits < 4 || digits > 9 || name[1 + digits]) {
		return false;
	}
	*number = (uint32_t)strtoul(name + 1, NULL, 10);
	return *number > 0;
}

static bool readSession(const JsonFlat *flat, BatchSession *session) {
	char format[32];
	uint64_t value = 0;
	bool ok = jsonFlat_string(flat, "format", format, sizeof format) &&
						strcmp(format, sessionFormat) == 0 &&
						jsonFlat_string(flat, "id", session->id, sizeof session->id) &&
						jsonFlat_time(flat, "started_at", &session->startedAt);
	if (!ok) {
		return false;
	}
	(void)jsonFlat_string(flat, "reason", session->reason, sizeof session->reason);
	session->pid =
		jsonFlat_unsigned(flat, "pid", &value) && value <= UINT32_MAX ? (uint32_t)value : 0;
	(void)jsonFlat_bool(flat, "launched_by_batch", &session->launchedByBatch);
	RunReport *harness = &session->harness;
	session->hasHarness =
		jsonFlat_string(flat, "harness.protocol", harness->protocol, sizeof harness->protocol) &&
		jsonFlat_string(flat, "harness.commit", harness->commit, sizeof harness->commit) &&
		jsonFlat_unsigned(flat, "harness.build", &value) && value <= UINT32_MAX &&
		jsonFlat_string(flat, "slice", harness->slice, sizeof harness->slice);
	harness->build = session->hasHarness ? (uint32_t)value : 0;
	bool flag = false;
	harness->dirty = jsonFlat_bool(flat, "harness.dirty", &flag) ? flag : -1;
	(void)jsonFlat_bool(flat, "translated", &harness->translated);
	time_t loaded = 0;
	harness->loaded = jsonFlat_time(flat, "loaded", &loaded) ? (int64_t)loaded : 0;
	(void)jsonFlat_bool(flat, "identity_changed", &session->identityChanged);
	(void)jsonFlat_string(flat, "save", session->save, sizeof session->save);
	(void)jsonFlat_string(flat, "game_date", session->gameDate, sizeof session->gameDate);
	(void)jsonFlat_bool(flat, "warmup", &session->warmedUp);
	(void)jsonFlat_string(flat, "canary", session->canary, sizeof session->canary);
	(void)jsonFlat_bool(flat, "nonrepeatable", &session->nonrepeatable);
	(void)jsonFlat_unsigned(flat, "jobs_at_start", &session->jobsAtStart);
	(void)jsonFlat_unsigned(flat, "foreign_jobs", &session->foreignJobs);
	return true;
}

// Adds a session's record, or replaces the one with its ID, keeping them ordered by ID.
static bool keepSession(Batch *batch, const BatchSession *session) {
	size_t at = 0;
	while (at < batch->sessionCount && strcmp(batch->sessions[at].id, session->id) < 0) {
		++at;
	}
	if (at < batch->sessionCount && strcmp(batch->sessions[at].id, session->id) == 0) {
		batch->sessions[at] = *session;
		return true;
	}
	if (batch->sessionCount == batch->sessionCapacity) {
		size_t capacity = batch->sessionCapacity ? batch->sessionCapacity * 2 : 8;
		BatchSession *grown = realloc(batch->sessions, capacity * sizeof *grown);
		if (!grown) {
			return false;
		}
		batch->sessions = grown;
		batch->sessionCapacity = capacity;
	}
	memmove(
		&batch->sessions[at + 1],
		&batch->sessions[at],
		(batch->sessionCount - at) * sizeof *batch->sessions
	);
	batch->sessions[at] = *session;
	++batch->sessionCount;
	return true;
}

static void writeProgress(JsonWriter *writer, const BatchProgress *progress) {
	jsonWriter_beginObject(writer, "progress");
	jsonWriter_unsigned(writer, "planned", progress->planned);
	jsonWriter_unsigned(writer, "done", progress->done);
	jsonWriter_unsigned(writer, "in_parts", progress->inParts);
	jsonWriter_decimal(
		writer,
		"percent",
		progress->planned ? 100.0 * (double)progress->done / (double)progress->planned : 0.0,
		1
	);
	jsonWriter_unsigned(writer, "chunks_done", progress->chunksDone);
	jsonWriter_unsigned(writer, "chunks_total", progress->chunks);
	if (progress->rate > 0) {
		jsonWriter_decimal(writer, "matches_per_s", progress->rate, 2);
	} else {
		jsonWriter_null(writer, "matches_per_s");
	}
	jsonWriter_unsigned(writer, "rate_chunks", progress->rateChunks);
	if (progress->hasEta) {
		jsonWriter_utcTime(writer, "eta", progress->eta);
	} else {
		jsonWriter_null(writer, "eta");
	}
	jsonWriter_endObject(writer);
}

static void writeOptionalText(JsonWriter *writer, const char *key, const char *text) {
	jsonWriter_string(writer, key, text && text[0] ? text : NULL);
}

static void writeTeam(JsonWriter *writer, const char *key, uint32_t team) {
	if (team) {
		jsonWriter_unsigned(writer, key, team);
	} else {
		jsonWriter_null(writer, key);
	}
}

// A file chunk.json lists: there, with the size and SHA-256 it records.
static bool verifyListedFile(
	const JsonFlat *flat,
	const wchar_t *folder,
	const char *name,
	FILE *report,
	const char *label
) {
	char key[KEY_LENGTH];
	char expected[SHA256_HEX_LENGTH];
	char actual[SHA256_HEX_LENGTH];
	uint64_t bytes = 0;
	uint64_t size = 0;
	wchar_t path[HARNESS_PATH_LENGTH];
	int at = fileIndex(flat, name);
	if (at < 0) {
		fprintf(report, "chunk %s: chunk.json does not list %s\n", label, name);
		return false;
	}
	(void)snprintf(key, sizeof key, "files.%d.sha256", at);
	bool listed = jsonFlat_string(flat, key, expected, sizeof expected);
	(void)snprintf(key, sizeof key, "files.%d.bytes", at);
	listed = jsonFlat_unsigned(flat, key, &bytes) && listed;
	if (!joinName(path, folder, name) || !hashFile(path, &size, actual)) {
		fprintf(report, "chunk %s: %s cannot be read\n", label, name);
		return false;
	}
	if (!listed || size != bytes || strcmp(actual, expected) != 0) {
		fprintf(
			report, "chunk %s: %s does not match its size and SHA-256 in chunk.json\n", label, name
		);
		return false;
	}
	return true;
}

// Decompresses the chunk's files, verifies their rows cover the chunk exactly once, and checks
// their contents against chunk.json.
static bool verifyDeep(
	Batch *batch,
	const BatchCommitted *chunk,
	const JsonFlat *flat,
	const wchar_t *folder,
	FILE *report,
	const char *label
) {
	const BatchPlan *plan = &batch->plan;
	bool gzip = plan->compress == BATCH_COMPRESS_GZIP;
	BatchVerified verified;
	if (!readChunkFiles(batch, chunk, flat, folder, &verified)) {
		fprintf(
			report, "chunk %s: its files cannot be read%s\n", label, gzip ? " or decompressed" : ""
		);
		return false;
	}
	bool ok = verified.verified == chunk->count && !verified.trailing;
	if (!ok) {
		fprintf(
			report,
			"chunk %s: its rows cover %llu of its %llu indices%s\n",
			label,
			(unsigned long long)verified.verified,
			(unsigned long long)chunk->count,
			verified.trailing ? ", with rows past them" : ""
		);
	}
	bool expected[BATCH_ROLES] = {
		true, plan->perMatch != 0, plan->perMatch != 0, plan->seedErrors == BATCH_SEED_ERRORS_SKIP};
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		char name[32];
		(void)snprintf(name, sizeof name, "%s%s", chunkFiles[role], gzip ? ".gz" : "");
		int at = fileIndex(flat, name);
		if (expected[role] && at >= 0 &&
				!sameContents(flat, at, &verified.inputs[role], chunk->rows[role], role == BATCH_RESULTS)) {
			fprintf(report, "chunk %s: the contents of %s do not match chunk.json\n", label, name);
			ok = false;
		}
	}
	return ok;
}

// Reads a committed chunk's files to their ends, decompressed, verifying that their rows cover it
// (with its exclusions from chunk.json). False if one cannot be read.
static bool readChunkFiles(
	Batch *batch,
	const BatchCommitted *chunk,
	const JsonFlat *flat,
	const wchar_t *folder,
	BatchVerified *verified
) {
	const BatchPlan *plan = &batch->plan;
	const BatchHost *host = batch->host;
	bool gzip = plan->compress == BATCH_COMPRESS_GZIP;
	uint64_t *excluded = malloc((chunk->excluded + 1) * sizeof *excluded);
	if (!excluded) {
		return false;
	}
	for (uint64_t i = 0; i < chunk->excluded; ++i) {
		char key[KEY_LENGTH];
		(void)snprintf(key, sizeof key, "excluded.%llu.index", (unsigned long long)i);
		(void)jsonFlat_unsigned(flat, key, &excluded[i]);
	}
	BatchSeeds seeds = batchPlan_armSeeds(plan, chunk->arm);
	BatchVerify request = {
		.seeds = &seeds,
		.first = chunk->first,
		.count = chunk->count,
		.chunk = true,
		.teams = {plan->arms[chunk->arm].pair[0], plan->arms[chunk->arm].pair[1]},
		.perMatch = plan->perMatch != 0,
		.failed = plan->seedErrors == BATCH_SEED_ERRORS_SKIP,
		.excluded = excluded,
		.excludedCount = (size_t)chunk->excluded,
		.hashInputs = true,
	};
	bool expected[BATCH_ROLES] = {true, request.perMatch, request.perMatch, request.failed};
	void *handles[BATCH_ROLES] = {0};
	bool ok = true;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		char name[32];
		wchar_t path[HARNESS_PATH_LENGTH];
		(void)snprintf(name, sizeof name, "%s%s", chunkFiles[role], gzip ? ".gz" : "");
		if (expected[role] && joinName(path, folder, name)) {
			request.inputs[role] = gzip ? host->openDecompressed(host->context, path, &handles[role])
																	: platform_openFile(path, "rb");
		}
		ok = ok && (!expected[role] || request.inputs[role]);
	}
	ok = ok && batchVerify_run(&request, verified);
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		FILE *input = request.inputs[role];
		if (input) {
			ok = (gzip ? host->closeDecompressed(host->context, input, handles[role])
								 : fclose(input) == 0) &&
					 ok;
		}
	}
	free(excluded);
	return ok;
}

// Whether a file read to its end has the size, rows and hashes chunk.json's files[at] records.
static bool
sameContents(const JsonFlat *flat, int at, const BatchInput *input, uint64_t rows, bool results) {
	char key[KEY_LENGTH];
	char text[SHA256_HEX_LENGTH];
	uint64_t value = 0;
	(void)snprintf(key, sizeof key, "files.%d.raw_bytes", at);
	bool same = jsonFlat_unsigned(flat, key, &value) && value == input->bytes;
	(void)snprintf(key, sizeof key, "files.%d.rows", at);
	same = same && jsonFlat_unsigned(flat, key, &value) && value == input->rows && value == rows;
	(void)snprintf(key, sizeof key, "files.%d.raw_sha256", at);
	same = same && jsonFlat_string(flat, key, text, sizeof text) && strcmp(text, input->sha256) == 0;
	if (results) {
		(void)snprintf(key, sizeof key, "files.%d.content_sha256", at);
		same = same && jsonFlat_string(flat, key, text, sizeof text) &&
					 strcmp(text, input->contentSha256) == 0;
	}
	return same;
}

// The position of the file named `name` in chunk.json's "files"; -1 if it is not there.
static int fileIndex(const JsonFlat *flat, const char *name) {
	for (int i = 0; i < 16; ++i) {
		char key[KEY_LENGTH];
		char listed[32];
		(void)snprintf(key, sizeof key, "files.%d.name", i);
		if (!jsonFlat_string(flat, key, listed, sizeof listed)) {
			return -1;
		}
		if (strcmp(listed, name) == 0) {
			return i;
		}
	}
	return -1;
}
