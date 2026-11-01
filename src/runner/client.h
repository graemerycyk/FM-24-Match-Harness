// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Submitting jobs to the injected DLL (see protocol.h).
#pragma once

#include "protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	CLIENT_SHORT_TIMEOUT_MS = 10 * 60 * 1000, // builds, exports, rebuilds
	CLIENT_LONG_TIMEOUT_MS = 6 * 3600 * 1000, // batches of matches
	CLIENT_STATUS_TIMEOUT_MS = 30 * 1000,     // a status query, which only waits for another job
	CLIENT_RESULT_LENGTH = HARNESS_RESULT_LENGTH,
};

// Sends the job to the DLL and waits for its result, at most `timeoutMs` in all (including any
// wait for another runner's job to finish). Injects the DLL first if this protocol's harness is not
// loaded in fm.exe yet. If `result` is non-NULL it receives the DLL's one-line result (or a note to
// check harness.log). Prints and returns false on failure.
bool client_submit(const char *job, unsigned timeoutMs, char *result, size_t resultSize);

// How a watched job ended (client_submitWatched).
typedef enum ClientOutcome {
	CLIENT_REPLIED,       // the harness replied with a result line
	CLIENT_NO_HARNESS,    // no harness of this protocol served the job, or it went before taking it
	CLIENT_QUEUE_TIMEOUT, // the time ran out before the harness took the job, behind another one
	CLIENT_RUN_TIMEOUT,   // the time ran out after the harness took the job, which may still run
	CLIENT_LOST,          // the harness took the job, then closed the connection without a reply
	CLIENT_ABANDONED,     // the tick stopped the wait; a job the harness has taken keeps running
	CLIENT_FAILED,        // anything else, such as a reply that is not one line
} ClientOutcome;

// What a watched job is waiting for.
typedef enum ClientPhase {
	CLIENT_CONNECTING, // a connection to the harness
	CLIENT_QUEUED,     // the harness to take the job, once any job before it has finished
	CLIENT_RUNNING,    // the result, after the harness has taken the job
} ClientPhase;

// Called about every `tickMs` while a watched job waits. False stops the wait (CLIENT_ABANDONED):
// the connection closes, and a job the harness has taken keeps running.
typedef bool ClientTick(void *context, ClientPhase phase);

typedef struct ClientWatch {
	unsigned tickMs;  // between ticks; no ticks if 0 or without `tick`
	ClientTick *tick; // NULL for none
	void *context;
	bool quiet; // no notice while the job waits for another runner's (the caller says why it waits)
	// Set by client_submitWatched:
	bool accepted;     // the harness took the job
	uint64_t queuedMs; // from the call until the harness took the job, or the call ended
	uint64_t runMs;    // from when it took the job until the call ended; 0 if it did not
} ClientWatch;

// As client_submit, telling how the job ended, with ticks while it waits and its timings in `watch`
// if given. The reply is in `result` only with CLIENT_REPLIED. Prints why the job failed, except
// when the tick stopped the wait. On Windows the harness takes a job when the pipe connects, there
// are no ticks yet, and a failure is CLIENT_RUN_TIMEOUT or CLIENT_FAILED.
ClientOutcome client_submitWatched(
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t resultSize
);

// True if a result line reports success.
bool client_succeeded(const char *result);
