// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A new order of a side's players in a fixture, for `fm24ctl lineup` and the viewer: whole player
// blocks change places and are renamed to them, the opponent's marking of a moved player follows
// the player, and the header gains "<side>_lineup": true. No file, game or platform access.
#pragma once

#include "buffer.h"
#include "fixture_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// A new order per side: from[side][place] is the place whose player moves to `place`. A side in
// `mark` gets its marker even if its order moves nobody, as a line-up kept through a rebuild does.
typedef struct FixtureLineup {
	bool changed[2];
	bool mark[2];
	uint8_t from[2][FIXTURE_PLAYERS];
} FixtureLineup;

typedef struct FixtureLineupResult {
	unsigned markings[2];            // opponent marking indices re-pointed, per reordered side
	bool opponentRecords[2];         // the opponent's part_4240 was not uniform (warned)
	bool marked[2][FIXTURE_PLAYERS]; // the new places of the players a re-pointed index marks
} FixtureLineupResult;

// What fixtureLineup_byPeople found of the people it was given, by their index.
typedef struct FixtureLineupPeople {
	unsigned missing;            // people not found
	bool found[FIXTURE_PLAYERS]; // whether each person was
	// Each person's new place, or for one not found the place another player took instead (-1 if
	// none was left to take it).
	int places[FIXTURE_PLAYERS];
} FixtureLineupPeople;

void fixtureLineup_identity(FixtureLineup *lineup);
// Whether `text`, without spaces around it, names a place on `side`: 1 for "player[N]" or
// "<side>.player[N]", with N in `place` (whether the side has it is not checked), 0 for other text
// (a name or an ID), and -1 with the message for a malformed place or one of the other side.
int fixtureLineup_readPlace(
	const char *text,
	unsigned side,
	unsigned *place,
	char *error,
	size_t errorSize
);
// The place `text` names on `side`: "player[N]" or "<side>.player[N]", a unique ID (digits only),
// or a name (whole, ASCII case ignored; otherwise a unique part). False with the message, which
// lists "player[N] Name (ID)" for an ambiguous name or ID.
bool fixtureLineup_find(
	const FixtureData *data,
	unsigned side,
	const char *text,
	unsigned *place,
	char *error,
	size_t errorSize
);
// Exchanges the players at places `a` and `b` of the side's `count`. False (and unchanged) for the
// same place twice or a place out of range.
bool fixtureLineup_exchange(
	FixtureLineup *lineup,
	unsigned side,
	unsigned count,
	unsigned a,
	unsigned b
);
// `listed` (11 or more distinct places) take places 0..listedCount-1. Each displaced player fills,
// in old-place order, the places the listed players left. Everyone else stays.
bool fixtureLineup_promote(
	FixtureLineup *lineup,
	unsigned side,
	unsigned count,
	const unsigned *listed,
	unsigned listedCount,
	char *error,
	size_t errorSize
);
// Refusals (false + error) and warnings (lines, possibly none) for a side's new order.
bool fixtureLineup_check(
	const FixtureData *data,
	unsigned side,
	const FixtureLineup *lineup,
	bool allowAiSide,
	char *error,
	size_t errorSize,
	char *warnings,
	size_t warningsSize
);
// Writes the new order of every changed side into the fixture `json`, checks the result and fills
// `result`. `out` is empty on failure.
bool fixtureLineup_apply(
	const char *json,
	size_t length,
	const FixtureLineup *lineup,
	ByteBuffer *out,
	FixtureLineupResult *result,
	char *error,
	size_t errorSize
);
// Sets `side`'s order in `lineup` to one putting `people` (`count` unique IDs, in order, matched by
// ID and occurrence) first. The place of a person not found goes to the first of the side's players
// whom `people` do not name, in file order, if one is left, so the people after it keep theirs; the
// rest of those players follow. False if `count` is more than FIXTURE_PLAYERS.
bool fixtureLineup_byPeople(
	const FixtureData *data,
	unsigned side,
	const uint32_t *people,
	unsigned count,
	FixtureLineup *lineup,
	FixtureLineupPeople *matched
);
