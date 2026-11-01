// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_plan.h"

#include "batch_season.h"
#include "platform.h"
#include "run_job.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32
#define SEPARATOR "\\"
#else
#define SEPARATOR "/"
#endif

enum {
	LINE_LENGTH = BATCH_PATH_LENGTH + 128, // the longest plan line
	LIST_LIMIT = 100000000,                // the most seeds a list may have
	PERCENT_SCALE = 1000,                  // max_seed_errors keeps thousandths of a percent
	FIXTURE_TOKENS = 5, // fixture=<arm> json_sha256= bin_sha256= home_team= away_team=
	// A season's defaults: seasons per chunk, the canary's seasons, a count of failed seeds.
	SEASON_CHUNK = 100,
	SEASON_CANARY = 1,
	SEASON_SEED_ERRORS = 5,
};

static const char planFormat[] = "fm24-batch-plan/1";
static const char hashKey[] = "plan_sha256=";

typedef enum SettingKind {
	SETTING_NUMBER,
	SETTING_CHOICE,
	SETTING_LIMIT, // max_seed_errors
} SettingKind;

// A setting: a uint32_t field of BatchPlan (but the limit), its bounds or names, and its default.
typedef struct Setting {
	const char *name;
	size_t offset;
	const char *const *choices; // in the order of the field's values; NULL-terminated
	SettingKind kind;
	uint32_t low;
	uint32_t high;
	uint32_t fallback;
} Setting;

static const char *const flagNames[] = {"0", "1", NULL};
static const char *const seedErrorNames[] = {"stop", "skip", NULL};
static const char *const orderNames[] = {"interleave", "sequential", NULL};
static const char *const compressNames[] = {"none", "gzip", NULL};
static const char *const mismatchNames[] = {"stop", "continue", NULL};
static const char *const sliceNames[] = {"arm64", "x86_64", NULL};

// In the order the frozen plan lists them: name, field, names, kind, bounds and default. The
// defaults of threads and compress are the computer's.
static const Setting settings[] = {
	{"chunk", offsetof(BatchPlan, chunk), NULL, SETTING_NUMBER, 1, BATCH_CHUNK_LIMIT, 1000},
	{"threads", offsetof(BatchPlan, threads), NULL, SETTING_NUMBER, 1, HARNESS_MAX_THREADS, 0},
	{"per_match", offsetof(BatchPlan, perMatch), flagNames, SETTING_CHOICE, 0, 0, 1},
	{"keep_build", offsetof(BatchPlan, keepBuild), flagNames, SETTING_CHOICE, 0, 0, 0},
	{"human_control", offsetof(BatchPlan, humanControl), flagNames, SETTING_CHOICE, 0, 0, 0},
	{"seed_errors",
		offsetof(BatchPlan, seedErrors),
		seedErrorNames,
		SETTING_CHOICE,
		0,
		0,
		BATCH_SEED_ERRORS_SKIP},
	{"max_seed_errors", offsetof(BatchPlan, maxSeedErrors), NULL, SETTING_LIMIT, 0, 0, 0},
	{"order", offsetof(BatchPlan, order), orderNames, SETTING_CHOICE, 0, 0, BATCH_ORDER_INTERLEAVE},
	{"compress", offsetof(BatchPlan, compress), compressNames, SETTING_CHOICE, 0, 0, 0},
	{"warmup", offsetof(BatchPlan, warmup), NULL, SETTING_NUMBER, 0, 1000, 1},
	{"canary", offsetof(BatchPlan, canary), NULL, SETTING_NUMBER, 0, 1000, 3},
	{"canary_every", offsetof(BatchPlan, canaryEvery), NULL, SETTING_NUMBER, 1, 1000000, 50},
	{"on_canary_mismatch",
		offsetof(BatchPlan, onCanaryMismatch),
		mismatchNames,
		SETTING_CHOICE,
		0,
		0,
		BATCH_MISMATCH_STOP},
	{"slice", offsetof(BatchPlan, slice), sliceNames, SETTING_CHOICE, 0, 0, BATCH_SLICE_ARM64},
	{"launch", offsetof(BatchPlan, launch), flagNames, SETTING_CHOICE, 0, 0, 1},
	{"any_save", offsetof(BatchPlan, anySave), flagNames, SETTING_CHOICE, 0, 0, 0},
	{"kill_game", offsetof(BatchPlan, killGame), flagNames, SETTING_CHOICE, 0, 0, 0},
	{"hang_minutes", offsetof(BatchPlan, hangMinutes), NULL, SETTING_NUMBER, 1, 1440, 10},
	{"queue_wait_minutes",
		offsetof(BatchPlan, queueWaitMinutes),
		NULL,
		SETTING_NUMBER,
		1,
		10080,
		360},
	{"busy_wait_minutes", offsetof(BatchPlan, busyWaitMinutes), NULL, SETTING_NUMBER, 1, 1440, 15},
	{"max_crashes_per_hour",
		offsetof(BatchPlan, maxCrashesPerHour),
		NULL,
		SETTING_NUMBER,
		0,
		1000,
		4},
	{"max_poison", offsetof(BatchPlan, maxPoison), NULL, SETTING_NUMBER, 0, 1000, 3},
	{"pause_on_battery", offsetof(BatchPlan, pauseOnBattery), flagNames, SETTING_CHOICE, 0, 0, 1},
	{"min_free_gb", offsetof(BatchPlan, minFreeGb), NULL, SETTING_NUMBER, 0, 1000000, 20},
	{"notify", offsetof(BatchPlan, notify), flagNames, SETTING_CHOICE, 0, 0, 1},
	{"trim_game_logs", offsetof(BatchPlan, trimGameLogs), flagNames, SETTING_CHOICE, 0, 0, 1},
};
enum { SETTINGS = sizeof settings / sizeof settings[0] };

// The keys other than settings, as bits of PlanReader.given.
typedef enum PlanKey {
	KEY_FORMAT,
	KEY_NAME,
	KEY_SEEDS,
	KEY_SAVE,
	KEY_MODE,
	KEY_BASE,
	KEY_SEEDS_SHA256, // frozen only, from here on
	KEY_SAVE_NAME,
	KEY_GAME_BUILD,
	KEY_GAME_DATE,
	KEY_BASE_FIXTURE,
	KEYS,
} PlanKey;

static const char *const keyNames[KEYS] = {
	"format",
	"name",
	"seeds",
	"save",
	"mode",
	"base",
	"seeds_sha256",
	"save_name",
	"game_build",
	"game_date",
	"base_fixture",
};

// A plan being read.
typedef struct PlanReader {
	BatchPlan *plan;
	bool frozen;
	unsigned line;
	uint64_t settingsGiven; // bit i: settings[i]
	unsigned keysGiven;     // bit k: PlanKey k
	uint32_t fixtures;      // bit a: arm a has its fixture line
	size_t pairs;           // a frozen season's pair lines so far
	size_t pairTeams;       // and its teams when the first was read
	char *error;
	size_t errorSize;
} PlanReader;

static bool fail(PlanReader *reader, const char *format, ...) PLATFORM_PRINTF(2, 3);
static bool readLines(PlanReader *reader, const char *text, size_t length);
static bool readLine(PlanReader *reader, char *line);
static bool readEntry(PlanReader *reader, const char *key, char *value);
static bool readSetting(PlanReader *reader, const Setting *setting, const char *value);
static bool readKey(PlanReader *reader, PlanKey key, char *value);
static bool
copyText(PlanReader *reader, const char *value, char *out, size_t size, const char *key);
static bool readArm(PlanReader *reader, char *value);
static bool readTeam(PlanReader *reader, const char *value);
static bool readPair(PlanReader *reader, const char *value);
static bool readSeeds(PlanReader *reader, const char *value);
static bool readFixture(PlanReader *reader, char *value);
static bool readLimit(const char *text, BatchErrorLimit *limit);
static bool checkComplete(PlanReader *reader);
static bool checkSeason(PlanReader *reader);
static bool settingGiven(const PlanReader *reader, const char *name);
static bool makeFixtures(BatchPlan *plan);
static bool checkHash(const char *text, size_t length, size_t *body, char hex[SHA256_HEX_LENGTH]);
static const char *readDecimal(const char *text, uint64_t max, uint64_t *value);
static bool readWhole(const char *text, uint64_t max, uint64_t *value);
static bool isName(const char *text, size_t longest, const char *extra);
static bool isHex(const char *text);
static char *trim(char *text);
static void appendText(ByteBuffer *out, const char *text, bool *ok);
static void appendLine(ByteBuffer *out, const char *key, const char *value, bool *ok);
static void formatSetting(const BatchPlan *plan, const Setting *setting, char *out, size_t size);
static void formatSeedsSpec(const BatchPlan *plan, char *out, size_t size);
static void formatTeam(uint32_t team, char *out, size_t size);
static bool formatPartJob(
	const BatchPlan *plan,
	const uint32_t teams[2],
	const BatchPartPaths *paths,
	const char *seeds,
	uint64_t indexBase,
	uint32_t threads,
	char *out,
	size_t size
);
static bool appendPath(char *out, size_t size, size_t *length, const char *text);
static int compareSeeds(const void *a, const void *b);
static uint32_t lowbias32(uint32_t x);
static uint64_t splitmix64(uint64_t seed);
static size_t jobBase(const BatchPlan *plan, const char *directory);
static size_t seedsLength(const BatchPlan *plan, uint32_t chunk);

uint32_t batchSeeds_at(const BatchSeeds *seeds, uint64_t index) {
	switch (seeds->kind) {
		case BATCH_SEEDS_SCATTER: return batchSeeds_scatter(seeds->key, (uint32_t)index);
		case BATCH_SEEDS_LIST: return seeds->list[index];
		case BATCH_SEEDS_SEASON:
			return batchSeeds_scatter(seeds->key, (uint32_t)(index * seeds->stride + seeds->offset));
		default: return (uint32_t)index;
	}
}

uint32_t batchSeeds_scatter(uint64_t key, uint32_t index) {
	uint64_t mixed = splitmix64(key);
	return lowbias32(lowbias32(index ^ (uint32_t)mixed) ^ (uint32_t)(mixed >> 32));
}

uint64_t batchSeeds_count(const BatchSeeds *seeds) {
	return seeds->last - seeds->first + 1;
}

bool batchSeeds_readList(
	const char *text,
	size_t length,
	uint32_t **seeds,
	size_t *count,
	char *error,
	size_t errorSize
) {
	*seeds = NULL;
	*count = 0;
	size_t lines = 0;
	for (size_t i = 0; i < length; ++i) {
		lines += text[i] == '\n' || (i + 1 == length && text[i] != '\n');
	}
	if (!lines || lines > LIST_LIMIT) {
		(void)snprintf(error, errorSize, "a seed list needs 1-%d seeds", LIST_LIMIT);
		return false;
	}
	uint32_t *list = malloc(lines * sizeof *list);
	uint32_t *sorted = malloc(lines * sizeof *sorted);
	bool ok = list && sorted;
	if (!ok) {
		(void)snprintf(error, errorSize, "out of memory");
	}
	const char *position = text;
	for (size_t line = 0; ok && line < lines; ++line) {
		const char *end = memchr(position, '\n', length - (size_t)(position - text));
		size_t size = end ? (size_t)(end - position) : length - (size_t)(position - text);
		size -= size && position[size - 1] == '\r';
		uint64_t value = 0;
		const char *after = size ? readDecimal(position, UINT32_MAX, &value) : NULL;
		if (!after || after != position + size) {
			(void)snprintf(error, errorSize, "line %zu is not a seed from 0 to 4294967295", line + 1);
			ok = false;
		}
		list[line] = (uint32_t)value;
		position = end ? end + 1 : text + length;
	}
	if (ok) {
		memcpy(sorted, list, lines * sizeof *list);
		qsort(sorted, lines, sizeof *sorted, compareSeeds);
	}
	for (size_t i = 1; ok && i < lines; ++i) {
		if (sorted[i] == sorted[i - 1]) {
			(void)snprintf(error, errorSize, "seed %u is listed more than once", sorted[i]);
			ok = false;
		}
	}
	free(sorted);
	if (!ok) {
		free(list);
		return false;
	}
	*seeds = list;
	*count = lines;
	return true;
}

bool batchSeeds_format(
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t last,
	char *out,
	size_t size
) {
	int written = 0;
	if (seeds->kind == BATCH_SEEDS_RANGE) {
		written =
			first == last
				? snprintf(out, size, "%llu", (unsigned long long)first)
				: snprintf(out, size, "%llu-%llu", (unsigned long long)first, (unsigned long long)last);
		return written > 0 && (size_t)written < size;
	}
	size_t length = 0;
	for (uint64_t index = first; index <= last; ++index) {
		written = snprintf(
			out + length, size - length, "%s%u", index == first ? "" : ",", batchSeeds_at(seeds, index)
		);
		if (written <= 0 || (size_t)written >= size - length) {
			return false;
		}
		length += (size_t)written;
	}
	return true;
}

void batchSeeds_hash(
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t last,
	char hex[SHA256_HEX_LENGTH]
) {
	Sha256 hash;
	sha256_start(&hash);
	for (uint64_t index = first; index <= last; ++index) {
		char line[16];
		int written = snprintf(line, sizeof line, "%u\n", batchSeeds_at(seeds, index));
		sha256_add(&hash, line, (size_t)written);
	}
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_finish(&hash, digest);
	sha256_hex(digest, hex);
}

bool batchPlan_parse(
	const char *text,
	size_t length,
	bool frozen,
	const BatchDefaults *defaults,
	BatchPlan *plan,
	char *error,
	size_t errorSize
) {
	memset(plan, 0, sizeof *plan);
	for (size_t i = 0; i < SETTINGS; ++i) {
		if (settings[i].kind != SETTING_LIMIT) {
			*(uint32_t *)((char *)plan + settings[i].offset) = settings[i].fallback;
		}
	}
	plan->threads = defaults->threads;
	plan->compress = defaults->compress;
	plan->maxSeedErrors = (BatchErrorLimit){true, PERCENT_SCALE};
	PlanReader reader = {.plan = plan, .frozen = frozen, .error = error, .errorSize = errorSize};
	error[0] = 0;
	size_t body = length;
	if (memchr(text, 0, length)) {
		return fail(&reader, "the plan is not text");
	}
	if (frozen && !checkHash(text, length, &body, plan->planSha256)) {
		return fail(&reader, "plan.txt has been changed: its last line is not the hash of the rest");
	}
	if (readLines(&reader, text, body) && checkComplete(&reader)) {
		return true;
	}
	batchPlan_free(plan);
	return false;
}

void batchPlan_free(BatchPlan *plan) {
	free(plan->arms);
	plan->arms = NULL;
	plan->armCount = 0;
}

bool batchPlan_format(BatchPlan *plan, ByteBuffer *out) {
	size_t start = out->length;
	bool ok = true;
	char value[BATCH_PATH_LENGTH + 64];
	appendLine(out, "format", planFormat, &ok);
	appendText(
		out,
		"# Frozen by fm24ctl batch create; do not edit. The last line is the SHA-256 of the lines "
		"above it.\n",
		&ok
	);
	appendLine(out, "name", plan->name, &ok);
	bool season = plan->mode == BATCH_MODE_SEASON;
	if (season) {
		appendLine(out, "mode", "season", &ok);
		appendLine(out, "base", plan->base.path, &ok);
		for (size_t i = 0; i < plan->teamCount; ++i) {
			(void)snprintf(value, sizeof value, "%u", plan->teams[i]);
			appendLine(out, "team", value, &ok);
		}
	}
	for (size_t i = 0; !season && i < plan->armCount; ++i) {
		(void)snprintf(value, sizeof value, "%s %s", plan->arms[i].id, plan->arms[i].path);
		appendLine(out, "arm", value, &ok);
	}
	formatSeedsSpec(plan, value, sizeof value);
	appendLine(out, "seeds", value, &ok);
	for (size_t i = 0; i < SETTINGS; ++i) {
		formatSetting(plan, &settings[i], value, sizeof value);
		appendLine(out, settings[i].name, value, &ok);
	}
	appendLine(out, "save", plan->save, &ok);
	char hashes[2 * SHA256_HEX_LENGTH + 64];
	if (season) {
		batchPlan_formatHashes(&plan->base, hashes, sizeof hashes);
		appendLine(out, "base_fixture", hashes, &ok);
	}
	for (size_t i = 0; season && i < plan->armCount; ++i) {
		const BatchArm *arm = &plan->arms[i];
		(void)snprintf(
			value, sizeof value, "%s home_team=%u away_team=%u", arm->id, arm->pair[0], arm->pair[1]
		);
		appendLine(out, "pair", value, &ok);
	}
	for (size_t i = 0; !season && i < plan->armCount; ++i) {
		batchPlan_formatHashes(&plan->arms[i], hashes, sizeof hashes);
		(void)snprintf(value, sizeof value, "%s %s", plan->arms[i].id, hashes);
		appendLine(out, "fixture", value, &ok);
	}
	if (plan->seeds.kind == BATCH_SEEDS_LIST) {
		appendLine(out, "seeds_sha256", plan->listSha256, &ok);
	}
	appendLine(out, "save_name", plan->saveName, &ok);
	(void)snprintf(value, sizeof value, "%u", plan->gameBuild);
	appendLine(out, "game_build", value, &ok);
	appendLine(out, "game_date", plan->gameDate, &ok);
	if (ok) {
		sha256_hexOf(out->data + start, out->length - start, plan->planSha256);
		appendLine(out, "plan_sha256", plan->planSha256, &ok);
	}
	return ok;
}

bool batchPlan_isFrozen(const char *text, size_t length) {
	size_t start = length > 1 ? length - 1 : 0;
	while (start && text[start - 1] != '\n') {
		--start;
	}
	return length && text[length - 1] == '\n' && length - start > sizeof hashKey - 1 &&
				 memcmp(text + start, hashKey, sizeof hashKey - 1) == 0;
}

void batchPlan_formatLimit(const BatchErrorLimit *limit, char *out, size_t size) {
	if (!limit->percent) {
		(void)snprintf(out, size, "%llu", (unsigned long long)limit->value);
		return;
	}
	unsigned long long whole = limit->value / PERCENT_SCALE;
	unsigned fraction = (unsigned)(limit->value % PERCENT_SCALE);
	if (!fraction) {
		(void)snprintf(out, size, "%llu%%", whole);
		return;
	}
	int digits = 3;
	for (; fraction % 10 == 0; fraction /= 10) {
		--digits;
	}
	(void)snprintf(out, size, "%llu.%0*u%%", whole, digits, fraction);
}

bool batchPlan_readHashes(char *text, BatchArm *arm) {
	static const char *const names[FIXTURE_TOKENS - 1] = {
		"json_sha256=", "bin_sha256=", "home_team=", "away_team="};
	char *tokens[FIXTURE_TOKENS - 1] = {0};
	size_t count = 0;
	for (char *token = text; token && *token && count < FIXTURE_TOKENS - 1; ++count) {
		tokens[count] = token;
		token = strchr(token, ' ');
		if (token) {
			*token++ = 0;
		}
	}
	bool ok = count == FIXTURE_TOKENS - 1;
	for (size_t i = 0; ok && i < FIXTURE_TOKENS - 1; ++i) {
		ok = strncmp(tokens[i], names[i], strlen(names[i])) == 0 && !strchr(tokens[i], ' ');
		tokens[i] += ok ? strlen(names[i]) : 0;
	}
	ok = ok && isHex(tokens[0]) && isHex(tokens[1]);
	uint32_t teams[2] = {0};
	for (size_t side = 0; ok && side < 2; ++side) {
		uint64_t team = 0;
		const char *value = tokens[2 + side];
		ok = strcmp(value, "-") == 0 || (readWhole(value, UINT32_MAX, &team) && team != 0);
		teams[side] = (uint32_t)team;
	}
	if (ok) {
		strcpy(arm->jsonSha256, tokens[0]);
		strcpy(arm->binSha256, tokens[1]);
		arm->teams[0] = teams[0];
		arm->teams[1] = teams[1];
	}
	return ok;
}

void batchPlan_formatHashes(const BatchArm *arm, char *out, size_t size) {
	char home[16];
	char away[16];
	formatTeam(arm->teams[0], home, sizeof home);
	formatTeam(arm->teams[1], away, sizeof away);
	(void)snprintf(
		out,
		size,
		"json_sha256=%s bin_sha256=%s home_team=%s away_team=%s",
		arm->jsonSha256,
		arm->binSha256,
		home,
		away
	);
}

BatchSeeds batchPlan_armSeeds(const BatchPlan *plan, size_t arm) {
	BatchSeeds seeds = plan->seeds;
	if (seeds.kind == BATCH_SEEDS_SEASON) {
		seeds.offset = arm;
	}
	return seeds;
}

bool batchPlan_isCheckArm(const BatchPlan *plan, size_t arm) {
	return plan->mode != BATCH_MODE_SEASON || batchSeason_isCheckFixture(plan->teamCount, arm);
}

uint64_t batchPlan_chunksPerArm(const BatchPlan *plan) {
	return (batchSeeds_count(&plan->seeds) - 1) / plan->chunk + 1;
}

void batchPlan_chunkRange(const BatchPlan *plan, uint64_t chunk, uint64_t *first, uint64_t *last) {
	*first = plan->seeds.first + chunk * plan->chunk;
	uint64_t left = plan->seeds.last - *first;
	*last = left < plan->chunk ? plan->seeds.last : *first + plan->chunk - 1;
}

bool batchPlan_chunkAt(const BatchPlan *plan, uint64_t first, uint64_t *chunk) {
	if (first < plan->seeds.first || first > plan->seeds.last ||
			(first - plan->seeds.first) % plan->chunk) {
		return false;
	}
	*chunk = (first - plan->seeds.first) / plan->chunk;
	return true;
}

void batchPlan_scheduled(const BatchPlan *plan, uint64_t position, size_t *arm, uint64_t *chunk) {
	if (plan->order == BATCH_ORDER_SEQUENTIAL) {
		uint64_t chunks = batchPlan_chunksPerArm(plan);
		*arm = (size_t)(position / chunks);
		*chunk = position % chunks;
	} else {
		*arm = (size_t)(position % plan->armCount);
		*chunk = position / plan->armCount;
	}
}

void batchPlan_chunkId(uint64_t first, char id[BATCH_CHUNK_ID_LENGTH]) {
	(void)snprintf(id, BATCH_CHUNK_ID_LENGTH, "%010llu", (unsigned long long)first);
}

bool batchPlan_partPaths(
	const char *directory,
	const char *arm,
	uint64_t first,
	uint32_t attempt,
	BatchPartPaths *out
) {
	char chunk[BATCH_CHUNK_ID_LENGTH];
	char number[16];
	batchPlan_chunkId(first, chunk);
	(void)snprintf(number, sizeof number, "a%u", attempt);
	size_t input = 0;
	size_t output = 0;
	size_t stop = 0;
	const char *const outputParts[] = {
		directory,
		SEPARATOR "attempts" SEPARATOR,
		arm,
		SEPARATOR,
		chunk,
		SEPARATOR,
		number,
		SEPARATOR "results.csv",
	};
	bool ok = appendPath(out->input, sizeof out->input, &input, directory) &&
						appendPath(out->input, sizeof out->input, &input, SEPARATOR "inputs" SEPARATOR) &&
						appendPath(out->input, sizeof out->input, &input, arm) &&
						appendPath(out->input, sizeof out->input, &input, ".bin") &&
						appendPath(out->stopFile, sizeof out->stopFile, &stop, directory) &&
						appendPath(
							out->stopFile, sizeof out->stopFile, &stop, SEPARATOR "control" SEPARATOR "stop-now"
						);
	for (size_t i = 0; ok && i < sizeof outputParts / sizeof outputParts[0]; ++i) {
		ok = appendPath(out->output, sizeof out->output, &output, outputParts[i]);
	}
	return ok;
}

bool batchPlan_formatJob(
	const BatchPlan *plan,
	size_t arm,
	const BatchPartPaths *paths,
	const char *seeds,
	uint64_t indexBase,
	uint32_t threads,
	char *out,
	size_t size
) {
	return formatPartJob(plan, plan->arms[arm].teams, paths, seeds, indexBase, threads, out, size);
}

size_t batchPlan_longestJob(const BatchPlan *plan, const char *directory, uint32_t chunk) {
	size_t base = jobBase(plan, directory);
	return base == SIZE_MAX ? SIZE_MAX : base + seedsLength(plan, chunk);
}

uint32_t batchPlan_largestChunk(const BatchPlan *plan, const char *directory) {
	size_t base = jobBase(plan, directory);
	if (base == SIZE_MAX || base + seedsLength(plan, 1) >= HARNESS_JOB_LENGTH) {
		return 0;
	}
	if (plan->seeds.kind == BATCH_SEEDS_RANGE) {
		return BATCH_CHUNK_LIMIT;
	}
	size_t largest = (HARNESS_JOB_LENGTH - base) / BATCH_SEED_LENGTH;
	return largest < BATCH_CHUNK_LIMIT ? (uint32_t)largest : BATCH_CHUNK_LIMIT;
}

// Writes "line N: " (while reading lines) and the message to the reader's error; false.
static bool fail(PlanReader *reader, const char *format, ...) {
	char message[512];
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(message, sizeof message, format, arguments);
	va_end(arguments);
	if (reader->line) {
		(void)snprintf(reader->error, reader->errorSize, "line %u: %s", reader->line, message);
	} else {
		(void)snprintf(reader->error, reader->errorSize, "%s", message);
	}
	return false;
}

static bool readLines(PlanReader *reader, const char *text, size_t length) {
	char line[LINE_LENGTH];
	const char *position = text;
	const char *end = text + length;
	while (position < end) {
		const char *newline = memchr(position, '\n', (size_t)(end - position));
		size_t size = newline ? (size_t)(newline - position) : (size_t)(end - position);
		++reader->line;
		if (size >= sizeof line) {
			return fail(reader, "the line is too long");
		}
		memcpy(line, position, size);
		line[size] = 0;
		if (!readLine(reader, line)) {
			return false;
		}
		position = newline ? newline + 1 : end;
	}
	reader->line = 0;
	return true;
}

// One line: a comment, a blank line or key=value. A '#' at the start starts a comment, and in a
// plan as written so does one after a space or tab. A frozen plan has no comments after values, so
// that a save's name or a folder with " #" in it reads back whole.
static bool readLine(PlanReader *reader, char *line) {
	for (char *c = line; *c; ++c) {
		if (*c == '#' && (c == line || (!reader->frozen && (c[-1] == ' ' || c[-1] == '\t')))) {
			*c = 0;
			break;
		}
	}
	char *text = trim(line);
	if (!*text) {
		return true;
	}
	char *equals = strchr(text, '=');
	if (!equals) {
		return fail(reader, "expected key=value");
	}
	*equals = 0;
	char *key = trim(text);
	char *value = trim(equals + 1);
	if (!(reader->keysGiven & (1U << KEY_FORMAT)) && strcmp(key, "format") != 0) {
		return fail(reader, "a plan starts with format=%s", planFormat);
	}
	if (strcmp(key, "arm") == 0) {
		return readArm(reader, value);
	}
	if (strcmp(key, "fixture") == 0 && reader->frozen) {
		return readFixture(reader, value);
	}
	if (strcmp(key, "team") == 0) {
		return readTeam(reader, value);
	}
	if (strcmp(key, "pair") == 0 && reader->frozen) {
		return readPair(reader, value);
	}
	if (strcmp(key, "detailed") == 0) {
		return fail(reader, "a batch writes per-match rows, not --detailed averages; remove detailed=");
	}
	return readEntry(reader, key, value);
}

// A setting or another key that is given once.
static bool readEntry(PlanReader *reader, const char *key, char *value) {
	for (size_t i = 0; i < SETTINGS; ++i) {
		if (strcmp(key, settings[i].name) == 0) {
			if (reader->settingsGiven & (1ULL << i)) {
				return fail(reader, "%s is given twice", key);
			}
			reader->settingsGiven |= 1ULL << i;
			return readSetting(reader, &settings[i], value);
		}
	}
	for (unsigned k = 0; k < KEYS; ++k) {
		if (strcmp(key, keyNames[k]) == 0) {
			if (k >= KEY_SEEDS_SHA256 && !reader->frozen) {
				return fail(reader, "%s is only in a batch's plan.txt", key);
			}
			if (reader->keysGiven & (1U << k)) {
				return fail(reader, "%s is given twice", key);
			}
			reader->keysGiven |= 1U << k;
			return readKey(reader, (PlanKey)k, value);
		}
	}
	return fail(reader, "unknown key %s", key);
}

static bool readSetting(PlanReader *reader, const Setting *setting, const char *value) {
	if (setting->kind == SETTING_LIMIT) {
		return readLimit(value, &reader->plan->maxSeedErrors) ||
					 fail(reader, "max_seed_errors is a count or a percentage up to 100%% (\"1%%\")");
	}
	uint32_t *field = (uint32_t *)((char *)reader->plan + setting->offset);
	if (setting->kind == SETTING_CHOICE) {
		for (uint32_t i = 0; setting->choices[i]; ++i) {
			if (strcmp(value, setting->choices[i]) == 0) {
				*field = i;
				return true;
			}
		}
		char names[64] = "";
		for (size_t i = 0; setting->choices[i]; ++i) {
			size_t used = strlen(names);
			(void)snprintf(
				names + used, sizeof names - used, "%s%s", i ? " or " : "", setting->choices[i]
			);
		}
		return fail(reader, "%s must be %s", setting->name, names);
	}
	uint64_t number = 0;
	if (!readWhole(value, setting->high, &number) || number < setting->low) {
		return fail(reader, "%s must be %u-%u", setting->name, setting->low, setting->high);
	}
	*field = (uint32_t)number;
	return true;
}

static bool readKey(PlanReader *reader, PlanKey key, char *value) {
	BatchPlan *plan = reader->plan;
	uint64_t number = 0;
	switch (key) {
		case KEY_FORMAT:
			return strcmp(value, planFormat) == 0 || fail(reader, "the format must be %s", planFormat);
		case KEY_NAME:
			if (!isName(value, BATCH_NAME_LENGTH - 1, "_.-")) {
				return fail(reader, "the name must be 1-64 of A-Z, a-z, 0-9, _, . and -");
			}
			strcpy(plan->name, value);
			return true;
		case KEY_SEEDS: return readSeeds(reader, value);
		case KEY_SAVE: return copyText(reader, value, plan->save, sizeof plan->save, "save");
		case KEY_MODE:
			if (strcmp(value, "match") != 0 && strcmp(value, "season") != 0) {
				return fail(reader, "mode must be match or season");
			}
			plan->mode = strcmp(value, "season") == 0 ? BATCH_MODE_SEASON : BATCH_MODE_MATCH;
			return true;
		case KEY_BASE: {
			size_t length = strlen(value);
			if (length < 6 || length >= sizeof plan->base.path ||
					platform_compareIgnoringCase(value + length - 5, ".json") != 0) {
				return fail(reader, "base= needs a JSON fixture (.json)");
			}
			strcpy(plan->base.id, "base");
			strcpy(plan->base.path, value);
			return true;
		}
		case KEY_BASE_FIXTURE:
			return batchPlan_readHashes(value, &plan->base) ||
						 fail(reader, "base_fixture is json_sha256= bin_sha256= home_team= away_team=");
		case KEY_SEEDS_SHA256:
			if (!isHex(value)) {
				return fail(reader, "seeds_sha256 must be a SHA-256");
			}
			strcpy(plan->listSha256, value);
			return true;
		case KEY_SAVE_NAME:
			return copyText(reader, value, plan->saveName, sizeof plan->saveName, "save_name");
		case KEY_GAME_BUILD:
			if (!readWhole(value, UINT32_MAX, &number)) {
				return fail(reader, "game_build must be a number");
			}
			plan->gameBuild = (uint32_t)number;
			return true;
		case KEY_GAME_DATE:
			return copyText(reader, value, plan->gameDate, sizeof plan->gameDate, "game_date");
		default: return false;
	}
}

static bool
copyText(PlanReader *reader, const char *value, char *out, size_t size, const char *key) {
	if (strlen(value) >= size) {
		return fail(reader, "%s is too long", key);
	}
	strcpy(out, value);
	return true;
}

// "<id> <path to the fixture>", the path after the first space.
static bool readArm(PlanReader *reader, char *value) {
	BatchPlan *plan = reader->plan;
	char *space = strpbrk(value, " \t");
	if (!space) {
		return fail(reader, "an arm is arm=<id> <fixture.json>");
	}
	*space = 0;
	char *path = trim(space + 1);
	if (!isName(value, BATCH_ARM_LENGTH - 1, "_-")) {
		return fail(reader, "an arm's ID must be 1-32 of A-Z, a-z, 0-9, _ and -");
	}
	if (plan->armCount == BATCH_MAX_ARMS) {
		return fail(reader, "a batch has at most %d arms", BATCH_MAX_ARMS);
	}
	if (!plan->arms && !(plan->arms = calloc(BATCH_MAX_ARMS, sizeof *plan->arms))) {
		return fail(reader, "out of memory");
	}
	for (size_t i = 0; i < plan->armCount; ++i) {
		// Their folders' names must differ on a file system that ignores case.
		if (platform_compareIgnoringCase(plan->arms[i].id, value) == 0) {
			return fail(reader, "arm %s is given twice (IDs must differ in more than case)", value);
		}
	}
	size_t length = strlen(path);
	if (length < 6 || length >= BATCH_PATH_LENGTH ||
			platform_compareIgnoringCase(path + length - 5, ".json") != 0) {
		return fail(reader, "arm %s needs a JSON fixture (.json)", value);
	}
	BatchArm *arm = &plan->arms[plan->armCount++];
	strcpy(arm->id, value);
	strcpy(arm->path, path);
	return true;
}

// A season's team: its unique ID, once.
static bool readTeam(PlanReader *reader, const char *value) {
	BatchPlan *plan = reader->plan;
	uint64_t team = 0;
	if (!readWhole(value, UINT32_MAX, &team) || !team) {
		return fail(reader, "a team is team=<unique ID>, 1-4294967295 (see list-teams)");
	}
	for (size_t i = 0; i < plan->teamCount; ++i) {
		if (plan->teams[i] == team) {
			return fail(reader, "team %u is given twice", (uint32_t)team);
		}
	}
	if (plan->teamCount == BATCH_MAX_TEAMS) {
		return fail(reader, "a season has at most %d teams", BATCH_MAX_TEAMS);
	}
	plan->teams[plan->teamCount++] = (uint32_t)team;
	return true;
}

// A frozen season's "<fixture> home_team=<uid> away_team=<uid>", in order: what its teams make.
static bool readPair(PlanReader *reader, const char *value) {
	const BatchPlan *plan = reader->plan;
	size_t teams = plan->teamCount;
	reader->pairTeams = reader->pairs ? reader->pairTeams : teams;
	char expected[BATCH_ARM_LENGTH + 64] = "";
	if (teams >= 2 && reader->pairs < batchSeason_fixtures(teams)) {
		size_t home = 0;
		size_t away = 0;
		char id[BATCH_ARM_LENGTH];
		batchSeason_pair(teams, reader->pairs, &home, &away);
		batchSeason_id(reader->pairs, id);
		(void)snprintf(
			expected,
			sizeof expected,
			"%s home_team=%u away_team=%u",
			id,
			plan->teams[home],
			plan->teams[away]
		);
	}
	++reader->pairs;
	return (expected[0] && strcmp(value, expected) == 0) ||
				 fail(reader, "the pair lines are not the fixtures of the season's teams");
}

// range:<first>-<last>, scatter:<key>:<from>-<to> or list:<file>.
static bool readSeeds(PlanReader *reader, const char *value) {
	BatchSeeds *seeds = &reader->plan->seeds;
	uint64_t first = 0;
	uint64_t last = 0;
	const char *bounds = NULL;
	if (strncmp(value, "list:", 5) == 0) {
		seeds->kind = BATCH_SEEDS_LIST;
		if (!value[5] || strlen(value + 5) >= sizeof reader->plan->listPath) {
			return fail(reader, "list: needs the seed list's path");
		}
		strcpy(reader->plan->listPath, value + 5);
		return true;
	}
	if (strncmp(value, "range:", 6) == 0) {
		seeds->kind = BATCH_SEEDS_RANGE;
		bounds = value + 6;
	} else if (strncmp(value, "scatter:", 8) == 0 || strncmp(value, "season:", 7) == 0) {
		bool season = strncmp(value, "season:", 7) == 0;
		seeds->kind = season ? BATCH_SEEDS_SEASON : BATCH_SEEDS_SCATTER;
		bounds = readDecimal(value + (season ? 7 : 8), UINT64_MAX, &seeds->key);
		bounds = bounds && *bounds == ':' ? bounds + 1 : NULL;
	}
	const char *dash = bounds ? readDecimal(bounds, UINT32_MAX, &first) : NULL;
	if (!dash || *dash != '-' || !readWhole(dash + 1, UINT32_MAX, &last) || last < first) {
		return fail(
			reader,
			"seeds must be range:<first>-<last>, scatter:<key>:<from>-<to>, list:<file> or "
			"season:<key>:<first>-<last>, with indices from 0 to 4294967295 and first <= last"
		);
	}
	seeds->first = first;
	seeds->last = last;
	return true;
}

// "<arm> json_sha256=<hex> bin_sha256=<hex> home_team=<n|-> away_team=<n|->".
static bool readFixture(PlanReader *reader, char *value) {
	char *space = strchr(value, ' ');
	BatchArm *arm = NULL;
	if (space) {
		*space = 0;
	}
	for (size_t i = 0; space && reader->plan->mode != BATCH_MODE_SEASON && i < reader->plan->armCount;
		++i) {
		if (strcmp(reader->plan->arms[i].id, value) == 0 && !(reader->fixtures & (1U << i))) {
			arm = &reader->plan->arms[i];
			reader->fixtures |= 1U << i;
		}
	}
	if (!arm || !batchPlan_readHashes(space + 1, arm)) {
		return fail(
			reader,
			"a fixture line is fixture=<arm> json_sha256= bin_sha256= home_team= away_team=, once for "
			"each arm"
		);
	}
	return true;
}

// A count, or a percentage with up to three decimals ("1%", "0.25%"), up to 100%.
static bool readLimit(const char *text, BatchErrorLimit *limit) {
	uint64_t whole = 0;
	const char *end = readDecimal(text, UINT64_MAX / PERCENT_SCALE, &whole);
	if (!end) {
		return false;
	}
	if (!*end) {
		*limit = (BatchErrorLimit){false, whole};
		return true;
	}
	uint64_t fraction = 0;
	uint64_t scale = PERCENT_SCALE;
	if (*end == '.') {
		const char *digit = end + 1;
		for (; *digit >= '0' && *digit <= '9' && scale > 1; ++digit) {
			scale /= 10;
			fraction += (uint64_t)(*digit - '0') * scale;
		}
		if (digit == end + 1) {
			return false;
		}
		end = digit;
	}
	uint64_t value = whole * PERCENT_SCALE + fraction;
	if (strcmp(end, "%") != 0 || value > 100ULL * PERCENT_SCALE) {
		return false;
	}
	*limit = (BatchErrorLimit){true, value};
	return true;
}

static bool checkComplete(PlanReader *reader) {
	const BatchPlan *plan = reader->plan;
	unsigned needed = (1U << KEY_FORMAT) | (1U << KEY_NAME) | (1U << KEY_SEEDS);
	if (reader->frozen) {
		needed |= (1U << KEY_SAVE_NAME) | (1U << KEY_GAME_BUILD) | (1U << KEY_GAME_DATE) |
							(plan->seeds.kind == BATCH_SEEDS_LIST ? 1U << KEY_SEEDS_SHA256 : 0);
	}
	for (unsigned k = 0; k < KEYS; ++k) {
		if ((needed & (1U << k)) && !(reader->keysGiven & (1U << k))) {
			return fail(reader, "the plan has no %s=", keyNames[k]);
		}
	}
	if (plan->mode == BATCH_MODE_SEASON) {
		return checkSeason(reader);
	}
	if (plan->teamCount || reader->pairs ||
			(reader->keysGiven & ((1U << KEY_BASE) | (1U << KEY_BASE_FIXTURE))) ||
			plan->seeds.kind == BATCH_SEEDS_SEASON) {
		return fail(reader, "base=, team= and season: seeds are for a season (mode=season)");
	}
	if (!plan->armCount) {
		return fail(reader, "the plan has no arm=");
	}
	if (reader->frozen && reader->fixtures != (1U << plan->armCount) - 1) {
		return fail(reader, "an arm has no fixture line");
	}
	return true;
}

// A season: its base, 2-40 teams, season: seeds within 32 bits and the settings it allows, with its
// own defaults; then its fixtures.
static bool checkSeason(PlanReader *reader) {
	BatchPlan *plan = reader->plan;
	if (plan->armCount) {
		return fail(reader, "a season plays its team= lines' fixtures, not arm=");
	}
	if (!(reader->keysGiven & (1U << KEY_BASE))) {
		return fail(
			reader, "a season needs base=<fixture.json>, the fixture its fixtures are built from"
		);
	}
	if (plan->teamCount < 2) {
		return fail(reader, "a season needs 2-%d team= lines", BATCH_MAX_TEAMS);
	}
	if (plan->seeds.kind != BATCH_SEEDS_SEASON) {
		return fail(reader, "a season's seeds are season:<key>:<first>-<last>, its seasons");
	}
	static const struct {
		size_t offset;
		uint32_t refused;
		const char *why;
	} refusals[] = {
		{offsetof(BatchPlan, order),
			BATCH_ORDER_SEQUENTIAL,
			"order=sequential: no season would be complete before the end"},
		{offsetof(BatchPlan, keepBuild), 1, "keep_build=1: each fixture is rebuilt for every seed"},
		{offsetof(BatchPlan, humanControl), 1, "human_control=1: the AI picks both sides"},
		{offsetof(BatchPlan, warmup), 0, "warmup=0: it keeps a session's first builds out of chunks"},
	};
	for (size_t i = 0; i < sizeof refusals / sizeof refusals[0]; ++i) {
		if (*(const uint32_t *)((const char *)plan + refusals[i].offset) == refusals[i].refused) {
			return fail(reader, "a season cannot have %s", refusals[i].why);
		}
	}
	uint64_t fixtures = batchSeason_fixtures(plan->teamCount);
	if ((plan->seeds.last + 1) * fixtures > 1ULL << 32) {
		return fail(
			reader,
			"%zu teams play %llu fixtures a season, so the last season can be at most %llu",
			plan->teamCount,
			(unsigned long long)fixtures,
			(unsigned long long)((1ULL << 32) / fixtures - 1)
		);
	}
	if (!settingGiven(reader, "chunk")) {
		plan->chunk = SEASON_CHUNK;
	}
	if (!settingGiven(reader, "canary")) {
		plan->canary = SEASON_CANARY;
	}
	if (!settingGiven(reader, "max_seed_errors")) {
		plan->maxSeedErrors = (BatchErrorLimit){false, SEASON_SEED_ERRORS};
	}
	plan->seeds.stride = fixtures;
	if (reader->frozen && !(reader->keysGiven & (1U << KEY_BASE_FIXTURE))) {
		return fail(reader, "the plan has no base_fixture=");
	}
	if (reader->frozen && (reader->pairs != fixtures || reader->pairTeams != plan->teamCount)) {
		return fail(reader, "the pair lines are not the fixtures of the season's teams");
	}
	return makeFixtures(plan) || fail(reader, "out of memory");
}

static bool settingGiven(const PlanReader *reader, const char *name) {
	for (size_t i = 0; i < SETTINGS; ++i) {
		if (strcmp(settings[i].name, name) == 0) {
			return (reader->settingsGiven & (1ULL << i)) != 0;
		}
	}
	return false;
}

// A season's fixtures, its arms: every ordered pair of its teams, without hashes or overrides.
static bool makeFixtures(BatchPlan *plan) {
	size_t count = batchSeason_fixtures(plan->teamCount);
	plan->arms = calloc(count, sizeof *plan->arms);
	if (!plan->arms) {
		return false;
	}
	for (size_t fixture = 0; fixture < count; ++fixture) {
		BatchArm *arm = &plan->arms[fixture];
		size_t home = 0;
		size_t away = 0;
		batchSeason_pair(plan->teamCount, fixture, &home, &away);
		batchSeason_id(fixture, arm->id);
		arm->pair[0] = plan->teams[home];
		arm->pair[1] = plan->teams[away];
	}
	plan->armCount = count;
	return true;
}

// A frozen plan's last line, plan_sha256=<hex>, is the hash of everything before it; `body` is
// that length.
static bool checkHash(const char *text, size_t length, size_t *body, char hex[SHA256_HEX_LENGTH]) {
	if (length < 2 || text[length - 1] != '\n') {
		return false;
	}
	size_t start = length - 1;
	while (start && text[start - 1] != '\n') {
		--start;
	}
	size_t keyLength = sizeof hashKey - 1;
	size_t lineLength = length - 1 - start;
	if (lineLength != keyLength + SHA256_HEX_LENGTH - 1 ||
			memcmp(text + start, hashKey, keyLength) != 0) {
		return false;
	}
	memcpy(hex, text + start + keyLength, SHA256_HEX_LENGTH - 1);
	hex[SHA256_HEX_LENGTH - 1] = 0;
	char actual[SHA256_HEX_LENGTH];
	sha256_hexOf(text, start, actual);
	*body = start;
	return strcmp(actual, hex) == 0;
}

// The decimal number at `text`, at most `max`; the character after it, or NULL if there is none or
// it is larger.
static const char *readDecimal(const char *text, uint64_t max, uint64_t *value) {
	*value = 0;
	const char *digit = text;
	for (; *digit >= '0' && *digit <= '9'; ++digit) {
		unsigned add = (unsigned)(*digit - '0');
		if (*value > (max - add) / 10) {
			return NULL;
		}
		*value = *value * 10 + add;
	}
	return digit == text ? NULL : digit;
}

static bool readWhole(const char *text, uint64_t max, uint64_t *value) {
	const char *end = readDecimal(text, max, value);
	return end && !*end;
}

// 1 to `longest` characters, each a letter, a digit or one of `extra`.
static bool isName(const char *text, size_t longest, const char *extra) {
	size_t length = strlen(text);
	if (!length || length > longest) {
		return false;
	}
	for (const char *c = text; *c; ++c) {
		bool alphanumeric =
			(*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
		if (!alphanumeric && !strchr(extra, *c)) {
			return false;
		}
	}
	return true;
}

// 64 lower-case hex digits.
static bool isHex(const char *text) {
	return strlen(text) == SHA256_HEX_LENGTH - 1 &&
				 strspn(text, "0123456789abcdef") == SHA256_HEX_LENGTH - 1;
}

static char *trim(char *text) {
	while (*text == ' ' || *text == '\t' || *text == '\r') {
		++text;
	}
	size_t length = strlen(text);
	while (
		length && (text[length - 1] == ' ' || text[length - 1] == '\t' || text[length - 1] == '\r')) {
		text[--length] = 0;
	}
	return text;
}

static void appendText(ByteBuffer *out, const char *text, bool *ok) {
	*ok = *ok && buffer_append(out, text, strlen(text));
}

static void appendLine(ByteBuffer *out, const char *key, const char *value, bool *ok) {
	appendText(out, key, ok);
	appendText(out, "=", ok);
	appendText(out, value, ok);
	appendText(out, "\n", ok);
}

static void formatSetting(const BatchPlan *plan, const Setting *setting, char *out, size_t size) {
	if (setting->kind == SETTING_LIMIT) {
		batchPlan_formatLimit(&plan->maxSeedErrors, out, size);
		return;
	}
	uint32_t value = *(const uint32_t *)((const char *)plan + setting->offset);
	if (setting->kind == SETTING_CHOICE) {
		(void)snprintf(out, size, "%s", setting->choices[value]);
	} else {
		(void)snprintf(out, size, "%u", value);
	}
}

static void formatSeedsSpec(const BatchPlan *plan, char *out, size_t size) {
	const BatchSeeds *seeds = &plan->seeds;
	unsigned long long first = seeds->first;
	unsigned long long last = seeds->last;
	switch (seeds->kind) {
		case BATCH_SEEDS_SCATTER:
			(void)snprintf(
				out, size, "scatter:%llu:%llu-%llu", (unsigned long long)seeds->key, first, last
			);
			break;
		case BATCH_SEEDS_LIST: (void)snprintf(out, size, "list:%s", plan->listPath); break;
		case BATCH_SEEDS_SEASON:
			(void)snprintf(
				out, size, "season:%llu:%llu-%llu", (unsigned long long)seeds->key, first, last
			);
			break;
		default: (void)snprintf(out, size, "range:%llu-%llu", first, last); break;
	}
}

static void formatTeam(uint32_t team, char *out, size_t size) {
	if (team) {
		(void)snprintf(out, size, "%u", team);
	} else {
		(void)snprintf(out, size, "-");
	}
}

static bool appendPath(char *out, size_t size, size_t *length, const char *text) {
	size_t add = strlen(text);
	if (*length + add >= size) {
		return false;
	}
	memcpy(out + *length, text, add + 1);
	*length += add;
	return true;
}

static int compareSeeds(const void *a, const void *b) {
	uint32_t left = *(const uint32_t *)a;
	uint32_t right = *(const uint32_t *)b;
	return (left > right) - (left < right);
}

static uint32_t lowbias32(uint32_t x) {
	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return x;
}

static uint64_t splitmix64(uint64_t seed) {
	seed += 0x9E3779B97F4A7C15ULL;
	seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ULL;
	seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBULL;
	return seed ^ (seed >> 31);
}

static bool formatPartJob(
	const BatchPlan *plan,
	const uint32_t teams[2],
	const BatchPartPaths *paths,
	const char *seeds,
	uint64_t indexBase,
	uint32_t threads,
	char *out,
	size_t size
) {
	RunJob job = {
		.input = paths->input,
		.seeds = seeds,
		.output = paths->output,
		.threads = threads,
		.profile = "",
		.humanControl = plan->humanControl != 0,
		.keepBuild = plan->keepBuild != 0,
		.teams = {teams[0], teams[1]},
		.perMatch = plan->perMatch != 0,
		.skipSeedErrors = plan->seedErrors == BATCH_SEED_ERRORS_SKIP,
		.report = true,
		.indexBase = indexBase,
		.stopFile = paths->stopFile,
	};
	return runJob_format(&job, out, size);
}

// The length of the longest part job of the plan without its seeds: the longest arm, chunk ID,
// attempt number, index and worker count, and a season's fixtures with the longest overrides, which
// are not known before they are built. SIZE_MAX if its paths do not fit.
static size_t jobBase(const BatchPlan *plan, const char *directory) {
	BatchPartPaths paths;
	char job[HARNESS_JOB_LENGTH];
	size_t longest = 0;
	static const uint32_t longestTeams[2] = {UINT32_MAX, UINT32_MAX};
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		const uint32_t *teams = plan->mode == BATCH_MODE_SEASON ? longestTeams : plan->arms[arm].teams;
		if (!batchPlan_partPaths(directory, plan->arms[arm].id, UINT32_MAX, UINT32_MAX, &paths) ||
				!formatPartJob(
					plan, teams, &paths, "", plan->seeds.last, HARNESS_MAX_THREADS, job, sizeof job
				)) {
			return SIZE_MAX;
		}
		size_t length = strlen(job);
		longest = length > longest ? length : longest;
	}
	return longest;
}

// The longest seeds= text of a part: a range of two 10-digit seeds, or `chunk` of them listed.
static size_t seedsLength(const BatchPlan *plan, uint32_t chunk) {
	if (plan->seeds.kind == BATCH_SEEDS_RANGE) {
		return 2 * (BATCH_SEED_LENGTH - 1) + 1;
	}
	return chunk ? (size_t)chunk * BATCH_SEED_LENGTH - 1 : 0;
}
