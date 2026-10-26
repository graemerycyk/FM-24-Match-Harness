// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_view.h"

#include "buffer.h"
#include "fixture_data.h"
#include "fixture_edit.h"
#include "fixture_export.h"
#include "json_reader.h"
#include "match_config.h"
#include "platform.h"
#include "protocol.h"
#include "saves.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// The squad lists' text columns: slot to fatigue, then each tactical familiarity category.
enum { FAMILIARITY_COLUMN = 9 };

// Seeds one range of a pattern may cover, and the longest list line read (as the harness and the
// Windows viewer have them).
enum { MAX_SEED_RANGE = 10000000, LIST_LINE_LENGTH = 1024, LIST_NAME_LENGTH = 256 };

_Static_assert(
	(int)FIXTURE_VIEW_COLUMNS == (int)FAMILIARITY_COLUMN + (int)FIXTURE_FAMILIARITY_CATEGORIES,
	""
);
_Static_assert((int)FIXTURE_VIEW_SLOTS == (int)FIXTURE_SLOTS, "");
_Static_assert((int)FIXTURE_VIEW_BLOCKS == (int)FIXTURE_PLAYERS, "");
_Static_assert((int)FIXTURE_VIEW_VENUE_FIELDS == (int)VENUE_FIELD_COUNT, "");
_Static_assert((int)FIXTURE_VIEW_SURFACE == (int)VENUE_SURFACE, "");
_Static_assert((int)FIXTURE_VIEW_MAX_HAPPINESS == FIXTURE_MAX_HAPPINESS, "");
_Static_assert((int)FIXTURE_VIEW_MAX_CONDITION == FIXTURE_MAX_CONDITION, "");
_Static_assert((int)FIXTURE_VIEW_MAX_FAMILIARITY == FIXTURE_MAX_FAMILIARITY, "");
_Static_assert(sizeof(FixturePlayerEdits) == (size_t)2 * FIXTURE_VIEW_BLOCKS, "");

struct FixtureView {
	FixtureData data;
	FixturePlayerEdits edits;
	char save[SAVES_NAME_LENGTH]; // the save fm24ctl checks before a job, "" for none
	char gameDate[CONFIG_DATE_LENGTH];
};

static const char *const columns[FIXTURE_VIEW_COLUMNS] = {
	"Slot",
	"Player",
	"Position",
	"Role",
	"Duty",
	"Morale",
	"Condition",
	"Sharpness",
	"Fatigue",
	"Mentality",
	"Passing Style",
	"Creative Freedom",
	"Trigger Press",
	"Marking",
	"Tempo",
	"Width",
};
static const unsigned columnWidths[FIXTURE_VIEW_COLUMNS] =
	{38, 210, 65, 185, 75, 65, 85, 90, 70, 85, 100, 125, 105, 85, 85, 85};
static const char *const venueLabels[VENUE_FIELD_COUNT] = {
	"Pitch width (m):",
	"Pitch length (m):",
	"City longitude:",
	"City latitude:",
	"Capacity:",
	"Seat Capacity:",
	"Home fans:",
	"Away fans:",
	"Season ticket holders:",
	"Surface:",
	"Pitch condition (0-255):",
};
static const char *const surfaces[] = {"Grass", "Grass/Synthetic mix"};
static const uint64_t surfaceValues[] = {1, 8};

// snprintf's result as a length ("" and 0 if it failed).
static size_t written(int length, char *out, size_t size) {
	if (length < 0) {
		if (size) {
			out[0] = 0;
		}
		return 0;
	}
	return (size_t)length;
}

static const FixtureTeam *sideTeam(const FixtureView *view, unsigned side) {
	return side < 2 ? &view->data.teams[side] : NULL;
}

static bool selectable(const FixturePlayer *player) {
	return player->present && player->name[0] && strcmp(player->name, "<Unknown>") != 0;
}

static unsigned playerCount(const FixtureTeam *team) {
	uint64_t count = team->playerCount.value;
	return count < FIXTURE_PLAYERS ? (unsigned)count : FIXTURE_PLAYERS;
}

// The player a row lists: with an unknown line-up, the tactic's slots come first, with no player.
static const FixturePlayer *rowPlayer(const FixtureTeam *team, unsigned row) {
	unsigned first = team->lineupKnown ? 0 : FIXTURE_SLOTS;
	if (row < first || row - first >= playerCount(team)) {
		return NULL;
	}
	return &team->players[row - first];
}

// The fixture's save as fm24ctl's commands read it (commands.c's readFixtureSave): the name of a
// "save" that can be one, without control characters, and its "game_date".
static void readSave(const char *json, size_t length, FixtureView *view) {
	ConfigOrigin origin;
	(void)matchConfig_readOrigin(json, length, &origin);
	bool named = origin.hasSave && saves_nameOfPath(origin.save, view->save, sizeof view->save) &&
							 saves_isName(view->save);
	for (const char *c = view->save; named && *c; ++c) {
		named = (unsigned char)*c >= 0x20 && *c != 0x7F;
	}
	if (!named) {
		view->save[0] = 0;
	}
	(void)snprintf(view->gameDate, sizeof view->gameDate, "%s", named ? origin.gameDate : "");
}

static void toNumbers(const FixtureViewNumber *values, FixtureNumber out[VENUE_FIELD_COUNT]) {
	for (unsigned i = 0; i < VENUE_FIELD_COUNT; ++i) {
		out[i] = (FixtureNumber){.value = values[i].value, .known = values[i].known};
	}
}

FixtureView *fixtureView_open(const char *json, size_t length, char *error, size_t errorSize) {
	FixtureView *view = calloc(1, sizeof *view);
	if (!view) {
		(void)snprintf(error, errorSize, "out of memory");
		return NULL;
	}
	if (!fixtureData_read(json, length, &view->data, error, errorSize)) {
		free(view);
		return NULL;
	}
	readSave(json, length, view);
	return view;
}

void fixtureView_close(FixtureView *view) {
	free(view);
}

size_t fixtureView_summaryLine(const FixtureView *view, unsigned line, char *out, size_t size) {
	const FixtureData *data = &view->data;
	if (line == 0) {
		const char *competition = strcmp(data->matchType, "Friendly") == 0 ? "Friendly"
															: data->competition[0]                   ? data->competition
																																			 : "Unknown competition";
		return written(
			snprintf(
				out,
				size,
				"%s @ %s, %s",
				competition,
				data->venue[0] ? data->venue : "Unknown",
				data->when[0] ? data->when : "Unknown (no export date annotation)"
			),
			out,
			size
		);
	}
	if (line == 1) {
		return written(
			snprintf(out, size, "Weather: Unknown (serialized weather fields have not been mapped)"),
			out,
			size
		);
	}
	if (line == 2) {
		const char *referee = data->officials[4][0] ? data->officials[4] : "Not stored/unassigned";
		return written(snprintf(out, size, "Referee: %s", referee), out, size);
	}
	return written(snprintf(out, size, "%s", ""), out, size);
}

size_t fixtureView_saveLine(const FixtureView *view, char *out, size_t size) {
	if (!view->save[0]) {
		return written(snprintf(out, size, "%s", ""), out, size);
	}
	return written(
		snprintf(out, size, "Save: %s%s%s", view->save, view->gameDate[0] ? ", " : "", view->gameDate),
		out,
		size
	);
}

void fixtureView_forgetSave(FixtureView *view) {
	view->save[0] = 0;
	view->gameDate[0] = 0;
}

const char *fixtureView_note(unsigned index, size_t *labelLength) {
	static const char *const notes[] = {
		"NOTE: Condition/sharpness are percentages. Fatigue is signed jadedness (0 = fresh), not a "
		"percentage.",
		"NOTE 2: AI/unknown control: assignments unmapped. Squad listed below; AI may replace this "
		"tactic.",
	};
	if (labelLength) {
		*labelLength = index == 0 ? 5 : index == 1 ? 7 : 0;
	}
	return index < 2 ? notes[index] : NULL;
}

size_t fixtureView_heading(const FixtureView *view, unsigned side, char *out, size_t size) {
	const FixtureTeam *team = sideTeam(view, side);
	if (!team) {
		return written(-1, out, size);
	}
	return written(
		snprintf(
			out,
			size,
			"%s: %s - %s [%s]",
			side ? "Away" : "Home",
			team->name,
			team->tactic[0] ? team->tactic : "Unnamed tactic",
			team->formation[0] ? team->formation : "Formation unknown"
		),
		out,
		size
	);
}

size_t fixtureView_teamName(const FixtureView *view, unsigned side, char *out, size_t size) {
	const FixtureTeam *team = sideTeam(view, side);
	return written(team ? snprintf(out, size, "%s", team->name) : -1, out, size);
}

size_t fixtureView_playerName(
	const FixtureView *view,
	unsigned side,
	unsigned block,
	char *out,
	size_t size
) {
	const FixtureTeam *team = sideTeam(view, side);
	bool listed = team && block < playerCount(team);
	return written(listed ? snprintf(out, size, "%s", team->players[block].name) : -1, out, size);
}

const char *fixtureView_columnTitle(unsigned column) {
	return column < FIXTURE_VIEW_COLUMNS ? columns[column] : "";
}

unsigned fixtureView_columnWidth(unsigned column) {
	return column < FIXTURE_VIEW_COLUMNS ? columnWidths[column] : 0;
}

unsigned fixtureView_rowCount(const FixtureView *view, unsigned side) {
	const FixtureTeam *team = sideTeam(view, side);
	return team ? playerCount(team) + (team->lineupKnown ? 0 : FIXTURE_SLOTS) : 0;
}

int fixtureView_rowBlock(const FixtureView *view, unsigned side, unsigned row) {
	const FixtureTeam *team = sideTeam(view, side);
	const FixturePlayer *player = team ? rowPlayer(team, row) : NULL;
	return player && selectable(player) ? (int)(player - team->players) : -1;
}

static size_t percentage(FixtureNumber number, char *out, size_t size) {
	if (!number.known) {
		return written(snprintf(out, size, "-"), out, size);
	}
	if (number.value > 10000) {
		return written(
			snprintf(out, size, "Invalid: %llu", (unsigned long long)number.value), out, size
		);
	}
	return written(snprintf(out, size, "%.2f%%", (double)number.value / 100.0), out, size);
}

static size_t sharpness(const FixturePlayer *player, char *out, size_t size) {
	const FixtureNumber *values = player->sharpness;
	if (!values[0].known || !values[1].known || values[0].value == values[1].value) {
		return percentage(values[0], out, size);
	}
	if (values[0].value > 10000 || values[1].value > 10000) {
		return written(snprintf(out, size, "Invalid sharpness"), out, size);
	}
	return written(
		snprintf(
			out, size, "%.2f / %.2f%%", (double)values[0].value / 100.0, (double)values[1].value / 100.0
		),
		out,
		size
	);
}

// A role's name in title case, with spaces for underscores ("pressing_forward" ->
// "Pressing Forward").
static size_t roleTitle(const char *name, char *out, size_t size) {
	size_t length = strlen(name);
	bool capital = true;
	for (size_t i = 0; i < length && i + 1 < size; ++i) {
		char c = name[i];
		out[i] = (char)(c == '_' ? ' ' : capital && c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
		capital = c == '_';
	}
	if (size) {
		out[length < size ? length : size - 1] = 0;
	}
	return length;
}

// The slot columns (slot, position, role and duty) of a row.
static size_t
slotCell(const FixtureTeam *team, unsigned row, unsigned column, char *out, size_t size) {
	if (row >= FIXTURE_SLOTS) {
		const char *text = column == 2 && team->lineupKnown ? "Sub" : "";
		return written(snprintf(out, size, "%s", text), out, size);
	}
	const FixtureSlot *slot = &team->slots[row];
	if (column == 0) {
		return written(snprintf(out, size, "%u", row), out, size);
	}
	if (column == 2 && slot->position.known) {
		char position[32];
		fixtureData_position(slot->position.value, position, sizeof position);
		return written(snprintf(out, size, "%s", position), out, size);
	}
	if (column == 3 && slot->role.known) {
		return roleTitle(fixtureData_role(slot->role.value), out, size);
	}
	if (column == 4 && slot->role.known) {
		return written(snprintf(out, size, "%s", fixtureData_duty(slot->role.value)), out, size);
	}
	return written(snprintf(out, size, "-"), out, size);
}

// The player columns (morale to the familiarity categories) of a row's player.
static size_t playerCell(
	const FixtureTeam *team,
	const FixturePlayer *player,
	unsigned column,
	char *out,
	size_t size
) {
	if (column == 5 && player->morale.known) {
		return written(
			snprintf(out, size, "%llu", (unsigned long long)player->morale.value), out, size
		);
	}
	if (column == 6) {
		return percentage(player->condition, out, size);
	}
	if (column == 7) {
		return sharpness(player, out, size);
	}
	if (column == 8 && player->fatigue.known) {
		int value = (int)player->fatigue.value;
		return written(snprintf(out, size, "%d", value >= 32768 ? value - 65536 : value), out, size);
	}
	unsigned familiarity = 0;
	if (column >= FAMILIARITY_COLUMN &&
			fixtureData_familiarity(team, player, column - FAMILIARITY_COLUMN, &familiarity)) {
		return percentage((FixtureNumber){.value = familiarity, .known = true}, out, size);
	}
	return written(snprintf(out, size, "-"), out, size);
}

size_t fixtureView_cell(
	const FixtureView *view,
	unsigned side,
	unsigned row,
	unsigned column,
	char *out,
	size_t size
) {
	const FixtureTeam *team = sideTeam(view, side);
	if (!team || row >= fixtureView_rowCount(view, side) || column >= FIXTURE_VIEW_COLUMNS) {
		return written(snprintf(out, size, "%s", ""), out, size);
	}
	const FixturePlayer *player = rowPlayer(team, row);
	if (column == 1) {
		const char *name = player && player->name[0] ? player->name : "<Unknown>";
		return written(snprintf(out, size, "%s", name), out, size);
	}
	if (column < 5) {
		return slotCell(team, row, column, out, size);
	}
	return player ? playerCell(team, player, column, out, size)
								: written(snprintf(out, size, "-"), out, size);
}

// Where a position mask goes on the pitch, in percent (the Windows viewer's layout).
static void pitchPoint(uint64_t mask, unsigned *x, unsigned *y) {
	uint64_t position = mask & 0xFFFF;
	*x = 50;
	*y = position == 1      ? 91
			 : position <= 16   ? 75
			 : position <= 128  ? 59
			 : position <= 1024 ? 45
			 : position <= 8192 ? 30
													: 12;
	if (position == 4 || position == 32 || position == 256 || position == 2048) {
		*x = 85;
	} else if (position == 8 || position == 64 || position == 512 || position == 4096) {
		*x = 15;
	} else if (mask & (UINT64_C(1) << 20)) {
		*x = 35;
	} else if (mask & (UINT64_C(1) << 21)) {
		*x = 65;
	}
}

bool fixtureView_slotSpot(
	const FixtureView *view,
	unsigned side,
	unsigned slot,
	unsigned *xPercent,
	unsigned *yPercent
) {
	const FixtureTeam *team = sideTeam(view, side);
	if (!team || slot >= FIXTURE_SLOTS || !team->slots[slot].position.known) {
		return false;
	}
	char position[32];
	fixtureData_position(team->slots[slot].position.value, position, sizeof position);
	if (strcmp(position, "Unknown") == 0) {
		return false;
	}
	pitchPoint(team->slots[slot].position.value, xPercent, yPercent);
	return true;
}

// Whether every familiarity category of the player, and the tactic's settings, were read.
static bool familiarityKnown(const FixtureTeam *team, const FixturePlayer *player) {
	int settings[FIXTURE_FAMILIARITY_CATEGORIES];
	bool known = fixtureData_familiaritySettings(team, settings);
	for (unsigned k = 0; known && k < FIXTURE_FAMILIARITY_CATEGORIES; ++k) {
		known = player->familiarity[k].known;
	}
	return known;
}

// Why an edit cannot change the player, or NULL if it can.
static const char *
playerProblem(const FixtureTeam *team, const FixturePlayer *player, FixtureViewEdit edit) {
	static const char wellbeing[] =
		"The selected player is missing wellbeing data; no players were changed.";
	if (!selectable(player)) {
		return wellbeing;
	}
	switch (edit) {
		case FIXTURE_VIEW_MAX_FAMILIARITY:
			return familiarityKnown(team, player)
							 ? NULL
							 : "The selected player or tactic is missing tactical familiarity data (export the "
								 "fixture again); no players were changed.";
		case FIXTURE_VIEW_MAX_HAPPINESS: return player->morale.known ? NULL : wellbeing;
		case FIXTURE_VIEW_MAX_CONDITION:
			return player->condition.known && player->sharpness[0].known && player->sharpness[1].known &&
								 player->fatigue.known
							 ? NULL
							 : wellbeing;
		default: return "Unknown player edit.";
	}
}

const char *fixtureView_editProblem(
	const FixtureView *view,
	unsigned side,
	const bool *selected,
	FixtureViewEdit edit
) {
	const FixtureTeam *team = sideTeam(view, side);
	for (unsigned i = 0; team && i < FIXTURE_PLAYERS; ++i) {
		const char *problem = selected[i] ? playerProblem(team, &team->players[i], edit) : NULL;
		if (problem) {
			return problem;
		}
	}
	return NULL;
}

// What FIXTURE_MAX_FAMILIARITY writes: each category's first entry becomes the tactic's setting at
// 10000. A category with no entries keeps its floor.
static void maxFamiliarity(const FixtureTeam *team, FixturePlayer *player) {
	int settings[FIXTURE_FAMILIARITY_CATEGORIES];
	if (!fixtureData_familiaritySettings(team, settings)) {
		return;
	}
	for (unsigned k = 0; k < FIXTURE_FAMILIARITY_CATEGORIES; ++k) {
		FixtureFamiliarity *familiarity = &player->familiarity[k];
		if (familiarity->count) {
			familiarity->setting[0] = (int8_t)settings[k];
			familiarity->amount[0] = 10000;
		}
	}
}

bool fixtureView_applyEdit(
	FixtureView *view,
	unsigned side,
	const bool *selected,
	FixtureViewEdit edit
) {
	bool any = false;
	for (unsigned i = 0; side < 2 && i < FIXTURE_PLAYERS; ++i) {
		any = any || selected[i];
	}
	if (!any || fixtureView_editProblem(view, side, selected, edit)) {
		return false;
	}
	FixtureTeam *team = &view->data.teams[side];
	for (unsigned i = 0; i < FIXTURE_PLAYERS; ++i) {
		if (!selected[i]) {
			continue;
		}
		FixturePlayer *player = &team->players[i];
		if (edit == FIXTURE_VIEW_MAX_FAMILIARITY) {
			maxFamiliarity(team, player);
		} else if (edit == FIXTURE_VIEW_MAX_HAPPINESS) {
			player->morale.value = 20;
		} else {
			player->condition.value = 10000;
			player->sharpness[0].value = 10000;
			player->sharpness[1].value = 10000;
			player->fatigue.value = 0;
		}
		view->edits.players[side][i] |= (uint8_t)edit;
	}
	return true;
}

void fixtureView_playerEdits(const FixtureView *view, uint8_t *flags) {
	memcpy(flags, view->edits.players, sizeof view->edits.players);
}

bool fixtureView_hasPlayerEdits(const FixtureView *view) {
	return fixtureData_hasPlayerEdits(&view->edits);
}

void fixtureView_clearPlayerEdits(FixtureView *view) {
	memset(&view->edits, 0, sizeof view->edits);
}

const char *fixtureView_venueLabel(unsigned field) {
	return field < VENUE_FIELD_COUNT ? venueLabels[field] : "";
}

void fixtureView_venue(const FixtureView *view, FixtureViewNumber *values) {
	for (unsigned i = 0; i < VENUE_FIELD_COUNT; ++i) {
		FixtureNumber number = view->data.venueValues[i];
		values[i] = (FixtureViewNumber){.value = number.value, .known = number.known};
	}
}

bool fixtureView_venueEditable(const FixtureView *view, unsigned field) {
	const FixtureNumber *values = view->data.venueValues;
	bool pitchKnown = values[VENUE_WIDTH].known && values[VENUE_LENGTH].known;
	return field < VENUE_FIELD_COUNT && values[field].known && (field > VENUE_LENGTH || pitchKnown);
}

size_t fixtureView_venueText(unsigned field, FixtureViewNumber value, char *out, size_t size) {
	if (!value.known || field >= VENUE_FIELD_COUNT) {
		return written(snprintf(out, size, "%s", ""), out, size);
	}
	if (field <= VENUE_LATITUDE) {
		double number = (double)fixtureData_toFloat((uint32_t)value.value);
		return written(snprintf(out, size, "%.9g", number), out, size);
	}
	if (field == VENUE_SURFACE) {
		int choice = fixtureView_surfaceChoice(value);
		const char *name = choice >= 0 ? surfaces[choice] : "";
		return written(snprintf(out, size, "%s", name), out, size);
	}
	return written(snprintf(out, size, "%llu", (unsigned long long)value.value), out, size);
}

const char *fixtureView_surfaceName(unsigned choice) {
	return choice < 2 ? surfaces[choice] : NULL;
}

int fixtureView_surfaceChoice(FixtureViewNumber surface) {
	for (int i = 0; surface.known && i < 2; ++i) {
		if (surface.value == surfaceValues[i]) {
			return i;
		}
	}
	return -1;
}

// A typed venue value, as the Windows viewer reads its fields (fewer than 64 characters).
static bool readVenueText(unsigned field, const char *text, uint64_t *value) {
	size_t length = strlen(text);
	bool decimal = field <= VENUE_LATITUDE;
	if (!length || length >= 64 ||
			strspn(text, decimal ? "+-0123456789.eE" : "0123456789") != length) {
		return false;
	}
	char *end = NULL;
	errno = 0;
	if (decimal) {
		*value = fixtureData_fromFloat(strtof(text, &end));
	} else {
		*value = strtoull(text, &end, 10);
	}
	return errno == 0 && !*end;
}

bool fixtureView_parseVenue(
	unsigned field,
	const char *text,
	int surfaceChoice,
	uint64_t *value,
	char *error,
	size_t errorSize
) {
	if (field >= VENUE_FIELD_COUNT) {
		(void)snprintf(error, errorSize, "Unknown venue field.");
		return false;
	}
	bool ok;
	if (field == VENUE_SURFACE) {
		ok = surfaceChoice == 0 || surfaceChoice == 1;
		*value = surfaceValues[ok ? surfaceChoice : 1];
	} else {
		ok = readVenueText(field, text, value);
	}
	if (!ok) {
		(void)snprintf(
			error,
			errorSize,
			"%s Enter a valid %s.",
			venueLabels[field],
			field == VENUE_SURFACE    ? "surface selection"
			: field <= VENUE_LATITUDE ? "number"
																: "whole number"
		);
	}
	return ok;
}

bool fixtureView_validateVenue(const FixtureViewNumber *values, char *error, size_t errorSize) {
	FixtureNumber numbers[VENUE_FIELD_COUNT];
	toNumbers(values, numbers);
	return fixtureData_validateVenue(numbers, error, errorSize);
}

void fixtureView_setVenue(FixtureView *view, const FixtureViewNumber *values) {
	toNumbers(values, view->data.venueValues);
}

bool fixtureView_hasVenue(const FixtureViewNumber *values) {
	for (unsigned i = 0; i < VENUE_FIELD_COUNT; ++i) {
		if (values[i].known) {
			return true;
		}
	}
	return false;
}

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
) {
	*out = NULL;
	*outLength = 0;
	FixturePlayerEdits edits = {0};
	if (playerEdits) {
		memcpy(edits.players, playerEdits, sizeof edits.players);
	}
	FixtureNumber values[VENUE_FIELD_COUNT];
	bool withVenue = venue && fixtureView_hasVenue(venue);
	if (withVenue) {
		toNumbers(venue, values);
	}
	ByteBuffer bytes = {0};
	if (!fixtureEdit_apply(
				json, length, &edits, withVenue ? values : NULL, binary, &bytes, error, errorSize
			)) {
		buffer_free(&bytes);
		return false;
	}
	*out = bytes.data;
	*outLength = bytes.length;
	return true;
}

void fixtureView_free(void *bytes) {
	free(bytes);
}

static bool isSaveKey(const ByteBuffer *key) {
	return (key->length == 4 && memcmp(key->data, "save", 4) == 0) ||
				 (key->length == 9 && memcmp(key->data, "game_date", 9) == 0);
}

// Copies the text of `json` around the members it drops: each with the comma before it, or, while
// no member has been kept, from its key to the next member's.
bool fixtureView_withoutSave(const char *json, size_t length, uint8_t **out, size_t *outLength) {
	*out = NULL;
	*outLength = 0;
	JsonReader reader;
	ByteBuffer key = {0};
	ByteBuffer kept = {0};
	jsonReader_init(&reader, json, length);
	const char *copied = json; // the text before it is in `kept`, or dropped
	bool ok = jsonReader_consume(&reader, '{');
	if (ok && !jsonReader_consume(&reader, '}')) {
		const char *lastEnd = NULL; // the end of the last member's value
		bool keptOne = false;
		bool toNextKey = false; // a leading member was dropped: so is what precedes the next key
		do {
			// The key's opening quote, which the string reader then reads from.
			ok = jsonReader_consume(&reader, '"');
			const char *start = ok ? --reader.position : reader.position;
			if (ok && toNextKey) {
				copied = start;
				toNextKey = false;
			}
			ok = ok && jsonReader_readString(&reader, &key) && jsonReader_consume(&reader, ':') &&
					 jsonReader_skipValue(&reader);
			if (ok && isSaveKey(&key)) {
				const char *from = keptOne ? lastEnd : start;
				ok = buffer_append(&kept, copied, (size_t)(from - copied));
				copied = reader.position;
				toNextKey = !keptOne;
			} else {
				keptOne = true;
			}
			lastEnd = reader.position;
		} while (ok && jsonReader_consume(&reader, ','));
		ok = ok && jsonReader_consume(&reader, '}');
	}
	ok = ok && buffer_append(&kept, copied, (size_t)(json + length - copied));
	buffer_free(&key);
	if (!ok) {
		buffer_free(&kept);
		return false;
	}
	*out = kept.data;
	*outLength = kept.length;
	return true;
}

bool fixtureView_teamOverrides(const char *json, size_t length, uint32_t teams[2]) {
	ByteBuffer bytes = {0};
	char error[256];
	bool encoded = matchConfig_encode(json, length, &bytes, teams, error, sizeof error);
	buffer_free(&bytes);
	return encoded;
}

bool fixtureView_exportName(
	const FixtureView *view,
	uint64_t unixTime,
	const char *extension,
	char *out,
	size_t size
) {
	return fixtureExport_filenameUtf8(&view->data, unixTime, extension, out, size);
}

unsigned long fixtureView_threadLimit(void) {
	unsigned long processors = platform_processorCount();
	unsigned long threads = processors > 1 ? processors - 1 : 1;
	return threads < HARNESS_MAX_THREADS ? threads : HARNESS_MAX_THREADS;
}

bool fixtureView_threadCount(const char *text, unsigned long *threads) {
	size_t length = strlen(text);
	if (!length || length >= 16 || strspn(text, "0123456789") != length) {
		return false;
	}
	char *end = NULL;
	errno = 0;
	unsigned long value = strtoul(text, &end, 10);
	if (errno || *end || value < 1 || value > fixtureView_threadLimit()) {
		return false;
	}
	*threads = value;
	return true;
}

bool fixtureView_seedPattern(const char *text) {
	size_t length = strlen(text);
	return length < 256 && (strcmp(text, "keep") == 0 || strspn(text, "0123456789,-") == length);
}

// A decimal number at `text` (job.c's parseNumber); `end` is left after it.
static bool seedNumber(const char *text, const char **end, unsigned long long *value) {
	if (*text < '0' || *text > '9') {
		return false;
	}
	char *after = NULL;
	errno = 0;
	*value = strtoull(text, &after, 10);
	*end = after;
	return errno == 0 && after != text;
}

bool fixtureView_seedCount(const char *pattern, size_t *count) {
	*count = 1;
	if (!pattern[0] || strcmp(pattern, "keep") == 0) {
		return true;
	}
	size_t total = 0;
	for (const char *part = pattern;;) {
		const char *end = NULL;
		unsigned long long first = 0;
		unsigned long long last = 0;
		if (!seedNumber(part, &end, &first)) {
			return false;
		}
		last = first;
		if (*end == '-' && !seedNumber(end + 1, &end, &last)) {
			return false;
		}
		if ((*end && *end != ',') || last < first || last - first > MAX_SEED_RANGE ||
				last > UINT32_MAX) {
			return false;
		}
		total += (size_t)(last - first + 1);
		if (!*end) {
			break;
		}
		part = end + 1;
		if (!*part) {
			return false;
		}
	}
	*count = total;
	return true;
}

// The first three comma-separated numbers of a results row (commands.c's parseCsvNumbers).
static bool resultNumbers(const char *line, unsigned long values[3]) {
	const char *position = line;
	for (size_t i = 0; i < 3; ++i) {
		if (*position < '0' || *position > '9') {
			return false;
		}
		char *end = NULL;
		errno = 0;
		values[i] = strtoul(position, &end, 10);
		if (errno != 0 || values[i] > UINT32_MAX || (i < 2 ? *end != ',' : *end && *end != ',')) {
			return false;
		}
		position = *end == ',' ? end + 1 : end;
	}
	return true;
}

static void readProgressLine(FixtureViewProgress *progress, const char *text, size_t length) {
	char line[256];
	if (length && text[length - 1] == '\r') {
		--length;
	}
	if (length >= sizeof line) {
		progress->failed = true;
		return;
	}
	memcpy(line, text, length);
	line[length] = 0;
	if (!progress->header) {
		progress->header = strncmp(line, "seed,home_goals,away_goals,", 27) == 0;
		progress->failed = progress->failed || !progress->header;
		return;
	}
	unsigned long values[3];
	if (strlen(line) != length || !resultNumbers(line, values)) {
		progress->failed = true;
		return;
	}
	++progress->completed;
	if (values[1] > values[2]) {
		++progress->homeWins;
	} else if (values[1] == values[2]) {
		++progress->draws;
	}
}

size_t fixtureView_readProgress(FixtureViewProgress *progress, const char *text, size_t length) {
	size_t used = 0;
	while (used < length) {
		const char *line = text + used;
		const char *newline = memchr(line, '\n', length - used);
		if (!newline) {
			break;
		}
		readProgressLine(progress, line, (size_t)(newline - line));
		used += (size_t)(newline - line) + 1;
	}
	return used;
}

size_t fixtureView_progressText(const FixtureViewProgress *progress, char *out, size_t size) {
	uint64_t completed = progress->completed;
	uint64_t home = progress->homeWins;
	uint64_t draws = progress->draws;
	uint64_t away = home + draws <= completed ? completed - home - draws : 0;
	double percent = completed ? 100.0 / (double)completed : 0;
	return written(
		snprintf(
			out,
			size,
			"n = %llu: home %llu (%.1f%%) / draw %llu (%.1f%%) / away %llu (%.1f%%)",
			(unsigned long long)completed,
			(unsigned long long)home,
			(double)home * percent,
			(unsigned long long)draws,
			(double)draws * percent,
			(unsigned long long)away,
			(double)away * percent
		),
		out,
		size
	);
}

// The UTF-8 character at `at`: its code point and length; false if it is not valid UTF-8.
static bool utf8Character(const unsigned char *at, uint32_t *code, unsigned *length) {
	static const uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
	unsigned lead = *at;
	unsigned extra = lead < 0x80             ? 0
									 : (lead & 0xE0) == 0xC0 ? 1
									 : (lead & 0xF0) == 0xE0 ? 2
									 : (lead & 0xF8) == 0xF0 ? 3
																					 : 4;
	if (extra > 3) {
		return false;
	}
	*code = extra ? lead & (0x3FU >> extra) : lead;
	for (unsigned i = 1; i <= extra; ++i) {
		if ((at[i] & 0xC0) != 0x80) {
			return false;
		}
		*code = *code << 6 | (at[i] & 0x3FU);
	}
	*length = extra + 1;
	return *code >= minimum[extra] && *code <= 0x10FFFF && (*code < 0xD800 || *code > 0xDFFF);
}

// The UTF-16 units `text` takes (as the Windows viewer holds it), or SIZE_MAX if it is not
// valid UTF-8.
static size_t utf16Units(const char *text) {
	const unsigned char *at = (const unsigned char *)text;
	size_t units = 0;
	while (*at) {
		uint32_t code = 0;
		unsigned length = 0;
		if (!utf8Character(at, &code, &length)) {
			return SIZE_MAX;
		}
		units += code >= 0x10000 ? 2 : 1;
		at += length;
	}
	return units;
}

static bool listedName(const char *text) {
	return utf16Units(text) < LIST_NAME_LENGTH;
}

// "Common name (Forename Surname) – Club (id)", leaving out what the player lacks; their names are
// what is matched. `fields` are the forename, surname, common name and club.
static bool playerChoice(
	uint32_t id,
	char *const fields[4],
	char *label,
	size_t labelSize,
	char *matched,
	size_t matchedSize
) {
	for (size_t i = 0; i < 4; ++i) {
		if (!listedName(fields[i])) {
			return false;
		}
	}
	char full[2 * LIST_NAME_LENGTH * 4];
	(void)snprintf(
		full, sizeof full, "%s%s%s", fields[0], fields[0][0] && fields[1][0] ? " " : "", fields[1]
	);
	const char *common = fields[2];
	if (!full[0] && !common[0]) {
		return false;
	}
	int labelLength = snprintf(
		label,
		labelSize,
		"%s%s%s%s%s%s (%lu)",
		common[0] ? common : full,
		common[0] && full[0] ? " (" : "",
		common[0] ? full : "",
		common[0] && full[0] ? ")" : "",
		fields[3][0] ? " \xE2\x80\x93 " : "",
		fields[3],
		(unsigned long)id
	);
	int matchedLength = snprintf(matched, matchedSize, "%s %s", full, common);
	return labelLength >= 0 && (size_t)labelLength < labelSize && matchedLength >= 0 &&
				 (size_t)matchedLength < matchedSize;
}

bool fixtureView_choice(
	const char *line,
	size_t length,
	bool players,
	uint32_t *id,
	char *label,
	size_t labelSize,
	char *matched,
	size_t matchedSize
) {
	uint64_t value = 0;
	size_t digits = 0;
	for (; digits < length && line[digits] >= '0' && line[digits] <= '9'; ++digits) {
		value = value * 10 + (uint64_t)(line[digits] - '0');
		if (value > UINT32_MAX) {
			return false;
		}
	}
	size_t restLength = digits < length ? length - digits - 1 : 0;
	if (!digits || digits >= length || line[digits] != '\t' || restLength >= LIST_LINE_LENGTH) {
		return false;
	}
	char rest[LIST_LINE_LENGTH];
	memcpy(rest, line + digits + 1, restLength);
	rest[restLength] = 0;
	rest[strcspn(rest, "\r")] = 0;
	*id = (uint32_t)value;
	if (!players) {
		if (strlen(rest) >= LIST_NAME_LENGTH || !listedName(rest)) {
			return false;
		}
		int labelLength = snprintf(label, labelSize, "%s (%lu)", rest, (unsigned long)value);
		int matchedLength = snprintf(matched, matchedSize, "%s (%lu)", rest, (unsigned long)value);
		return labelLength >= 0 && (size_t)labelLength < labelSize && matchedLength >= 0 &&
					 (size_t)matchedLength < matchedSize;
	}
	char *fields[4] = {rest, NULL, NULL, NULL};
	size_t count = 1;
	for (char *c = rest; *c && count < 4; ++c) {
		if (*c == '\t') {
			*c = 0;
			fields[count++] = c + 1;
		}
	}
	if (count < 4 || strchr(fields[3], '\t')) {
		return false;
	}
	return playerChoice(*id, fields, label, labelSize, matched, matchedSize);
}

const char *fixtureView_protocol(void) {
	return HARNESS_PROTOCOL_TEXT;
}
