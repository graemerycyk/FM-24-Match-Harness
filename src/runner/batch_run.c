// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_run.h"

#include "batch_policy.h"
#include "batch_season.h"
#include "files.h"
#include "paths.h"
#include "saves.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


enum {
	EXIT_COMPLETE = 0,
	CRASH_WINDOW_S = 3600, // max_crashes_per_hour's hour
	MAX_CRASHES = 1024,    // crashes the run remembers
	UTF8_LENGTH = BATCH_PATH_LENGTH,
};

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

// A job's own limit, about 49 days: the watchdog decides first.
static const unsigned jobTimeoutMs = 0xFFFFFFFFU;

// The files a job's harness writes beside its results CSV.
static const char *const siblingSuffixes[BATCH_ROLES] = {
	"",
	"-match-players",
	"-match-teams",
	"-failed",
};

// What the loop does next.
typedef enum Flow {
	FLOW_ON,     // go on with this step
	FLOW_AGAIN,  // back to the top of the loop
	FLOW_EXIT,   // stop with the run's exit code
	FLOW_KILLED, // BatchHost.reached stopped the run (tests)
} Flow;

// A chunk committed in the current session since its last good canary.
typedef struct CommittedRef {
	size_t arm;
	uint64_t first;
} CommittedRef;

typedef struct Run {
	Batch *batch;
	const BatchHost *host;
	const BatchRunOptions *options;
	uint32_t threads;
	uint64_t secondMs;
	wchar_t stopPath[HARNESS_PATH_LENGTH];
	wchar_t stopNowPath[HARNESS_PATH_LENGTH];
	// control/stop-now as a stopped run left it for the job it may have left in the game, if
	// `hasStaleStopNow`: it asks that job alone to stop, not this run, until the barrier is over.
	ByteBuffer staleStopNow;
	int exitCode;
	char reason[160];
	bool notifyStop; // the stop is worth a notification
	bool hasStaleStopNow;
	// The current game session.
	bool inSession;
	BatchSession session;
	bool haveJobs;     // `lastJobs` is known
	uint64_t lastJobs; // the harness's job count at the session's last report
	uint64_t ownJobs;  // jobs the batch sent since then that the harness took
	bool needsCanary;
	uint64_t sessionCommits;
	uint64_t canaryCommits; // sessionCommits at the last good canary
	CommittedRef *sinceCanary;
	size_t sinceCanaryCount;
	const char *nextReason; // why the next session starts
	// Incidents.
	uint64_t crashes[MAX_CRASHES]; // when, on the monotonic clock
	size_t crashCount;
	bool busy;
	// The kinds of failure to trim FM24's logs that the run has warned of, and whether it has
	// trimmed them yet.
	bool gameLogsWarned[GAME_LOGS_PROBLEMS];
	bool gameLogsTrimmed;
	uint64_t busySince; // uptime
	unsigned launchFailures;
	uint64_t committed; // chunks this run has committed
	// A season's build: the session's home intermediates so far, with their team overrides, the
	// first fixture that may not be built yet, and the jobs and time the build took this run.
	size_t homesBuilt;
	size_t fixtureCursor;
	uint64_t buildJobs;
	uint64_t buildStartedMs;
	uint32_t homeTeams[BATCH_MAX_TEAMS][2];
	bool building;
	bool needsWarmup; // the session's warm-ups are to run again: a season's, once it is frozen
} Run;

// Watches a job while the client waits for it (the client's tick).
typedef struct Watchdog {
	Run *run;
	uint32_t pid;                          // the game's process, 0 if unknown
	wchar_t files[2][HARNESS_PATH_LENGTH]; // the results and failed seeds whose growth is progress
	size_t fileCount;
	bool running;        // the harness has taken the job
	uint64_t lastSize;   // of the files
	uint64_t lastGrowth; // uptime
	uint64_t queuedSince;
	uint64_t lastUptime;
	uint64_t lastMonotonic;
	uint64_t sleptMs;
	uint64_t statusUntil; // a status query's tick stops it at this uptime (0: not one)
	BatchAsked asked;     // and the stop asked when it began, so that a new one ends it at once
	bool nowSeen;         // a stop-now was seen while the job ran
	uint64_t nowSince;    // from then (uptime)
	BatchWatchStop stopped;
} Watchdog;

// How a job ended.
typedef struct JobEnd {
	ClientOutcome client;
	BatchWatchStop watch;
	ClientWatch timing;
	bool adopted;   // the batch stopped waiting for it after it hung, then saw it end
	bool killed;    // the game was quit after the job hung
	bool attempt;   // an attempt at a chunk, not a warm-up, canary or lookup
	bool isolating; // an attempt shaped to find a seed that crashes the game
	uint64_t wallMs;
	uint64_t sleptMs;
	char result[CLIENT_RESULT_LENGTH];
	BatchReply reply;
	bool hasReport;
	RunReport report;
	bool gameRunning;
	bool harnessListening;
} JobEnd;

// How waiting for a job that may still run ended.
typedef enum JobWait {
	JOB_ENDED,     // the harness answered a status query: it is free
	JOB_GAME_GONE, // the game, or its harness, went
	JOB_KILLED,    // it hung, and the game was quit (kill_game=1)
	JOB_GAVE_UP,   // queue_wait_minutes passed
	JOB_STOPPED,   // a stop was asked once it hung, or a stop-now it left unanswered
	JOB_SIGNAL,    // a third signal
} JobWait;

// What a report says of the session.
typedef enum ReportCheck {
	REPORT_FITS,
	REPORT_CHANGED_HARNESS, // another harness, build or slice than the pinned one, not allowed
	REPORT_OTHER_GAME,      // another game process than the session's
} ReportCheck;

// A replacement job of a season's build: `side` of `input` (a .bin with the overrides `teams`)
// replaced with `team`, into <stem>.tmp.json; `label`, `home` and `away` name it in reports (0: not
// known).
typedef struct Replacement {
	const char *label;
	uint32_t home;
	uint32_t away;
	unsigned side;
	uint32_t team;
	const uint32_t *teams;
	char input[UTF8_LENGTH];
	char stem[64];
} Replacement;

// The canary reference check of chunk 0 (BatchCommitFacts.checkCanary).
typedef struct ReferenceCheck {
	Run *run;
	size_t arm;
	int result;
} ReferenceCheck;

static int start(Run *run, const wchar_t *directory);
static Flow takeLock(Run *run, const wchar_t *directory, PlatformFileLock *lock);
static Flow openBatch(Run *run, const wchar_t *directory);
static Flow barrier(Run *run, BatchPartOutcome *unevaluated);
static Flow recover(Run *run, BatchPartOutcome unevaluated);
static Flow checkReferences(Run *run);
static void forgetStaleStopNow(Run *run);
static void loop(Run *run);
static Flow playChunk(Run *run);
static BatchAsked askedStop(const Run *run);
static bool stopNowAsked(const Run *run);
static Flow stopAtBoundary(Run *run, bool underWay);
static Flow commitChunk(Run *run, size_t arm, uint64_t chunk);
static void trimGameLogs(Run *run);
static void trimGameLogsFirst(Run *run);
static Flow noteReferenceMismatch(Run *run, size_t arm, const BatchCommitted *record);
static Flow checkHost(Run *run);
static Flow checkPower(Run *run);
static Flow ensureSession(Run *run);
static bool askedWhileChecking(void *context);
static Flow checkSlice(Run *run, uint32_t pid);
static Flow startSession(Run *run, uint32_t pid, bool launched, const char *date);
static Flow warmUpArms(Run *run);
static Flow warmUp(Run *run, const BatchArm *fixture, const BatchSeeds *seeds);
static Flow buildFixtures(Run *run);
static Flow buildHome(Run *run, size_t team);
static Flow buildFixture(Run *run, size_t fixture);
static Flow replaceTeam(Run *run, const Replacement *job, uint32_t built[2], ByteBuffer *bin);
static Flow buildFailed(Run *run, const Replacement *job, const char *why);
static Flow freezeFixtures(Run *run);
static bool playedEdited(const Run *run, const JobEnd *end);
static Flow stopEdited(Run *run, size_t arm, const JobEnd *end);
static Flow lookUp(Run *run, const char *command, const char *name);
static Flow runCanaries(Run *run);
static Flow runCanary(Run *run, size_t arm, int *result);
static Flow playAttempt(Run *run, size_t arm, uint64_t chunk);
static BatchPartOutcome partOutcome(const JobEnd *end);
static Flow checkSeedErrors(Run *run, size_t arm);
static Flow
runJob(Run *run, const char *job, const char *output, bool attempt, bool reportAsked, JobEnd *end);
static Flow actOnStall(Run *run, Watchdog *dog, uint32_t pid, const char *output, JobEnd *end);
static Flow
leaveJob(Run *run, uint32_t pid, const char *output, bool attempt, const BatchDecision *decision);
static Flow giveUpOnJob(Run *run, uint32_t pid, const char *output, bool attempt);
static void sayJobLeft(const Run *run, uint32_t pid, const char *output, bool attempt);
static Flow judgeJob(Run *run, JobEnd *end, const BatchPart *part, const BatchPart *previous);
static bool watchTick(void *context, ClientPhase phase);
static void noteClocks(Watchdog *dog, uint64_t uptime, uint64_t monotonic);
static bool progressed(Watchdog *dog, uint64_t uptime);
static bool leftUnanswered(Watchdog *dog, uint64_t uptime);
static JobWait waitForJob(Run *run, Watchdog *dog, bool hangNoted);
static bool stopEndsWait(Run *run, Watchdog *dog, bool hangNoted, BatchAsked asked);
static bool outlivesHarness(Run *run, uint32_t pid);
static bool statusAnswers(Run *run, GameStatus *status, BatchAsked asked);
static ReportCheck noteReport(Run *run, const JobEnd *end);
static bool sameHarness(const RunReport *a, const RunReport *b);
static Flow decide(Run *run, const BatchEvent *event, uint32_t pid);
static Flow apply(Run *run, const BatchDecision *decision, uint32_t pid);
static Flow pause(Run *run, unsigned seconds);
static Flow stopWith(Run *run, int exitCode, const char *reason, bool notify);
static void finish(Run *run);
static BatchPolicyState policyState(const Run *run, size_t arm, const BatchPart *current);
static unsigned recentCrashes(const Run *run);
static void notify(Run *run, const char *text);
static void logEvent(Run *run, const char *format, ...) PLATFORM_PRINTF(2, 3);
static bool jobPath(const Run *run, char *out, const char *format, ...) PLATFORM_PRINTF(3, 4);
static bool siblingOf(const char *output, size_t role, char *out);
static bool fileExists(const char *path);
static bool fileThere(const wchar_t *path);
static uint64_t fileSize(const wchar_t *path);
static bool formatRunJob(
	const Run *run,
	const BatchArm *fixture,
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t count,
	uint32_t threads,
	bool warmup,
	const char *output,
	char *job
);
static bool copyCanary(const char *from, const char *to, int *result);
static int compareChunkWithReference(void *context, const wchar_t *folder);
static bool
canonicalRows(const ByteBuffer *text, size_t role, bool indexed, uint64_t limit, ByteBuffer *out);
static bool canonicalRow(
	const char *row,
	size_t length,
	size_t role,
	bool indexed,
	uint64_t limit,
	ByteBuffer *out
);
static const char *powerName(BatchPower power);

int batchRun_run(const wchar_t *directory, const BatchRunOptions *options, const BatchHost *host) {
	Run *run = calloc(1, sizeof *run);
	if (!run) {
		fputs("out of memory\n", stderr);
		return BATCH_EXIT_FAILED;
	}
	run->host = host;
	run->options = options;
	run->secondMs = options->secondMs ? options->secondMs : 1000;
	run->nextReason = "batch run started";
	int code = start(run, directory);
	buffer_free(&run->staleStopNow);
	free(run->sinceCanary);
	free(run);
	return code;
}

time_t batchRun_nextLocalTime(time_t now, int hour, int minute) {
	struct tm local = {0};
#ifdef _WIN32
	bool ok = localtime_s(&local, &now) == 0;
#else
	bool ok = localtime_r(&now, &local) != NULL;
#endif
	if (!ok) {
		return now;
	}
	for (int day = 0; day < 3; ++day) {
		struct tm candidate = local;
		candidate.tm_mday += day;
		candidate.tm_hour = hour;
		candidate.tm_min = minute;
		candidate.tm_sec = 0;
		candidate.tm_isdst = -1;
		time_t at = mktime(&candidate);
		if (at != (time_t)-1 && at >= now) {
			return at;
		}
	}
	return now;
}

// Locks and opens the batch, waits for a job an earlier run left, tidies, then runs it.
static int start(Run *run, const wchar_t *directory) {
	PlatformFileLock lock;
	Flow flow = takeLock(run, directory, &lock);
	if (flow != FLOW_ON) {
		return run->exitCode;
	}
	Batch *batch = malloc(sizeof *batch);
	run->batch = batch;
	flow = batch ? openBatch(run, directory) : stopWith(run, 1, "out of memory", false);
	if (flow != FLOW_ON) {
		free(batch);
		platform_unlockFile(lock);
		return run->exitCode;
	}
	const BatchHost *host = run->host;
	// The stop an earlier run was asked for is done with. Its stop-now stays for the job it may
	// have left in the game, which stops at it; only another one asks this run to stop.
	(void)platform_removeFile(run->stopPath);
	run->hasStaleStopNow = files_read(run->stopNowPath, &run->staleStopNow);
	// On battery power the batch pauses, and lets the computer sleep, unless pause_on_battery=0.
	if (!host->keepAwake(host->context, !batch->plan.pauseOnBattery)) {
		fputs("warning: cannot keep the computer awake (caffeinate)\n", stderr);
	}
	(void)host->watchSignals(host->context, run->stopPath, run->stopNowPath);
	logEvent(
		run, "run_started threads=%u pid=%lu", run->threads, (unsigned long)platform_processId()
	);
	BatchPartOutcome unevaluated = BATCH_PART_CRASHED;
	flow = barrier(run, &unevaluated);
	if (flow == FLOW_ON) {
		forgetStaleStopNow(run);
	}
	flow = flow == FLOW_ON ? recover(run, unevaluated) : flow;
	flow = flow == FLOW_ON ? checkReferences(run) : flow;
	if (flow == FLOW_ON) {
		BatchOutcome outcome = {"running", NULL, -1, host->utcNow(host->context)};
		BatchStatus status = batch_writeManifest(batch, &outcome);
		flow = status == BATCH_STATUS_KILLED ? FLOW_KILLED : FLOW_ON;
	}
	if (flow == FLOW_ON) {
		loop(run);
	} else if (flow == FLOW_EXIT) {
		finish(run);
	} else {
		run->exitCode = BATCH_RUN_KILLED;
	}
	batch_close(batch);
	free(batch);
	platform_unlockFile(lock);
	return run->exitCode;
}

// batch.lock, held while this process runs; the holder writes its process ID into it.
static Flow takeLock(Run *run, const wchar_t *directory, PlatformFileLock *lock) {
	wchar_t path[HARNESS_PATH_LENGTH];
	ByteBuffer plan = {0};
	bool frozen = paths_join(path, HARNESS_PATH_LENGTH, directory, L"plan.txt") &&
								files_read(path, &plan) && batchPlan_isFrozen(buffer_string(&plan), plan.length);
	buffer_free(&plan);
	if (!frozen) {
		fprintf(stderr, "%ls is not a batch: it has no plan.txt that batch create froze\n", directory);
		return stopWith(run, BATCH_EXIT_FAILED, "not a batch", false);
	}
	if (!paths_join(path, HARNESS_PATH_LENGTH, directory, L"batch.lock")) {
		return stopWith(run, BATCH_EXIT_FAILED, "the batch folder's path is too long", false);
	}
	switch (platform_lockFile(path, lock)) {
		case PLATFORM_LOCK_TAKEN: break;
		case PLATFORM_LOCK_HELD: {
			ByteBuffer holder = {0};
			unsigned long pid = files_read(path, &holder) ? strtoul(buffer_string(&holder), NULL, 10) : 0;
			buffer_free(&holder);
			fprintf(stderr, "another batch run (pid %lu) is running this batch\n", pid);
			BatchEvent event = {.kind = BATCH_EVENT_LOCKED};
			BatchPolicyState state = {0};
			BatchDecision decision = batchPolicy_decide(&state, &event);
			return stopWith(run, decision.exitCode, decision.reason, false);
		}
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot lock the batch", false);
	}
	FILE *file = platform_openFile(path, "wb");
	if (file) {
		fprintf(file, "%lu\n", platform_processId());
		(void)fclose(file);
	}
	return FLOW_ON;
}

static Flow openBatch(Run *run, const wchar_t *directory) {
	Batch *batch = run->batch;
	switch (batch_open(batch, directory, run->host)) {
		case BATCH_OPENED: break;
		case BATCH_OPEN_CHANGED: {
			BatchEvent event = {.kind = BATCH_EVENT_CHANGED};
			BatchPolicyState state = {0};
			BatchDecision decision = batchPolicy_decide(&state, &event);
			return stopWith(run, decision.exitCode, decision.reason, false);
		}
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot open the batch", false);
	}
	run->threads = run->options->threads ? run->options->threads : batch->plan.threads;
	char path[UTF8_LENGTH];
	wchar_t control[HARNESS_PATH_LENGTH];
	bool ok = jobPath(run, path, "control/stop") &&
						platform_fromUtf8(path, run->stopPath, HARNESS_PATH_LENGTH) &&
						jobPath(run, path, "control/stop-now") &&
						platform_fromUtf8(path, run->stopNowPath, HARNESS_PATH_LENGTH) &&
						jobPath(run, path, "control") && platform_fromUtf8(path, control, HARNESS_PATH_LENGTH);
	if (!ok || !platform_makeDirectory(control)) {
		batch_close(batch);
		return stopWith(run, BATCH_EXIT_FAILED, "cannot use the batch's control folder", false);
	}
	return FLOW_ON;
}

// Only a batch run holding the lock submits the batch's jobs, so a job still running in the game
// is one an earlier run left. Its attempt is evaluated once it has ended: adopted, or crashed if
// the game went.
static Flow barrier(Run *run, BatchPartOutcome *unevaluated) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	const BatchChunk *newest = NULL;
	const BatchPart *part = NULL;
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		const BatchChunk *chunk = &batch->pending[i];
		for (size_t p = 0; p < chunk->partCount; ++p) {
			const BatchPart *candidate = &chunk->parts[p];
			if (!candidate->evaluated && (!part || candidate->startedAt >= part->startedAt)) {
				newest = chunk;
				part = candidate;
			}
		}
	}
	uint32_t pid = part ? host->findGame(host->context) : 0;
	if (!pid || !host->harnessListening(host->context)) {
		return FLOW_ON;
	}
	BatchEvent event = {.kind = BATCH_EVENT_RESUMED};
	(void)decide(run, &event, pid);
	Watchdog dog = {.run = run, .pid = pid};
	BatchPartPaths paths = {0};
	char sibling[UTF8_LENGTH];
	if (batchPlan_partPaths(
				batch->directoryUtf8, batch->plan.arms[newest->arm].id, newest->first, part->attempt, &paths
			) &&
			platform_fromUtf8(paths.output, dog.files[0], HARNESS_PATH_LENGTH) &&
			siblingOf(paths.output, BATCH_FAILED, sibling) &&
			platform_fromUtf8(sibling, dog.files[1], HARNESS_PATH_LENGTH)) {
		dog.fileCount = 2;
	}
	printf("waiting for the job a stopped batch run left in the game to end...\n");
	switch (waitForJob(run, &dog, false)) {
		case JOB_ENDED: *unevaluated = BATCH_PART_ADOPTED; return FLOW_ON;
		case JOB_GAME_GONE:
		case JOB_KILLED: return FLOW_ON;
		case JOB_SIGNAL: return stopWith(run, BATCH_EXIT_STOPPED, "stopped by a signal", false);
		case JOB_STOPPED: {
			BatchEvent stopped = {
				.kind = BATCH_EVENT_JOB,
				.job = {.client = CLIENT_ABANDONED,
					.watch = BATCH_WATCH_STOP,
					.accepted = true,
					.gameRunning = true,
					.harnessListening = true},
			};
			BatchPolicyState state = policyState(run, 0, NULL);
			BatchDecision decision = batchPolicy_decide(&state, &stopped);
			if (decision.action != BATCH_KILL_AND_RELAUNCH) {
				return leaveJob(run, pid, paths.output, true, &decision);
			}
			// Its attempt is evaluated as crashed, and the loop then stops as asked.
			bool quit = host->stopGame(host->context, pid, BATCH_STOP_GRACE_S);
			logEvent(run, "game_killed pid=%u ok=%d", pid, quit);
			return FLOW_ON;
		}
		default: return giveUpOnJob(run, pid, paths.output, true);
	}
}

// Once no job an earlier run left is running, the stop-now it was asked for is done with; one asked
// for since stays, and stops this run.
static void forgetStaleStopNow(Run *run) {
	if (run->hasStaleStopNow && !stopNowAsked(run)) {
		(void)platform_removeFile(run->stopNowPath);
	}
	run->hasStaleStopNow = false;
	buffer_free(&run->staleStopNow);
}

// Tidies what a stopped run left and evaluates its attempts (batch_recover).
static Flow recover(Run *run, BatchPartOutcome unevaluated) {
	BatchRecovery recovery;
	switch (batch_recover(run->batch, unevaluated, &recovery)) {
		case BATCH_STATUS_OK: break;
		case BATCH_STATUS_KILLED: return FLOW_KILLED;
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot tidy the batch's folder", true);
	}
	if (recovery.removed || recovery.evaluated || recovery.stale || recovery.committed) {
		printf(
			"resumed: %zu leftover folder%s removed, %zu attempt%s evaluated, %zu chunk%s committed\n",
			recovery.removed,
			recovery.removed == 1 ? "" : "s",
			recovery.evaluated,
			recovery.evaluated == 1 ? "" : "s",
			recovery.committed,
			recovery.committed == 1 ? "" : "s"
		);
	}
	if (recovery.shortened) {
		printf(
			"%zu part%s held fewer rows than %s part.json (a power cut?); their indices play again\n",
			recovery.shortened,
			recovery.shortened == 1 ? "" : "s",
			recovery.shortened == 1 ? "its" : "their"
		);
	}
	return FLOW_ON;
}

// An arm's chunk 0 that differs from the canary reference but that suspect.csv does not list: the
// run that committed it stopped before it could act on that.
static Flow checkReferences(Run *run) {
	Batch *batch = run->batch;
	for (size_t arm = 0; arm < batch->plan.armCount; ++arm) {
		const BatchCommitted *record = batch_committed(batch, arm, 0);
		if (!record || !record->readable || record->canaryMatch != 0 ||
				batch_isSuspect(batch, arm, record->first, "reference_mismatch")) {
			continue;
		}
		logEvent(run, "canary arm=%s chunk=0 reference=mismatch recovered=1", batch->plan.arms[arm].id);
		Flow flow = noteReferenceMismatch(run, arm, record);
		if (flow != FLOW_ON) {
			return flow;
		}
	}
	return FLOW_ON;
}

// Plays the chunks in the plan's order until each is committed or the run stops.
// Builds a season's fixtures, then plays the chunks in the plan's order until each is committed or
// the run stops.
static void loop(Run *run) {
	const Batch *batch = run->batch;
	for (;;) {
		bool building = batch->plan.mode == BATCH_MODE_SEASON && !batch->fixturesFrozen;
		Flow flow = building ? buildFixtures(run) : playChunk(run);
		if (flow == FLOW_EXIT) {
			finish(run);
			return;
		}
		if (flow == FLOW_KILLED) {
			run->exitCode = BATCH_RUN_KILLED;
			return;
		}
	}
}

// The next chunk of the plan's order: committed if its parts cover it, else an attempt at it.
// FLOW_EXIT with exit code 0 once every chunk is committed.
static Flow playChunk(Run *run) {
	Batch *batch = run->batch;
	size_t arm = 0;
	uint64_t chunk = 0;
	if (!batch_nextChunk(batch, &arm, &chunk)) {
		run->exitCode = EXIT_COMPLETE;
		return FLOW_EXIT;
	}
	const BatchChunk *pending = batch_pending(batch, arm, chunk);
	bool underWay = pending && pending->partCount;
	bool done = false;
	BatchChain chain;
	if (underWay && batchChain_compute(pending, &chain)) {
		done = chain.cursor > pending->last;
		batchChain_free(&chain);
	}
	Flow flow = done ? commitChunk(run, arm, chunk) : stopAtBoundary(run, underWay);
	if (flow == FLOW_ON && !done) {
		trimGameLogsFirst(run);
		flow = checkHost(run);
	}
	if (flow == FLOW_ON && !done) {
		flow = ensureSession(run);
	}
	// Again, for a stop asked while the game was checked or a session started.
	if (flow == FLOW_ON && !done) {
		flow = stopAtBoundary(run, underWay);
	}
	return flow == FLOW_ON && !done ? playAttempt(run, arm, chunk) : flow;
}

// The stop asked of this run: by signals, control/stop, or a control/stop-now other than the one
// a stopped run left for its job.
static BatchAsked askedStop(const Run *run) {
	const BatchHost *host = run->host;
	unsigned signals = host->signals(host->context);
	if (signals >= 2 || stopNowAsked(run)) {
		return BATCH_ASKED_STOP_NOW;
	}
	return signals >= 1 || fileThere(run->stopPath) ? BATCH_ASKED_STOP : BATCH_ASKED_NONE;
}

// control/stop-now is there, and, during the barrier, is not the one a stopped run left (batch
// stop writes each request's own line in it).
static bool stopNowAsked(const Run *run) {
	if (!run->hasStaleStopNow) {
		return fileThere(run->stopNowPath);
	}
	ByteBuffer text = {0};
	const ByteBuffer *stale = &run->staleStopNow;
	bool asked = files_read(run->stopNowPath, &text) &&
							 (text.length != stale->length ||
								 (text.length && memcmp(text.data, stale->data, text.length) != 0));
	buffer_free(&text);
	return asked;
}

// Stops at a chunk boundary as asked: control/stop-now at once, and control/stop, --until and
// --max-chunks unless a chunk is under way.
static Flow stopAtBoundary(Run *run, bool underWay) {
	const BatchHost *host = run->host;
	const BatchRunOptions *options = run->options;
	BatchAsked asked = askedStop(run);
	if (asked == BATCH_ASKED_STOP_NOW) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped as asked (stop-now)", false);
	}
	if (underWay) {
		return FLOW_ON;
	}
	if (asked == BATCH_ASKED_STOP) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped as asked", false);
	}
	if (options->hasUntil && host->utcNow(host->context) >= options->until) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped at the time --until gave", false);
	}
	if (options->maxChunks && run->committed >= options->maxChunks) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped after --max-chunks chunks", false);
	}
	return FLOW_ON;
}

static Flow commitChunk(Run *run, size_t arm, uint64_t chunk) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	const BatchPlan *plan = &batch->plan;
	uint32_t pid = run->inSession ? run->session.pid : 0;
	BatchCommitFacts facts = {
		.power = powerName(host->powerSource(host->context)),
		.gameRssBytes = pid ? host->residentBytes(host->context, pid) : 0,
		.canaryMatch = -1,
	};
	ReferenceCheck check = {.run = run, .arm = arm, .result = -1};
	char reference[UTF8_LENGTH];
	if (chunk == 0 && plan->canary && jobPath(run, reference, "canary/%s.csv", plan->arms[arm].id) &&
			fileExists(reference)) {
		facts.checkCanary = compareChunkWithReference;
		facts.canaryContext = &check;
	}
	switch (batch_commit(batch, arm, chunk, &facts)) {
		case BATCH_STATUS_OK: break;
		case BATCH_STATUS_KILLED: return FLOW_KILLED;
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot commit a chunk", true);
	}
	const BatchCommitted *record = batch_committed(batch, arm, chunk);
	++run->committed;
	++run->sessionCommits;
	CommittedRef *grown =
		realloc(run->sinceCanary, (run->sinceCanaryCount + 1) * sizeof *run->sinceCanary);
	if (grown) {
		run->sinceCanary = grown;
		grown[run->sinceCanaryCount++] = (CommittedRef){arm, record->first};
	}
	char line[512];
	batch_progressLine(batch, record, host->utcNow(host->context), line, sizeof line);
	printf("%s\n", line);
	if (plan->mode == BATCH_MODE_SEASON && batch_blockComplete(batch, chunk)) {
		uint64_t first = 0;
		uint64_t last = 0;
		batchPlan_chunkRange(plan, chunk, &first, &last);
		printf("seasons %llu-%llu complete\n", (unsigned long long)first, (unsigned long long)last);
		logEvent(
			run,
			"seasons_complete first=%llu last=%llu",
			(unsigned long long)first,
			(unsigned long long)last
		);
	}
	trimGameLogs(run);
	if (check.result >= 0) {
		logEvent(
			run,
			"canary arm=%s chunk=0 reference=%s",
			plan->arms[arm].id,
			check.result ? "match" : "mismatch"
		);
	}
	if (check.result == 0) {
		Flow flow = noteReferenceMismatch(run, arm, record);
		if (flow != FLOW_ON) {
			return flow;
		}
	}
	if (plan->canary && run->sessionCommits - run->canaryCommits >= plan->canaryEvery) {
		run->needsCanary = true;
	}
	return FLOW_ON;
}

// With trim_game_logs=1, empties FM24's newest file-list error log and deletes the older ones,
// before the run's first chunk and after each commit, when no job of the batch's runs. Nothing
// that goes wrong stops the batch: each kind of failure is logged and said once a run.
static void trimGameLogs(Run *run) {
	const BatchHost *host = run->host;
	run->gameLogsTrimmed = true;
	if (!run->batch->plan.trimGameLogs || !host->trimGameLogs) {
		return;
	}
	GameLogsTrim trim = {0};
	host->trimGameLogs(host->context, &trim);
	if (trim.emptied || trim.deleted) {
		logEvent(
			run,
			"game_logs_trimmed emptied=%u deleted=%u freed_bytes=%llu",
			trim.emptied,
			trim.deleted,
			(unsigned long long)trim.freedBytes
		);
	}
	static const char *const kinds[GAME_LOGS_PROBLEMS] = {"folder", "read", "empty", "delete"};
	static const char *const what[GAME_LOGS_PROBLEMS] = {
		"cannot find FM24's data folder for its file-list error logs",
		"cannot read FM24's logs folder",
		"cannot empty FM24's newest file-list error log in",
		"cannot delete an older FM24 file-list error log in",
	};
	for (size_t kind = 0; kind < GAME_LOGS_PROBLEMS; ++kind) {
		if (!trim.failed[kind][0] || run->gameLogsWarned[kind]) {
			continue;
		}
		run->gameLogsWarned[kind] = true;
		logEvent(run, "game_logs_failed kind=%s error=%s", kinds[kind], trim.failed[kind]);
		fprintf(
			stderr,
			"warning: %s%s%s (%s); the batch goes on, and says this once a run\n",
			what[kind],
			kind == GAME_LOGS_FOLDER ? "" : " ",
			kind == GAME_LOGS_FOLDER ? "" : trim.folder,
			trim.failed[kind]
		);
	}
}

// The run's first trim, before its first attempt: it waits for a chunk to play, so that a run that
// plays none, such as a complete batch's, leaves FM24's logs alone, and comes before checkHost, so
// that the free-space check sees the space it freed. A commit before it has trimmed already.
static void trimGameLogsFirst(Run *run) {
	if (!run->gameLogsTrimmed) {
		trimGameLogs(run);
	}
}

// Chunk 0 differs from the canary reference: it goes to suspect.csv, and on_canary_mismatch says
// whether the batch goes on.
static Flow noteReferenceMismatch(Run *run, size_t arm, const BatchCommitted *record) {
	Batch *batch = run->batch;
	fprintf(stderr, "arm %s: chunk 0 differs from the canary reference\n", batch->plan.arms[arm].id);
	(void)batch_addSuspect(
		batch,
		arm,
		record->first,
		run->inSession ? run->session.id : record->sessions,
		"reference_mismatch"
	);
	BatchEvent event = {.kind = BATCH_EVENT_CANARY};
	Flow flow = decide(run, &event, run->inSession ? run->session.pid : 0);
	if (flow == FLOW_ON && run->inSession) {
		run->session.nonrepeatable = true;
	}
	return flow;
}

// Free space and power, at a chunk boundary or between attempts.
static Flow checkHost(Run *run) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	const BatchPlan *plan = &batch->plan;
	const BatchCommitted *latest = NULL;
	for (size_t i = 0; i < batch->committedCount; ++i) {
		const BatchCommitted *chunk = &batch->committed[i];
		latest = !latest || chunk->committedAt >= latest->committedAt ? chunk : latest;
	}
	uint64_t chunkBytes = latest && latest->rawBytes
													? latest->rawBytes
													: (uint64_t)plan->chunk * (plan->perMatch ? 6800 : 60);
	uint64_t needed = ((uint64_t)plan->minFreeGb << 30) + 3 * chunkBytes;
	uint64_t available = 0;
	if (host->freeBytes(host->context, batch->directory, &available) && available < needed) {
		fprintf(
			stderr,
			"%.1f GB free; the batch keeps %u GB free and needs about %.1f GB more for a chunk\n",
			(double)available / (double)(1ULL << 30),
			plan->minFreeGb,
			(double)(3 * chunkBytes) / (double)(1ULL << 30)
		);
		logEvent(
			run,
			"disk free_bytes=%llu needed=%llu",
			(unsigned long long)available,
			(unsigned long long)needed
		);
		BatchEvent event = {.kind = BATCH_EVENT_DISK};
		return decide(run, &event, 0);
	}
	return checkPower(run);
}

// On battery with pause_on_battery=1, waits until the computer is on AC power again.
static Flow checkPower(Run *run) {
	const BatchHost *host = run->host;
	bool paused = false;
	uint64_t since = host->uptimeMs(host->context);
	while (host->powerSource(host->context) == BATCH_POWER_BATTERY) {
		BatchEvent event = {.kind = BATCH_EVENT_BATTERY};
		BatchPolicyState state = policyState(run, 0, NULL);
		BatchDecision decision = batchPolicy_decide(&state, &event);
		if (decision.action != BATCH_WAIT) {
			break;
		}
		if (!paused) {
			printf("on battery power: paused until the computer is on AC power again\n");
			logEvent(run, "paused reason=battery");
			paused = true;
		}
		Flow flow = pause(run, decision.waitS);
		if (flow != FLOW_ON) {
			return flow;
		}
	}
	if (paused) {
		uint64_t pausedMs = host->uptimeMs(host->context) - since;
		printf("on AC power again: resuming\n");
		logEvent(run, "resumed paused_ms=%llu", (unsigned long long)pausedMs);
	}
	return FLOW_ON;
}

// The game with the save loaded and idle, in the plan's slice, at the fixtures' date, in a session
// that is warmed up and whose canary matches.
static Flow ensureSession(Run *run) {
	const BatchHost *host = run->host;
	const BatchPlan *plan = &run->batch->plan;
	uint32_t before = host->findGame(host->context);
	// A stop asked for never starts the game.
	if (!before && askedStop(run) != BATCH_ASKED_NONE) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped as asked; the game is not running", false);
	}
	Flow flow = before ? checkSlice(run, before) : FLOW_ON;
	if (flow != FLOW_ON) {
		return flow;
	}
	bool path = strchr(plan->save, '/') || strchr(plan->save, '\\');
	SessionStop stop = {
		.asked = askedWhileChecking,
		.context = run,
		.tickMs = (unsigned)(run->secondMs < 200 ? run->secondMs : 200),
	};
	SessionNeed need = {
		.batch = true,
		.save = plan->saveName,
		.launch = plan->launch != 0,
		.anySave = plan->anySave != 0,
		.timeoutMs = (unsigned)((uint64_t)plan->queueWaitMinutes * 60 * run->secondMs),
		.savePath = path ? plan->save : NULL,
		.rosetta = plan->slice == BATCH_SLICE_X86_64,
		.readyS = plan->busyWaitMinutes * 60,
		.stop = &stop,
	};
	char date[BATCH_TEXT_LENGTH] = "";
	SessionOutcome outcome = SESSION_WAITING;
	int code = host->ensureSave(host->context, &need, date, sizeof date, &outcome);
	uint32_t pid = host->findGame(host->context);
	bool launched = !before && pid;
	if (launched) {
		logEvent(run, "game_launched pid=%u", pid);
	}
	BatchEvent event = {
		.kind = BATCH_EVENT_SAVE,
		.save = {.code = code, .outcome = outcome, .gameRunning = pid != 0},
	};
	flow = decide(run, &event, pid);
	if (flow != FLOW_ON) {
		return flow;
	}
	run->launchFailures = 0;
	flow = pid ? checkSlice(run, pid) : FLOW_ON;
	if (flow != FLOW_ON) {
		return flow;
	}
	if (!plan->keepBuild && plan->gameDate[0] && date[0] && strcmp(date, plan->gameDate) != 0) {
		fprintf(
			stderr,
			"the save is at %s, but the fixtures were exported at %s; was Continue pressed?\n",
			date,
			plan->gameDate
		);
		BatchEvent moved = {.kind = BATCH_EVENT_DATE};
		return decide(run, &moved, pid);
	}
	if (!run->inSession || pid != run->session.pid) {
		if (run->inSession && !launched) {
			run->nextReason = "the game was restarted outside the batch";
		}
		return startSession(run, pid, launched, date);
	}
	if (run->needsWarmup) {
		run->needsWarmup = false;
		flow = warmUpArms(run);
		if (flow != FLOW_ON) {
			return flow;
		}
	}
	return run->needsCanary ? runCanaries(run) : FLOW_ON;
}

// SessionStop.asked: the check has started no job, so any stop ends its wait.
static bool askedWhileChecking(void *context) {
	return askedStop(context) != BATCH_ASKED_NONE;
}

static Flow checkSlice(Run *run, uint32_t pid) {
	const BatchHost *host = run->host;
	const BatchPlan *plan = &run->batch->plan;
	if (host->gameSlice(host->context, pid) == plan->slice) {
		return FLOW_ON;
	}
	fprintf(
		stderr,
		"the game (pid %u) runs its %s slice; the plan asks for %s\n",
		pid,
		plan->slice == BATCH_SLICE_X86_64 ? "arm64" : "x86_64",
		plan->slice == BATCH_SLICE_X86_64 ? "x86_64" : "arm64"
	);
	BatchEvent event = {.kind = BATCH_EVENT_SLICE};
	return decide(run, &event, pid);
}

// A new session of game process `pid`: its warm-up, the lookups, its canary and session.json.
static Flow startSession(Run *run, uint32_t pid, bool launched, const char *date) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	const BatchPlan *plan = &batch->plan;
	BatchSession *session = &run->session;
	*session = (BatchSession){.pid = pid, .launchedByBatch = launched};
	if (!batch_startSession(batch, session->id)) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot start a session", true);
	}
	session->startedAt = host->utcNow(host->context);
	(void)snprintf(session->reason, sizeof session->reason, "%s", run->nextReason);
	(void)snprintf(session->save, sizeof session->save, "%s", plan->saveName);
	(void)snprintf(session->gameDate, sizeof session->gameDate, "%s", date);
	GameStatus status;
	if (pid && statusAnswers(run, &status, askedStop(run)) && status.save[0]) {
		(void)snprintf(session->save, sizeof session->save, "%s", status.save);
		(void)snprintf(session->gameDate, sizeof session->gameDate, "%s", status.date);
	}
	run->inSession = true;
	run->haveJobs = false;
	run->ownJobs = 0;
	run->needsCanary = false;
	run->sessionCommits = 0;
	run->canaryCommits = 0;
	run->sinceCanaryCount = 0;
	run->needsWarmup = false;
	run->homesBuilt = 0;
	run->nextReason = "the game was started again";
	printf("session %s: game pid %u (%s)\n", session->id, pid, session->reason);
	logEvent(run, "session_started id=%s pid=%u reason=%s", session->id, pid, session->reason);
	Flow flow = plan->warmup ? warmUpArms(run) : FLOW_ON;
	session->warmedUp = flow == FLOW_ON && plan->warmup;
	if (flow == FLOW_ON) {
		flow = lookUp(run, "list_teams", "teams");
	}
	if (flow == FLOW_ON) {
		flow = lookUp(run, "list_players", "players");
	}
	// A season's canaries wait for its fixtures (freezeFixtures).
	bool building = plan->mode == BATCH_MODE_SEASON && !batch->fixturesFrozen;
	if (flow == FLOW_ON && plan->canary && !building) {
		flow = runCanaries(run);
	} else if (flow == FLOW_ON && !building) {
		(void)snprintf(session->canary, sizeof session->canary, "off");
	}
	if (!batch_saveSession(batch, session) && flow == FLOW_ON) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot write session.json", true);
	}
	if (flow != FLOW_ON) {
		// The session never got going: the next attempt starts another.
		run->inSession = run->inSession && flow == FLOW_EXIT;
	}
	return flow;
}

// The session's warm-ups: each arm's, or a season's check fixtures', or while its fixtures are
// being built, its base's, so that no replacement is the session's first build.
static Flow warmUpArms(Run *run) {
	const Batch *batch = run->batch;
	const BatchPlan *plan = &batch->plan;
	if (plan->mode == BATCH_MODE_SEASON && !batch->fixturesFrozen) {
		BatchSeeds seeds = batchPlan_armSeeds(plan, 0);
		return warmUp(run, &plan->base, &seeds);
	}
	Flow flow = FLOW_ON;
	for (size_t arm = 0; flow == FLOW_ON && arm < plan->armCount; ++arm) {
		BatchSeeds seeds = batchPlan_armSeeds(plan, arm);
		flow = batchPlan_isCheckArm(plan, arm) ? warmUp(run, &plan->arms[arm], &seeds) : FLOW_ON;
	}
	return flow;
}

// The first `warmup` seeds of a fixture (inputs/<id>.bin) with one worker, so that every recorded
// job's edit detection compares against a later build than the session's first (see
// docs/TECHNICAL.md).
static Flow warmUp(Run *run, const BatchArm *fixture, const BatchSeeds *seeds) {
	const BatchPlan *plan = &run->batch->plan;
	char output[UTF8_LENGTH];
	char job[HARNESS_JOB_LENGTH];
	if (!jobPath(run, output, "sessions/%s/warmup-%s.csv", run->session.id, fixture->id) ||
			!formatRunJob(run, fixture, seeds, seeds->first, plan->warmup, 1, true, output, job)) {
		return stopWith(run, BATCH_EXIT_FAILED, "the warm-up's job is too long", true);
	}
	JobEnd end;
	Flow flow = runJob(run, job, output, false, true, &end);
	flow = flow == FLOW_ON ? judgeJob(run, &end, NULL, NULL) : flow;
	if (flow == FLOW_ON) {
		logEvent(run, "warmup session=%s arm=%s seeds=%u", run->session.id, fixture->id, plan->warmup);
	}
	return flow;
}

// One step of a season's build: a job, or once every fixture is built, their freeze. Each session
// builds every home intermediate before any fixture; stops, --until and the checks of the game
// and the computer come between jobs, as between chunks.
static Flow buildFixtures(Run *run) {
	Batch *batch = run->batch;
	const BatchPlan *plan = &batch->plan;
	Flow flow = stopAtBoundary(run, false);
	flow = flow == FLOW_ON ? checkHost(run) : flow;
	flow = flow == FLOW_ON ? ensureSession(run) : flow;
	flow = flow == FLOW_ON ? stopAtBoundary(run, false) : flow;
	if (flow != FLOW_ON) {
		return flow;
	}
	if (!run->building) {
		run->building = true;
		run->buildStartedMs = run->host->monotonicMs(run->host->context);
	}
	if (run->homesBuilt < plan->teamCount) {
		return buildHome(run, run->homesBuilt);
	}
	while (run->fixtureCursor < plan->armCount && batch_fixtureBuilt(batch, run->fixtureCursor)) {
		++run->fixtureCursor;
	}
	return run->fixtureCursor < plan->armCount ? buildFixture(run, run->fixtureCursor)
																						 : freezeFixtures(run);
}

// A team's home intermediate, inputs/build/home-<uid>: the base with the team at home. The base's
// away team against itself is built by copying its block.
static Flow buildHome(Run *run, size_t team) {
	Batch *batch = run->batch;
	const BatchPlan *plan = &batch->plan;
	uint32_t uid = plan->teams[team];
	char label[32];
	(void)snprintf(label, sizeof label, "home-%u", uid);
	Replacement job = {.label = label, .home = uid, .team = uid, .teams = plan->base.teams};
	(void)snprintf(job.stem, sizeof job.stem, "inputs/build/home-%u", uid);
	char folder[UTF8_LENGTH];
	wchar_t wide[HARNESS_PATH_LENGTH];
	if (!jobPath(run, job.input, "inputs/base.bin") || !jobPath(run, folder, "inputs/build") ||
			!platform_fromUtf8(folder, wide, HARNESS_PATH_LENGTH) || !platform_makeDirectory(wide)) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot make inputs/build", true);
	}
	ByteBuffer bin = {0};
	Flow flow = replaceTeam(run, &job, run->homeTeams[team], &bin);
	BatchStatus status =
		flow == FLOW_ON ? batch_keepBuilt(batch, job.stem, false, &bin) : BATCH_STATUS_OK;
	buffer_free(&bin);
	if (status == BATCH_STATUS_KILLED) {
		return FLOW_KILLED;
	}
	if (status != BATCH_STATUS_OK) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot keep a home intermediate", true);
	}
	run->homesBuilt += flow == FLOW_ON;
	return flow;
}

// A fixture, inputs/<id>: its home team's intermediate with its away team replaced.
static Flow buildFixture(Run *run, size_t fixture) {
	Batch *batch = run->batch;
	const BatchPlan *plan = &batch->plan;
	const BatchArm *arm = &plan->arms[fixture];
	size_t home = 0;
	size_t away = 0;
	batchSeason_pair(plan->teamCount, fixture, &home, &away);
	Replacement job = {
		.label = arm->id,
		.home = arm->pair[0],
		.away = arm->pair[1],
		.side = 1,
		.team = arm->pair[1],
		.teams = run->homeTeams[home],
	};
	(void)snprintf(job.stem, sizeof job.stem, "inputs/%s", arm->id);
	if (!jobPath(run, job.input, "inputs/build/home-%u.bin", arm->pair[0])) {
		return stopWith(run, BATCH_EXIT_FAILED, "the batch folder's path is too long", true);
	}
	ByteBuffer bin = {0};
	uint32_t built[2] = {0};
	Flow flow = replaceTeam(run, &job, built, &bin);
	bool gzip = plan->compress == BATCH_COMPRESS_GZIP;
	BatchStatus status =
		flow == FLOW_ON ? batch_keepBuilt(batch, job.stem, gzip, &bin) : BATCH_STATUS_OK;
	buffer_free(&bin);
	if (status == BATCH_STATUS_KILLED) {
		return FLOW_KILLED;
	}
	if (status != BATCH_STATUS_OK) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot keep a fixture", true);
	}
	if (flow == FLOW_ON) {
		batch->fixturesBuilt += 1;
		++run->fixtureCursor;
	}
	return flow;
}

// A replacement and its checks: what it built, encoded, goes to `bin` and its overrides to
// `built`. A crash, a hang or a busy game is handled as a warm-up's.
static Flow replaceTeam(Run *run, const Replacement *job, uint32_t built[2], ByteBuffer *bin) {
	const BatchPlan *plan = &run->batch->plan;
	char output[UTF8_LENGTH];
	char text[HARNESS_JOB_LENGTH];
	wchar_t wide[HARNESS_PATH_LENGTH];
	if (!jobPath(run, output, "%s.tmp.json", job->stem) ||
			!platform_fromUtf8(output, wide, HARNESS_PATH_LENGTH) ||
			!batchSeason_formatReplace(
				job->side != 0, job->input, job->team, job->teams, output, text, sizeof text
			)) {
		return stopWith(run, BATCH_EXIT_FAILED, "the replacement's job is too long", true);
	}
	JobEnd end;
	Flow flow = runJob(run, text, output, false, false, &end);
	if (flow != FLOW_ON) {
		return flow;
	}
	if (end.client == CLIENT_REPLIED && end.reply == BATCH_REPLY_ERROR) {
		return buildFailed(run, job, end.result);
	}
	flow = judgeJob(run, &end, NULL, NULL);
	if (flow != FLOW_ON) {
		return flow;
	}
	++run->buildJobs;
	char why[320];
	ByteBuffer json = {0};
	bool ok = batchSeason_checkReply(end.result, job->team, why, sizeof why);
	if (ok && !files_read(wide, &json)) {
		(void)snprintf(why, sizeof why, "it wrote no %s.tmp.json", job->stem);
		ok = false;
	}
	ok = ok && batchSeason_checkBuilt(
							 plan, &json, job->side, job->team, job->teams, bin, built, why, sizeof why
						 );
	buffer_free(&json);
	return ok ? FLOW_ON : buildFailed(run, job, why);
}

// A replacement that failed, or built what the batch did not ask for, stops the batch.
static Flow buildFailed(Run *run, const Replacement *job, const char *why) {
	char teams[2][16] = {"-", "-"};
	if (job->home) {
		(void)snprintf(teams[0], sizeof teams[0], "%u", job->home);
	}
	if (job->away) {
		(void)snprintf(teams[1], sizeof teams[1], "%u", job->away);
	}
	logEvent(
		run, "build_failed fixture=%s home=%s away=%s reply=%s", job->label, teams[0], teams[1], why
	);
	fprintf(stderr, "%s (%s v %s) failed to build: %s\n", job->label, teams[0], teams[1], why);
	char reason[sizeof run->reason];
	(void)snprintf(
		reason,
		sizeof reason,
		"%s (%s v %s) failed to build; see events.log",
		job->label,
		teams[0],
		teams[1]
	);
	return stopWith(run, BATCH_EXIT_FAILED, reason, true);
}

// Every fixture is built: inputs/fixtures.txt freezes them, and the session warms the check
// fixtures up and runs their canaries before the first chunk.
static Flow freezeFixtures(Run *run) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	switch (batch_freezeFixtures(batch)) {
		case BATCH_STATUS_OK: break;
		case BATCH_STATUS_KILLED: return FLOW_KILLED;
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot freeze the season's fixtures", true);
	}
	uint64_t wallMs = host->monotonicMs(host->context) - run->buildStartedMs;
	logEvent(
		run,
		"fixtures_frozen fixtures=%zu jobs=%llu wall_ms=%llu",
		batch->plan.armCount,
		(unsigned long long)run->buildJobs,
		(unsigned long long)wallMs
	);
	printf("fixtures: %zu built and frozen in inputs/fixtures.txt\n", batch->plan.armCount);
	run->needsWarmup = batch->plan.warmup != 0;
	run->needsCanary = batch->plan.canary != 0;
	BatchOutcome outcome = {"running", NULL, -1, host->utcNow(host->context)};
	return batch_writeManifest(batch, &outcome) == BATCH_STATUS_KILLED ? FLOW_KILLED : FLOW_ON;
}

// A season's fixture played with edits: as frozen, it does not rebuild to itself, so what played
// is not what was built.
static bool playedEdited(const Run *run, const JobEnd *end) {
	return run->batch->plan.mode == BATCH_MODE_SEASON && end->hasReport && end->report.hasEdits &&
				 end->report.edits;
}

static Flow stopEdited(Run *run, size_t arm, const JobEnd *end) {
	const char *id = run->batch->plan.arms[arm].id;
	logEvent(run, "edits arm=%s edits=%llu", id, (unsigned long long)end->report.edits);
	fprintf(
		stderr,
		"fixture %s does not rebuild to itself (%llu edits)\n",
		id,
		(unsigned long long)end->report.edits
	);
	BatchEvent event = {.kind = BATCH_EVENT_EDITS};
	Flow flow = decide(run, &event, run->inSession ? run->session.pid : 0);
	// The policy stops; nothing that played can be kept.
	return flow == FLOW_ON ? FLOW_AGAIN : flow;
}

// Writes lookups/<name>.txt with `command` once for the batch: the names of the IDs in its rows.
static Flow lookUp(Run *run, const char *command, const char *name) {
	char path[UTF8_LENGTH];
	char temporary[UTF8_LENGTH];
	char job[HARNESS_JOB_LENGTH];
	wchar_t from[HARNESS_PATH_LENGTH];
	wchar_t to[HARNESS_PATH_LENGTH];
	if (!jobPath(run, path, "lookups/%s.txt", name) || fileExists(path)) {
		return FLOW_ON;
	}
	if (!jobPath(run, temporary, "lookups/%s.txt.tmp", name) ||
			snprintf(job, sizeof job, "command=%s\noutput=%s\n", command, temporary) >= (int)sizeof job) {
		return stopWith(run, BATCH_EXIT_FAILED, "the lookup's path is too long", true);
	}
	char folder[UTF8_LENGTH];
	wchar_t wide[HARNESS_PATH_LENGTH];
	if (!jobPath(run, folder, "lookups") || !platform_fromUtf8(folder, wide, HARNESS_PATH_LENGTH) ||
			!platform_makeDirectory(wide)) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot make the lookups folder", true);
	}
	JobEnd end;
	Flow flow = runJob(run, job, temporary, false, false, &end);
	flow = flow == FLOW_ON ? judgeJob(run, &end, NULL, NULL) : flow;
	if (flow != FLOW_ON) {
		return flow;
	}
	if (!platform_fromUtf8(temporary, from, HARNESS_PATH_LENGTH) ||
			!platform_fromUtf8(path, to, HARNESS_PATH_LENGTH) || !platform_replaceFile(from, to)) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot keep a lookup", true);
	}
	logEvent(run, "lookup file=lookups/%s.txt", name);
	return FLOW_ON;
}

// Each arm's canary; the first session's becomes the reference the others must match.
static Flow runCanaries(Run *run) {
	Batch *batch = run->batch;
	const BatchPlan *plan = &batch->plan;
	BatchSession *session = &run->session;
	bool mismatch = false;
	bool reference = true;
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		int result = -1;
		if (!batchPlan_isCheckArm(plan, arm)) {
			continue;
		}
		Flow flow = runCanary(run, arm, &result);
		if (flow != FLOW_ON) {
			return flow;
		}
		static const char *const results[] = {"mismatch", "match", "reference"};
		logEvent(
			run, "canary session=%s arm=%s result=%s", session->id, plan->arms[arm].id, results[result]
		);
		mismatch = mismatch || result == 0;
		reference = reference && result == 2;
	}
	run->needsCanary = false;
	(void)snprintf(
		session->canary,
		sizeof session->canary,
		"%s",
		mismatch    ? "mismatch"
		: reference ? "reference"
								: "match"
	);
	if (!mismatch) {
		run->canaryCommits = run->sessionCommits;
		run->sinceCanaryCount = 0;
		return FLOW_ON;
	}
	fprintf(stderr, "session %s: the canary differs from the reference\n", session->id);
	for (size_t i = 0; i < run->sinceCanaryCount; ++i) {
		(void)batch_addSuspect(
			batch, run->sinceCanary[i].arm, run->sinceCanary[i].first, session->id, "canary_mismatch"
		);
	}
	run->sinceCanaryCount = 0;
	BatchEvent event = {.kind = BATCH_EVENT_CANARY};
	Flow flow = decide(run, &event, session->pid);
	if (flow == FLOW_ON) {
		session->nonrepeatable = true;
		run->canaryCommits = run->sessionCommits;
	}
	return flow;
}

// The arm's first `canary` seeds with one worker. `result`: 2 if they became the reference, else 1
// if they match it, 0 if not.
static Flow runCanary(Run *run, size_t arm, int *result) {
	const BatchPlan *plan = &run->batch->plan;
	const char *id = plan->arms[arm].id;
	char output[UTF8_LENGTH];
	char reference[UTF8_LENGTH];
	char job[HARNESS_JOB_LENGTH];
	wchar_t folder[HARNESS_PATH_LENGTH];
	BatchSeeds seeds = batchPlan_armSeeds(plan, arm);
	if (!jobPath(run, output, "sessions/%s/canary-%s.csv", run->session.id, id) ||
			!jobPath(run, reference, "canary") ||
			!platform_fromUtf8(reference, folder, HARNESS_PATH_LENGTH) ||
			!platform_makeDirectory(folder) || !jobPath(run, reference, "canary/%s.csv", id) ||
			!formatRunJob(
				run, &plan->arms[arm], &seeds, seeds.first, plan->canary, 1, false, output, job
			)) {
		return stopWith(run, BATCH_EXIT_FAILED, "the canary's job is too long", true);
	}
	JobEnd end;
	Flow flow = runJob(run, job, output, false, true, &end);
	flow = flow == FLOW_ON ? judgeJob(run, &end, NULL, NULL) : flow;
	if (flow != FLOW_ON) {
		return flow;
	}
	if (playedEdited(run, &end)) {
		return stopEdited(run, arm, &end);
	}
	if (!copyCanary(output, reference, result)) {
		return stopWith(run, BATCH_EXIT_FAILED, "cannot compare or keep a canary", true);
	}
	return FLOW_ON;
}

// One attempt at a chunk, from where its parts stop, then its part and what to do next.
static Flow playAttempt(Run *run, size_t arm, uint64_t chunk) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	const BatchChunk *pending = batch_pending(batch, arm, chunk);
	BatchAttemptShape shape;
	batchPolicy_shape(
		pending ? pending->parts : NULL, pending ? pending->partCount : 0, run->threads, &shape
	);
	BatchAttemptFacts facts = {
		.threads = shape.threads,
		.session = run->session.id,
		.startedAt = host->utcNow(host->context),
		.maxCount = shape.maxCount,
	};
	BatchAttempt *attempt = malloc(sizeof *attempt);
	if (!attempt) {
		return stopWith(run, BATCH_EXIT_FAILED, "out of memory", true);
	}
	Flow flow = FLOW_ON;
	switch (batch_beginAttempt(batch, arm, chunk, &facts, attempt)) {
		case BATCH_STATUS_OK: break;
		case BATCH_STATUS_KILLED: flow = FLOW_KILLED; break;
		default: flow = stopWith(run, BATCH_EXIT_FAILED, "cannot start an attempt", true); break;
	}
	JobEnd end;
	if (flow == FLOW_ON) {
		flow = runJob(run, attempt->job, attempt->paths.output, true, true, &end);
	}
	if (flow != FLOW_ON) {
		// A job still running in the game is evaluated by the next batch run.
		free(attempt);
		return flow;
	}
	end.isolating = shape.isolating;
	ReportCheck check = end.hasReport ? noteReport(run, &end) : REPORT_FITS;
	bool edited = check == REPORT_FITS && playedEdited(run, &end);
	BatchPartFacts part = {
		.outcome = partOutcome(&end),
		.clientOutcome = end.adopted ? "abandoned" : manifest_clientOutcomeName(end.client),
		.resultLine = end.result,
		.finishedAt = host->utcNow(host->context),
		.wallMs = end.wallMs,
		.sleptMs = end.sleptMs,
		.hasPoison = shape.hasCandidate,
		.poison = shape.candidate,
		.discard = check != REPORT_FITS || edited,
	};
	uint32_t number = attempt->attempt;
	free(attempt);
	switch (batch_finishAttempt(batch, arm, chunk, number, &part)) {
		case BATCH_STATUS_OK: break;
		case BATCH_STATUS_KILLED: return FLOW_KILLED;
		default: return stopWith(run, BATCH_EXIT_FAILED, "cannot evaluate an attempt", true);
	}
	if (check == REPORT_CHANGED_HARNESS) {
		BatchEvent event = {.kind = BATCH_EVENT_IDENTITY};
		return decide(run, &event, run->session.pid);
	}
	if (check == REPORT_OTHER_GAME) {
		run->inSession = false;
		return FLOW_AGAIN;
	}
	if (edited) {
		return stopEdited(run, arm, &end);
	}
	pending = batch_pending(batch, arm, chunk);
	const BatchPart *evaluated = NULL;
	const BatchPart *previous = NULL;
	for (size_t i = 0; pending && i < pending->partCount; ++i) {
		if (pending->parts[i].attempt == number) {
			evaluated = &pending->parts[i];
			previous = i ? &pending->parts[i - 1] : NULL;
		}
	}
	flow = judgeJob(run, &end, evaluated, previous);
	return flow == FLOW_ON || flow == FLOW_AGAIN ? checkSeedErrors(run, arm) : flow;
}

static BatchPartOutcome partOutcome(const JobEnd *end) {
	if (end->adopted) {
		return BATCH_PART_ADOPTED;
	}
	if (end->killed) {
		return BATCH_PART_ABANDONED;
	}
	if (end->client == CLIENT_REPLIED) {
		switch (end->reply) {
			case BATCH_REPLY_OK: return BATCH_PART_OK;
			case BATCH_REPLY_STOPPED: return BATCH_PART_STOPPED;
			default: return BATCH_PART_ERROR;
		}
	}
	return end->timing.accepted ? BATCH_PART_CRASHED : BATCH_PART_ERROR;
}

// Stops once the arm's rebuild failures pass max_seed_errors.
static Flow checkSeedErrors(Run *run, size_t arm) {
	Batch *batch = run->batch;
	BatchArmTotals totals;
	batch_armTotals(batch, arm, &totals);
	uint64_t failed = totals.rows[BATCH_FAILED];
	uint64_t done = totals.covered;
	for (size_t i = 0; i < batch->pendingCount; ++i) {
		const BatchChunk *chunk = &batch->pending[i];
		BatchChain chain;
		if (chunk->arm != arm || !batchChain_compute(chunk, &chain)) {
			continue;
		}
		for (size_t p = 0; p < chain.count; ++p) {
			failed += chunk->parts[chain.parts[p]].rows[BATCH_FAILED];
		}
		done += chain.cursor - chunk->first;
		batchChain_free(&chain);
	}
	if (!batchPolicy_tooManySeedErrors(&batch->plan.maxSeedErrors, failed, done)) {
		return FLOW_AGAIN;
	}
	fprintf(
		stderr,
		"arm %s: %llu of %llu seeds failed to rebuild\n",
		batch->plan.arms[arm].id,
		(unsigned long long)failed,
		(unsigned long long)done
	);
	BatchEvent event = {.kind = BATCH_EVENT_SEED_ERRORS};
	Flow flow = decide(run, &event, 0);
	return flow == FLOW_ON ? FLOW_AGAIN : flow;
}

// Submits a job with the watchdog, then, if it hung, quits the game or waits for it to end.
// `attempt` if it is an attempt at a chunk, whose part keeps what it played.
static Flow
runJob(Run *run, const char *job, const char *output, bool attempt, bool reportAsked, JobEnd *end) {
	const BatchHost *host = run->host;
	*end = (JobEnd){.attempt = attempt};
	Watchdog dog = {.run = run, .pid = run->inSession ? run->session.pid : 0};
	char failed[UTF8_LENGTH];
	if (platform_fromUtf8(output, dog.files[0], HARNESS_PATH_LENGTH) &&
			siblingOf(output, BATCH_FAILED, failed) &&
			platform_fromUtf8(failed, dog.files[1], HARNESS_PATH_LENGTH)) {
		dog.fileCount = 2;
	}
	ClientWatch watch = {
		.tickMs = (unsigned)(BATCH_TICK_S * run->secondMs), .tick = watchTick, .context = &dog};
	uint64_t started = host->monotonicMs(host->context);
	end->client =
		host->submit(host->context, job, jobTimeoutMs, &watch, end->result, sizeof end->result);
	end->timing = watch;
	end->watch = dog.stopped;
	run->ownJobs += watch.accepted;
	if (end->client == CLIENT_REPLIED) {
		end->reply = batchPolicy_readReply(end->result, reportAsked, &end->report);
		end->hasReport = runReport_parse(end->result, &end->report);
	} else {
		end->result[0] = 0;
	}
	if (dog.stopped == BATCH_WATCH_SIGNAL) {
		return stopWith(run, BATCH_EXIT_STOPPED, "stopped by a signal", false);
	}
	uint32_t pid = dog.pid ? dog.pid : host->findGame(host->context);
	end->gameRunning = pid && host->gameRunning(host->context, pid);
	end->harnessListening = end->gameRunning && host->harnessListening(host->context);
	if (end->client != CLIENT_REPLIED && end->gameRunning && !end->harnessListening) {
		end->gameRunning = outlivesHarness(run, pid);
	}
	// A job that made no progress: in hang_minutes, or since it was asked to stop now.
	bool stalled = end->watch == BATCH_WATCH_HANG || end->client == CLIENT_RUN_TIMEOUT ||
								 (end->watch == BATCH_WATCH_STOP && watch.accepted);
	if (stalled && end->gameRunning && end->harnessListening) {
		Flow flow = actOnStall(run, &dog, pid, output, end);
		if (flow != FLOW_ON) {
			return flow;
		}
	}
	end->sleptMs = dog.sleptMs;
	end->wallMs = host->monotonicMs(host->context) - started;
	return FLOW_ON;
}

// A job that made no progress in game process `pid`, still running and serving: quits the game
// (kill_game=1), leaves the job running if a stop is asked, or waits for it to end.
static Flow actOnStall(Run *run, Watchdog *dog, uint32_t pid, const char *output, JobEnd *end) {
	const BatchHost *host = run->host;
	BatchEvent event = {
		.kind = BATCH_EVENT_JOB,
		.job = {.client = end->client,
			.watch = end->watch,
			.accepted = true,
			.gameRunning = true,
			.harnessListening = true},
	};
	BatchPolicyState state = policyState(run, 0, NULL);
	BatchDecision decision = batchPolicy_decide(&state, &event);
	if (end->watch != BATCH_WATCH_STOP) {
		logEvent(run, "hang pid=%u minutes=%u", pid, run->batch->plan.hangMinutes);
	}
	if (decision.action == BATCH_STOP) {
		return leaveJob(run, pid, output, end->attempt, &decision);
	}
	notify(run, decision.reason);
	fprintf(stderr, "%s\n", decision.reason);
	if (decision.action == BATCH_KILL_AND_RELAUNCH) {
		bool stopped = host->stopGame(host->context, pid, BATCH_STOP_GRACE_S);
		logEvent(run, "game_killed pid=%u ok=%d", pid, stopped);
		run->nextReason = "started again after the batch quit the game";
		end->killed = true;
	} else {
		switch (waitForJob(run, dog, true)) {
			case JOB_ENDED: end->adopted = true; break;
			case JOB_KILLED: end->killed = true; break;
			case JOB_GAME_GONE: break;
			case JOB_SIGNAL: return stopWith(run, BATCH_EXIT_STOPPED, "stopped by a signal", false);
			case JOB_STOPPED:
				state = policyState(run, 0, NULL);
				decision = batchPolicy_decide(&state, &event);
				return leaveJob(run, pid, output, end->attempt, &decision);
			default: return giveUpOnJob(run, pid, output, end->attempt);
		}
	}
	end->gameRunning = host->gameRunning(host->context, pid);
	end->harnessListening = end->gameRunning && host->harnessListening(host->context);
	if (end->gameRunning && !end->harnessListening) {
		end->gameRunning = outlivesHarness(run, pid);
	}
	return FLOW_ON;
}

// Stops with a job that made no progress still running in the game (kill_game=0), saying so.
static Flow
leaveJob(Run *run, uint32_t pid, const char *output, bool attempt, const BatchDecision *decision) {
	sayJobLeft(run, pid, output, attempt);
	bool asked = decision->action == BATCH_STOP;
	char reason[sizeof run->reason];
	(void)snprintf(
		reason,
		sizeof reason,
		"%s (pid %u)",
		asked ? decision->reason : "stopped as asked; a job that made no progress still runs in FM24",
		pid
	);
	return stopWith(run, asked ? decision->exitCode : BATCH_EXIT_STOPPED, reason, true);
}

// queue_wait_minutes passed while waiting for a job that may still run.
static Flow giveUpOnJob(Run *run, uint32_t pid, const char *output, bool attempt) {
	sayJobLeft(run, pid, output, attempt);
	BatchEvent gaveUp = {
		.kind = BATCH_EVENT_JOB,
		.job = {.client = CLIENT_ABANDONED,
			.watch = BATCH_WATCH_GAVE_UP,
			.accepted = true,
			.gameRunning = true,
			.harnessListening = true},
	};
	return decide(run, &gaveUp, pid);
}

// What is left running when batch run stops waiting for a job, and how it ends. The next run's
// barrier waits for an attempt and keeps its rows; a warm-up, canary or lookup keeps none, and the
// next run's first job waits behind it as behind another runner's.
static void sayJobLeft(const Run *run, uint32_t pid, const char *output, bool attempt) {
	const char *directory = run->batch->directoryUtf8;
	size_t length = strlen(directory);
	const char *name = strncmp(output, directory, length) == 0 && output[length] == SEPARATOR
											 ? output + length + 1
											 : output;
	fprintf(
		stderr,
		attempt ? "FM24 (pid %u) still runs the job writing %s. The next batch run waits for it to end "
							"and keeps the rows it wrote; quitting FM24 ends it.\n"
						: "FM24 (pid %u) still runs the job writing %s, whose rows are not kept. The next "
							"batch run waits behind it as behind another runner's job; quit FM24 to end it.\n",
		pid,
		name[0] ? name : "the batch's files"
	);
}

// What to do after a job: a warm-up, canary or lookup (`part` NULL), or an attempt.
static Flow judgeJob(Run *run, JobEnd *end, const BatchPart *part, const BatchPart *previous) {
	BatchEvent event = {
		.kind = BATCH_EVENT_JOB,
		.job =
			{
				.client = end->client,
				.watch = end->watch,
				.accepted = end->timing.accepted,
				.gameRunning = end->gameRunning,
				.harnessListening = end->harnessListening,
				.isolating = end->isolating,
				.reply = end->reply,
				.hasReport = end->hasReport,
				.report = end->report,
				.part = part,
				.previous = previous,
			},
	};
	if (!part && end->adopted) {
		// A warm-up or canary that hung tells nothing: start the session again.
		run->inSession = false;
		return FLOW_AGAIN;
	}
	if (!part && end->client == CLIENT_REPLIED && end->reply == BATCH_REPLY_OK) {
		run->busy = false;
		switch (end->hasReport ? noteReport(run, end) : REPORT_FITS) {
			case REPORT_CHANGED_HARNESS: {
				BatchEvent changed = {.kind = BATCH_EVENT_IDENTITY};
				return decide(run, &changed, run->session.pid);
			}
			case REPORT_OTHER_GAME: run->inSession = false; return FLOW_AGAIN;
			default: return FLOW_ON;
		}
	}
	if (end->client == CLIENT_REPLIED && end->reply != BATCH_REPLY_BUSY) {
		run->busy = false;
	}
	if (end->result[0] && end->reply != BATCH_REPLY_OK) {
		fprintf(stderr, "the harness replied: %s\n", end->result);
	}
	Flow flow = decide(run, &event, run->inSession ? run->session.pid : 0);
	if (!part && flow == FLOW_AGAIN) {
		run->inSession = false;
	}
	return flow;
}

static bool watchTick(void *context, ClientPhase phase) {
	Watchdog *dog = context;
	Run *run = dog->run;
	const BatchHost *host = run->host;
	const BatchPlan *plan = &run->batch->plan;
	uint64_t uptime = host->uptimeMs(host->context);
	noteClocks(dog, uptime, host->monotonicMs(host->context));
	if (host->signals(host->context) >= 3) {
		dog->stopped = BATCH_WATCH_SIGNAL;
		return false;
	}
	BatchAsked asked = askedStop(run);
	if (dog->statusUntil) {
		// A status query while a job may still run: a stop asked meanwhile ends it, so that
		// waitForJob acts on it at once.
		if (asked > dog->asked) {
			dog->stopped = BATCH_WATCH_STOP;
			return false;
		}
		return uptime < dog->statusUntil;
	}
	if (dog->pid && !host->gameRunning(host->context, dog->pid)) {
		dog->stopped = BATCH_WATCH_GAME_GONE;
		return false;
	}
	// A game that is exiting may still have its process: runJob gives it a moment before deciding.
	if (!host->harnessListening(host->context)) {
		dog->stopped = dog->pid ? BATCH_WATCH_HARNESS_GONE : BATCH_WATCH_GAME_GONE;
		return false;
	}
	// Before the harness takes the job, nothing of it has started, and a job whose runner has gone
	// is never sent: any stop ends the wait.
	if (phase != CLIENT_RUNNING && asked != BATCH_ASKED_NONE) {
		dog->stopped = BATCH_WATCH_STOP;
		return false;
	}
	if (phase == CLIENT_QUEUED) {
		dog->queuedSince = dog->queuedSince ? dog->queuedSince : uptime;
		if (uptime - dog->queuedSince > (uint64_t)plan->queueWaitMinutes * 60 * run->secondMs) {
			dog->stopped = BATCH_WATCH_QUEUE;
			return false;
		}
	}
	if (phase == CLIENT_RUNNING && !progressed(dog, uptime)) {
		dog->stopped = BATCH_WATCH_HANG;
		return false;
	}
	if (phase == CLIENT_RUNNING && asked == BATCH_ASKED_STOP_NOW && leftUnanswered(dog, uptime)) {
		dog->stopped = BATCH_WATCH_STOP;
		return false;
	}
	return true;
}

// Notes time the computer spent asleep: the monotonic clock runs on through sleep, uptime does not.
static void noteClocks(Watchdog *dog, uint64_t uptime, uint64_t monotonic) {
	Run *run = dog->run;
	if (dog->lastUptime) {
		uint64_t awake = uptime - dog->lastUptime;
		uint64_t passed = monotonic - dog->lastMonotonic;
		if (passed > awake && passed - awake > BATCH_SLEEP_GAP_S * run->secondMs) {
			uint64_t slept = passed - awake;
			dog->sleptMs += slept;
			dog->lastGrowth = uptime;
			BatchEvent event = {.kind = BATCH_EVENT_SLEPT};
			BatchPolicyState state = policyState(run, 0, NULL);
			(void)batchPolicy_decide(&state, &event);
			logEvent(run, "slept ms=%llu", (unsigned long long)slept);
		}
	}
	dog->lastUptime = uptime;
	dog->lastMonotonic = monotonic;
}

// False if the job's files have not grown for hang_minutes of uptime. Rows are written as an
// in-order prefix, so a stuck seed stops growth even while other workers play.
static bool progressed(Watchdog *dog, uint64_t uptime) {
	Run *run = dog->run;
	uint64_t size = 0;
	for (size_t i = 0; i < dog->fileCount; ++i) {
		size += fileSize(dog->files[i]);
	}
	if (!dog->running || size != dog->lastSize) {
		dog->running = true;
		dog->lastSize = size;
		dog->lastGrowth = uptime;
		return true;
	}
	return uptime - dog->lastGrowth <= (uint64_t)run->batch->plan.hangMinutes * 60 * run->secondMs;
}

// True once a job asked to stop now has made no progress for BATCH_STOP_NOW_GRACE_S since the
// stop was seen, which is long enough for the matches under way to finish. Call progressed first.
static bool leftUnanswered(Watchdog *dog, uint64_t uptime) {
	if (!dog->nowSeen) {
		dog->nowSeen = true;
		dog->nowSince = uptime;
	}
	uint64_t since = dog->lastGrowth > dog->nowSince ? dog->lastGrowth : dog->nowSince;
	return uptime - since >= BATCH_STOP_NOW_GRACE_S * dog->run->secondMs;
}

// Waits for a job that may still run to end: until a status query is answered, the game goes, a
// stop is asked, or queue_wait_minutes pass. A job that hangs meanwhile is quit with kill_game=1,
// and is noted once unless `hangNoted`. Once it has hung any stop ends the wait; before, only a
// stop-now it leaves unanswered does, and a stop takes effect at the chunk boundary. A stop asked
// during a status query ends it; one asked between queries is seen before the next, which a hung
// job would hold for BATCH_STATUS_WAIT_S.
static JobWait waitForJob(Run *run, Watchdog *dog, bool hangNoted) {
	const BatchHost *host = run->host;
	const BatchPlan *plan = &run->batch->plan;
	uint64_t since = host->uptimeMs(host->context);
	dog->running = false;
	dog->nowSeen = false;
	for (bool slept = false;; slept = true) {
		if (slept) {
			host->sleepMs(host->context, (unsigned)(BATCH_TICK_S * run->secondMs));
		}
		if (host->signals(host->context) >= 3) {
			return JOB_SIGNAL;
		}
		if ((dog->pid && !host->gameRunning(host->context, dog->pid)) ||
				!host->harnessListening(host->context)) {
			return JOB_GAME_GONE;
		}
		BatchAsked asked = askedStop(run);
		if (slept && stopEndsWait(run, dog, hangNoted, asked)) {
			return JOB_STOPPED;
		}
		GameStatus status;
		if (statusAnswers(run, &status, asked)) {
			return JOB_ENDED;
		}
		uint64_t uptime = host->uptimeMs(host->context);
		if (!progressed(dog, uptime) && !hangNoted) {
			hangNoted = true;
			if (plan->killGame) {
				bool stopped = host->stopGame(host->context, dog->pid, BATCH_STOP_GRACE_S);
				logEvent(run, "game_killed pid=%u ok=%d", dog->pid, stopped);
				return JOB_KILLED;
			}
			logEvent(run, "hang pid=%u minutes=%u", dog->pid, plan->hangMinutes);
			notify(run, "a job made no progress; waiting for it to end");
		}
		if (stopEndsWait(run, dog, hangNoted, askedStop(run))) {
			return JOB_STOPPED;
		}
		if (uptime - since > (uint64_t)plan->queueWaitMinutes * 60 * run->secondMs) {
			return JOB_GAVE_UP;
		}
	}
}

// True if the stop `asked` ends waitForJob's wait: any stop once the job's hang is noted, before
// then a stop-now it has left unanswered.
static bool stopEndsWait(Run *run, Watchdog *dog, bool hangNoted, BatchAsked asked) {
	if (asked == BATCH_ASKED_NONE || hangNoted) {
		return asked != BATCH_ASKED_NONE;
	}
	uint64_t uptime = run->host->uptimeMs(run->host->context);
	(void)progressed(dog, uptime); // growth since the last check counts; waitForJob notes a hang
	return asked == BATCH_ASKED_STOP_NOW && leftUnanswered(dog, uptime);
}

// Whether the game's process `pid` still runs once its harness has gone. A process that exits
// closes its connection and frees its harness's lock before its process ID goes, so it is given
// BATCH_EXIT_GRACE_S to go before it counts as running without its harness.
static bool outlivesHarness(Run *run, uint32_t pid) {
	const BatchHost *host = run->host;
	uint64_t step = run->secondMs / 10 ? run->secondMs / 10 : 1;
	for (uint64_t waited = 0; host->gameRunning(host->context, pid); waited += step) {
		if (waited >= BATCH_EXIT_GRACE_S * run->secondMs) {
			return true;
		}
		host->sleepMs(host->context, (unsigned)step);
	}
	return false;
}

// Asks the harness for the game's status, waiting at most BATCH_STATUS_WAIT_S for a job before
// it, quietly, and less if a stop beyond `asked` is asked meanwhile: true if it answered.
static bool statusAnswers(Run *run, GameStatus *status, BatchAsked asked) {
	const BatchHost *host = run->host;
	if (!host->harnessListening(host->context)) {
		return false;
	}
	Watchdog dog = {
		.run = run,
		.statusUntil = host->uptimeMs(host->context) + BATCH_STATUS_WAIT_S * run->secondMs,
		.asked = asked,
	};
	ClientWatch watch = {.tickMs = (unsigned)(run->secondMs < 200 ? run->secondMs : 200),
		.tick = watchTick,
		.context = &dog,
		.quiet = true};
	char result[CLIENT_RESULT_LENGTH];
	ClientOutcome outcome =
		host->submit(host->context, "command=status\n", jobTimeoutMs, &watch, result, sizeof result);
	if (outcome != CLIENT_REPLIED) {
		return false;
	}
	if (!saves_parseStatus(result, status)) {
		*status = (GameStatus){0};
	}
	return true;
}

// Checks a report against the session and the pinned harness, and counts jobs another runner ran.
static ReportCheck noteReport(Run *run, const JobEnd *end) {
	Batch *batch = run->batch;
	BatchSession *session = &run->session;
	const RunReport *report = &end->report;
	if (!run->inSession) {
		return REPORT_FITS;
	}
	if (session->hasHarness &&
			(report->pid != session->harness.pid || report->loaded != session->harness.loaded)) {
		fprintf(stderr, "the game restarted under the batch (now pid %u)\n", report->pid);
		return REPORT_OTHER_GAME;
	}
	if (session->pid && report->pid != session->pid) {
		return REPORT_OTHER_GAME;
	}
	ReportCheck check = REPORT_FITS;
	if (!session->hasHarness) {
		const BatchSession *pinned = batch_pinnedSession(batch);
		session->hasHarness = true;
		session->harness = *report;
		session->jobsAtStart = report->jobs >= run->ownJobs ? report->jobs - run->ownJobs : 0;
		if (pinned && strcmp(pinned->id, session->id) != 0 && !sameHarness(&pinned->harness, report)) {
			fprintf(
				stderr,
				"the harness, game build or slice changed: %s %s build %u %s, pinned %s %s build %u %s\n",
				report->protocol,
				report->commit,
				report->build,
				report->slice,
				pinned->harness.protocol,
				pinned->harness.commit,
				pinned->harness.build,
				pinned->harness.slice
			);
			logEvent(
				run,
				"identity_changed session=%s commit=%s build=%u slice=%s",
				session->id,
				report->commit,
				report->build,
				report->slice
			);
			session->identityChanged = true;
			check = run->options->allowHarnessChange ? REPORT_FITS : REPORT_CHANGED_HARNESS;
		}
	} else if (session->identityChanged && !run->options->allowHarnessChange) {
		check = REPORT_CHANGED_HARNESS;
	}
	uint64_t expected = run->lastJobs + run->ownJobs;
	if (run->haveJobs && report->jobs > expected) {
		uint64_t foreign = report->jobs - expected;
		session->foreignJobs += foreign;
		run->needsCanary = batch->plan.canary != 0;
		logEvent(run, "foreign_jobs session=%s count=%llu", session->id, (unsigned long long)foreign);
	}
	run->haveJobs = true;
	run->lastJobs = report->jobs;
	run->ownJobs = 0;
	return check;
}

static bool sameHarness(const RunReport *a, const RunReport *b) {
	return strcmp(a->protocol, b->protocol) == 0 && strcmp(a->commit, b->commit) == 0 &&
				 a->dirty == b->dirty && a->build == b->build && strcmp(a->slice, b->slice) == 0;
}

// Asks the policy what to do about `event` and does it.
static Flow decide(Run *run, const BatchEvent *event, uint32_t pid) {
	size_t arm = 0;
	if (event->kind == BATCH_EVENT_JOB && event->job.part) {
		for (size_t i = 0; i < run->batch->pendingCount; ++i) {
			const BatchChunk *chunk = &run->batch->pending[i];
			if (event->job.part >= chunk->parts && event->job.part < chunk->parts + chunk->partCount) {
				arm = chunk->arm;
			}
		}
	}
	BatchPolicyState state =
		policyState(run, arm, event->kind == BATCH_EVENT_JOB ? event->job.part : NULL);
	BatchDecision decision = batchPolicy_decide(&state, event);
	return apply(run, &decision, pid);
}

static Flow apply(Run *run, const BatchDecision *decision, uint32_t pid) {
	const BatchHost *host = run->host;
	if (decision->crash && decision->counted) {
		if (run->crashCount == MAX_CRASHES) {
			memmove(run->crashes, run->crashes + 1, (MAX_CRASHES - 1) * sizeof *run->crashes);
			--run->crashCount;
		}
		run->crashes[run->crashCount++] = host->monotonicMs(host->context);
	}
	if (decision->crash) {
		logEvent(run, "crash pid=%u%s", pid, decision->counted ? "" : " isolating=1");
	}
	if (decision->busy && !run->busy) {
		run->busy = true;
		run->busySince = host->uptimeMs(host->context);
	}
	run->launchFailures += decision->launchFailed;
	if (decision->action != BATCH_STOP && decision->reason) {
		printf("%s\n", decision->reason);
	}
	if (decision->notify && decision->action != BATCH_STOP) {
		notify(run, decision->reason);
	}
	switch (decision->action) {
		case BATCH_CONTINUE: return FLOW_ON;
		case BATCH_WAIT: {
			Flow flow = pause(run, decision->waitS);
			return flow == FLOW_ON ? FLOW_AGAIN : flow;
		}
		case BATCH_KILL_AND_RELAUNCH: {
			bool stopped = host->stopGame(host->context, pid, BATCH_STOP_GRACE_S);
			logEvent(run, "game_killed pid=%u ok=%d", pid, stopped);
			if (!stopped) {
				return stopWith(run, BATCH_EXIT_FAILED, "cannot quit the game", true);
			}
			run->nextReason = "started again after the batch quit the game";
			return FLOW_AGAIN;
		}
		case BATCH_STOP: return stopWith(run, decision->exitCode, decision->reason, decision->notify);
		default:
			if (decision->crash) {
				run->nextReason = "started again after the game exited";
			}
			return FLOW_AGAIN;
	}
}

// Waits `seconds`, in ticks, unless a stop is asked for meanwhile.
static Flow pause(Run *run, unsigned seconds) {
	const BatchHost *host = run->host;
	uint64_t left = seconds * run->secondMs;
	uint64_t step = BATCH_TICK_S * run->secondMs;
	while (left) {
		uint64_t now = step < left ? step : left;
		host->sleepMs(host->context, (unsigned)now);
		left -= now;
		if (askedStop(run) != BATCH_ASKED_NONE) {
			return stopWith(run, BATCH_EXIT_STOPPED, "stopped as asked", false);
		}
	}
	return FLOW_ON;
}

static Flow stopWith(Run *run, int exitCode, const char *reason, bool notify) {
	run->exitCode = exitCode;
	(void)snprintf(run->reason, sizeof run->reason, "%s", reason ? reason : "");
	run->notifyStop = notify;
	return FLOW_EXIT;
}

// Records how the run ended in events.log and batch.json, says so, and notifies.
static void finish(Run *run) {
	Batch *batch = run->batch;
	const BatchHost *host = run->host;
	bool complete = run->exitCode == EXIT_COMPLETE;
	if (run->inSession && run->session.id[0]) {
		(void)batch_saveSession(batch, &run->session);
	}
	if (complete) {
		logEvent(run, "complete");
	} else {
		logEvent(run, "stopped exit_code=%d reason=%s", run->exitCode, run->reason);
	}
	BatchOutcome outcome = {
		complete ? "complete" : "stopped",
		complete ? NULL : run->reason,
		run->exitCode,
		host->utcNow(host->context),
	};
	if (batch_writeManifest(batch, &outcome) == BATCH_STATUS_KILLED) {
		run->exitCode = BATCH_RUN_KILLED;
		return;
	}
	char text[256];
	if (complete) {
		(void)snprintf(text, sizeof text, "batch %s is complete", batch->plan.name);
		printf("%s\n", text);
		notify(run, text);
		return;
	}
	(void)snprintf(text, sizeof text, "batch %s stopped: %s", batch->plan.name, run->reason);
	fprintf(stderr, "%s (exit code %d)\n", text, run->exitCode);
	if (run->notifyStop) {
		notify(run, text);
	}
}

// The policy's state; `current`, the part being decided on, is not among the poison seeds counted.
static BatchPolicyState policyState(const Run *run, size_t arm, const BatchPart *current) {
	const BatchHost *host = run->host;
	BatchArmTotals totals = {0};
	if (run->batch) {
		batch_armTotals(run->batch, arm, &totals);
	}
	uint64_t poison = totals.excluded;
	for (size_t i = 0; run->batch && i < run->batch->pendingCount; ++i) {
		const BatchChunk *chunk = &run->batch->pending[i];
		for (size_t p = 0; chunk->arm == arm && p < chunk->partCount; ++p) {
			poison += chunk->parts[p].hasPoison && &chunk->parts[p] != current;
		}
	}
	return (BatchPolicyState){
		.plan = &run->batch->plan,
		.threads = run->threads,
		.allowHarnessChange = run->options->allowHarnessChange,
		.crashes = recentCrashes(run),
		.armPoison = poison,
		.busyMs = run->busy ? host->uptimeMs(host->context) - run->busySince : 0,
		.launchFailures = run->launchFailures,
		.minuteMs = 60 * run->secondMs,
		.asked = run->batch ? askedStop(run) : BATCH_ASKED_NONE,
	};
}

static unsigned recentCrashes(const Run *run) {
	uint64_t now = run->host->monotonicMs(run->host->context);
	uint64_t window = CRASH_WINDOW_S * run->secondMs;
	unsigned count = 0;
	for (size_t i = 0; i < run->crashCount; ++i) {
		count += now - run->crashes[i] < window;
	}
	return count;
}

static void notify(Run *run, const char *text) {
	if (run->batch->plan.notify && text) {
		run->host->notify(run->host->context, text);
	}
}

static void logEvent(Run *run, const char *format, ...) {
	char line[768];
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	batch_logEvent(run->batch, "%s", line);
}

// The batch's folder, then a path inside it (UTF-8, '/' between its parts), as jobs name files.
static bool jobPath(const Run *run, char *out, const char *format, ...) {
	char relative[UTF8_LENGTH];
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(relative, sizeof relative, format, arguments);
	va_end(arguments);
	if (written <= 0 || (size_t)written >= sizeof relative) {
		return false;
	}
	for (char *c = relative; *c; ++c) {
		*c = *c == '/' ? SEPARATOR : *c;
	}
	written = snprintf(out, UTF8_LENGTH, "%s%c%s", run->batch->directoryUtf8, SEPARATOR, relative);
	return written > 0 && (size_t)written < UTF8_LENGTH;
}

// A results CSV's sibling, as the harness names it: X.csv, X-match-players.csv and so on.
static bool siblingOf(const char *output, size_t role, char *out) {
	size_t length = strlen(output);
	if (length < 4 || strcmp(output + length - 4, ".csv") != 0) {
		return false;
	}
	int written =
		snprintf(out, UTF8_LENGTH, "%.*s%s.csv", (int)(length - 4), output, siblingSuffixes[role]);
	return written > 0 && (size_t)written < UTF8_LENGTH;
}

static bool fileExists(const char *path) {
	wchar_t wide[HARNESS_PATH_LENGTH];
	return platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) && fileThere(wide);
}

static bool fileThere(const wchar_t *path) {
	FILE *file = platform_openFile(path, "rb");
	if (file) {
		(void)fclose(file);
	}
	return file != NULL;
}

static uint64_t fileSize(const wchar_t *path) {
	FILE *file = platform_openFile(path, "rb");
	if (!file) {
		return 0;
	}
	long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
	(void)fclose(file);
	return size > 0 ? (uint64_t)size : 0;
}

// A run job of `count` indices of a fixture (inputs/<id>.bin) from `first`, playing `seeds`, into
// `output`, with report=1 and the plan's settings but `threads`; a warm-up writes no per-match rows
// and skips seeds that fail to rebuild.
static bool formatRunJob(
	const Run *run,
	const BatchArm *fixture,
	const BatchSeeds *seeds,
	uint64_t first,
	uint64_t count,
	uint32_t threads,
	bool warmup,
	const char *output,
	char *job
) {
	const BatchPlan *plan = &run->batch->plan;
	char input[UTF8_LENGTH];
	char stop[UTF8_LENGTH];
	char listed[HARNESS_JOB_LENGTH];
	uint64_t last = first + count - 1;
	if (!count || last > seeds->last || !jobPath(run, input, "inputs/%s.bin", fixture->id) ||
			!jobPath(run, stop, "control/stop-now") ||
			!batchSeeds_format(seeds, first, last, listed, sizeof listed)) {
		return false;
	}
	RunJob settings = {
		.input = input,
		.seeds = listed,
		.output = output,
		.threads = threads,
		.profile = "",
		.humanControl = plan->humanControl != 0,
		.keepBuild = plan->keepBuild != 0,
		.teams = {fixture->teams[0], fixture->teams[1]},
		.perMatch = !warmup && plan->perMatch,
		.skipSeedErrors = warmup || plan->seedErrors == BATCH_SEED_ERRORS_SKIP,
		.report = true,
		.indexBase = first,
		.stopFile = stop,
	};
	return runJob_format(&settings, job, HARNESS_JOB_LENGTH);
}

// Compares a canary's files with the reference's, or makes them the reference if there is none
// (its results last, which mark it whole). `result`: 2 made the reference, 1 the same, 0 not.
// False if a file cannot be read or kept.
static bool copyCanary(const char *from, const char *to, int *result) {
	bool missing = !fileExists(to);
	bool same = true;
	for (size_t i = BATCH_ROLES; i-- > 0;) {
		char source[UTF8_LENGTH];
		char target[UTF8_LENGTH];
		wchar_t sourceWide[HARNESS_PATH_LENGTH];
		wchar_t targetWide[HARNESS_PATH_LENGTH];
		if (!siblingOf(from, i, source) || !siblingOf(to, i, target) ||
				!platform_fromUtf8(source, sourceWide, HARNESS_PATH_LENGTH) ||
				!platform_fromUtf8(target, targetWide, HARNESS_PATH_LENGTH)) {
			return false;
		}
		ByteBuffer ours = {0};
		ByteBuffer theirs = {0};
		ByteBuffer left = {0};
		ByteBuffer right = {0};
		bool have = files_read(sourceWide, &ours);
		bool ok = true;
		if (missing) {
			ok = !have || files_replace(targetWide, ours.data, ours.length);
		} else if (have != files_read(targetWide, &theirs)) {
			same = false;
		} else if (have) {
			ok = canonicalRows(&ours, i, false, UINT64_MAX, &left) &&
					 canonicalRows(&theirs, i, false, UINT64_MAX, &right);
			same = same && left.length == right.length &&
						 (!left.length || memcmp(left.data, right.data, left.length) == 0);
		}
		buffer_free(&ours);
		buffer_free(&theirs);
		buffer_free(&left);
		buffer_free(&right);
		if (!ok) {
			return false;
		}
	}
	*result = missing ? 2 : same ? 1 : 0;
	return true;
}

// BatchCommitFacts.checkCanary for chunk 0: its first `canary` indices against the reference.
static int compareChunkWithReference(void *context, const wchar_t *folder) {
	ReferenceCheck *check = context;
	Run *run = check->run;
	const BatchPlan *plan = &run->batch->plan;
	static const char *const chunkNames[BATCH_ROLES] = {
		"results.csv", "match-players.csv", "match-teams.csv", "failed.csv"};
	char reference[UTF8_LENGTH];
	uint64_t limit = plan->seeds.first + plan->canary;
	bool same = jobPath(run, reference, "canary/%s.csv", plan->arms[check->arm].id);
	for (size_t role = 0; same && role < BATCH_ROLES; ++role) {
		char referencePath[UTF8_LENGTH];
		wchar_t referenceWide[HARNESS_PATH_LENGTH];
		wchar_t chunkPath[HARNESS_PATH_LENGTH];
		size_t base = wcslen(folder);
		size_t name = strlen(chunkNames[role]);
		if (base + 1 + name >= HARNESS_PATH_LENGTH || !siblingOf(reference, role, referencePath) ||
				!platform_fromUtf8(referencePath, referenceWide, HARNESS_PATH_LENGTH)) {
			return -1;
		}
		wmemcpy(chunkPath, folder, base);
		chunkPath[base] = PLATFORM_PATH_SEPARATOR;
		for (size_t i = 0; i <= name; ++i) {
			chunkPath[base + 1 + i] = (wchar_t)chunkNames[role][i];
		}
		ByteBuffer ours = {0};
		ByteBuffer theirs = {0};
		ByteBuffer left = {0};
		ByteBuffer right = {0};
		bool haveChunk = files_read(chunkPath, &ours);
		bool haveReference = files_read(referenceWide, &theirs);
		if (haveChunk != haveReference) {
			same = false;
		} else if (haveChunk) {
			same = canonicalRows(&ours, role, role == BATCH_RESULTS, limit, &left) &&
						 canonicalRows(&theirs, role, false, limit, &right) && left.length == right.length &&
						 (!left.length || memcmp(left.data, right.data, left.length) == 0);
		}
		buffer_free(&ours);
		buffer_free(&theirs);
		buffer_free(&left);
		buffer_free(&right);
	}
	check->result = same ? 1 : 0;
	return check->result;
}

// The rows of a file after its header, as a canary compares them: results without `ms`, the last
// field, and without a chunk's `index` (`indexed`); only rows whose index is below `limit` (the
// harness's results have none, and are all kept).
static bool
canonicalRows(const ByteBuffer *text, size_t role, bool indexed, uint64_t limit, ByteBuffer *out) {
	const char *end = (const char *)text->data + text->length;
	const char *line = memchr(text->data, '\n', text->length);
	for (line = line ? line + 1 : end; line < end;) {
		const char *next = memchr(line, '\n', (size_t)(end - line));
		if (!next) {
			break; // a torn last line
		}
		if (!canonicalRow(line, (size_t)(next - line), role, indexed, limit, out)) {
			return false;
		}
		line = next + 1;
	}
	return true;
}

// A row of canonicalRows, without its line ending; false only if memory runs out.
static bool canonicalRow(
	const char *row,
	size_t length,
	size_t role,
	bool indexed,
	uint64_t limit,
	ByteBuffer *out
) {
	if (role != BATCH_RESULTS || indexed) {
		char *after = NULL;
		unsigned long long index = strtoull(row, &after, 10);
		if (after == row || *after != ',' || index >= limit) {
			return true;
		}
		if (role == BATCH_RESULTS) {
			length -= (size_t)(after + 1 - row);
			row = after + 1;
		}
	}
	if (role == BATCH_RESULTS) {
		const char *comma = NULL;
		for (const char *c = row; c < row + length; ++c) {
			comma = *c == ',' ? c : comma;
		}
		length = comma ? (size_t)(comma - row) : length;
	}
	return buffer_append(out, row, length) && buffer_append(out, "\n", 1);
}

static const char *powerName(BatchPower power) {
	switch (power) {
		case BATCH_POWER_AC: return "ac";
		case BATCH_POWER_BATTERY: return "battery";
		default: return NULL;
	}
}
