// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A season batch (mode=season; see docs/BATCH.md "Season runs"): its fixtures, every ordered pair
// of its teams once, the check fixtures that cover every team, how `batch run` builds each fixture
// by team replacement from the base and checks what it built, and inputs/fixtures.txt, which
// freezes the fixtures once built. Pure: nothing here reads a file.
#pragma once

#include "batch_plan.h"
#include "buffer.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// The number of fixtures of `teams` teams: n (n - 1).
size_t batchSeason_fixtures(size_t teams);
// Fixture `fixture` (from 0) of `teams` teams: team `home` at home to `away`, by their places in
// the plan. Fixture h (n - 1) + (a < h ? a : a - 1) is t_h at home to t_a: the first is t0 v t1.
void batchSeason_pair(size_t teams, size_t fixture, size_t *home, size_t *away);
// The fixture of `home` at home to `away` (they differ).
size_t batchSeason_fixture(size_t teams, size_t home, size_t away);
// Whether a fixture is a check fixture: t0 v t1, t2 v t3 and so on, and t(n-1) v t0 when n is odd,
// so that they cover every team. Only they are warmed up and checked with the canary.
bool batchSeason_isCheckFixture(size_t teams, size_t fixture);
// A fixture's ID, "f" and its number from 1 as four digits ("f0001").
void batchSeason_id(size_t fixture, char id[BATCH_ARM_LENGTH]);

// inputs/fixtures.txt ("fm24-season-fixtures/1"): the plan's hash, a fixture=<id> line of each
// fixture's hashes and team overrides in order (as a frozen plan's fixture lines), then the
// SHA-256 of the lines above it, which goes into `hash` too. False if memory runs out.
bool batchSeason_formatFixtures(
	const BatchPlan *plan,
	ByteBuffer *out,
	char hash[SHA256_HEX_LENGTH]
);
// Reads it into the plan's fixtures. False (with why in `error`) if it is not one of this plan,
// lacks a fixture or has been changed.
bool batchSeason_readFixtures(
	const char *text,
	size_t length,
	BatchPlan *plan,
	char hash[SHA256_HEX_LENGTH],
	char *error,
	size_t errorSize
);

// The replace_home (or with `away`, replace_away) job that builds `input` (a .bin with the team
// overrides `teams`) against `team` into `output` (a .json). False if it does not fit in `size`.
bool batchSeason_formatReplace(
	bool away,
	const char *input,
	uint32_t team,
	const uint32_t teams[2],
	const char *output,
	char *out,
	size_t size
);
// Whether a replacement's reply is a success that built `team` and kept no edits of the input's:
// "ok: <home> vs <away>, <when>, seed N (team U); 0 edits kept, ...". If not, why is in `why`.
bool batchSeason_checkReply(const char *result, uint32_t team, char *why, size_t size);
// Checks the fixture a replacement of `side` (0 home, 1 away) with `team` built from an input with
// the overrides `input`: a fixture of the plan's game build, save and in-game date that encodes
// (into `bin`), whose replaced side's override is `team` or none (the side's own team) and whose
// other side's is the input's. Its overrides go to `teams`. False with why in `why`.
bool batchSeason_checkBuilt(
	const BatchPlan *plan,
	const ByteBuffer *json,
	unsigned side,
	uint32_t team,
	const uint32_t input[2],
	ByteBuffer *bin,
	uint32_t teams[2],
	char *why,
	size_t size
);
