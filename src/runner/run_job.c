// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "run_job.h"

#include "match_config.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include "macos_game.h"
#endif


// What runJob_ensureSave returns, besides macosGame_ensureSave's exit codes (see commands.h).
enum {
	SAVE_READY = 0,  // submit the job
	SAVE_FAILED = 1, // the exit code of a failure
};

enum {
	SEED_RANGE_LIMIT = 10000000, // the most seeds a range may have after its first (as job.c)
	REPORT_VALUE_LENGTH = 48,    // the longest report value, a commit, and its NUL
};

// The report's keys, in the order the harness writes them (job_run.c's formatSummary).
typedef enum ReportKey {
	REPORT_PROTOCOL,
	REPORT_COMMIT,
	REPORT_DIRTY,
	REPORT_BUILD,
	REPORT_SLICE,
	REPORT_TRANSLATED,
	REPORT_PID,
	REPORT_LOADED,
	REPORT_JOBS,
	REPORT_THREADS,
	REPORT_EDITS,
	REPORT_ELAPSED,
	REPORT_WRITTEN,
	REPORT_SKIPPED,
	REPORT_STOPPED,
	REPORT_FAILED_INDEX,
	REPORT_FAILED_STAGE,
	REPORT_OUTPUT_FAILED,
	REPORT_KEYS,
} ReportKey;

static const char *const reportKeys[REPORT_KEYS] = {
	"protocol",
	"commit",
	"dirty",
	"build",
	"slice",
	"translated",
	"pid",
	"loaded",
	"jobs",
	"threads",
	"edits",
	"elapsed_ms",
	"written",
	"skipped",
	"stopped",
	"failed_index",
	"failed_stage",
	"output_failed",
};

static const char reportMarker[] = "; report ";

static bool readReportValues(const char *text, char values[REPORT_KEYS][REPORT_VALUE_LENGTH]);
static bool readReport(char values[REPORT_KEYS][REPORT_VALUE_LENGTH], RunReport *report);
static const char *readDecimal(const char *text, uint64_t max, uint64_t *value);
static bool readWhole(const char *text, uint64_t max, uint64_t *value);
static bool readOptional(const char *text, bool *has, uint64_t *value);
static bool readFlag(const char *text, bool *value);
static bool readWord(const char *text, const char *allowed, char *out, size_t size);
static bool isCommit(const char *text);

bool runJob_format(const RunJob *job, char *out, size_t size) {
	char indexBase[24];
	(void)snprintf(indexBase, sizeof indexBase, "%llu", (unsigned long long)job->indexBase);
	const char *const added[][2] = {
		{job->perMatch ? "per_match" : NULL, "1"},
		{job->indexBase ? "index_base" : NULL, indexBase},
		{job->skipSeedErrors ? "seed_errors" : NULL, "skip"},
		{job->stopFile && job->stopFile[0] ? "stop_file" : NULL, job->stopFile},
		{job->report ? "report" : NULL, "1"},
	};
	int written = snprintf(
		out,
		size,
		"command=run\ninput=%s\nseeds=%s\noutput=%s\nthreads=%lu\nprofile=%s\ndetailed=%d\n"
		"human_control=%d\nkeep_build=%d\nhome_team=%.0u\naway_team=%.0u\nprogress_window=%llu\n",
		job->input,
		job->seeds,
		job->output,
		job->threads,
		job->profile ? job->profile : "",
		job->detailed,
		job->humanControl,
		job->keepBuild,
		job->teams[0],
		job->teams[1],
		(unsigned long long)job->progressWindow
	);
	size_t length = written >= 0 ? (size_t)written : size;
	for (size_t i = 0; length < size && i < sizeof added / sizeof added[0]; ++i) {
		if (added[i][0]) {
			written = snprintf(out + length, size - length, "%s=%s\n", added[i][0], added[i][1]);
			length = written >= 0 ? length + (size_t)written : size;
		}
	}
	return length < size && length < HARNESS_JOB_LENGTH;
}

bool runReport_parse(const char *result, RunReport *report) {
	*report = (RunReport){0};
	const char *text = strstr(result, reportMarker);
	char values[REPORT_KEYS][REPORT_VALUE_LENGTH];
	bool ok =
		text && readReportValues(text + sizeof reportMarker - 1, values) && readReport(values, report);
	if (!ok) {
		*report = (RunReport){0};
	}
	return ok;
}

bool runJob_countSeeds(const char *seeds, uint64_t *count, uint32_t *first, uint32_t *last) {
	*count = 0;
	*first = 0;
	*last = 0;
	for (const char *part = seeds;;) {
		uint64_t low = 0;
		const char *end = readDecimal(part, UINT32_MAX, &low);
		uint64_t high = low;
		if (end && *end == '-') {
			end = readDecimal(end + 1, UINT32_MAX, &high);
		}
		if (!end || high < low || high - low > SEED_RANGE_LIMIT || (*end && *end != ',')) {
			*count = 0;
			*first = 0;
			*last = 0;
			return false;
		}
		*first = *count ? *first : (uint32_t)low;
		*last = (uint32_t)high;
		*count += high - low + 1;
		if (!*end) {
			return true;
		}
		part = end + 1;
	}
}

bool runJob_isSaveName(const char *name) {
	for (const char *c = name; *c; ++c) {
		if ((unsigned char)*c < 0x20 || *c == 0x7F) {
			return false;
		}
	}
	return saves_isName(name);
}

void runJob_readFixtureSave(const ByteBuffer *config, SaveCheck *check) {
	ConfigOrigin origin;
	(void)matchConfig_readOrigin(buffer_string(config), config->length, &origin);
	(void)snprintf(check->date, sizeof check->date, "%s", origin.gameDate);
	check->badSave =
		origin.hasSave && !(saves_nameOfPath(origin.save, check->save, sizeof check->save) &&
												runJob_isSaveName(check->save));
	if (!origin.hasSave || check->badSave) {
		check->save[0] = 0;
	}
}

int runJob_ensureSave(const SaveCheck *check, unsigned timeoutMs, char *date, size_t size) {
	if (date && size) {
		date[0] = 0;
	}
#ifdef _WIN32
	(void)check;
	(void)timeoutMs;
	return SAVE_READY;
#else
	if (check->badSave) {
		fprintf(stderr, "the input's \"save\" is not a save's name; remove it to use any save\n");
		return SAVE_FAILED;
	}
	if (!check->save[0]) {
		return SAVE_READY;
	}
	SessionNeed need = {.save = check->save,
		.launch = !check->noLaunch,
		.anySave = check->anySave,
		.timeoutMs = timeoutMs};
	return macosGame_ensureSave(&need, date, size);
#endif
}

void runJob_warnIfDated(const SaveCheck *check, const char *date) {
	if (check->date[0] && date[0] && strcmp(check->date, date) != 0) {
		fprintf(
			stderr,
			"warning: the save is at %s, the fixture was exported at %s; the results reflect the save\n",
			date,
			check->date
		);
	}
}

// Splits the report after its marker into the values of its keys, which must come in order, each
// once, separated by single spaces, up to the end of the line.
static bool readReportValues(const char *text, char values[REPORT_KEYS][REPORT_VALUE_LENGTH]) {
	for (size_t i = 0; i < REPORT_KEYS; ++i) {
		size_t keyLength = strlen(reportKeys[i]);
		if (strncmp(text, reportKeys[i], keyLength) != 0 || text[keyLength] != '=') {
			return false;
		}
		text += keyLength + 1;
		size_t length = strcspn(text, " \r\n");
		if (length == 0 || length >= REPORT_VALUE_LENGTH) {
			return false;
		}
		memcpy(values[i], text, length);
		values[i][length] = 0;
		text += length;
		bool last = i + 1 == REPORT_KEYS;
		if (!last && *text++ != ' ') {
			return false;
		}
	}
	return !*text || strcmp(text, "\n") == 0 || strcmp(text, "\r\n") == 0;
}

static bool readReport(char values[REPORT_KEYS][REPORT_VALUE_LENGTH], RunReport *report) {
	uint64_t build = 0;
	uint64_t pid = 0;
	uint64_t threads = 0;
	uint64_t loaded = 0;
	const char *loadedText = values[REPORT_LOADED];
	bool negative = loadedText[0] == '-';
	// The dirty flag is "-" exactly when the commit is unknown.
	bool known = strcmp(values[REPORT_COMMIT], "unknown") != 0;
	bool dirty = false;
	bool ok =
		readWord(values[REPORT_PROTOCOL], "0123456789.", report->protocol, sizeof report->protocol) &&
		(isCommit(values[REPORT_COMMIT]) || !known) &&
		readWord(values[REPORT_COMMIT], NULL, report->commit, sizeof report->commit) &&
		(known ? readFlag(values[REPORT_DIRTY], &dirty) : strcmp(values[REPORT_DIRTY], "-") == 0) &&
		readWhole(values[REPORT_BUILD], UINT32_MAX, &build) &&
		readWord(
			values[REPORT_SLICE],
			"abcdefghijklmnopqrstuvwxyz0123456789_",
			report->slice,
			sizeof report->slice
		) &&
		readFlag(values[REPORT_TRANSLATED], &report->translated) &&
		readWhole(values[REPORT_PID], UINT32_MAX, &pid) &&
		readWhole(loadedText + negative, (uint64_t)INT64_MAX, &loaded) &&
		readWhole(values[REPORT_JOBS], UINT64_MAX, &report->jobs) &&
		readWhole(values[REPORT_THREADS], UINT32_MAX, &threads) &&
		readOptional(values[REPORT_EDITS], &report->hasEdits, &report->edits) &&
		readWhole(values[REPORT_ELAPSED], UINT64_MAX, &report->elapsedMs) &&
		readWhole(values[REPORT_WRITTEN], UINT64_MAX, &report->written) &&
		readWhole(values[REPORT_SKIPPED], report->written, &report->skipped) &&
		readFlag(values[REPORT_STOPPED], &report->stopped) &&
		readOptional(values[REPORT_FAILED_INDEX], &report->hasFailure, &report->failedIndex) &&
		readFlag(values[REPORT_OUTPUT_FAILED], &report->outputFailed);
	const char *stage = values[REPORT_FAILED_STAGE];
	bool namedStage = strcmp(stage, "rebuild") == 0 || strcmp(stage, "deserialize") == 0 ||
										strcmp(stage, "stats") == 0;
	ok = ok && (report->hasFailure ? namedStage : strcmp(stage, "-") == 0);
	if (!ok) {
		return false;
	}
	report->dirty = known ? dirty : -1;
	report->build = (uint32_t)build;
	report->pid = (uint32_t)pid;
	report->loaded = negative ? -(int64_t)loaded : (int64_t)loaded;
	report->threads = (uint32_t)threads;
	(void)snprintf(report->failedStage, sizeof report->failedStage, "%s", namedStage ? stage : "");
	return true;
}

// The decimal number at `text`, at most `max`; the character after it, or NULL if there is no
// number or it is larger.
static const char *readDecimal(const char *text, uint64_t max, uint64_t *value) {
	*value = 0;
	const char *digit = text;
	for (; *digit >= '0' && *digit <= '9'; ++digit) {
		unsigned add = (unsigned)(*digit - '0');
		if (add > max || *value > (max - add) / 10) {
			return NULL;
		}
		*value = *value * 10 + add;
	}
	return digit == text ? NULL : digit;
}

// True if `text` is only a decimal number, at most `max`.
static bool readWhole(const char *text, uint64_t max, uint64_t *value) {
	const char *end = readDecimal(text, max, value);
	return end && !*end;
}

// A number, or "-" for none.
static bool readOptional(const char *text, bool *has, uint64_t *value) {
	*has = strcmp(text, "-") != 0;
	*value = 0;
	return !*has || readWhole(text, UINT64_MAX, value);
}

static bool readFlag(const char *text, bool *value) {
	*value = text[0] == '1';
	return (text[0] == '0' || text[0] == '1') && !text[1];
}

// Copies `text` if it fits and, with `allowed`, has only those characters.
static bool readWord(const char *text, const char *allowed, char *out, size_t size) {
	size_t length = strlen(text);
	if (length >= size || (allowed && strspn(text, allowed) != length)) {
		return false;
	}
	memcpy(out, text, length + 1);
	return true;
}

// A commit as build_info.h has it: 40 lower-case hex digits.
static bool isCommit(const char *text) {
	return strlen(text) == 40 && strspn(text, "0123456789abcdef") == 40;
}
