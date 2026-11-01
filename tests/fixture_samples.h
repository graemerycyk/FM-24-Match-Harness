// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Synthetic fixture configs for the tests: the C suites and the macOS viewer's Swift tests, which
// import this header (tests/viewer/module.modulemap), so it uses only standard headers.
#pragma once

#include <stdbool.h>
#include <stddef.h>


struct ByteBuffer;
struct FixturePlayerEdits;

typedef enum FixtureSample {
	FIXTURE_SAMPLE_LABELLED,    // Aston Villa (human) v Bristol City, labelled wellbeing, dated
	FIXTURE_SAMPLE_UNLABELLED,  // the same, with wellbeing found by position only
	FIXTURE_SAMPLE_SWAPPED,     // the away side human: the home line-up is not known
	FIXTURE_SAMPLE_VENUE,       // every venue field and the values derived from them
	FIXTURE_SAMPLE_FAMILIARITY, // home players 1 and 2 with tactical familiarity
	FIXTURE_SAMPLE_LINEUP,      // Liverpool (human) v Newcastle (AI), laid out as an export
	FIXTURE_SAMPLE_COUNT,
} FixtureSample;

// The save and in-game date of fixtureSamples_savedJson's copies.
#define FIXTURE_SAMPLE_SAVE "LIV_MAIN_FM24"
#define FIXTURE_SAMPLE_GAME_DATE "2023 day 180 09:00"

// A sample as malloc'd, NUL-terminated JSON (`length` excludes the NUL), or NULL if out of memory.
char *fixtureSamples_json(int sample, size_t *length);
// The same with "save" (FIXTURE_SAMPLE_SAVE) and "game_date" in its header, as an export of the
// macOS harness records them (docs/FIXTURE.md).
char *fixtureSamples_savedJson(int sample, size_t *length);

// Appends printf-formatted text (up to 2047 bytes); false if it does not fit or memory runs out.
bool fixtureSamples_append(struct ByteBuffer *out, const char *format, ...);
// Aston Villa v Bristol City, 12 players a side with 11 slots each: the home side is human (the
// away side if `swapControl`), wellbeing fields are named if `labelled`, and `edits` (or NULL)
// are already applied, as fixtureData_writePlayers writes them. `source` (or NULL) is the source
// annotation.
bool fixtureSamples_config(
	struct ByteBuffer *out,
	bool labelled,
	bool swapControl,
	const char *source,
	const struct FixturePlayerEdits *edits
);
// Every venue field, with the pitch geometry and attendance copies derived from them, and two
// small squads; the annotations a venue edit must keep.
bool fixtureSamples_venue(struct ByteBuffer *out);
// The unlabelled config, whose home plan gives familiarity settings {16, 5, 10, 20, 10, 15, 5}
// and whose home players 1 and 2 carry two real players' familiarity entries.
bool fixtureSamples_familiarity(struct ByteBuffer *out);
// The value (0-10000) the game's tactic familiarity panel showed for the familiarity sample's
// player (0 or 1: home players 1 and 2) in a category.
unsigned fixtureSamples_familiarityShown(unsigned player, unsigned category);
// Liverpool v Newcastle as an export lays it out, one field a line, for line-up changes. Home
// (flags 0x20308) has 22 players: an XI, a bench of 9 (formation[11..19]) with a goalkeeper at
// place 11, and 2 reserves; Curtis Jones (17) and Lewis Jones (14) share a surname, and Luis Díaz
// (18) is the accented name. Away (flags 0x34000, the AI's) has 13, with a bench of 2. Each block
// starts with the serialiser's 15 header fields, has named wellbeing values and holds its unique ID
// twice, named and not, with 0-3 more fields by place. Away's slot 9 marks home place 19 (Florian
// Wirtz); every other marking is 255. Both sides have a uniform part_4240 and a has_team_extra
// after their blocks.
bool fixtureSamples_lineup(struct ByteBuffer *out);
// The unique ID of the lineup sample's player at `place` on `side`, or 0.
unsigned fixtureSamples_lineupId(unsigned side, unsigned place);
