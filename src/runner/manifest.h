// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The manifest `fm24ctl run --manifest` writes beside its results, "fm24-run-manifest/1" (see
// docs/CLI.md): what ran, on what, how it went and what it wrote. commands.c gathers the facts and
// manifest_formatRun only formats them, so that tests can give their own clock and host.
#pragma once

#include "buffer.h"
#include "client.h"
#include "host_info.h"
#include "json_writer.h"
#include "match_config.h"
#include "run_job.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>


typedef enum RunOutcome {
	RUN_NOT_SUBMITTED, // the harness never took the job
	RUN_OK,
	RUN_ERROR,
	RUN_STOPPED, // the stop file stopped the job, and nothing failed
} RunOutcome;

// A results CSV's matches, summed up as `run` prints them.
typedef struct ResultsSummary {
	uint64_t matches; // the rows that read as results
	uint64_t home;
	uint64_t draw;
	uint64_t away;
	uint64_t homeGoals;
	uint64_t awayGoals;
	uint64_t unfinished; // `finished` 0
	double totalMs;      // the `ms` column's sum
} ResultsSummary;

// A file a run wrote.
typedef struct ManifestOutput {
	const char *role; // "results", "match_players", ...
	const char *name; // its file name, in the manifest's folder (UTF-8)
	bool read;        // false if it could not be read
	uint64_t bytes;
	uint64_t rows; // complete lines after the header
	char sha256[SHA256_HEX_LENGTH];
	// The results CSV's SHA-256 without each line's last field, `ms`, which differs between runs
	// ("" for the other files).
	char contentSha256[SHA256_HEX_LENGTH];
} ManifestOutput;

// The fixture a run played, from the bytes `run` reads and encodes.
typedef struct FixtureFacts {
	bool read; // the file was read
	uint64_t bytes;
	char sha256[SHA256_HEX_LENGTH];
	bool encoded; // the input the harness plays is known: the .bin, or the JSON's encoding
	uint64_t inputBytes;
	char inputSha256[SHA256_HEX_LENGTH];
	bool hasHeader; // a JSON config, whose header could be read
	ConfigHeader header;
} FixtureFacts;

// The game a run's job went to (macOS).
typedef struct GameFacts {
	uint32_t pid; // 0 if it is not running
	bool underRosetta;
	uint64_t residentBytes; // 0 if unknown
	const char *save;       // the save the job was checked for; NULL or "" for none
	const char *date;       // its in-game date then; NULL or "" if unknown
} GameFacts;

typedef struct RunManifest {
	time_t createdAt;
	RunOutcome outcome;
	int exitCode;
	bool submitted;              // the job went to the client
	ClientOutcome clientOutcome; // with `submitted`
	const char *result;          // the result line; NULL or "" for none
	// The command line from "run" on, in UTF-8; NULL for an argument that cannot be converted.
	const char *const *command;
	size_t commandCount;
	const char *commit; // fm24ctl's build (build_info.h)
	int dirty;
	const char *slice;
	const HostInfo *host;
	const RunReport *report; // NULL without one
	const GameFacts *game;   // NULL on Windows
	const char *fixturePath; // as given, made absolute
	const FixtureFacts *fixture;
	const RunJob *job; // the job's seeds, threads and flags
	bool noLaunch;
	bool anySave;
	time_t startedAt;  // when the job was submitted
	time_t finishedAt; // and when the client returned
	const ClientWatch *watch;
	uint64_t sleptMs; // how long the computer slept meanwhile
	const ManifestOutput *outputs;
	size_t outputCount;
	const ResultsSummary *summary; // NULL without the results
} RunManifest;

// How client outcomes are named in manifests: "replied", "no_harness", "queue_timeout",
// "run_timeout", "lost", "abandoned" and "failed".
const char *manifest_clientOutcomeName(ClientOutcome outcome);

// A run's outcome: not submitted unless the harness took the job (`watch` NULL if it never went to
// the client); ok if it replied with success; stopped if its report says only the stop file stopped
// it; else an error.
RunOutcome manifest_runOutcome(
	const ClientWatch *watch,
	ClientOutcome outcome,
	const char *result,
	const RunReport *report
);

// Reads a file a run wrote, once: its size, rows and SHA-256, and, given `summary`, as a results
// CSV, its SHA-256 without each line's last field and its matches. False (with `output->read`
// false) if it cannot be read.
bool manifest_readOutput(const wchar_t *path, ManifestOutput *output, ResultsSummary *summary);

// Appends the manifest's JSON to `out`; false if memory runs out.
bool manifest_formatRun(const RunManifest *manifest, ByteBuffer *out);

// Writes a run report as the member "harness" of the object being written: null without one. Batch
// manifests record the reports of their parts the same way.
void manifest_writeHarness(JsonWriter *writer, const RunReport *report);
