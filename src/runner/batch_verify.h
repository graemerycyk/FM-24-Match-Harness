// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The batch verifier: reads the files of a part (the results, per-match rows and failed seeds the
// harness wrote for one job) or of a committed chunk, in one forward pass, and finds the longest
// run of indices from the first whose rows are complete and agree in every file. Torn lines, the
// zero-filled tails power loss leaves and rows past that run do not count. With outputs, the same
// pass merges the rows of that run into a chunk's files, giving each result row its index. Pure:
// it reads and writes only the streams it is given.
#pragma once

#include "batch_plan.h"
#include "manifest.h"
#include "sha256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>


// The files of a part or a chunk.
typedef enum BatchRole {
	BATCH_RESULTS, // part: <stem>.csv; chunk: results.csv ("fm24-batch-results/1", index first)
	BATCH_PLAYERS, // -match-players.csv
	BATCH_TEAMS,   // -match-teams.csv
	BATCH_FAILED,  // -failed.csv
	BATCH_ROLES,
} BatchRole;

// Each file's header line: the harness's, and a chunk's results, which have an index first.
extern const char batchVerify_partResultsHeader[];
extern const char batchVerify_chunkResultsHeader[];
extern const char batchVerify_playersHeader[];
extern const char batchVerify_teamsHeader[];
extern const char batchVerify_failedHeader[];

// A file being written, with its size, rows and SHA-256 (and for results, the SHA-256 without each
// line's last field, `ms`, as a run manifest's content_sha256).
typedef struct BatchSink {
	FILE *file;
	uint64_t bytes;
	uint64_t rows; // lines after the header
	Sha256 raw;
	Sha256 content;
	bool hashContent;
	bool failed; // a write failed
} BatchSink;

// What the verifier reads, and where it merges to.
typedef struct BatchVerify {
	const BatchSeeds *seeds;
	uint64_t first; // the first index
	uint64_t count; // how many indices to verify at most
	bool chunk;     // a committed chunk's files, not a part's
	bool perMatch;  // the per-match rows are expected
	bool failed;    // the failed seeds are expected (seed_errors=skip)
	// A chunk's indices that have no rows (poison seeds), ascending.
	const uint64_t *excluded;
	size_t excludedCount;
	// Each expected file, NULL if it is missing (then nothing verifies); NULL for the others.
	FILE *inputs[BATCH_ROLES];
	BatchSink *outputs; // [BATCH_ROLES] to merge into, or NULL
	bool hashInputs;    // read each input to its end, hashing it
	uint32_t teams[2];  // the home and away team_uid each match's rows must have (0: any)
} BatchVerify;

// An input read to its end (with hashInputs).
typedef struct BatchInput {
	uint64_t bytes;
	uint64_t rows; // complete lines after the header
	char sha256[SHA256_HEX_LENGTH];
	char contentSha256[SHA256_HEX_LENGTH]; // results only
} BatchInput;

typedef struct BatchVerified {
	uint64_t verified;          // indices covered from the first: by rows, failed rows or exclusion
	uint64_t rows[BATCH_ROLES]; // the rows of those indices
	ResultsSummary summary;     // of their results
	bool trailing;              // with hashInputs: an input has more after those rows
	BatchInput inputs[BATCH_ROLES];
} BatchVerified;

// Verifies (and merges) as above. False if memory runs out or a stream cannot be read or written
// (an output's `failed` tells which).
bool batchVerify_run(const BatchVerify *request, BatchVerified *out);

// Starts a sink on `file` (NULL: one that writes nothing) with its header line.
void batchSink_start(BatchSink *sink, FILE *file, const char *header, bool hashContent);
// Appends a row, `length` bytes ending with its newline.
void batchSink_line(BatchSink *sink, const char *line, size_t length);
// The sink's SHA-256 and content SHA-256 ("" without hashContent).
void batchSink_finish(
	BatchSink *sink,
	char sha256[SHA256_HEX_LENGTH],
	char contentSha256[SHA256_HEX_LENGTH]
);
