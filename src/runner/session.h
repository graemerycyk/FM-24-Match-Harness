// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Waiting for Football Manager 2024 to have a save loaded and idle, from the harness's status job
// (see saves.h). The wait is a state machine over samples of the game, one a second, so it can be
// checked with scripted samples; the caller says how to take one. A command whose job is for a
// save waits the same way first, or has the game started with the save (session_plan).
#pragma once

#include "saves.h"

#include <stdbool.h>
#include <time.h>


enum {
	SESSION_POLL_MS = 1000,       // between samples
	SESSION_STABLE_SAMPLES = 3,   // ready samples in a row, after a launch or a wait
	SESSION_MENU_GRACE_S = 15,    // at the start screen this long after a launch: no load
	SESSION_NO_TEAM_GRACE_S = 30, // a game without the human manager's team this long
	SESSION_PROGRESS_S = 60,      // between "still waiting" lines
};

typedef enum SessionAnswer {
	SESSION_ANSWER_NONE,   // no harness answered (client.h printed why, if it was asked)
	SESSION_ANSWER_ERROR,  // the harness answered something other than a status, which was printed
	SESSION_ANSWER_STATUS, // the harness answered with the game's status
} SessionAnswer;

typedef enum SessionOutcome {
	SESSION_WAITING,
	SESSION_READY,       // the save is loaded and idle, or a save the harness cannot name is
	SESSION_OTHER_SAVE,  // another save is loaded and idle
	SESSION_AT_MENU,     // the running game is at its start screen without a save
	SESSION_NO_AUTOPLAY, // the started game reached its start screen without the auto-play file
	SESSION_NOT_LOADED,  // it read the auto-play file, but did not load the save
	SESSION_NO_TEAM,     // a game is loaded, but the harness finds no human manager's team
	SESSION_REFUSED,     // the harness did not answer with a status
	SESSION_EXITED,      // the game exited
	SESSION_TIMED_OUT,
	SESSION_STOPPED, // a batch was asked to stop while the wait went on (SessionStop)
} SessionOutcome;

typedef struct SessionSample {
	time_t at;
	bool running; // the game's process is running
	SessionAnswer answer;
	GameStatus status; // with SESSION_ANSWER_STATUS
	bool stopped;      // a stop was asked (SessionStop): a wait it does not decide ends
} SessionSample;

typedef struct SessionWait {
	const char *save; // the save wanted, as the harness names it
	bool launched;    // fm24ctl started the game to load it
	time_t started;
	unsigned timeoutS;
	time_t reported;       // the last "still waiting" line
	time_t menuSince;      // since when the game has been at its start screen (0: it is not)
	time_t noTeamSince;    // since when it has had a game without the human team (0: it has not)
	unsigned readySamples; // ready samples in a row with `last.save`
	bool waited;           // a sample was not ready, so readiness must last
	bool answered;         // `last` holds an answer
	GameStatus last;       // the harness's last answer
} SessionWait;

// Takes a sample of the game; the wait sets its time.
typedef void SessionPoll(void *context, SessionSample *sample);

// What a batch's check of the game asks while it waits, behind another runner's job or for the
// game: true from `asked` ends the wait (SESSION_STOPPED). A check the first sample decides is not
// stopped.
typedef struct SessionStop {
	bool (*asked)(void *context);
	void *context;
	unsigned tickMs; // how often a status query asks while it waits for the harness to take it
} SessionStop;

// A command's job for a save: the game must have that save loaded and idle before it is submitted.
typedef struct SessionNeed {
	const char *save;   // as the harness names it; NULL or "" if the job is for whatever is loaded
	bool launch;        // start the game with the save if it is not running (not --no-launch)
	bool anySave;       // the job may use another save the game has loaded (--any-save)
	unsigned timeoutMs; // how long the job may wait for another runner's job, and so may a check
	// A batch's (zero for a command's job):
	bool batch;           // the plan's launch= and any_save= decide, not a command's options
	const char *savePath; // the save's .fm file to start the game with; NULL to look the name up
	bool rosetta;         // start the game's x86_64 slice under Rosetta, as launch --rosetta does
	unsigned readyS;      // how long the save may take to be loaded and idle; 0 for launch's default
	SessionStop *stop;    // what ends the check while it waits; NULL for nothing
} SessionNeed;

typedef enum SessionPlan {
	SESSION_PLAN_SUBMIT, // submit the job as before, without checking the game
	SESSION_PLAN_LAUNCH, // start the game with the save, then submit the job once it is ready
	SESSION_PLAN_CHECK,  // wait for the running game, then submit the job if session_submits
} SessionPlan;

// Asks the harness what the game is doing, waiting at most `timeoutMs` for another runner's job,
// or, with `stop`, until a stop is asked meanwhile (SESSION_ANSWER_NONE).
SessionAnswer session_query(GameStatus *status, unsigned timeoutMs, SessionStop *stop);

// What to do before a job for `need`, given whether a game process is `running` and whether it
// `serves` this protocol's harness. A game without it gets the job as before, which explains.
SessionPlan session_plan(const SessionNeed *need, bool running, bool serving);

// True to submit the job once the wait for the game has ended in `outcome`. A harness that answers
// the status job with something else is one that cannot report the save: the job goes ahead.
bool session_submits(const SessionNeed *need, SessionOutcome outcome);

// Starts waiting at `now` for `save`, for at most `timeoutS`.
void session_start(
	SessionWait *wait,
	const char *save,
	bool launched,
	unsigned timeoutS,
	time_t now
);

// The wait's outcome after one more sample: SESSION_WAITING until it is decided. A game that was
// already running is decided by its first sample unless that finds it busy; one fm24ctl started
// must be ready for SESSION_STABLE_SAMPLES in a row. A sample that does not decide it ends it if
// `stopped`.
SessionOutcome session_step(SessionWait *wait, const SessionSample *sample);

// Samples the game every SESSION_POLL_MS until the wait is decided, printing its state as it
// changes and a line every SESSION_PROGRESS_S.
SessionOutcome session_waitReady(SessionWait *wait, SessionPoll *poll, void *context);
