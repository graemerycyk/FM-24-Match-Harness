// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Per-seed match inputs that match the game's own build with each seed (docs/PLAN.md Phase 19).
//
// The game's match seed is used twice: GAME_MATCH__init_from_fixture draws the pre-match values
// from it (a random number per player, the officials' match-day ratings, attendance, weather), and
// the deserialiser seeds the in-match RNG with it. An input file carries the pre-match values of
// the seed it was built with, so patching only its seed field does not reproduce the game. A seed
// builder rebuilds the input's fixture with each seed instead and re-applies the input's edits:
// every field where the input differs from a build with its own seed keeps the input's value.
// The match's observers are the exception (sites.h): the game picks them from its own state while
// it builds the match, so each build keeps its own, and the input's are never edits.
// Players are matched by person, not by their place in the team's list: the game can list a team's
// players in another order for another seed or opponent, or the first time it builds a club's
// match after the game starts, so edits to a player follow the player.
#pragma once

#include "buffer.h"
#include "fixture.h"
#include "platform.h"
#include "trace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// A player's records in a serialised team.
typedef struct SeedPlayer {
	size_t start; // records [start, end)
	size_t end;
	size_t uidRecord;  // the record of the player's unique ID, or SIZE_MAX
	uint32_t person;   // that unique ID, or UINT32_MAX
	size_t occurrence; // earlier players of the team with the same `person`
} SeedPlayer;

// One team's players in the input, in its order. Each is identified (`person`, `occurrence`) as
// the player of the game's build with the input's seed that it pairs with: an injected player as
// the one it replaced.
typedef struct SeedPlayers {
	SeedPlayer *list;
	size_t count; // 0: the team has none, or they pair by their place (seed_build.c)
	size_t moved; // how many pair with a player at another place in that build
} SeedPlayers;

typedef struct SeedBuilder {
	NextFixture next;        // the human manager's next fixture (the input's)
	const uint8_t *teams[2]; // home/away db::TEAM overrides, or NULL for the fixture's own
	Serialised input;        // the input, deserialised and serialised again (traced)
	bool *edited;            // per input record: differs from a build with the input's own seed
	size_t editCount;
	SeedPlayers players[2]; // home, away
	PlatformLock lock;      // builds share the game's fixture and the trace hooks
} SeedBuilder;

// Prepares to rebuild `input` (serialised match bytes built with `inputSeed`), with optional
// home/away team overrides. Fails, with a message in `error`, unless the input is the human
// manager's next fixture with the same layout as a fresh build, apart from the match's observers
// and the order of each team's players. Must run on the job thread (it loads matches through
// stream slot 0).
bool seedBuilder_open(
	SeedBuilder *builder,
	const ByteBuffer *input,
	uint32_t inputSeed,
	const uint8_t *const teams[2],
	char *error,
	size_t errorSize
);

// Forgets the input's edits to the selected team's block (0: home, 1: away).
bool seedBuilder_dropTeamEdits(SeedBuilder *builder, unsigned side, size_t *dropped);

// How many of the input's fields are re-applied to each build.
size_t seedBuilder_editCount(const SeedBuilder *builder);

// The match input for `seed`: the game's build with `seed` (with builder->teams, which may be
// changed between builds), with the input's edits re-applied. Thread-safe (builds one at a
// time). The build and the input are aligned part by part, and their players by person (see
// seed_build.c), so their structures may differ, e.g. another team; an edit in a part that cannot
// be aligned, or to a player the build leaves out, fails the build.
bool seedBuilder_build(SeedBuilder *builder, uint32_t seed, ByteBuffer *out);

void seedBuilder_close(SeedBuilder *builder);
