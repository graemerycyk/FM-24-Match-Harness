// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// What `fm24ctl batch run` does when something goes wrong (see docs/CLI.md "When things go
// wrong"): pure functions of what happened and what the batch knows, so that each row of the
// failure table has its own test. batch_run.c gathers the facts and does what they decide.
#pragma once

#include "batch_plan.h"
#include "batch_store.h"
#include "client.h"
#include "run_job.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	BATCH_EXIT_FAILED = 1,       // stopped by a failure; batch run resumes it
	BATCH_EXIT_CHANGED = 2,      // a usage error, or the frozen files changed
	BATCH_EXIT_WRONG_STATE = 3,  // the game is not in the state the batch needs
	BATCH_EXIT_STOPPED = 4,      // stopped as asked
	BATCH_BUSY_RETRY_S = 60,     // between tries while the game is busy
	BATCH_LAUNCH_RETRY_S = 300,  // the first wait after the game failed to start; the second is
	BATCH_LAUNCH_RETRY2_S = 900, // longer, and a third failure stops the batch
	BATCH_BATTERY_POLL_S = 60,   // between checks of the power while paused on battery
};

typedef enum BatchAction {
	BATCH_CONTINUE,          // go on: the next attempt, or the next chunk
	BATCH_RETRY,             // another attempt at the chunk from where its parts stop
	BATCH_RETRY_ONE_THREAD,  // the same with one worker, to find the seed that crashes the game
	BATCH_MARK_POISON,       // that seed is excluded from its chunk; go on after it
	BATCH_RELAUNCH,          // the game has gone: start it with the save, then retry
	BATCH_WAIT,              // wait `waitS`, then try again
	BATCH_KILL_AND_RELAUNCH, // quit the game (kill_game=1), then as BATCH_RELAUNCH
	BATCH_STOP,              // stop with `exitCode`
} BatchAction;

// How a job's reply reads.
typedef enum BatchReply {
	BATCH_REPLY_NONE,            // no reply (see the client's outcome)
	BATCH_REPLY_OK,              // every seed played or skipped
	BATCH_REPLY_STOPPED,         // the stop file stopped it
	BATCH_REPLY_SEED_FAILED,     // a seed failed: the report's failed_index and failed_stage
	BATCH_REPLY_OUTPUT_FAILED,   // a file could not be written
	BATCH_REPLY_BUSY,            // FM24 was loading or processing
	BATCH_REPLY_BAD_JOB,         // the harness could not read the job, or it was too long
	BATCH_REPLY_UNSUPPORTED,     // the harness does not support this game build
	BATCH_REPLY_NOT_REBUILDABLE, // the fixture can no longer be rebuilt per seed
	BATCH_REPLY_NO_REPORT,       // the reply lacks the report the job asked for
	BATCH_REPLY_ERROR,           // any other error
} BatchReply;

// Why the watchdog stopped waiting for a job.
typedef enum BatchWatchStop {
	BATCH_WATCH_NONE,
	BATCH_WATCH_GAME_GONE,    // the game's process exited
	BATCH_WATCH_HARNESS_GONE, // the game runs, but its harness no longer holds its lock
	BATCH_WATCH_HANG,         // no progress for hang_minutes while the game ran
	BATCH_WATCH_QUEUE,        // another job held the harness for queue_wait_minutes
	BATCH_WATCH_STOP,         // a stop before the harness took the job, or a stop-now unanswered
	BATCH_WATCH_GAVE_UP,      // a job that may still run did not end within queue_wait_minutes
	BATCH_WATCH_SIGNAL,       // a third signal
} BatchWatchStop;

// A stop asked of batch run and not yet acted on: it never starts the game again.
typedef enum BatchAsked {
	BATCH_ASKED_NONE,
	BATCH_ASKED_STOP,     // once the chunk under way is committed: control/stop, or a first signal
	BATCH_ASKED_STOP_NOW, // now: a control/stop-now of this run's, or a second signal
} BatchAsked;

typedef enum BatchEventKind {
	BATCH_EVENT_JOB,         // a job ended: an attempt (with its part), a warm-up, canary or lookup
	BATCH_EVENT_SAVE,        // the check of the game's save came back
	BATCH_EVENT_SLICE,       // the game runs the other slice than the plan's
	BATCH_EVENT_DATE,        // the save is at another in-game date than the fixtures
	BATCH_EVENT_IDENTITY,    // a report's harness, build or slice is not the pinned one
	BATCH_EVENT_CANARY,      // a canary, or chunk 0, differs from the reference
	BATCH_EVENT_SEED_ERRORS, // an arm's rebuild failures passed max_seed_errors
	BATCH_EVENT_DISK,        // less free space than the batch keeps
	BATCH_EVENT_BATTERY,     // the computer runs on battery
	BATCH_EVENT_SLEPT,       // the computer slept during a job
	BATCH_EVENT_RESUMED,     // a batch run starts after another stopped mid-chunk
	BATCH_EVENT_LOCKED,      // another batch run holds the batch's lock
	BATCH_EVENT_CHANGED,     // the plan or an input changed since create
	BATCH_EVENT_EDITS,       // a season's fixture played with edits: it does not rebuild to itself
} BatchEventKind;

// A job that ended, as far as the policy needs it.
typedef struct BatchJobEvent {
	ClientOutcome client;
	BatchWatchStop watch;
	bool accepted;         // the harness took the job
	bool gameRunning;      // the game's process, once the job ended
	bool harnessListening; // and its harness
	bool isolating;        // an attempt the policy shaped to find a seed that crashes the game
	BatchReply reply;
	bool hasReport;
	RunReport report;
	// An attempt's part, evaluated, and the attempt at the chunk before it (NULL for none).
	const BatchPart *part;
	const BatchPart *previous;
} BatchJobEvent;

// What the check of the game's save came back with.
typedef struct BatchSaveEvent {
	int code;               // macosGame_checkSave's: 0, 1 or 3
	SessionOutcome outcome; // how the wait for the save ended (SESSION_WAITING if none)
	bool gameRunning;       // after the check
} BatchSaveEvent;

typedef struct BatchEvent {
	BatchEventKind kind;
	BatchJobEvent job;   // BATCH_EVENT_JOB
	BatchSaveEvent save; // BATCH_EVENT_SAVE
} BatchEvent;

// What the batch knows besides the event.
typedef struct BatchPolicyState {
	const BatchPlan *plan;
	uint32_t threads; // the batch's workers per job
	bool allowHarnessChange;
	unsigned crashes;        // the game's counted crashes in the last hour, before this event
	uint64_t armPoison;      // poison seeds of the attempt's arm, before this part
	uint64_t busyMs;         // how long the game has been busy so far (since the first refusal)
	unsigned launchFailures; // the game's failed starts in a row, before this one
	uint64_t minuteMs;       // a minute (shorter in tests)
	BatchAsked asked;        // a stop asked for
} BatchPolicyState;

typedef struct BatchDecision {
	BatchAction action;
	int exitCode;       // BATCH_STOP's
	const char *reason; // a fixed text for events.log, batch.json and notifications
	unsigned waitS;     // BATCH_WAIT's
	bool crash;         // the event is a crash of the game
	bool counted;       // and counts toward max_crashes_per_hour (one isolating a seed does not)
	bool busy;          // the game was busy (busy_wait_minutes)
	bool launchFailed;  // the game failed to start
	bool notify;        // worth a notification: a stop, a hang or a canary mismatch
} BatchDecision;

// The next attempt at a chunk, from its attempts so far.
typedef struct BatchAttemptShape {
	uint32_t threads;
	uint64_t maxCount;  // the most indices to give it; 0 for all that are left
	bool hasCandidate;  // an index that crashed the game with one worker: offered as a poison seed
	uint64_t candidate; // (see BatchPartFacts)
	bool isolating;     // the shape is one of those above, which look for a seed (not all workers)
} BatchAttemptShape;

BatchDecision batchPolicy_decide(const BatchPolicyState *state, const BatchEvent *event);

// After a crash, the next attempt plays with one worker, over the seeds the last one had under way
// when the game went before; after a crash with one worker, it plays only the seed it crashed at,
// which is poison if the game crashes there again. Attempts at the end that neither crashed nor
// played anything (a busy game, a stop before the first match) tell nothing, and are passed over.
void batchPolicy_shape(
	const BatchPart *parts,
	size_t count,
	uint32_t threads,
	BatchAttemptShape *out
);

// True if an attempt ended with the game gone: crashed, or abandoned after a hang and quit.
bool batchPolicy_crashLike(const BatchPart *part);

// How a job's result line reads; `reportAsked` if the job had report=1.
BatchReply batchPolicy_readReply(const char *result, bool reportAsked, RunReport *report);

// True if `failed` rebuild failures among `done` indices of an arm pass the plan's limit: a count
// at once, a percentage once 1,000 indices are done.
bool batchPolicy_tooManySeedErrors(const BatchErrorLimit *limit, uint64_t failed, uint64_t done);
