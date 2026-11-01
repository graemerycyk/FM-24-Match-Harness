// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "manifest.h"

#include "json_writer.h"
#include "platform.h"
#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { RESULT_FIELDS = 7 }; // seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms

// What manifest_readOutput is reading.
typedef struct OutputScan {
	ManifestOutput *output;
	ResultsSummary *summary; // NULL: not a results CSV
	Sha256 content;
	ByteBuffer line; // the results line read so far
	uint64_t lines;  // complete lines
	bool failed;     // out of memory
} OutputScan;

static void scanBytes(OutputScan *scan, const uint8_t *bytes, size_t size);
static void scanLine(OutputScan *scan, bool complete);
static void addResult(ResultsSummary *summary, const char *line);
static void writeText(JsonWriter *writer, const char *key, const char *text);
static void writeOptional(JsonWriter *writer, const char *key, bool has, uint64_t value);
static void writeTime(JsonWriter *writer, const char *key, bool has, time_t seconds);
static void writeDirty(JsonWriter *writer, const char *key, int dirty);
static void writeMatch(JsonWriter *writer, const RunManifest *manifest);
static void writeGame(JsonWriter *writer, const GameFacts *game);
static void writeHost(JsonWriter *writer, const HostInfo *host);
static void writeFixture(JsonWriter *writer, const RunManifest *manifest);
static void writeHeader(JsonWriter *writer, const ConfigHeader *header);
static void writeSeeds(JsonWriter *writer, const RunJob *job);
static void writeFlags(JsonWriter *writer, const RunManifest *manifest);
static void writeTiming(JsonWriter *writer, const RunManifest *manifest);
static void writeOutputs(JsonWriter *writer, const RunManifest *manifest);
static void writeSummary(JsonWriter *writer, const RunManifest *manifest);

const char *manifest_clientOutcomeName(ClientOutcome outcome) {
	switch (outcome) {
		case CLIENT_REPLIED: return "replied";
		case CLIENT_NO_HARNESS: return "no_harness";
		case CLIENT_QUEUE_TIMEOUT: return "queue_timeout";
		case CLIENT_RUN_TIMEOUT: return "run_timeout";
		case CLIENT_LOST: return "lost";
		case CLIENT_ABANDONED: return "abandoned";
		default: return "failed";
	}
}

RunOutcome manifest_runOutcome(
	const ClientWatch *watch,
	ClientOutcome outcome,
	const char *result,
	const RunReport *report
) {
	if (!watch || !watch->accepted) {
		return RUN_NOT_SUBMITTED;
	}
	if (outcome != CLIENT_REPLIED) {
		return RUN_ERROR;
	}
	if (client_succeeded(result)) {
		return RUN_OK;
	}
	bool onlyStopped = report && report->stopped && !report->hasFailure && !report->outputFailed;
	return onlyStopped ? RUN_STOPPED : RUN_ERROR;
}

bool manifest_readOutput(const wchar_t *path, ManifestOutput *output, ResultsSummary *summary) {
	output->read = false;
	output->bytes = 0;
	output->rows = 0;
	output->sha256[0] = 0;
	output->contentSha256[0] = 0;
	if (summary) {
		*summary = (ResultsSummary){0};
	}
	FILE *file = platform_openFile(path, "rb");
	if (!file) {
		return false;
	}
	Sha256 raw;
	sha256_start(&raw);
	OutputScan scan = {.output = output, .summary = summary};
	sha256_start(&scan.content);
	uint8_t chunk[1 << 16];
	size_t got = 0;
	while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) {
		sha256_add(&raw, chunk, got);
		output->bytes += got;
		scanBytes(&scan, chunk, got);
	}
	if (summary && scan.line.length) {
		scanLine(&scan, false); // a last line cut short counts only towards the content's hash
	}
	bool ok = !ferror(file) && !scan.failed;
	ok = fclose(file) == 0 && ok;
	buffer_free(&scan.line);
	if (!ok) {
		if (summary) {
			*summary = (ResultsSummary){0};
		}
		output->bytes = 0;
		return false;
	}
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_finish(&raw, digest);
	sha256_hex(digest, output->sha256);
	if (summary) {
		sha256_finish(&scan.content, digest);
		sha256_hex(digest, output->contentSha256);
	}
	output->rows = scan.lines ? scan.lines - 1 : 0;
	output->read = true;
	return true;
}

bool manifest_formatRun(const RunManifest *manifest, ByteBuffer *out) {
	static const char *const outcomes[] = {"not_submitted", "ok", "error", "stopped"};
	JsonWriter writer;
	jsonWriter_start(&writer, out);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", "fm24-run-manifest/1");
	jsonWriter_string(&writer, "outcome", outcomes[manifest->outcome]);
	jsonWriter_signed(&writer, "exit_code", manifest->exitCode);
	jsonWriter_string(
		&writer,
		"client_outcome",
		manifest->submitted ? manifest_clientOutcomeName(manifest->clientOutcome) : NULL
	);
	writeText(&writer, "result_line", manifest->result);
	jsonWriter_utcTime(&writer, "created_at", manifest->createdAt);
	jsonWriter_beginArray(&writer, "command");
	for (size_t i = 0; i < manifest->commandCount; ++i) {
		jsonWriter_string(&writer, NULL, manifest->command[i]);
	}
	jsonWriter_endArray(&writer);
	jsonWriter_beginObject(&writer, "fm24ctl");
	jsonWriter_string(&writer, "protocol", HARNESS_PROTOCOL_TEXT);
	jsonWriter_string(&writer, "commit", manifest->commit);
	writeDirty(&writer, "dirty", manifest->dirty);
	jsonWriter_string(&writer, "slice", manifest->slice);
	jsonWriter_bool(&writer, "translated", manifest->host->translated);
	jsonWriter_endObject(&writer);
	manifest_writeHarness(&writer, manifest->report);
	writeMatch(&writer, manifest);
	writeGame(&writer, manifest->game);
	writeHost(&writer, manifest->host);
	writeFixture(&writer, manifest);
	writeSeeds(&writer, manifest->job);
	writeFlags(&writer, manifest);
	writeTiming(&writer, manifest);
	writeOutputs(&writer, manifest);
	writeSummary(&writer, manifest);
	jsonWriter_endObject(&writer);
	return jsonWriter_finish(&writer) && buffer_append(out, "\n", 1);
}

// Hashes and counts the lines of `size` more bytes of an output.
static void scanBytes(OutputScan *scan, const uint8_t *bytes, size_t size) {
	while (size) {
		const uint8_t *end = memchr(bytes, '\n', size);
		size_t length = end ? (size_t)(end - bytes) : size;
		if (scan->summary && !buffer_append(&scan->line, bytes, length)) {
			scan->failed = true;
		}
		if (end) {
			++scan->lines;
			if (scan->summary) {
				scanLine(scan, true);
			}
			++length;
		}
		bytes += length;
		size -= length;
	}
}

// A results line without its ending: into the content's hash without its last field, and into
// the summary unless it is the header or was cut short.
static void scanLine(OutputScan *scan, bool complete) {
	const char *line = buffer_string(&scan->line);
	size_t kept = scan->line.length;
	while (kept && line[kept - 1] != ',') {
		--kept;
	}
	sha256_add(&scan->content, line, kept ? kept - 1 : scan->line.length);
	if (complete) {
		sha256_add(&scan->content, "\n", 1);
		if (scan->lines > 1) {
			addResult(scan->summary, line);
		}
	}
	buffer_clear(&scan->line);
}

// Adds a results row: "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms". A row that
// does not read as one is left out.
static void addResult(ResultsSummary *summary, const char *line) {
	unsigned long long values[5] = {0}; // up to `finished`
	const char *field = line;
	for (size_t i = 0; i < 5; ++i) {
		char *end = NULL;
		errno = 0;
		values[i] = *field >= '0' && *field <= '9' ? strtoull(field, &end, 10) : 0;
		if (!end || errno || *end != ',' || values[i] > UINT32_MAX) {
			return;
		}
		field = end + 1;
	}
	const char *ms = strchr(field, ',');
	char *end = NULL;
	double milliseconds = ms ? strtod(ms + 1, &end) : 0.0;
	if (!ms || !end || end == ms + 1 || (*end && *end != '\r') || values[4] > 1) {
		return;
	}
	++summary->matches;
	summary->homeGoals += values[1];
	summary->awayGoals += values[2];
	summary->home += values[1] > values[2];
	summary->draw += values[1] == values[2];
	summary->away += values[1] < values[2];
	summary->unfinished += values[4] == 0;
	summary->totalMs += milliseconds;
}

// Text, or null if there is none.
static void writeText(JsonWriter *writer, const char *key, const char *text) {
	jsonWriter_string(writer, key, text && text[0] ? text : NULL);
}

static void writeOptional(JsonWriter *writer, const char *key, bool has, uint64_t value) {
	if (has) {
		jsonWriter_unsigned(writer, key, value);
	} else {
		jsonWriter_null(writer, key);
	}
}

static void writeTime(JsonWriter *writer, const char *key, bool has, time_t seconds) {
	if (has) {
		jsonWriter_utcTime(writer, key, seconds);
	} else {
		jsonWriter_null(writer, key);
	}
}

// A build's dirty flag: null when its commit is unknown.
static void writeDirty(JsonWriter *writer, const char *key, int dirty) {
	if (dirty < 0) {
		jsonWriter_null(writer, key);
	} else {
		jsonWriter_bool(writer, key, dirty != 0);
	}
}

void manifest_writeHarness(JsonWriter *writer, const RunReport *report) {
	if (!report) {
		jsonWriter_null(writer, "harness");
		return;
	}
	jsonWriter_beginObject(writer, "harness");
	jsonWriter_string(writer, "protocol", report->protocol);
	jsonWriter_string(writer, "commit", report->commit);
	writeDirty(writer, "dirty", report->dirty);
	jsonWriter_unsigned(writer, "build", report->build);
	jsonWriter_string(writer, "slice", report->slice);
	jsonWriter_bool(writer, "translated", report->translated);
	jsonWriter_unsigned(writer, "pid", report->pid);
	writeTime(writer, "loaded", report->loaded > 0, (time_t)report->loaded);
	jsonWriter_unsigned(writer, "jobs", report->jobs);
	jsonWriter_unsigned(writer, "threads", report->threads);
	writeOptional(writer, "edits", report->hasEdits, report->edits);
	jsonWriter_unsigned(writer, "elapsed_ms", report->elapsedMs);
	jsonWriter_unsigned(writer, "written", report->written);
	jsonWriter_unsigned(writer, "skipped", report->skipped);
	jsonWriter_bool(writer, "stopped", report->stopped);
	writeOptional(writer, "failed_index", report->hasFailure, report->failedIndex);
	writeText(writer, "failed_stage", report->failedStage);
	jsonWriter_bool(writer, "output_failed", report->outputFailed);
	jsonWriter_endObject(writer);
}

// Whether the harness was built from fm24ctl's commit: null without a report or a known commit.
// The same protocol is not enough (an older build of it stays loaded until the game restarts), and
// two builds with uncommitted changes to one commit can still differ.
static void writeMatch(JsonWriter *writer, const RunManifest *manifest) {
	const RunReport *report = manifest->report;
	if (!report || report->dirty < 0 || manifest->dirty < 0) {
		jsonWriter_null(writer, "harness_matches_fm24ctl");
		return;
	}
	jsonWriter_bool(
		writer,
		"harness_matches_fm24ctl",
		strcmp(report->protocol, HARNESS_PROTOCOL_TEXT) == 0 &&
			strcmp(report->commit, manifest->commit) == 0 && report->dirty == manifest->dirty
	);
}

static void writeGame(JsonWriter *writer, const GameFacts *game) {
	if (!game) {
		jsonWriter_null(writer, "game");
		return;
	}
	jsonWriter_beginObject(writer, "game");
	writeOptional(writer, "pid", game->pid != 0, game->pid);
	if (game->pid) {
		jsonWriter_bool(writer, "under_rosetta", game->underRosetta);
	} else {
		jsonWriter_null(writer, "under_rosetta");
	}
	writeText(writer, "save", game->save);
	writeText(writer, "date", game->date);
	writeOptional(writer, "rss_bytes", game->residentBytes != 0, game->residentBytes);
	jsonWriter_endObject(writer);
}

static void writeHost(JsonWriter *writer, const HostInfo *host) {
	jsonWriter_beginObject(writer, "host");
	writeText(writer, "model", host->model);
	writeText(writer, "cpu", host->cpu);
	writeOptional(writer, "logical_cpus", host->logicalCpus != 0, host->logicalCpus);
	writeOptional(writer, "performance_cores", host->performanceCores != 0, host->performanceCores);
	writeOptional(writer, "efficiency_cores", host->efficiencyCores != 0, host->efficiencyCores);
	writeOptional(writer, "memory_bytes", host->memoryBytes != 0, host->memoryBytes);
	writeText(writer, "os", host->os);
	writeText(writer, "os_build", host->osBuild);
	jsonWriter_endObject(writer);
}

static void writeFixture(JsonWriter *writer, const RunManifest *manifest) {
	const FixtureFacts *fixture = manifest->fixture;
	jsonWriter_beginObject(writer, "fixture");
	writeText(writer, "path", manifest->fixturePath);
	writeOptional(writer, "bytes", fixture->read, fixture->bytes);
	writeText(writer, "sha256", fixture->read ? fixture->sha256 : NULL);
	writeOptional(writer, "input_bytes", fixture->encoded, fixture->inputBytes);
	writeText(writer, "input_sha256", fixture->encoded ? fixture->inputSha256 : NULL);
	if (fixture->hasHeader) {
		writeHeader(writer, &fixture->header);
	} else {
		jsonWriter_null(writer, "header");
	}
	jsonWriter_endObject(writer);
}

static void writeHeader(JsonWriter *writer, const ConfigHeader *header) {
	jsonWriter_beginObject(writer, "header");
	writeText(writer, "format", header->format);
	writeOptional(writer, "game_build", header->gameBuild != 0, header->gameBuild);
	writeText(writer, "engine_version", header->engineVersion);
	writeText(writer, "source", header->source);
	writeText(writer, "save", header->origin.save);
	writeText(writer, "game_date", header->origin.gameDate);
	writeOptional(writer, "home_team", header->teams[0] != 0, header->teams[0]);
	writeOptional(writer, "away_team", header->teams[1] != 0, header->teams[1]);
	jsonWriter_endObject(writer);
}

static void writeSeeds(JsonWriter *writer, const RunJob *job) {
	uint64_t count = 0;
	uint32_t first = 0;
	uint32_t last = 0;
	bool keep = strcmp(job->seeds, "keep") == 0;
	bool listed = runJob_countSeeds(job->seeds, &count, &first, &last);
	// The seeds are "" when they were too long to send.
	char hash[SHA256_HEX_LENGTH] = "";
	if (job->seeds[0]) {
		sha256_hexOf(job->seeds, strlen(job->seeds), hash);
	}
	jsonWriter_beginObject(writer, "seeds");
	writeText(writer, "spec", job->seeds);
	writeOptional(writer, "count", listed || keep, keep ? 1 : count);
	writeOptional(writer, "first", listed, first);
	writeOptional(writer, "last", listed, last);
	jsonWriter_unsigned(writer, "index_base", job->indexBase);
	writeText(writer, "sha256", hash);
	jsonWriter_endObject(writer);
}

static void writeFlags(JsonWriter *writer, const RunManifest *manifest) {
	const RunJob *job = manifest->job;
	jsonWriter_beginObject(writer, "flags");
	jsonWriter_unsigned(writer, "threads", job->threads);
	jsonWriter_bool(writer, "per_seed_rebuild", !job->keepBuild && strcmp(job->seeds, "keep") != 0);
	jsonWriter_bool(writer, "keep_build", job->keepBuild);
	jsonWriter_bool(writer, "human_control", job->humanControl);
	jsonWriter_bool(writer, "detailed", job->detailed);
	jsonWriter_bool(writer, "per_match", job->perMatch);
	jsonWriter_string(writer, "seed_errors", job->skipSeedErrors ? "skip" : "stop");
	writeText(writer, "profile", job->profile);
	jsonWriter_bool(writer, "no_launch", manifest->noLaunch);
	jsonWriter_bool(writer, "any_save", manifest->anySave);
	jsonWriter_endObject(writer);
}

// When the job went to the harness, was taken and finished, and how fast it played: matches per
// second of its run, and the mean of the results' `ms`.
static void writeTiming(JsonWriter *writer, const RunManifest *manifest) {
	const ClientWatch *watch = manifest->watch;
	bool submitted = manifest->submitted && watch;
	bool accepted = submitted && watch->accepted;
	const RunReport *report = manifest->report;
	const ResultsSummary *summary = manifest->summary;
	jsonWriter_beginObject(writer, "timing");
	writeTime(writer, "started_at", submitted, manifest->startedAt);
	writeTime(
		writer,
		"accepted_at",
		accepted,
		accepted ? manifest->startedAt + (time_t)((watch->queuedMs + 500) / 1000) : 0
	);
	writeTime(writer, "finished_at", submitted, manifest->finishedAt);
	writeOptional(writer, "queue_ms", submitted, submitted ? watch->queuedMs : 0);
	writeOptional(writer, "run_ms", accepted, accepted ? watch->runMs : 0);
	writeOptional(writer, "slept_ms", submitted, manifest->sleptMs);
	if (accepted && report && watch->runMs) {
		double played = (double)(report->written - report->skipped);
		jsonWriter_decimal(writer, "matches_per_s", played * 1000.0 / (double)watch->runMs, 2);
	} else {
		jsonWriter_null(writer, "matches_per_s");
	}
	if (summary && summary->matches) {
		jsonWriter_decimal(writer, "match_ms_mean", summary->totalMs / (double)summary->matches, 1);
	} else {
		jsonWriter_null(writer, "match_ms_mean");
	}
	jsonWriter_endObject(writer);
}

static void writeOutputs(JsonWriter *writer, const RunManifest *manifest) {
	jsonWriter_beginArray(writer, "outputs");
	for (size_t i = 0; i < manifest->outputCount; ++i) {
		const ManifestOutput *output = &manifest->outputs[i];
		jsonWriter_beginObject(writer, NULL);
		jsonWriter_string(writer, "role", output->role);
		jsonWriter_string(writer, "path", output->name);
		writeOptional(writer, "bytes", output->read, output->bytes);
		writeOptional(writer, "rows", output->read, output->rows);
		writeText(writer, "sha256", output->read ? output->sha256 : NULL);
		if (strcmp(output->role, "results") == 0) {
			writeText(writer, "content_sha256", output->read ? output->contentSha256 : NULL);
		}
		jsonWriter_endObject(writer);
	}
	jsonWriter_endArray(writer);
}

static void writeSummary(JsonWriter *writer, const RunManifest *manifest) {
	const ResultsSummary *summary = manifest->summary;
	if (!summary) {
		jsonWriter_null(writer, "summary");
		return;
	}
	jsonWriter_beginObject(writer, "summary");
	jsonWriter_unsigned(writer, "matches", summary->matches);
	jsonWriter_unsigned(writer, "home", summary->home);
	jsonWriter_unsigned(writer, "draw", summary->draw);
	jsonWriter_unsigned(writer, "away", summary->away);
	jsonWriter_unsigned(writer, "home_goals", summary->homeGoals);
	jsonWriter_unsigned(writer, "away_goals", summary->awayGoals);
	jsonWriter_unsigned(writer, "unfinished", summary->unfinished);
	writeOptional(
		writer, "skipped", manifest->report != NULL, manifest->report ? manifest->report->skipped : 0
	);
	jsonWriter_endObject(writer);
}
