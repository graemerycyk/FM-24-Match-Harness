// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Job "status": what the game is doing (game_state.h), read without touching the game, as one line
// whose keys keep this order (a missing date or save is left out; the save runs to the line's end):
//   ok: <phase> in_progress=0|1 processing=0|1 kind=N people=N human_team=0|1 menu_seen=0|1
//       autoplay=0|1 autoplay_read=0|1 date=YYYY day D HH:MM save=<name>

#include "game_state.h"
#include "job.h"


void jobs_status(const Job *job) {
	(void)job;
#ifdef _WIN32
	job_writeResult("error: status is not available on Windows yet");
#else
	GameState state;
	if (!gameState_read(&state, true)) {
		job_writeResult("error: cannot read the game's state");
		return;
	}
	job_writeResult(
		"ok: %s in_progress=%d processing=%d kind=%d people=%zu human_team=%d menu_seen=%d "
		"autoplay=%d autoplay_read=%d%s%s%s%s",
		gameState_phaseName(gameState_phase(&state)),
		state.inProgress,
		state.processing,
		(int)state.processingKind,
		state.people,
		state.humanTeam,
		state.startScreenSeen,
		state.autoPlay,
		state.autoPlayRead,
		state.date[0] ? " date=" : "",
		state.date,
		state.save[0] ? " save=" : "",
		state.save
	);
#endif
}
