// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The run job fm24ctl submits to the harness (see protocol.h), the report its result line can
// carry, and the check of the game's save that comes before a job.
#pragma once

#include "buffer.h"
#include "saves.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// A run job's settings; the texts are UTF-8 and the paths absolute.
typedef struct RunJob {
	const char *input;  // the encoded fixture (.bin) to play
	const char *seeds;  // "keep", "42", "1-1000" or "1,5,10-20"
	const char *output; // the results CSV
	unsigned long threads;
	const char *profile; // the profiler's samples file; NULL or "" for none
	bool detailed;
	bool humanControl;
	bool keepBuild;
	uint32_t teams[2];       // the home and away team overrides, 0 for none
	uint64_t progressWindow; // Windows: the viewer's window for progress messages, 0 for none
	// Protocol 1.4.0's keys, sent only when set (see job.h):
	bool perMatch;        // write each match's player and team rows
	bool skipSeedErrors;  // record and skip seeds whose rebuild fails (seed_errors=skip)
	bool report;          // append the run's report to the result line
	uint64_t indexBase;   // the per-match rows' index of the first seed
	const char *stopFile; // no seed starts once this file exists; NULL or "" for none
} RunJob;

// The report a run's result line carries with report=1 (protocol 1.4.0; see docs/TECHNICAL.md).
typedef struct RunReport {
	char protocol[16];  // the harness's protocol
	char commit[48];    // its commit's 40 hex digits, or "unknown"
	int dirty;          // 1 or 0, -1 with an unknown commit
	uint32_t build;     // the game's build
	char slice[16];     // "arm64" or "x86_64"
	bool translated;    // the game runs under Rosetta
	uint32_t pid;       // the game's process
	int64_t loaded;     // when the harness loaded, in seconds since 1970 (0 if unknown)
	uint64_t jobs;      // the jobs other than status it has received since, this one included
	uint32_t threads;   // workers started
	bool hasEdits;      // false without a per-seed rebuild ("-")
	uint64_t edits;     // the input's edits re-applied to each seed
	uint64_t elapsedMs; // the job's time
	uint64_t written;   // places written, played or skipped
	uint64_t skipped;   // seeds skipped because their rebuild failed
	bool stopped;       // the stop file kept a seed from starting
	bool hasFailure;    // a failed seed stopped the job
	uint64_t failedIndex;
	char failedStage[16]; // "rebuild", "deserialize" or "stats"; "" without a failure
	bool outputFailed;    // a file could not be written
} RunReport;

// The save a job is for, which the game must have loaded and idle first (see runJob_ensureSave).
typedef struct SaveCheck {
	char save[SAVES_NAME_LENGTH]; // "" for none: the job goes ahead without a check
	char date[SAVES_DATE_LENGTH]; // the in-game date the fixture was exported at, "" if unknown
	bool badSave;                 // the fixture's "save" is not a save's name
	bool noLaunch;                // --no-launch
	bool anySave;                 // --any-save
} SaveCheck;

// Writes the job's text, its keys in a fixed order: those of protocol 1.4.0 last, and only when
// set, so that a job without them is unchanged. False if it and its NUL do not fit in `size` bytes
// or in a job (HARNESS_JOB_LENGTH).
bool runJob_format(const RunJob *job, char *out, size_t size);

// Reads the report after "; report " at the end of a result line: every key once, in the
// harness's order, with valid values. False (with `report` cleared) if there is none or it cannot
// be read.
bool runReport_parse(const char *result, RunReport *report);

// The number of seeds a numeric seed list ("42", "1-1000", "1,5,10-20") plays, and its first and
// last seeds, as the harness reads the list. False for "keep" and for a list the harness refuses.
bool runJob_countSeeds(const char *seeds, uint64_t *count, uint32_t *first, uint32_t *last);

// True if `name` can be a save's name (saves_isName) and has no control characters.
bool runJob_isSaveName(const char *name);

// The save check a fixture's "save" and "game_date" ask for. Only the save's last path part counts,
// as in the harness's status (saves_isSave).
void runJob_readFixtureSave(const ByteBuffer *config, SaveCheck *check);

// Before a job for `check`'s save: makes sure the game has it loaded and idle (see
// macosGame_ensureSave), letting each status query wait for another runner's job as long as the job
// itself may (`timeoutMs`), and writes the save's in-game date to `date` if given ("" if unknown).
// 0 to submit the job, else the exit code (see commands.h). Without a save, and on Windows, whose
// harness has no status job yet, the job goes ahead as before.
int runJob_ensureSave(const SaveCheck *check, unsigned timeoutMs, char *date, size_t size);

// Warns if the loaded save is at another in-game date (`date`) than the one the fixture was
// exported at.
void runJob_warnIfDated(const SaveCheck *check, const char *date);
