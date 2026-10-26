// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "session.h"

#include "client.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>


static SessionOutcome judge(SessionWait *wait, const SessionSample *sample);
static time_t since(time_t start, bool holds, time_t now);

SessionAnswer session_query(GameStatus *status, unsigned timeoutMs) {
	char result[CLIENT_RESULT_LENGTH];
	if (!client_submit("command=status\n", timeoutMs, result, sizeof result)) {
		return SESSION_ANSWER_NONE;
	}
	if (saves_parseStatus(result, status)) {
		return SESSION_ANSWER_STATUS;
	}
	fprintf(
		stderr, "%s%s\n", client_succeeded(result) ? "cannot read the harness's status: " : "", result
	);
	return SESSION_ANSWER_ERROR;
}

SessionPlan session_plan(const SessionNeed *need, bool running, bool serving) {
	if (!need->save || !need->save[0]) {
		return SESSION_PLAN_SUBMIT;
	}
	if (!running) {
		return need->launch ? SESSION_PLAN_LAUNCH : SESSION_PLAN_SUBMIT;
	}
	return serving ? SESSION_PLAN_CHECK : SESSION_PLAN_SUBMIT;
}

bool session_submits(const SessionNeed *need, SessionOutcome outcome) {
	switch (outcome) {
		case SESSION_READY:
		case SESSION_REFUSED: return true;
		case SESSION_OTHER_SAVE: return need->anySave;
		default: return false;
	}
}

void session_start(
	SessionWait *wait,
	const char *save,
	bool launched,
	unsigned timeoutS,
	time_t now
) {
	*wait = (SessionWait){
		.save = save,
		.launched = launched,
		.started = now,
		.timeoutS = timeoutS,
		.reported = now,
	};
}

SessionOutcome session_step(SessionWait *wait, const SessionSample *sample) {
	if (!sample->running) {
		return SESSION_EXITED;
	}
	if (sample->answer == SESSION_ANSWER_ERROR) {
		return SESSION_REFUSED;
	}
	SessionOutcome outcome = SESSION_WAITING;
	if (sample->answer == SESSION_ANSWER_STATUS) {
		outcome = judge(wait, sample);
	} else {
		wait->readySamples = 0;
		wait->waited = true;
	}
	if (outcome == SESSION_WAITING && sample->at - wait->started >= (time_t)wait->timeoutS) {
		return SESSION_TIMED_OUT;
	}
	return outcome;
}

SessionOutcome session_waitReady(SessionWait *wait, SessionPoll *poll, void *context) {
	for (;;) {
		SessionSample sample = {.at = time(NULL)};
		poll(context, &sample);
		bool changed = sample.answer == SESSION_ANSWER_STATUS &&
									 (!wait->answered || sample.status.phase != wait->last.phase);
		SessionOutcome outcome = session_step(wait, &sample);
		if (outcome != SESSION_WAITING) {
			return outcome;
		}
		if (changed) {
			printf("FM24 state: %s\n", saves_phaseName(sample.status.phase));
		}
		if (sample.at - wait->reported >= SESSION_PROGRESS_S) {
			printf("still waiting (%lld s)...\n", (long long)(sample.at - wait->started));
			wait->reported = sample.at;
		}
		platform_sleep(SESSION_POLL_MS);
	}
}

// The outcome after the harness's answer.
static SessionOutcome judge(SessionWait *wait, const SessionSample *sample) {
	const GameStatus *status = &sample->status;
	bool sameSave = wait->answered && strcmp(status->save, wait->last.save) == 0;
	wait->last = *status;
	wait->answered = true;
	wait->menuSince = since(wait->menuSince, status->phase == STATUS_MENU, sample->at);
	wait->noTeamSince = since(wait->noTeamSince, status->phase == STATUS_NO_TEAM, sample->at);
	if (status->phase != STATUS_READY) {
		wait->readySamples = 0;
		wait->waited = true;
		if (status->phase == STATUS_MENU && !wait->launched) {
			return SESSION_AT_MENU;
		}
		if (status->phase == STATUS_MENU && sample->at - wait->menuSince >= SESSION_MENU_GRACE_S) {
			return status->autoPlayRead ? SESSION_NOT_LOADED : SESSION_NO_AUTOPLAY;
		}
		if (status->phase == STATUS_NO_TEAM &&
				sample->at - wait->noTeamSince >= SESSION_NO_TEAM_GRACE_S) {
			return SESSION_NO_TEAM;
		}
		return SESSION_WAITING;
	}
	wait->readySamples = wait->readySamples && sameSave ? wait->readySamples + 1 : 1;
	unsigned needed = wait->launched || wait->waited ? SESSION_STABLE_SAMPLES : 1;
	if (wait->readySamples < needed) {
		return SESSION_WAITING;
	}
	// A save the harness cannot name is taken to be the one asked for.
	return !status->save[0] || saves_isSave(status->save, wait->save) ? SESSION_READY
																																		: SESSION_OTHER_SAVE;
}

// When a condition that `holds` at `now` began: `start` if it already held, 0 if it does not.
static time_t since(time_t start, bool holds, time_t now) {
	return holds ? (start ? start : now) : 0;
}
