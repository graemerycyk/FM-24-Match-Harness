// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game_state.h"
#include "job.h"
#include "log.h"

#include <stdio.h>


static bool mustWait(const Job *job, char *reason, size_t size);

void job_execute(char *text) {
	Job job;
	if (!job_parse(text, &job)) {
		job_writeResult("error: cannot read the job; see harness.log");
		return;
	}
	char reason[48];
	if (mustWait(&job, reason, sizeof reason)) {
		log_message("refusing a job: FM24 is busy (%s)", reason);
		job_writeResult("error: FM24 is busy (%s); wait until it is idle", reason);
		job_free(&job);
		return;
	}
	switch (job.command) {
		case JOB_NONE: break; // job_parse rejects it
		case JOB_RUN: jobs_run(&job); break;
		case JOB_BUILD_NEXT: jobs_buildNext(&job); break;
		case JOB_INSPECT: jobs_inspect(&job); break;
		case JOB_REPLACE_AWAY:
		case JOB_REPLACE_HOME: jobs_replaceTeam(&job); break;
		case JOB_LIST_TEAMS: jobs_listTeams(&job); break;
		case JOB_LIST_PLAYERS: jobs_listPlayers(&job); break;
		case JOB_GET_PLAYER: jobs_getPlayer(&job); break;
		case JOB_STATUS: jobs_status(&job); break;
	}
	job_free(&job);
}

// Every job but status uses the game's objects, which the game's own threads change while it loads
// or processes a game, so those jobs wait for it. Windows has no game state yet, so jobs never wait
// there. A game that starts processing just after this check still races the job.
static bool mustWait(const Job *job, char *reason, size_t size) {
	GameState state;
	if (job->command == JOB_STATUS || !gameState_read(&state, false)) {
		return false;
	}
	switch (gameState_phase(&state)) {
		case GAME_PHASE_LOADING: (void)snprintf(reason, size, "loading"); return true;
		case GAME_PHASE_BUSY:
			(void)snprintf(reason, size, "processing kind %d", (int)state.processingKind);
			return true;
		default: return false;
	}
}
