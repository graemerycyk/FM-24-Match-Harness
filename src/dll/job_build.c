// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Job "build_next": serialise the human manager's next fixture.

#include "fixture.h"
#include "game_state.h"
#include "job.h"
#include "log.h"
#include "match.h"
#include "output.h"
#include "platform.h"
#include "trace.h"

#include <stdio.h>
#ifdef __APPLE__
#include <time.h>
#endif


static uint32_t clockSeed(void);

void jobs_buildNext(const Job *job) {
	NextFixture next;
	const char *error = NULL;
	if (!fixture_findNext(&next, &error)) {
		log_message("build_next: %s", error);
		job_writeResult("error: %s", error);
		return;
	}
	uint32_t seed = job->hasSeed ? job->seed : clockSeed();
	log_message(
		"build_next: %s, %s, fixture %p, seed %u",
		next.description,
		next.when,
		(void *)next.fixture,
		seed
	);
	bool attended = fixture_isAttended(next.fixture);
	uint8_t *match = match_build(next.fixture, seed);
	if (!match) {
		job_writeResult("error: match allocation failed");
		return;
	}
	Serialised serialised = {0};
	bool ok = trace_serialize(match, &serialised, output_wantsJson(job->output));
	match_freeBuilt(match);
	char source[256];
	(void)snprintf(source, sizeof source, "%s, %s, seed %u", next.description, next.when, seed);
	GameState state;
	(void)gameState_read(&state, false);
	if (ok &&
			output_write(job->output, &serialised, source, state.save, state.date, NULL, job->full)) {
		log_message("build_next: wrote %zu bytes", serialised.bytes.length);
		if (attended) {
			job_writeResult("ok: %s, %zu bytes", source, serialised.bytes.length);
		} else {
			log_message("build_next: the fixture is not among the human's attended matches");
			job_writeResult(
				"ok: %s, %zu bytes; warning: the game has not yet scheduled you to attend this match "
				"(it does once match day reaches the match and shows Go to Match), so the AI picked your "
				"team's line-up and tactic",
				source,
				serialised.bytes.length
			);
		}
	} else {
		job_writeResult("error: serialise or write failed; see harness.log");
	}
	trace_free(&serialised);
}

// The seed MatchBuilder_Create would pick: on Windows, QueryPerformanceCounter's low 32 bits in
// milliseconds; on macOS, mach_absolute_time (the uptime clock) in milliseconds, which the game
// folds as ms + ms / (2^32 - 1) in 32 bits: ms modulo 2^32 - 1.
static uint32_t clockSeed(void) {
#ifdef _WIN32
	uint32_t counter = (uint32_t)platform_ticks();
	int32_t frequency = (int32_t)(uint32_t)platform_tickFrequency();
	return (uint32_t)((double)counter / ((double)frequency / 1000.0) + 0.5);
#else
	int64_t milliseconds = (int64_t)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + 500000) / 1000000);
	return (uint32_t)(milliseconds % 0xFFFFFFFF);
#endif
}
