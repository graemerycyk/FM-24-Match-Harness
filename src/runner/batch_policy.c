// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_policy.h"

#include <string.h>


enum {
	PERCENT_SCALE = 100000, // BatchErrorLimit's percentages are in thousandths of a percent
	PERCENT_AFTER = 1000,   // indices of an arm done before a percentage counts
};

static BatchDecision decideJob(const BatchPolicyState *state, const BatchJobEvent *job);
static BatchDecision decideStalled(const BatchPolicyState *state);
static BatchDecision decideReply(const BatchPolicyState *state, const BatchJobEvent *job);
static BatchDecision decideCrash(const BatchPolicyState *state, const BatchJobEvent *job);
static BatchDecision decideAdopted(const BatchJobEvent *job);
static BatchDecision decideBusy(const BatchPolicyState *state);
static BatchDecision decideSave(const BatchPolicyState *state, const BatchSaveEvent *save);
static bool sameFailure(const BatchJobEvent *job);
static BatchDecision act(BatchAction action, const char *reason);
static BatchDecision stop(int exitCode, const char *reason);
static BatchDecision stopAsked(const char *reason);

BatchDecision batchPolicy_decide(const BatchPolicyState *state, const BatchEvent *event) {
	const BatchPlan *plan = state->plan;
	BatchDecision decision;
	switch (event->kind) {
		case BATCH_EVENT_JOB: return decideJob(state, &event->job);
		case BATCH_EVENT_SAVE: return decideSave(state, &event->save);
		case BATCH_EVENT_SLICE:
			if (plan->killGame) {
				return act(BATCH_KILL_AND_RELAUNCH, "the game runs the other slice; quitting it");
			}
			return stop(BATCH_EXIT_WRONG_STATE, "the game runs the other slice than the plan's");
		case BATCH_EVENT_DATE:
			return stop(BATCH_EXIT_FAILED, "the save is at another in-game date than the fixtures");
		case BATCH_EVENT_IDENTITY:
			if (state->allowHarnessChange) {
				return act(BATCH_CONTINUE, "the harness, game build or slice changed, as allowed");
			}
			return stop(BATCH_EXIT_FAILED, "the harness, game build or slice is not the pinned one");
		case BATCH_EVENT_CANARY:
			if (plan->onCanaryMismatch == BATCH_MISMATCH_CONTINUE) {
				decision = act(BATCH_CONTINUE, "results no longer repeat; going on, as the plan asks");
				decision.notify = true;
				return decision;
			}
			return stop(BATCH_EXIT_FAILED, "results no longer repeat: a canary differs");
		case BATCH_EVENT_SEED_ERRORS:
			return stop(BATCH_EXIT_FAILED, "too many seeds failed to rebuild (max_seed_errors)");
		case BATCH_EVENT_DISK: return stop(BATCH_EXIT_FAILED, "not enough free space (min_free_gb)");
		case BATCH_EVENT_BATTERY:
			if (!plan->pauseOnBattery) {
				return act(BATCH_CONTINUE, "on battery power");
			}
			decision = act(BATCH_WAIT, "paused on battery power");
			decision.waitS = BATCH_BATTERY_POLL_S;
			return decision;
		case BATCH_EVENT_SLEPT: return act(BATCH_CONTINUE, "the computer slept");
		case BATCH_EVENT_RESUMED: return act(BATCH_CONTINUE, "resumed after an interruption");
		case BATCH_EVENT_LOCKED:
			decision = stop(BATCH_EXIT_FAILED, "another batch run is running this batch");
			decision.notify = false;
			return decision;
		case BATCH_EVENT_CHANGED:
			decision = stop(BATCH_EXIT_CHANGED, "the plan or an input changed since create");
			decision.notify = false;
			return decision;
		case BATCH_EVENT_EDITS:
			return stop(BATCH_EXIT_FAILED, "a season's fixture does not rebuild to itself (edits)");
	}
	return stop(BATCH_EXIT_FAILED, "an unknown event");
}

void batchPolicy_shape(
	const BatchPart *parts,
	size_t count,
	uint32_t threads,
	BatchAttemptShape *out
) {
	*out = (BatchAttemptShape){.threads = threads};
	while (count && !batchPolicy_crashLike(&parts[count - 1]) && !parts[count - 1].verified) {
		--count;
	}
	const BatchPart *last = count ? &parts[count - 1] : NULL;
	const BatchPart *before = count > 1 ? &parts[count - 2] : NULL;
	if (!last || !batchPolicy_crashLike(last) || last->hasPoison || last->verified >= last->count) {
		return;
	}
	if (last->threads == 1) {
		out->threads = 1;
		out->maxCount = 1;
		out->hasCandidate = true;
		out->candidate = last->first + last->verified;
		out->isolating = true;
	} else if (before && batchPolicy_crashLike(before) && last->verified < last->threads) {
		out->threads = 1;
		out->maxCount = last->threads;
		out->isolating = true;
	}
}

bool batchPolicy_crashLike(const BatchPart *part) {
	return part->evaluated &&
				 (part->outcome == BATCH_PART_CRASHED || part->outcome == BATCH_PART_ABANDONED);
}

BatchReply batchPolicy_readReply(const char *result, bool reportAsked, RunReport *report) {
	bool hasReport = result && runReport_parse(result, report);
	if (!result || !result[0]) {
		return BATCH_REPLY_NONE;
	}
	if (strncmp(result, "error: FM24 is busy", 19) == 0) {
		return BATCH_REPLY_BUSY;
	}
	if (strstr(result, "cannot read the job") || strstr(result, "the job is too long")) {
		return BATCH_REPLY_BAD_JOB;
	}
	if (strstr(result, "does not support this game build")) {
		return BATCH_REPLY_UNSUPPORTED;
	}
	if (strstr(result, "(--keep-build plays the input with only its seed changed)")) {
		return BATCH_REPLY_NOT_REBUILDABLE;
	}
	if (strncmp(result, "error: cannot start the run", 27) == 0) {
		return BATCH_REPLY_OUTPUT_FAILED;
	}
	if (reportAsked && !hasReport) {
		return BATCH_REPLY_NO_REPORT;
	}
	if (hasReport && report->outputFailed) {
		return BATCH_REPLY_OUTPUT_FAILED;
	}
	if (hasReport && report->hasFailure) {
		return BATCH_REPLY_SEED_FAILED;
	}
	if (hasReport && report->stopped) {
		return BATCH_REPLY_STOPPED;
	}
	return client_succeeded(result) ? BATCH_REPLY_OK : BATCH_REPLY_ERROR;
}

bool batchPolicy_tooManySeedErrors(const BatchErrorLimit *limit, uint64_t failed, uint64_t done) {
	if (!limit->percent) {
		return failed > limit->value;
	}
	return done >= PERCENT_AFTER && failed * PERCENT_SCALE > limit->value * done;
}

static BatchDecision decideJob(const BatchPolicyState *state, const BatchJobEvent *job) {
	const BatchPlan *plan = state->plan;
	BatchDecision decision;
	if (job->client == CLIENT_REPLIED) {
		return decideReply(state, job);
	}
	if (job->watch == BATCH_WATCH_SIGNAL) {
		return stopAsked("stopped by a signal");
	}
	if (job->watch == BATCH_WATCH_GAVE_UP) {
		return stop(
			BATCH_EXIT_FAILED, "a job that may still run in FM24 did not end within queue_wait_minutes"
		);
	}
	if (job->client == CLIENT_QUEUE_TIMEOUT || job->watch == BATCH_WATCH_QUEUE) {
		return stop(BATCH_EXIT_FAILED, "a job held the harness for queue_wait_minutes");
	}
	if (job->watch == BATCH_WATCH_STOP && !job->accepted) {
		// Asked to stop before the harness took the job, which it now never runs.
		return stopAsked(
			state->asked == BATCH_ASKED_STOP_NOW ? "stopped as asked (stop-now)" : "stopped as asked"
		);
	}
	if (!job->gameRunning) {
		if (job->accepted) {
			return decideCrash(state, job);
		}
		// The game went before the job reached it: nothing of the batch's was under way.
		if (state->asked) {
			return stopAsked("stopped as asked; the game is not running");
		}
		if (!plan->launch) {
			return stop(BATCH_EXIT_FAILED, "the game is not running, and launch=0");
		}
		return act(BATCH_RELAUNCH, "the game is not running");
	}
	if (!job->harnessListening) {
		if (!plan->killGame) {
			return stop(BATCH_EXIT_FAILED, "the game runs without this harness; restart it with it");
		}
		decision = act(BATCH_KILL_AND_RELAUNCH, "the game runs without this harness; quitting it");
		decision.crash = job->accepted;
		decision.counted = job->accepted;
		return decision;
	}
	if (job->part && job->part->outcome == BATCH_PART_ADOPTED) {
		return decideAdopted(job);
	}
	if (job->watch == BATCH_WATCH_HANG || job->watch == BATCH_WATCH_STOP ||
			job->client == CLIENT_RUN_TIMEOUT) {
		return decideStalled(state);
	}
	if (job->client == CLIENT_FAILED) {
		return stop(BATCH_EXIT_FAILED, "the harness's reply could not be read");
	}
	// The harness dropped the job without a reply but still serves: try it again.
	return act(BATCH_RETRY, "the harness ended a job without a reply");
}

// A job made no progress (a hang, or a stop-now left unanswered) in a game still running and
// serving.
static BatchDecision decideStalled(const BatchPolicyState *state) {
	BatchDecision decision;
	if (state->plan->killGame) {
		// With a stop asked, the crash that follows stops the batch rather than relaunching.
		decision = act(BATCH_KILL_AND_RELAUNCH, "a job made no progress; quitting the game");
		decision.crash = true;
		decision.counted = true;
	} else if (state->asked) {
		// The job is left to run: the next batch run waits for it, and keeps what it played.
		decision = stopAsked("stopped as asked; a job that made no progress still runs in FM24");
	} else {
		// The batch watches the job until it ends, as at a start, and keeps what it played.
		decision = act(BATCH_WAIT, "a job made no progress; waiting for it to end");
	}
	decision.notify = true;
	return decision;
}

static BatchDecision decideReply(const BatchPolicyState *state, const BatchJobEvent *job) {
	const BatchPart *part = job->part;
	const BatchPart *previous = job->previous;
	switch (job->reply) {
		case BATCH_REPLY_OK:
			if (!part || part->verified >= part->count) {
				return act(BATCH_CONTINUE, NULL);
			}
			// The files do not hold all the job reported: keep what they do, once.
			if (previous && previous->evaluated && previous->outcome == BATCH_PART_OK &&
					previous->first == part->first && previous->verified == part->verified) {
				return stop(BATCH_EXIT_FAILED, "a job's files twice lacked rows it reported");
			}
			return act(BATCH_RETRY, "a job's files lack rows it reported");
		case BATCH_REPLY_STOPPED: return stopAsked("stopped as asked (stop-now)");
		case BATCH_REPLY_SEED_FAILED:
			if (!part) {
				return stop(BATCH_EXIT_FAILED, "a seed of a warm-up or canary failed");
			}
			if (sameFailure(job)) {
				return stop(BATCH_EXIT_FAILED, "the same seed failed twice; see harness.log");
			}
			return act(BATCH_RETRY, "a seed failed; trying it once more");
		case BATCH_REPLY_OUTPUT_FAILED:
			return stop(BATCH_EXIT_FAILED, "the harness could not write a file; is the disk full?");
		case BATCH_REPLY_BUSY: return decideBusy(state);
		case BATCH_REPLY_BAD_JOB:
			return stop(BATCH_EXIT_CHANGED, "the harness could not read a job (a bug)");
		case BATCH_REPLY_UNSUPPORTED:
			return stop(BATCH_EXIT_FAILED, "the harness does not support this game build");
		case BATCH_REPLY_NOT_REBUILDABLE:
			return stop(BATCH_EXIT_FAILED, "the fixture can no longer be rebuilt per seed");
		case BATCH_REPLY_NO_REPORT:
			return stop(BATCH_EXIT_FAILED, "the harness's reply has no report; is it 1.4.0?");
		default: return stop(BATCH_EXIT_FAILED, "a job failed; see harness.log");
	}
}

// The game went while a job of the batch's ran in it.
static BatchDecision decideCrash(const BatchPolicyState *state, const BatchJobEvent *job) {
	const BatchPlan *plan = state->plan;
	const BatchPart *part = job->part;
	BatchDecision decision;
	// A crash while isolating a seed is bounded by max_poison: only the others count toward the
	// hour's limit, so that a seed found to crash the game does not use it up. With a stop asked,
	// the game is not started again: the part (and any poison seed) is kept for the next run, which
	// starts the game unless launch=0.
	static const char exitedAsked[] =
		"stopped as asked after the game exited; the next batch run starts it and goes on";
	static const char exitedAskedNoLaunch[] =
		"stopped as asked after the game exited; start it with the save, then run the batch again";
	if (!plan->launch) {
		decision = state->asked ? stopAsked(exitedAskedNoLaunch)
														: stop(BATCH_EXIT_FAILED, "the game exited, and launch=0");
	} else if (!job->isolating && state->crashes >= plan->maxCrashesPerHour) {
		decision = stop(BATCH_EXIT_FAILED, "the game crashed more than max_crashes_per_hour times");
	} else if (part && part->hasPoison) {
		decision = state->armPoison >= plan->maxPoison
								 ? stop(BATCH_EXIT_FAILED, "more seeds crashed the game than max_poison")
							 : state->asked ? stopAsked(exitedAsked)
															: act(BATCH_MARK_POISON, "a seed crashed the game twice: excluded");
	} else if (state->asked) {
		decision = stopAsked(exitedAsked);
	} else {
		BatchAttemptShape shape = {.threads = state->threads};
		if (part) {
			const BatchPart parts[2] = {job->previous ? *job->previous : (BatchPart){0}, *part};
			batchPolicy_shape(parts, 2, state->threads, &shape);
		}
		if (shape.hasCandidate) {
			decision = act(BATCH_RETRY_ONE_THREAD, "the game crashed with one worker; trying that seed");
		} else if (shape.threads < state->threads) {
			decision = act(BATCH_RETRY_ONE_THREAD, "the game crashed twice; finding the seed");
		} else {
			decision = act(BATCH_RELAUNCH, "the game exited during a job");
		}
	}
	decision.crash = true;
	decision.counted = !job->isolating;
	return decision;
}

// A job the batch stopped waiting for has ended, and what it played is kept.
static BatchDecision decideAdopted(const BatchJobEvent *job) {
	const BatchPart *part = job->part;
	return part->verified >= part->count ? act(BATCH_CONTINUE, NULL)
																			 : act(BATCH_RETRY, "a job ended without every index");
}

static BatchDecision decideBusy(const BatchPolicyState *state) {
	const BatchPlan *plan = state->plan;
	BatchDecision decision;
	if (state->busyMs >= (uint64_t)plan->busyWaitMinutes * state->minuteMs) {
		decision = stop(BATCH_EXIT_WRONG_STATE, "the game stayed busy for busy_wait_minutes");
	} else {
		decision = act(BATCH_WAIT, "the game is busy");
		decision.waitS = BATCH_BUSY_RETRY_S;
	}
	decision.busy = true;
	return decision;
}

static BatchDecision decideSave(const BatchPolicyState *state, const BatchSaveEvent *save) {
	const BatchPlan *plan = state->plan;
	BatchDecision decision;
	if (save->outcome == SESSION_STOPPED) {
		// Asked while the check waited, before any job of the batch's.
		return stopAsked(
			state->asked == BATCH_ASKED_STOP_NOW ? "stopped as asked (stop-now)" : "stopped as asked"
		);
	}
	if (save->code == 0) {
		// With launch=0 nothing starts a game that is not running, so no session would play.
		return save->gameRunning || plan->launch
						 ? act(BATCH_CONTINUE, NULL)
						 : stop(BATCH_EXIT_FAILED, "the game is not running, and launch=0");
	}
	if (save->code == BATCH_EXIT_WRONG_STATE) {
		switch (save->outcome) {
			case SESSION_OTHER_SAVE: return stop(BATCH_EXIT_WRONG_STATE, "another save is loaded");
			case SESSION_AT_MENU:
				if (plan->killGame) {
					return act(BATCH_KILL_AND_RELAUNCH, "the game is at its start screen; quitting it");
				}
				return stop(BATCH_EXIT_WRONG_STATE, "the game is at its start screen without the save");
			case SESSION_NO_TEAM:
				return stop(BATCH_EXIT_WRONG_STATE, "the loaded game has no human manager's team");
			case SESSION_TIMED_OUT: return decideBusy(state);
			default: return stop(BATCH_EXIT_WRONG_STATE, "the game is not in the state the batch needs");
		}
	}
	if (save->gameRunning) {
		return stop(BATCH_EXIT_FAILED, "the game is not ready for the batch");
	}
	// The game did not start (Steam not running, or asking to log in): try again later.
	unsigned failures = state->launchFailures + 1;
	if (failures >= 3) {
		decision = stop(BATCH_EXIT_FAILED, "the game failed to start three times; is Steam running?");
	} else {
		decision = act(BATCH_WAIT, "the game failed to start");
		decision.waitS = failures == 1 ? BATCH_LAUNCH_RETRY_S : BATCH_LAUNCH_RETRY2_S;
	}
	decision.launchFailed = true;
	return decision;
}

// The attempt failed at the same index, at the same stage, as the one before it.
static bool sameFailure(const BatchJobEvent *job) {
	const BatchPart *previous = job->previous;
	RunReport before;
	return previous && previous->evaluated && previous->outcome == BATCH_PART_ERROR &&
				 job->hasReport && job->report.hasFailure &&
				 runReport_parse(previous->resultLine, &before) && before.hasFailure &&
				 before.failedIndex == job->report.failedIndex &&
				 strcmp(before.failedStage, job->report.failedStage) == 0;
}

static BatchDecision act(BatchAction action, const char *reason) {
	return (BatchDecision){.action = action, .reason = reason};
}

static BatchDecision stop(int exitCode, const char *reason) {
	return (BatchDecision){
		.action = BATCH_STOP, .exitCode = exitCode, .reason = reason, .notify = true};
}

// Stopped as asked: no notification, as the person who asked knows.
static BatchDecision stopAsked(const char *reason) {
	BatchDecision decision = stop(BATCH_EXIT_STOPPED, reason);
	decision.notify = false;
	return decision;
}
