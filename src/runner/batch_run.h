// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// `fm24ctl batch run` (see docs/CLI.md "Run a batch: batch run"): plays a batch's chunks in the
// plan's order until each is committed, resuming wherever an earlier run stopped. Everything it
// needs from the game and the computer goes through the BatchHost, so that tests can stand in for
// them; batch_policy.c decides what to do when something goes wrong.
#pragma once

#include "batch_store.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wchar.h>


enum {
	BATCH_TICK_S = 5,         // between the watchdog's checks while a job runs
	BATCH_STATUS_WAIT_S = 10, // how long a status query may wait while a job may still run
	BATCH_SLEEP_GAP_S = 30,   // the clocks drifting apart this much in a tick is the computer asleep
	BATCH_STOP_GRACE_S = 60,  // how long the game has to quit (kill_game=1) before it is killed
	BATCH_STOP_NOW_GRACE_S = 30, // how long a job asked to stop now may make no progress (the
															 // matches under way finishing) before batch run stops waiting
	BATCH_EXIT_GRACE_S = 10,     // how long a game whose harness went has to exit before it counts
															 // as running without its harness
	BATCH_RUN_KILLED = 255,      // a test's BatchHost.reached stopped the run, as if it were killed
};

typedef struct BatchRunOptions {
	uint32_t threads; // --threads; 0 for the plan's
	bool hasUntil;
	time_t until;       // --until: stop at the first chunk boundary from then
	uint64_t maxChunks; // --max-chunks: stop once this run has committed this many; 0 for no limit
	bool allowHarnessChange;
	unsigned secondMs; // a second of the batch's waits and timers; 0 for 1000 (tests shorten it)
} BatchRunOptions;

// Runs the batch in `directory` (absolute) until every chunk is committed, or something stops it.
// Returns the exit code: 0 complete; 1 stopped by a failure, which batch run resumes; 2 the frozen
// files changed; 3 the game is not in the state the batch needs; 4 stopped as asked.
int batchRun_run(const wchar_t *directory, const BatchRunOptions *options, const BatchHost *host);

// The first time from `now` on whose local time of day is hour:minute (--until).
time_t batchRun_nextLocalTime(time_t now, int hour, int minute);
