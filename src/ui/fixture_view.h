// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The fixture viewer's rules, as the Windows viewer (fixture_window.c) applies them, for the macOS
// viewer: what the summary, headings, squad lists, pitch and venue show, how player and venue
// edits apply and are written, and how the simulation's input and progress are read. Swift imports
// this header (see module.modulemap), so it uses only standard headers and an opaque handle, and
// never exposes the large fixture structs. Nothing here touches files or the game.
//
// Text is UTF-8. A function that fills `out` returns the text's full length, as snprintf does,
// and writes at most `size` bytes including the NUL.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	FIXTURE_VIEW_COLUMNS = 16,      // squad list columns
	FIXTURE_VIEW_SLOTS = 11,        // tactic slots, numbered on the pitch
	FIXTURE_VIEW_BLOCKS = 256,      // player blocks per side
	FIXTURE_VIEW_VENUE_FIELDS = 11, // in fixture_data.h's VENUE_* order
	FIXTURE_VIEW_SURFACE = 9,       // the venue field chosen from fixtureView_surfaceName's list
	FIXTURE_VIEW_TEXT = 2048,       // room for any text below
};

// The squad edits, as flags (fixture_data.h's FIXTURE_MAX_*).
typedef enum FixtureViewEdit {
	FIXTURE_VIEW_MAX_HAPPINESS = 1,
	FIXTURE_VIEW_MAX_CONDITION = 2,
	FIXTURE_VIEW_MAX_FAMILIARITY = 4,
} FixtureViewEdit;

// A stored value; floats keep their serialised bits (fixture_data.h's FixtureNumber).
typedef struct FixtureViewNumber {
	uint64_t value;
	bool known;
} FixtureViewNumber;

// A fixture read from JSON, with its pending player edits. Use one from one thread at a time.
typedef struct FixtureView FixtureView;

FixtureView *fixtureView_open(const char *json, size_t length, char *error, size_t errorSize);
void fixtureView_close(FixtureView *view);

// The summary after its first line: 0 "<competition> @ <venue>, <date>", 1 the weather and 2 the
// referee; then a blank line and the two notes, whose first `labelLength` bytes are bold.
size_t fixtureView_summaryLine(const FixtureView *view, unsigned line, char *out, size_t size);
const char *fixtureView_note(unsigned index, size_t *labelLength);

// "Save: <name>, <game_date>" for a fixture that names the save it was exported from, as fm24ctl
// reads its "save" and "game_date" before a job (just "Save: <name>" without a date), or "" for one
// that names none, or none fm24ctl can use.
size_t fixtureView_saveLine(const FixtureView *view, char *out, size_t size);
// The view of a fixture saved as binary, which has no place for the save: it names none.
void fixtureView_forgetSave(FixtureView *view);

// "Home: <team> - <tactic> [<formation>]", or "Away: ..." for side 1.
size_t fixtureView_heading(const FixtureView *view, unsigned side, char *out, size_t size);
size_t fixtureView_teamName(const FixtureView *view, unsigned side, char *out, size_t size);
// The name of the side's player in `block` ("" if the side has no such block).
size_t fixtureView_playerName(
	const FixtureView *view,
	unsigned side,
	unsigned block,
	char *out,
	size_t size
);

// The squad lists: column titles and widths (points), rows and cell text.
const char *fixtureView_columnTitle(unsigned column);
unsigned fixtureView_columnWidth(unsigned column);
unsigned fixtureView_rowCount(const FixtureView *view, unsigned side);
// The player block a row shows when that player can be selected (and edited), or -1.
int fixtureView_rowBlock(const FixtureView *view, unsigned side, unsigned row);
size_t fixtureView_cell(
	const FixtureView *view,
	unsigned side,
	unsigned row,
	unsigned column,
	char *out,
	size_t size
);
// Where a slot's numbered circle goes on the pitch, in percent of its width and height from the
// top left; false if the slot's position is not known.
bool fixtureView_slotSpot(
	const FixtureView *view,
	unsigned side,
	unsigned slot,
	unsigned *xPercent,
	unsigned *yPercent
);

// Why `edit` cannot change the side's selected players (FIXTURE_VIEW_BLOCKS flags), or NULL.
const char *fixtureView_editProblem(
	const FixtureView *view,
	unsigned side,
	const bool *selected,
	FixtureViewEdit edit
);
// Applies `edit` to every selected player and records it. False, changing nothing, if
// fixtureView_editProblem gives a problem or no player is selected.
bool fixtureView_applyEdit(
	FixtureView *view,
	unsigned side,
	const bool *selected,
	FixtureViewEdit edit
);
// The pending edits as 2 * FIXTURE_VIEW_BLOCKS flags, home first.
void fixtureView_playerEdits(const FixtureView *view, uint8_t *flags);
bool fixtureView_hasPlayerEdits(const FixtureView *view);
void fixtureView_clearPlayerEdits(FixtureView *view);

// The venue fields: labels, stored values (FIXTURE_VIEW_VENUE_FIELDS) and their text ("" when not
// stored; the surface's name, if it is one of the choices).
const char *fixtureView_venueLabel(unsigned field);
void fixtureView_venue(const FixtureView *view, FixtureViewNumber *values);
// A field can be edited when it is stored; the pitch's width and length only together.
bool fixtureView_venueEditable(const FixtureView *view, unsigned field);
size_t fixtureView_venueText(unsigned field, FixtureViewNumber value, char *out, size_t size);
// The surface choices: 0 "Grass" (1) and 1 "Grass/Synthetic mix" (8). NULL for others.
const char *fixtureView_surfaceName(unsigned choice);
// The choice a stored surface is, or -1.
int fixtureView_surfaceChoice(FixtureViewNumber surface);
// A field's value from its text (or, for the surface, the chosen index); false with the viewer's
// message ("<label> Enter a valid ...") if it is not one.
bool fixtureView_parseVenue(
	unsigned field,
	const char *text,
	int surfaceChoice,
	uint64_t *value,
	char *error,
	size_t errorSize
);
// The checks across fields (fixtureData_validateVenue).
bool fixtureView_validateVenue(const FixtureViewNumber *values, char *error, size_t errorSize);
// Keeps venue values as the fixture's, after they have been written.
void fixtureView_setVenue(FixtureView *view, const FixtureViewNumber *values);
// True if any venue value is stored: then every save writes them, which recomputes the values
// derived from them.
bool fixtureView_hasVenue(const FixtureViewNumber *values);

// What a save writes for the fixture `json`: the player edits (`playerEdits` as
// fixtureView_playerEdits gives them, or NULL), then the venue (`venue`, or NULL; written only if
// fixtureView_hasVenue), as JSON or, if `binary`, encoded. `*out` is to be freed with
// fixtureView_free.
bool fixtureView_write(
	const char *json,
	size_t length,
	const uint8_t *playerEdits,
	const FixtureViewNumber *venue,
	bool binary,
	uint8_t **out,
	size_t *outLength,
	char *error,
	size_t errorSize
);
void fixtureView_free(void *bytes);
// Fixture JSON without its "save" and "game_date" keys, as a binary copy keeps neither, and
// otherwise unchanged; `*out` is to be freed with fixtureView_free. False, with `*out` NULL, if
// `json` is not a JSON object or memory runs out.
bool fixtureView_withoutSave(const char *json, size_t length, uint8_t **out, size_t *outLength);
// The teams that replacements left in fixture JSON (its home_team and away_team; 0 for a side not
// replaced), which binary fixtures do not keep. False, with both 0, if `json` is not a fixture.
bool fixtureView_teamOverrides(const char *json, size_t length, uint32_t teams[2]);

// "<home>-v-<away>-<time><extension>" (fixture_export.h); false if it is too long.
bool fixtureView_exportName(
	const FixtureView *view,
	uint64_t unixTime,
	const char *extension,
	char *out,
	size_t size
);

// The simulation form: the most threads (and the default), a typed thread count, and a seed
// pattern as the form accepts it ("" or "keep" plays the fixture's own seed).
unsigned long fixtureView_threadLimit(void);
bool fixtureView_threadCount(const char *text, unsigned long *threads);
bool fixtureView_seedPattern(const char *text);
// How many matches a pattern plays, by the harness's rules; false if the harness would refuse it.
bool fixtureView_seedCount(const char *pattern, size_t *count);

// A run's progress, from its results CSV as the harness writes it.
typedef struct FixtureViewProgress {
	uint64_t completed;
	uint64_t homeWins;
	uint64_t draws;
	bool header; // the header line has been read
	bool failed; // a line was not a results line
} FixtureViewProgress;

// Reads the complete lines at the start of `text`, returning how many bytes they take.
size_t fixtureView_readProgress(FixtureViewProgress *progress, const char *text, size_t length);
// "n = <completed>: home <wins> (<percent>%) / draw ... / away ...".
size_t fixtureView_progressText(const FixtureViewProgress *progress, char *out, size_t size);

// One line of a team list (`players` false: "id<TAB>name", from list-teams) or a player list
// ("id<TAB>forename<TAB>surname<TAB>common name<TAB>club", from list-players): the ID, the label
// the pickers show and the text a search matches. False if the line is not listed.
bool fixtureView_choice(
	const char *line,
	size_t length,
	bool players,
	uint32_t *id,
	char *label,
	size_t labelSize,
	char *matched,
	size_t matchedSize
);

// The harness protocol version this build speaks: HARNESS_PROTOCOL_TEXT (protocol.h).
const char *fixtureView_protocol(void);
