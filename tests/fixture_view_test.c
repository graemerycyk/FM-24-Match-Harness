// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/dll/job.h"
#include "buffer.h"
#include "fixture_data.h"
#include "fixture_edit.h"
#include "fixture_lineup.h"
#include "fixture_samples.h"
#include "fixture_view.h"
#include "match_config.h"
#include "protocol.h"
#include "test.h"
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#endif


// Checks a view text and the length its function returned.
static void checkText(size_t length, const char *text, const char *expected) {
	if (strcmp(text, expected) != 0) {
		fprintf(stderr, "expected \"%s\", got \"%s\"\n", expected, text);
	}
	ASSERT_INT_EQ(strcmp(text, expected), 0);
	ASSERT_BOOL_EQ(length == strlen(expected), true);
}

static void checkCell(
	const FixtureView *view,
	unsigned side,
	unsigned row,
	unsigned column,
	const char *expected
) {
	char text[FIXTURE_VIEW_TEXT];
	checkText(fixtureView_cell(view, side, row, column, text, sizeof text), text, expected);
}

// Checks one row's cells; `cells` has FIXTURE_VIEW_COLUMNS entries.
static void checkRow(
	const FixtureView *view,
	unsigned side,
	unsigned row,
	int block,
	const char *const *cells
) {
	ASSERT_INT_EQ(fixtureView_rowBlock(view, side, row), block);
	for (unsigned column = 0; column < FIXTURE_VIEW_COLUMNS; ++column) {
		checkCell(view, side, row, column, cells[column]);
	}
}

static FixtureView *openJson(const ByteBuffer *json) {
	char error[256] = "";
	FixtureView *view = fixtureView_open(buffer_string(json), json->length, error, sizeof error);
	if (!view) {
		fprintf(stderr, "%s\n", error);
	}
	ASSERT_BOOL_EQ(view != NULL, true);
	return view;
}

static bool sampleJson(int sample, ByteBuffer *out) {
	size_t length = 0;
	char *json = fixtureSamples_json(sample, &length);
	bool ok = json && buffer_append(out, json, length);
	free(json);
	ASSERT_BOOL_EQ(ok, true);
	return ok;
}

static FixtureView *openSample(int sample) {
	ByteBuffer json = {0};
	FixtureView *view = sampleJson(sample, &json) ? openJson(&json) : NULL;
	buffer_free(&json);
	return view;
}

// Replaces the first `from` in `json` with `to`.
static void replaceText(ByteBuffer *json, const char *from, const char *to) {
	const char *found = strstr(buffer_string(json), from);
	ASSERT_BOOL_EQ(found != NULL, true);
	if (!found) {
		return;
	}
	size_t at = (size_t)(found - (const char *)json->data);
	ByteBuffer out = {0};
	bool ok = buffer_append(&out, json->data, at) && buffer_append(&out, to, strlen(to)) &&
						buffer_append(&out, found + strlen(from), json->length - at - strlen(from));
	ASSERT_BOOL_EQ(ok, true);
	buffer_free(json);
	*json = out;
}

TEST(opens_and_summarises_fixtures) {
	char error[256] = "";
	static const char *const invalid[] = {
		"", "{}", "{\"format\":\"fm24-match-config/1\",\"fields\":[]}"};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		error[0] = 0;
		ASSERT_BOOL_EQ(
			fixtureView_open(invalid[i], strlen(invalid[i]), error, sizeof error) == NULL, true
		);
		ASSERT_BOOL_EQ(error[0] != 0, true);
	}
	// Only `length` bytes are read: the text need not end in a NUL, and what follows is ignored.
	ByteBuffer json = {0};
	if (!sampleJson(FIXTURE_SAMPLE_LABELLED, &json)) {
		return;
	}
	size_t length = json.length;
	ASSERT_BOOL_EQ(buffer_append(&json, "garbage", 7), true);
	FixtureView *view = fixtureView_open((const char *)json.data, length, error, sizeof error);
	ASSERT_BOOL_EQ(view != NULL, true);
	buffer_free(&json);
	if (!view) {
		return;
	}
	char text[FIXTURE_VIEW_TEXT];
	checkText(
		fixtureView_summaryLine(view, 0, text, sizeof text),
		text,
		"Friendly @ Villa Park, 2023-07-05 19:30"
	);
	checkText(
		fixtureView_summaryLine(view, 1, text, sizeof text),
		text,
		"Weather: Unknown (serialized weather fields have not been mapped)"
	);
	checkText(fixtureView_summaryLine(view, 2, text, sizeof text), text, "Referee: Aimee Keir");
	checkText(fixtureView_summaryLine(view, 3, text, sizeof text), text, "");
	checkText(
		fixtureView_heading(view, 0, text, sizeof text),
		text,
		"Home: Aston Villa - Custom 4-2-2-2 [4-2-2-2]"
	);
	checkText(
		fixtureView_heading(view, 1, text, sizeof text),
		text,
		"Away: Bristol City - Custom 4-2-2-2 [4-2-2-2]"
	);
	checkText(fixtureView_heading(view, 2, text, sizeof text), text, "");
	checkText(fixtureView_teamName(view, 1, text, sizeof text), text, "Bristol City");
	// Text that does not fit is cut short, and the full length is returned.
	ASSERT_BOOL_EQ(fixtureView_heading(view, 0, NULL, 0) == 44, true);
	ASSERT_BOOL_EQ(fixtureView_heading(view, 0, text, 5) == 44, true);
	ASSERT_INT_EQ(strcmp(text, "Home"), 0);
	size_t label = 0;
	ASSERT_INT_EQ(
		strcmp(
			fixtureView_note(0, &label),
			"NOTE: Condition/sharpness are percentages. Fatigue is signed jadedness (0 = fresh), not a "
			"percentage."
		),
		0
	);
	ASSERT_BOOL_EQ(label == 5, true);
	ASSERT_INT_EQ(
		strcmp(
			fixtureView_note(1, &label),
			"NOTE 2: AI/unknown control: assignments unmapped. Squad listed below; AI may replace this "
			"tactic."
		),
		0
	);
	ASSERT_BOOL_EQ(label == 7, true);
	ASSERT_BOOL_EQ(fixtureView_note(2, &label) == NULL && label == 0, true);
	ASSERT_INT_EQ(strcmp(fixtureView_protocol(), HARNESS_PROTOCOL_TEXT), 0);
	fixtureView_close(view);

	// What is not stored.
	view = openSample(FIXTURE_SAMPLE_VENUE);
	if (view) {
		checkText(
			fixtureView_summaryLine(view, 0, text, sizeof text),
			text,
			"Unknown competition @ Unknown, Unknown (no export date annotation)"
		);
		checkText(
			fixtureView_summaryLine(view, 2, text, sizeof text), text, "Referee: Not stored/unassigned"
		);
		checkText(
			fixtureView_heading(view, 0, text, sizeof text),
			text,
			"Home: Test team - Unnamed tactic [Formation unknown]"
		);
		fixtureView_close(view);
	}
}

TEST(lists_squads_as_windows) {
	static const char *const titles[FIXTURE_VIEW_COLUMNS] = {
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
	static const unsigned widths[FIXTURE_VIEW_COLUMNS] = {
		38, 210, 65, 185, 75, 65, 85, 90, 70, 85, 100, 125, 105, 85, 85, 85};
	for (unsigned column = 0; column < FIXTURE_VIEW_COLUMNS; ++column) {
		ASSERT_INT_EQ(strcmp(fixtureView_columnTitle(column), titles[column]), 0);
		ASSERT_BOOL_EQ(fixtureView_columnWidth(column) == widths[column], true);
	}
	ASSERT_BOOL_EQ(fixtureView_columnWidth(FIXTURE_VIEW_COLUMNS) == 0, true);

	FixtureView *view = openSample(FIXTURE_SAMPLE_LABELLED);
	if (!view) {
		return;
	}
	// The human side's known line-up: its slots, then the substitutes.
	ASSERT_BOOL_EQ(fixtureView_rowCount(view, 0) == 12, true);
	static const char *const keeper[] = {"0",
		"Emiliano Mart\xC3\xADnez",
		"GK",
		"Sweeper Keeper",
		"Defend",
		"12",
		"98.76%",
		"81.25 / 56.50%",
		"-1",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-"};
	checkRow(view, 0, 0, 0, keeper);
	static const char *const forward[] = {"6",
		"Injected player",
		"STR",
		"Pressing Forward",
		"Support",
		"12",
		"98.76%",
		"81.25 / 56.50%",
		"-1",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-"};
	checkRow(view, 0, 6, 6, forward);
	static const char *const substitute[] = {"",
		"Injected player",
		"Sub",
		"",
		"",
		"12",
		"98.76%",
		"81.25 / 56.50%",
		"-1",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-"};
	checkRow(view, 0, 11, 11, substitute);
	// The AI side's unknown line-up: eleven unassigned slots, then every player.
	ASSERT_BOOL_EQ(fixtureView_rowCount(view, 1) == 23, true);
	static const char *const slot[] = {"4",
		"<Unknown>",
		"DR",
		"Full Back",
		"Attack",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-"};
	checkRow(view, 1, 4, -1, slot);
	static const char *const listed[] = {"",
		"Emiliano Mart\xC3\xADnez",
		"",
		"",
		"",
		"12",
		"98.76%",
		"81.25 / 56.50%",
		"-1",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-",
		"-"};
	checkRow(view, 1, 11, 0, listed);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 22), 11);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 23), -1);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 2, 0), -1);
	ASSERT_BOOL_EQ(fixtureView_rowCount(view, 2) == 0, true);
	checkCell(view, 1, 23, 1, "");
	checkCell(view, 0, 0, FIXTURE_VIEW_COLUMNS, "");
	fixtureView_close(view);

	// Unlabelled wellbeing values, and familiarity against the tactic.
	view = openSample(FIXTURE_SAMPLE_FAMILIARITY);
	if (view) {
		static const char *const familiar[] = {"2",
			"Injected player",
			"DMCL",
			"Defensive Midfielder",
			"Support",
			"15",
			"100.00%",
			"56.00 / 56.50%",
			"-140",
			"90.00%",
			"80.00%",
			"80.00%",
			"50.00%",
			"15.00%",
			"50.00%",
			"80.00%"};
		checkRow(view, 0, 2, 2, familiar);
		fixtureView_close(view);
	}
	// Players without names (or wellbeing) are listed but cannot be selected.
	view = openSample(FIXTURE_SAMPLE_VENUE);
	if (view) {
		ASSERT_BOOL_EQ(fixtureView_rowCount(view, 0) == 22, true);
		ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 11), -1);
		checkCell(view, 0, 0, 2, "-");
		checkCell(view, 0, 11, 1, "<Unknown>");
		checkCell(view, 0, 11, 6, "-");
		fixtureView_close(view);
	}
}

TEST(shows_invalid_wellbeing_values) {
	static const struct {
		const char *from;
		const char *to;
		unsigned column;
		const char *expected;
	} cases[] = {
		{"\"home.player[0].condition\",\"u16\":9876",
			"\"home.player[0].condition\",\"u16\":10001",
			6,
			"Invalid: 10001"},
		{"\"home.player[0].condition\",\"u16\":9876",
			"\"home.player[0].condition\",\"u16\":0",
			6,
			"0.00%"},
		{"\"home.player[0].sharpness[0]\",\"u16\":8125",
			"\"home.player[0].sharpness[0]\",\"u16\":10002",
			7,
			"Invalid sharpness"},
		{"\"home.player[0].sharpness[0]\",\"u16\":8125",
			"\"home.player[0].sharpness[0]\",\"u16\":5650",
			7,
			"56.50%"},
		{"\"home.player[0].fatigue\",\"u16\":65535",
			"\"home.player[0].fatigue\",\"u16\":32767",
			8,
			"32767"},
		{"\"home.player[0].fatigue\",\"u16\":65535",
			"\"home.player[0].fatigue\",\"u16\":32768",
			8,
			"-32768"},
		{"\"home.player[0].morale\",\"u8\":12", "\"home.player[0].morale\",\"u8\":0", 5, "0"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		ByteBuffer json = {0};
		if (!sampleJson(FIXTURE_SAMPLE_LABELLED, &json)) {
			return;
		}
		replaceText(&json, cases[i].from, cases[i].to);
		FixtureView *view = openJson(&json);
		if (view) {
			checkCell(view, 0, 0, cases[i].column, cases[i].expected);
			fixtureView_close(view);
		}
		buffer_free(&json);
	}
}

TEST(places_slots_on_the_pitch) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_LABELLED);
	if (!view) {
		return;
	}
	static const unsigned spots[FIXTURE_VIEW_SLOTS][2] = {
		{50, 91},
		{65, 75},
		{35, 59},
		{35, 75},
		{85, 75},
		{15, 30},
		{65, 12},
		{65, 59},
		{15, 75},
		{35, 12},
		{85, 30},
	};
	for (unsigned side = 0; side < 2; ++side) {
		for (unsigned slot = 0; slot < FIXTURE_VIEW_SLOTS; ++slot) {
			unsigned x = 0;
			unsigned y = 0;
			ASSERT_BOOL_EQ(fixtureView_slotSpot(view, side, slot, &x, &y), true);
			ASSERT_BOOL_EQ(x == spots[slot][0] && y == spots[slot][1], true);
		}
	}
	unsigned x = 0;
	unsigned y = 0;
	ASSERT_BOOL_EQ(fixtureView_slotSpot(view, 0, FIXTURE_VIEW_SLOTS, &x, &y), false);
	ASSERT_BOOL_EQ(fixtureView_slotSpot(view, 2, 0, &x, &y), false);
	fixtureView_close(view);
	// Unknown positions (none stored, or both sides of the pitch at once) are not drawn.
	view = openSample(FIXTURE_SAMPLE_VENUE);
	if (view) {
		ASSERT_BOOL_EQ(fixtureView_slotSpot(view, 0, 0, &x, &y), false);
		fixtureView_close(view);
	}
	ByteBuffer json = {0};
	if (sampleJson(FIXTURE_SAMPLE_LABELLED, &json)) {
		replaceText(
			&json,
			"\"flags\":[\"deliberately wrong\"],\"u32\":2097168",
			"\"flags\":[\"deliberately wrong\"],\"u32\":3145744"
		);
		view = openJson(&json);
		if (view) {
			ASSERT_BOOL_EQ(fixtureView_slotSpot(view, 0, 1, &x, &y), false);
			checkCell(view, 0, 1, 2, "Unknown");
			fixtureView_close(view);
		}
	}
	buffer_free(&json);
}

// Both sides' orders, as fixtureView_write takes them.
static void viewOrder(const FixtureView *view, uint8_t order[2 * FIXTURE_VIEW_BLOCKS]) {
	fixtureView_order(view, 0, order);
	fixtureView_order(view, 1, order + FIXTURE_VIEW_BLOCKS);
}

// The cells of every row after a save of the view's edits and order, read back, are those it shows.
static void checkWrittenEdits(const FixtureView *view, const ByteBuffer *json) {
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	uint8_t order[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	viewOrder(view, order);
	uint8_t *bytes = NULL;
	size_t length = 0;
	char error[256] = "";
	bool ok = fixtureView_write(
		buffer_string(json),
		json->length,
		flags,
		order,
		NULL,
		false,
		&bytes,
		&length,
		error,
		sizeof error
	);
	if (!ok) {
		fprintf(stderr, "%s\n", error);
	}
	ASSERT_BOOL_EQ(ok, true);
	FixtureView *saved =
		ok ? fixtureView_open((const char *)bytes, length, error, sizeof error) : NULL;
	ASSERT_BOOL_EQ(saved != NULL, true);
	for (unsigned side = 0; saved && side < 2; ++side) {
		ASSERT_BOOL_EQ(fixtureView_rowCount(saved, side) == fixtureView_rowCount(view, side), true);
		for (unsigned row = 0; row < fixtureView_rowCount(view, side); ++row) {
			for (unsigned column = 0; column < FIXTURE_VIEW_COLUMNS; ++column) {
				char shown[FIXTURE_VIEW_TEXT];
				char written[FIXTURE_VIEW_TEXT];
				fixtureView_cell(view, side, row, column, shown, sizeof shown);
				fixtureView_cell(saved, side, row, column, written, sizeof written);
				ASSERT_INT_EQ(strcmp(shown, written), 0);
			}
		}
	}
	fixtureView_close(saved);
	fixtureView_free(bytes);
}

TEST(edits_players_as_saves_write_them) {
	ByteBuffer json = {0};
	FixtureView *view = sampleJson(FIXTURE_SAMPLE_LABELLED, &json) ? openJson(&json) : NULL;
	if (!view) {
		buffer_free(&json);
		return;
	}
	bool selected[FIXTURE_VIEW_BLOCKS] = {false};
	ASSERT_BOOL_EQ(
		fixtureView_editProblem(view, 0, selected, FIXTURE_VIEW_MAX_HAPPINESS) == NULL, true
	);
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_HAPPINESS), false);
	selected[0] = true;
	selected[11] = true;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 2, selected, FIXTURE_VIEW_MAX_HAPPINESS), false);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), false);
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_HAPPINESS), true);
	checkCell(view, 0, 0, 5, "20");
	checkCell(view, 0, 11, 5, "20");
	checkCell(view, 0, 1, 5, "12");
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_CONDITION), true);
	checkCell(view, 0, 0, 6, "100.00%");
	checkCell(view, 0, 0, 7, "100.00%");
	checkCell(view, 0, 0, 8, "0");
	// The other side's list shows its own players: block 0 there is unchanged.
	selected[11] = false;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 1, selected, FIXTURE_VIEW_MAX_HAPPINESS), true);
	checkCell(view, 1, 11, 5, "20");
	checkCell(view, 1, 11, 6, "98.76%");
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	ASSERT_INT_EQ(flags[0], FIXTURE_VIEW_MAX_HAPPINESS | FIXTURE_VIEW_MAX_CONDITION);
	ASSERT_INT_EQ(flags[11], FIXTURE_VIEW_MAX_HAPPINESS | FIXTURE_VIEW_MAX_CONDITION);
	ASSERT_INT_EQ(flags[1], 0);
	ASSERT_INT_EQ(flags[FIXTURE_VIEW_BLOCKS], FIXTURE_VIEW_MAX_HAPPINESS);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), true);
	checkWrittenEdits(view, &json);
	// Without familiarity data nothing changes, and the reason is the Windows viewer's.
	const char *problem = fixtureView_editProblem(view, 0, selected, FIXTURE_VIEW_MAX_FAMILIARITY);
	ASSERT_INT_EQ(
		strcmp(
			problem ? problem : "",
			"The selected player or tactic is missing tactical familiarity data (export the fixture "
			"again); no players were changed."
		),
		0
	);
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_FAMILIARITY), false);
	fixtureView_playerEdits(view, flags);
	ASSERT_INT_EQ(flags[0], FIXTURE_VIEW_MAX_HAPPINESS | FIXTURE_VIEW_MAX_CONDITION);
	fixtureView_clearPlayerEdits(view);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), false);
	fixtureView_close(view);
	buffer_free(&json);

	// Maximum familiarity: each category's first entry.
	view = sampleJson(FIXTURE_SAMPLE_FAMILIARITY, &json) ? openJson(&json) : NULL;
	if (view) {
		memset(selected, 0, sizeof selected);
		selected[1] = true;
		ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_FAMILIARITY), true);
		for (unsigned column = 9; column < FIXTURE_VIEW_COLUMNS; ++column) {
			checkCell(view, 0, 1, column, "100.00%");
		}
		checkCell(view, 0, 2, 9, "90.00%");
		checkWrittenEdits(view, &json);
		fixtureView_close(view);
	}
	buffer_free(&json);

	// A player who cannot be selected cannot be edited either.
	view = openSample(FIXTURE_SAMPLE_VENUE);
	if (view) {
		memset(selected, 0, sizeof selected);
		selected[0] = true;
		problem = fixtureView_editProblem(view, 0, selected, FIXTURE_VIEW_MAX_CONDITION);
		ASSERT_INT_EQ(
			strcmp(
				problem ? problem : "",
				"The selected player is missing wellbeing data; no players were changed."
			),
			0
		);
		ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_CONDITION), false);
		fixtureView_close(view);
	}
}

// Reads typed venue values over the stored ones, as the Windows viewer's form does: each field,
// then the checks across fields. `texts` is NULL for a field that keeps its stored text.
static bool readVenueForm(
	const FixtureViewNumber stored[FIXTURE_VIEW_VENUE_FIELDS],
	const char *const texts[FIXTURE_VIEW_VENUE_FIELDS],
	int surfaceChoice,
	FixtureViewNumber values[FIXTURE_VIEW_VENUE_FIELDS],
	char *error,
	size_t errorSize
) {
	memcpy(values, stored, FIXTURE_VIEW_VENUE_FIELDS * sizeof *values);
	for (unsigned i = 0; i < FIXTURE_VIEW_VENUE_FIELDS; ++i) {
		char text[64];
		fixtureView_venueText(i, stored[i], text, sizeof text);
		if (values[i].known &&
				!fixtureView_parseVenue(
					i, texts[i] ? texts[i] : text, surfaceChoice, &values[i].value, error, errorSize
				)) {
			return false;
		}
	}
	return fixtureView_validateVenue(values, error, errorSize);
}

static bool sameVenue(const FixtureViewNumber *a, const FixtureViewNumber *b) {
	for (unsigned i = 0; i < FIXTURE_VIEW_VENUE_FIELDS; ++i) {
		if (a[i].known != b[i].known || a[i].value != b[i].value) {
			return false;
		}
	}
	return true;
}

TEST(reads_parses_and_validates_venue_values) {
	static const char *const labels[] = {
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
	static const char *const texts[] = {
		"67.5999985",
		"105.099998",
		"-0.944999993",
		"52.4860229",
		"42777",
		"42000",
		"30000",
		"2000",
		"30000",
		"Grass/Synthetic mix",
		"200",
	};
	FixtureView *view = openSample(FIXTURE_SAMPLE_VENUE);
	if (!view) {
		return;
	}
	FixtureViewNumber stored[FIXTURE_VIEW_VENUE_FIELDS];
	fixtureView_venue(view, stored);
	ASSERT_BOOL_EQ(fixtureView_hasVenue(stored), true);
	for (unsigned i = 0; i < FIXTURE_VIEW_VENUE_FIELDS; ++i) {
		char text[64];
		ASSERT_INT_EQ(strcmp(fixtureView_venueLabel(i), labels[i]), 0);
		checkText(fixtureView_venueText(i, stored[i], text, sizeof text), text, texts[i]);
		ASSERT_BOOL_EQ(fixtureView_venueEditable(view, i), true);
	}
	ASSERT_INT_EQ(fixtureView_surfaceChoice(stored[FIXTURE_VIEW_SURFACE]), 1);
	ASSERT_INT_EQ(strcmp(fixtureView_surfaceName(0), "Grass"), 0);
	ASSERT_INT_EQ(strcmp(fixtureView_surfaceName(1), "Grass/Synthetic mix"), 0);
	ASSERT_BOOL_EQ(fixtureView_surfaceName(2) == NULL, true);
	ASSERT_INT_EQ(fixtureView_surfaceChoice((FixtureViewNumber){.value = 2, .known = true}), -1);
	ASSERT_INT_EQ(fixtureView_surfaceChoice((FixtureViewNumber){.value = 1, .known = false}), -1);

	// The stored text reads back as the stored values.
	FixtureViewNumber values[FIXTURE_VIEW_VENUE_FIELDS];
	char error[256] = "";
	const char *const unchanged[FIXTURE_VIEW_VENUE_FIELDS] = {NULL};
	ASSERT_BOOL_EQ(readVenueForm(stored, unchanged, 1, values, error, sizeof error), true);
	ASSERT_BOOL_EQ(sameVenue(values, stored), true);

	// The Windows viewer's rejected entries, with its messages.
	static const struct {
		unsigned field;
		const char *text;
		const char *error;
	} invalid[] = {
		{0, "", "Pitch width (m): Enter a valid number."},
		{0, "nan", "Pitch width (m): Enter a valid number."},
		{0, "1e39", "Pitch width (m): Enter a valid number."},
		{1, "0", "Invalid venue: enter a finite positive pitch dimension."},
		{2, "-181", "Invalid venue.city_longitude: enter a finite coordinate within range."},
		{3, "91", "Invalid venue.city_latitude: enter a finite coordinate within range."},
		{4, "4294967296", "venue.capacity is out of range."},
		{6, "-1", "Home fans: Enter a valid whole number."},
		{6, "1.5", "Home fans: Enter a valid whole number."},
		{6, " 1", "Home fans: Enter a valid whole number."},
		{6, "41000", "Home fans + Away fans cannot exceed Capacity."},
		{5, "42778", "Seat Capacity cannot exceed Capacity."},
		{10, "256", "venue.pitch_condition is out of range."},
		{10,
			"1234567890123456789012345678901234567890123456789012345678901234",
			"Pitch condition (0-255): Enter a valid whole number."},
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		const char *typed[FIXTURE_VIEW_VENUE_FIELDS] = {NULL};
		typed[invalid[i].field] = invalid[i].text;
		error[0] = 0;
		ASSERT_BOOL_EQ(readVenueForm(stored, typed, 1, values, error, sizeof error), false);
		if (strcmp(error, invalid[i].error) != 0) {
			fprintf(stderr, "field %u \"%s\": %s\n", invalid[i].field, invalid[i].text, error);
		}
		ASSERT_INT_EQ(strcmp(error, invalid[i].error), 0);
	}
	ASSERT_BOOL_EQ(readVenueForm(stored, unchanged, -1, values, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "Surface: Enter a valid surface selection."), 0);

	const char *typed[FIXTURE_VIEW_VENUE_FIELDS] = {"72.5", NULL, "-1.89"};
	typed[6] = "40000";
	typed[10] = "255";
	ASSERT_BOOL_EQ(readVenueForm(stored, typed, 0, values, error, sizeof error), true);
	ASSERT_BOOL_EQ(values[0].value == fixtureData_fromFloat(72.5f), true);
	ASSERT_BOOL_EQ(values[2].value == fixtureData_fromFloat(-1.89f), true);
	ASSERT_BOOL_EQ(values[6].value == 40000 && values[10].value == 255, true);
	ASSERT_BOOL_EQ(values[FIXTURE_VIEW_SURFACE].value == 1, true);
	fixtureView_setVenue(view, values);
	fixtureView_venue(view, stored);
	ASSERT_BOOL_EQ(sameVenue(values, stored), true);
	fixtureView_close(view);

	// A fixture storing only the pitch length: it alone is written, and is not editable alone.
	view = openSample(FIXTURE_SAMPLE_LABELLED);
	if (view) {
		fixtureView_venue(view, stored);
		ASSERT_BOOL_EQ(fixtureView_hasVenue(stored), true);
		ASSERT_BOOL_EQ(stored[1].known && !stored[0].known, true);
		for (unsigned i = 0; i < FIXTURE_VIEW_VENUE_FIELDS; ++i) {
			ASSERT_BOOL_EQ(fixtureView_venueEditable(view, i), false);
		}
		fixtureView_close(view);
	}
	memset(stored, 0, sizeof stored);
	ASSERT_BOOL_EQ(fixtureView_hasVenue(stored), false);
}

// fixtureView_write's bytes against fixture_data.h, fixture_lineup.h and match_config.h's, step by
// step: the Max edits, the order, the venue, the encoding.
static void checkWrite(
	const ByteBuffer *json,
	const FixturePlayerEdits *edits,
	const FixtureLineup *lineup,
	const FixtureViewNumber *venue,
	bool binary
) {
	ByteBuffer expected = {0};
	ByteBuffer step = {0};
	char error[256] = "";
	bool ok = buffer_append(&expected, json->data, json->length);
	if (ok && fixtureData_hasPlayerEdits(edits)) {
		ok = fixtureData_writePlayers(
			buffer_string(&expected), expected.length, edits, &step, error, sizeof error
		);
		buffer_free(&expected);
		expected = step;
		step = (ByteBuffer){0};
	}
	if (ok && lineup) {
		FixtureLineupResult result;
		ok = fixtureLineup_apply(
			buffer_string(&expected), expected.length, lineup, &step, &result, error, sizeof error
		);
		buffer_free(&expected);
		expected = step;
		step = (ByteBuffer){0};
	}
	if (ok && venue && fixtureView_hasVenue(venue)) {
		FixtureNumber values[VENUE_FIELD_COUNT];
		for (unsigned i = 0; i < VENUE_FIELD_COUNT; ++i) {
			values[i] = (FixtureNumber){.value = venue[i].value, .known = venue[i].known};
		}
		ok = fixtureData_writeVenue(
			buffer_string(&expected), expected.length, values, &step, error, sizeof error
		);
		buffer_free(&expected);
		expected = step;
		step = (ByteBuffer){0};
	}
	if (ok && binary) {
		ok = matchConfig_encode(
			buffer_string(&expected), expected.length, &step, NULL, error, sizeof error
		);
		buffer_free(&expected);
		expected = step;
	}
	ASSERT_BOOL_EQ(ok, true);
	uint8_t *bytes = NULL;
	size_t length = 0;
	ASSERT_BOOL_EQ(
		fixtureView_write(
			buffer_string(json),
			json->length,
			edits ? &edits->players[0][0] : NULL,
			lineup ? &lineup->from[0][0] : NULL,
			venue,
			binary,
			&bytes,
			&length,
			error,
			sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(length == expected.length, true);
	if (bytes && length == expected.length) {
		ASSERT_INT_EQ(memcmp(bytes, expected.data, length), 0);
	}
	fixtureView_free(bytes);
	buffer_free(&expected);
}

TEST(writes_the_bytes_windows_saves) {
	ByteBuffer json = {0};
	if (!sampleJson(FIXTURE_SAMPLE_FAMILIARITY, &json)) {
		return;
	}
	FixturePlayerEdits edits = {0};
	edits.players[0][1] = FIXTURE_MAX_FAMILIARITY | FIXTURE_MAX_HAPPINESS;
	edits.players[1][5] = FIXTURE_MAX_CONDITION;
	for (int binary = 0; binary < 2; ++binary) {
		checkWrite(&json, NULL, NULL, NULL, binary != 0);
		checkWrite(&json, &edits, NULL, NULL, binary != 0);
	}
	// Nothing to apply: the JSON as it is.
	uint8_t *bytes = NULL;
	size_t length = 0;
	char error[256] = "";
	ASSERT_BOOL_EQ(
		fixtureView_write(
			buffer_string(&json),
			json.length,
			NULL,
			NULL,
			NULL,
			false,
			&bytes,
			&length,
			error,
			sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(length == json.length && bytes && memcmp(bytes, json.data, length) == 0, true);
	fixtureView_free(bytes);
	// An edit the fixture cannot take: no bytes and the reason.
	edits.players[0][12] = FIXTURE_MAX_HAPPINESS;
	ASSERT_BOOL_EQ(
		fixtureView_write(
			buffer_string(&json),
			json.length,
			&edits.players[0][0],
			NULL,
			NULL,
			true,
			&bytes,
			&length,
			error,
			sizeof error
		),
		false
	);
	ASSERT_BOOL_EQ(bytes == NULL && length == 0, true);
	ASSERT_INT_EQ(strcmp(error, "Cannot edit home.player[12]."), 0);
	buffer_free(&json);

	// The venue is written whenever any value is stored, recomputing what derives from it.
	if (!sampleJson(FIXTURE_SAMPLE_VENUE, &json)) {
		return;
	}
	FixtureView *view = openJson(&json);
	if (view) {
		FixtureViewNumber venue[FIXTURE_VIEW_VENUE_FIELDS];
		fixtureView_venue(view, venue);
		for (int binary = 0; binary < 2; ++binary) {
			checkWrite(&json, NULL, NULL, venue, binary != 0);
		}
		ASSERT_BOOL_EQ(
			fixtureView_write(
				buffer_string(&json),
				json.length,
				NULL,
				NULL,
				venue,
				false,
				&bytes,
				&length,
				error,
				sizeof error
			),
			true
		);
		ASSERT_BOOL_EQ(length != json.length || memcmp(bytes, json.data, length) != 0, true);
		fixtureView_free(bytes);
		venue[FIXTURE_VIEW_SURFACE].value = 2;
		ASSERT_BOOL_EQ(
			fixtureView_write(
				buffer_string(&json),
				json.length,
				NULL,
				NULL,
				venue,
				false,
				&bytes,
				&length,
				error,
				sizeof error
			),
			false
		);
		ASSERT_INT_EQ(strcmp(error, "Select Grass or Grass/Synthetic mix."), 0);
		fixtureView_close(view);
	}
	// fixtureEdit_apply directly: NULL edits and venue copy the JSON, or encode it.
	ByteBuffer out = {0};
	ASSERT_BOOL_EQ(
		fixtureEdit_apply(
			buffer_string(&json), json.length, NULL, NULL, NULL, false, &out, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(out.length == json.length && memcmp(out.data, json.data, json.length) == 0, true);
	ASSERT_BOOL_EQ(
		fixtureEdit_apply("{", 1, NULL, NULL, NULL, true, &out, error, sizeof error), false
	);
	ASSERT_BOOL_EQ(out.length == 0 && error[0] != 0, true);
	buffer_free(&out);
	buffer_free(&json);
}

// Writes `json` as a save does, into `out`.
static bool writeFixture(
	const char *json,
	size_t length,
	const uint8_t *players,
	const FixtureViewNumber *venue,
	bool binary,
	ByteBuffer *out
) {
	uint8_t *bytes = NULL;
	size_t written = 0;
	char error[256] = "";
	bool ok = fixtureView_write(
		json, length, players, NULL, venue, binary, &bytes, &written, error, sizeof error
	);
	if (!ok) {
		fprintf(stderr, "%s\n", error);
	}
	ok = ok && buffer_append(out, bytes, written);
	fixtureView_free(bytes);
	return ok;
}

TEST(keeps_the_save_and_date_of_exports) {
	// What fm24ctl's save check reads (matchConfig_readOrigin): without it, run and the
	// replacements would play whichever save is loaded.
	static const char keys[] =
		"\"save\":\"" FIXTURE_SAMPLE_SAVE "\",\"game_date\":\"" FIXTURE_SAMPLE_GAME_DATE "\",";
	static const int samples[] = {FIXTURE_SAMPLE_FAMILIARITY, FIXTURE_SAMPLE_VENUE};
	FixturePlayerEdits edits = {0};
	edits.players[0][1] = FIXTURE_MAX_FAMILIARITY | FIXTURE_MAX_HAPPINESS;
	edits.players[1][5] = FIXTURE_MAX_CONDITION;
	for (size_t i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
		size_t savedLength = 0;
		size_t plainLength = 0;
		char *saved = fixtureSamples_savedJson(samples[i], &savedLength);
		char *plain = fixtureSamples_json(samples[i], &plainLength);
		char error[256] = "";
		FixtureView *view = plain ? fixtureView_open(plain, plainLength, error, sizeof error) : NULL;
		ASSERT_BOOL_EQ(saved && view && savedLength == plainLength + sizeof keys - 1, true);
		ConfigOrigin origin;
		ASSERT_BOOL_EQ(saved && matchConfig_readOrigin(saved, savedLength, &origin), true);
		ASSERT_BOOL_EQ(origin.hasSave && strcmp(origin.save, FIXTURE_SAMPLE_SAVE) == 0, true);
		// The familiarity sample with player edits, the venue sample with its venue written.
		FixtureViewNumber venue[FIXTURE_VIEW_VENUE_FIELDS];
		bool venueSample = samples[i] == FIXTURE_SAMPLE_VENUE;
		if (view && venueSample) {
			fixtureView_venue(view, venue);
		}
		const uint8_t *players = venueSample ? NULL : &edits.players[0][0];
		for (int binary = 0; saved && view && binary < 2; ++binary) {
			ByteBuffer fromSaved = {0};
			ByteBuffer fromPlain = {0};
			ASSERT_BOOL_EQ(
				writeFixture(saved, savedLength, players, venueSample ? venue : NULL, binary, &fromSaved) &&
					writeFixture(plain, plainLength, players, venueSample ? venue : NULL, binary, &fromPlain),
				true
			);
			if (binary) {
				// Binary fixtures have no place for them: the same bytes as without.
				ASSERT_BOOL_EQ(
					fromSaved.length == fromPlain.length &&
						memcmp(fromSaved.data, fromPlain.data, fromSaved.length) == 0,
					true
				);
			} else {
				// The edited JSON keeps both keys, where they were, and changes what the edits change.
				ASSERT_BOOL_EQ(
					fromPlain.length != plainLength || memcmp(fromPlain.data, plain, plainLength) != 0, true
				);
				ASSERT_BOOL_EQ(
					matchConfig_readOrigin(buffer_string(&fromSaved), fromSaved.length, &origin), true
				);
				ASSERT_BOOL_EQ(origin.hasSave && strcmp(origin.save, FIXTURE_SAMPLE_SAVE) == 0, true);
				ASSERT_INT_EQ(strcmp(origin.gameDate, FIXTURE_SAMPLE_GAME_DATE), 0);
				const char *at = strstr(buffer_string(&fromSaved), keys);
				size_t before = at ? (size_t)(at - buffer_string(&fromSaved)) : 0;
				ASSERT_BOOL_EQ(
					at && fromSaved.length == fromPlain.length + sizeof keys - 1 &&
						memcmp(fromSaved.data, fromPlain.data, before) == 0 &&
						memcmp(at + sizeof keys - 1, fromPlain.data + before, fromPlain.length - before) == 0,
					true
				);
			}
			buffer_free(&fromSaved);
			buffer_free(&fromPlain);
		}
		fixtureView_close(view);
		free(saved);
		free(plain);
	}
}

TEST(names_the_save_of_exports) {
	// The save fm24ctl checks before a job, as its commands read it, with the in-game date.
	static const struct {
		const char *keys;
		const char *line;
	} cases[] = {
		{"\"save\":\"" FIXTURE_SAMPLE_SAVE "\",\"game_date\":\"" FIXTURE_SAMPLE_GAME_DATE "\",",
			"Save: " FIXTURE_SAMPLE_SAVE ", " FIXTURE_SAMPLE_GAME_DATE},
		{"\"save\":\"Career Two\",", "Save: Career Two"},
		{"\"game_date\":\"2024 day 1 09:00\",\"save\":\"/Saves/My Save.fm\",",
			"Save: My Save, 2024 day 1 09:00"},
		{"\"save\":\"Copy.fm.fm\",\"game_date\":\"2024 day 1 09:00\",", ""},
		{"\"save\":\"Saves/\",", ""},
		{"\"save\":\"Odd\\u007f\",", ""},
		{"\"save\":\"\",\"game_date\":\"2024 day 1 09:00\",", ""},
		{"\"save\":42,", ""},
		{"\"game_date\":\"2024 day 1 09:00\",", ""},
		{"", ""},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		ByteBuffer json = {0};
		ASSERT_BOOL_EQ(sampleJson(FIXTURE_SAMPLE_LABELLED, &json), true);
		char keys[256];
		(void)snprintf(keys, sizeof keys, "{\"format\":\"fm24-match-config/1\",%s", cases[i].keys);
		replaceText(&json, "{\"format\":\"fm24-match-config/1\",", keys);
		FixtureView *view = openJson(&json);
		ASSERT_BOOL_EQ(view != NULL, true);
		if (view) {
			char line[FIXTURE_VIEW_TEXT];
			checkText(fixtureView_saveLine(view, line, sizeof line), line, cases[i].line);
			// Cut to fit, with the full length.
			char small[8];
			size_t length = fixtureView_saveLine(view, small, sizeof small);
			ASSERT_BOOL_EQ(
				length == strlen(cases[i].line) && strlen(small) == (length < 8 ? length : 7), true
			);
			fixtureView_close(view);
		}
		buffer_free(&json);
	}
}

TEST(drops_the_save_from_binary_copies) {
	// A binary file has no place for the save, so the JSON kept with a binary copy loses both keys,
	// wherever they are, and nothing else.
	static const struct {
		const char *json;
		const char *expected;
	} cases[] = {
		{"{\"a\":1,\"save\":\"S\",\"game_date\":\"D\",\"b\":2}", "{\"a\":1,\"b\":2}"},
		{"{\n  \"a\": 1,\n  \"save\": \"S\",\n  \"b\": [1, 2]\n}",
			"{\n  \"a\": 1,\n  \"b\": [1, 2]\n}"},
		{"{\"save\":\"S\",\"a\":{\"save\":1}}", "{\"a\":{\"save\":1}}"},
		{"{ \"game_date\" : \"D\" , \"save\":\"S\", \"a\":1 }", "{ \"a\":1 }"},
		{"{\"a\":1,\"game_date\":\"D\"}", "{\"a\":1}"},
		{"{\"save\":\"S\",\"game_date\":\"D\"} ", "{} "},
		{"{\"a\":\"save\",\"saves\":1,\"b\":[\"game_date\"]}",
			"{\"a\":\"save\",\"saves\":1,\"b\":[\"game_date\"]}"},
		{"{}", "{}"},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		uint8_t *out = NULL;
		size_t length = 0;
		ASSERT_BOOL_EQ(
			fixtureView_withoutSave(cases[i].json, strlen(cases[i].json), &out, &length), true
		);
		ASSERT_BOOL_EQ(
			out && length == strlen(cases[i].expected) && memcmp(out, cases[i].expected, length) == 0,
			true
		);
		fixtureView_free(out);
	}
	static const char *const invalid[] = {"[]", "{\"save\":}", "{\"a\":1,}", "", "{\"save\":\"S\""};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		uint8_t *out = (uint8_t *)invalid;
		size_t length = 1;
		ASSERT_BOOL_EQ(fixtureView_withoutSave(invalid[i], strlen(invalid[i]), &out, &length), false);
		ASSERT_BOOL_EQ(out == NULL && length == 0, true);
	}
	// An export: the fixture as it was before the macOS harness recorded its save, and a view of
	// it that names none.
	size_t savedLength = 0;
	size_t plainLength = 0;
	char *saved = fixtureSamples_savedJson(FIXTURE_SAMPLE_LABELLED, &savedLength);
	char *plain = fixtureSamples_json(FIXTURE_SAMPLE_LABELLED, &plainLength);
	uint8_t *out = NULL;
	size_t length = 0;
	ASSERT_BOOL_EQ(
		saved && plain && fixtureView_withoutSave(saved, savedLength, &out, &length), true
	);
	ASSERT_BOOL_EQ(out && length == plainLength && memcmp(out, plain, length) == 0, true);
	fixtureView_free(out);
	char error[256] = "";
	FixtureView *view = saved ? fixtureView_open(saved, savedLength, error, sizeof error) : NULL;
	ASSERT_BOOL_EQ(view != NULL, true);
	if (view) {
		char line[FIXTURE_VIEW_TEXT];
		checkText(
			fixtureView_saveLine(view, line, sizeof line),
			line,
			"Save: " FIXTURE_SAMPLE_SAVE ", " FIXTURE_SAMPLE_GAME_DATE
		);
		fixtureView_forgetSave(view);
		checkText(fixtureView_saveLine(view, line, sizeof line), line, "");
		fixtureView_close(view);
	}
	free(saved);
	free(plain);
}

TEST(names_exports_in_utf8) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_LABELLED);
	if (!view) {
		return;
	}
	char name[256];
	ASSERT_BOOL_EQ(fixtureView_exportName(view, 1792074287, ".json", name, sizeof name), true);
	ASSERT_INT_EQ(strcmp(name, "aston-villa-v-bristol-city-1792074287.json"), 0);
	ASSERT_BOOL_EQ(fixtureView_exportName(view, 42, ".csv", name, sizeof name), true);
	ASSERT_INT_EQ(strcmp(name, "aston-villa-v-bristol-city-42.csv"), 0);
	ASSERT_BOOL_EQ(fixtureView_exportName(view, 42, ".json", name, 20), false);
	ASSERT_INT_EQ(name[0], 0);
	fixtureView_close(view);
}

TEST(checks_simulation_input) {
	unsigned long limit = fixtureView_threadLimit();
	ASSERT_BOOL_EQ(limit >= 1 && limit <= 16, true);
	unsigned long threads = 0;
	char text[32];
	(void)snprintf(text, sizeof text, "%lu", limit);
	ASSERT_BOOL_EQ(fixtureView_threadCount(text, &threads) && threads == limit, true);
	ASSERT_BOOL_EQ(fixtureView_threadCount("1", &threads) && threads == 1, true);
	ASSERT_BOOL_EQ(fixtureView_threadCount("001", &threads) && threads == 1, true);
	(void)snprintf(text, sizeof text, "%lu", limit + 1);
	static const char *const badThreads[] = {"", "0", "-1", "+1", " 1", "1 ", "1.0", "x"};
	ASSERT_BOOL_EQ(fixtureView_threadCount(text, &threads), false);
	for (size_t i = 0; i < sizeof badThreads / sizeof badThreads[0]; ++i) {
		ASSERT_BOOL_EQ(fixtureView_threadCount(badThreads[i], &threads), false);
	}
	// Fewer than 16 characters, as the Windows field holds.
	ASSERT_BOOL_EQ(fixtureView_threadCount("000000000000001", &threads) && threads == 1, true);
	ASSERT_BOOL_EQ(fixtureView_threadCount("0000000000000001", &threads), false);
	static const char *const patterns[] = {"", "keep", "1", "1,4,7-70", "1-", ",", "--"};
	for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; ++i) {
		ASSERT_BOOL_EQ(fixtureView_seedPattern(patterns[i]), true);
	}
	static const char *const badPatterns[] = {"KEEP", "keep1", " 1", "1 2", "a", "1;2"};
	for (size_t i = 0; i < sizeof badPatterns / sizeof badPatterns[0]; ++i) {
		ASSERT_BOOL_EQ(fixtureView_seedPattern(badPatterns[i]), false);
	}
	char longest[257];
	memset(longest, '1', sizeof longest - 1);
	longest[255] = 0;
	ASSERT_BOOL_EQ(fixtureView_seedPattern(longest), true);
	longest[255] = '1';
	longest[256] = 0;
	ASSERT_BOOL_EQ(fixtureView_seedPattern(longest), false);
}

// The harness's own count for a pattern (job.c), or 0 if it refuses it.
static size_t harnessSeeds(const char *pattern) {
	char text[512];
	(void)snprintf(
		text, sizeof text, "command=run\ninput=/in.json\noutput=/out.csv\nseeds=%s\n", pattern
	);
	Job *job = calloc(1, sizeof *job);
	size_t count = 0;
	if (job && job_parse(text, job)) {
		count = job->keepSeed ? 1 : job->seedCount;
		job_free(job);
	}
	free(job);
	return count;
}

TEST(counts_seeds_as_the_harness) {
	static const char *const patterns[] = {
		"keep",
		"0",
		"42",
		"1-1000",
		"1,5,10-20",
		"1,1",
		"1-3,4294967295",
		"4294967295",
		"0-10000000",
		"5-5",
		"",
		",1",
		"1,",
		"1,,2",
		"-1",
		"1-",
		"2-1",
		"1--2",
		"1-2-3",
		"4294967296",
		"0-10000001",
		"18446744073709551616",
		"4294967290-4294967296",
	};
	for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; ++i) {
		size_t count = 0;
		bool valid = fixtureView_seedCount(patterns[i], &count);
		if (!patterns[i][0]) {
			// Blank means the fixture's own seed in the viewer; the harness needs "keep" for that.
			ASSERT_BOOL_EQ(valid && count == 1, true);
			continue;
		}
		size_t expected = harnessSeeds(patterns[i]);
		if (valid != (expected > 0) || (valid && count != expected)) {
			fprintf(stderr, "%s: viewer %zu (%d), harness %zu\n", patterns[i], count, valid, expected);
		}
		ASSERT_BOOL_EQ(valid, expected > 0);
		ASSERT_BOOL_EQ(!valid || count == expected, true);
	}
	size_t count = 0;
	ASSERT_BOOL_EQ(fixtureView_seedCount("1-1000,2001-3000", &count) && count == 2000, true);
}

TEST(reads_results_progress) {
	char text[FIXTURE_VIEW_TEXT];
	FixtureViewProgress progress = {0};
	checkText(
		fixtureView_progressText(&progress, text, sizeof text),
		text,
		"n = 0: home 0 (0.0%) / draw 0 (0.0%) / away 0 (0.0%)"
	);
	static const char csv[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
														"1,2,1,5400,1,00000000deadbeef,12.5\r\n"
														"2,0,0,5400,1,00000000deadbeef,12.5\n"
														"3,1,3,5400,1,00000000deadbeef,12.5\n"
														"4,1,0,54";
	size_t used = fixtureView_readProgress(&progress, csv, sizeof csv - 1);
	ASSERT_BOOL_EQ(used == sizeof csv - 1 - strlen("4,1,0,54"), true);
	ASSERT_BOOL_EQ(progress.header && !progress.failed, true);
	ASSERT_BOOL_EQ(progress.completed == 3 && progress.homeWins == 1 && progress.draws == 1, true);
	checkText(
		fixtureView_progressText(&progress, text, sizeof text),
		text,
		"n = 3: home 1 (33.3%) / draw 1 (33.3%) / away 1 (33.3%)"
	);
	static const char rest[] = "4,1,0,5400,1,00000000deadbeef,12.5\n";
	ASSERT_BOOL_EQ(
		fixtureView_readProgress(&progress, rest, sizeof rest - 1) == sizeof rest - 1, true
	);
	checkText(
		fixtureView_progressText(&progress, text, sizeof text),
		text,
		"n = 4: home 2 (50.0%) / draw 1 (25.0%) / away 1 (25.0%)"
	);
	// A line that is not a result marks the progress as failed; the rows around it still count.
	static const char bad[] = "x,1,2\n5,0,4,1\n4294967296,0,0\n6,0,0\n";
	ASSERT_BOOL_EQ(fixtureView_readProgress(&progress, bad, sizeof bad - 1) == sizeof bad - 1, true);
	ASSERT_BOOL_EQ(progress.failed && progress.completed == 6 && progress.draws == 2, true);
	// So does a file that does not start with the header.
	progress = (FixtureViewProgress){0};
	ASSERT_BOOL_EQ(fixtureView_readProgress(&progress, "1,2,1\n", 6) == 6, true);
	ASSERT_BOOL_EQ(progress.failed && !progress.header && progress.completed == 0, true);
	ASSERT_BOOL_EQ(fixtureView_readProgress(&progress, "seed", 4) == 0, true);
}

static void checkChoice(
	const char *line,
	bool players,
	bool listed,
	uint32_t id,
	const char *label,
	const char *matched
) {
	uint32_t found = 0;
	char labelText[FIXTURE_VIEW_TEXT] = "";
	char matchedText[FIXTURE_VIEW_TEXT] = "";
	bool ok = fixtureView_choice(
		line,
		strlen(line),
		players,
		&found,
		labelText,
		sizeof labelText,
		matchedText,
		sizeof matchedText
	);
	if (ok != listed) {
		fprintf(stderr, "choice \"%s\": %d\n", line, ok);
	}
	ASSERT_BOOL_EQ(ok, listed);
	if (ok && listed) {
		ASSERT_BOOL_EQ(found == id, true);
		checkText(strlen(labelText), labelText, label);
		checkText(strlen(matchedText), matchedText, matched);
	}
}

TEST(lists_team_and_player_choices) {
	checkChoice("677\tLuton Town", false, true, 677, "Luton Town (677)", "Luton Town (677)");
	checkChoice("12\tAston Villa\r", false, true, 12, "Aston Villa (12)", "Aston Villa (12)");
	checkChoice(
		"4294967295\tUnited", false, true, UINT32_MAX, "United (4294967295)", "United (4294967295)"
	);
	static const char *const badTeams[] = {
		"", "bad", "12", "12 Aston Villa", "\t12", "-1\tX", "+1\tX", "4294967296\tX", "1\t\xC0\xAF"};
	for (size_t i = 0; i < sizeof badTeams / sizeof badTeams[0]; ++i) {
		checkChoice(badTeams[i], false, false, 0, "", "");
	}
	// Team names are shorter than 256 bytes.
	char longName[300];
	(void)snprintf(longName, sizeof longName, "1\t%0256d", 0);
	checkChoice(longName, false, false, 0, "", "");
	longName[strlen(longName) - 1] = 0;
	char longLabel[300];
	(void)snprintf(longLabel, sizeof longLabel, "%s (1)", &longName[2]);
	checkChoice(longName, false, true, 1, longLabel, longLabel);

	checkChoice(
		"40347\tMartin\t\xC3\x98"
		"degaard\t\tArsenal",
		true,
		true,
		40347,
		"Martin \xC3\x98"
		"degaard \xE2\x80\x93 Arsenal (40347)",
		"Martin \xC3\x98"
		"degaard "
	);
	checkChoice(
		"1001\tEdson\tArantes do Nascimento\tPel\xC3\xA9\t",
		true,
		true,
		1001,
		"Pel\xC3\xA9 (Edson Arantes do Nascimento) (1001)",
		"Edson Arantes do Nascimento Pel\xC3\xA9"
	);
	checkChoice(
		"5005\tJan\tBodegaard\t\t\r", true, true, 5005, "Jan Bodegaard (5005)", "Jan Bodegaard "
	);
	checkChoice("6\t\tOnly\t\t", true, true, 6, "Only (6)", "Only ");
	checkChoice("7\t\t\tCommon\tClub", true, true, 7, "Common \xE2\x80\x93 Club (7)", " Common");
	static const char *const badPlayers[] = {
		"6006\tToo\tFew", "7007\t\t\t\t", "8\ta\tb\tc\td\te", "9\ta\t\xFF\t\t", "bad"};
	for (size_t i = 0; i < sizeof badPlayers / sizeof badPlayers[0]; ++i) {
		checkChoice(badPlayers[i], true, false, 0, "", "");
	}
	// Too small a label buffer drops the line rather than cut its ID off.
	uint32_t id = 0;
	char label[8];
	char matched[64];
	ASSERT_BOOL_EQ(
		fixtureView_choice(
			"677\tLuton Town", 14, false, &id, label, sizeof label, matched, sizeof matched
		),
		false
	);
}

TEST(names_swapped_players_and_replaced_teams) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_SWAPPED);
	if (!view) {
		return;
	}
	char text[FIXTURE_VIEW_TEXT];
	checkText(
		fixtureView_playerName(view, 1, 0, text, sizeof text), text, "Emiliano Mart\xC3\xADnez"
	);
	// A block named by its player note, as after inject-player.
	checkText(fixtureView_playerName(view, 0, 11, text, sizeof text), text, "Injected player");
	// Blocks past the side's players, and sides that are not one, have no name.
	checkText(fixtureView_playerName(view, 0, 12, text, sizeof text), text, "");
	checkText(fixtureView_playerName(view, 2, 0, text, sizeof text), text, "");
	ASSERT_BOOL_EQ(fixtureView_playerName(view, 1, 0, text, 3) == 18, true);
	ASSERT_INT_EQ(strcmp(text, "Em"), 0);
	fixtureView_close(view);

	// What binary data would lose: the replaced teams in the header.
	ByteBuffer json = {0};
	uint32_t teams[2] = {1, 1};
	if (sampleJson(FIXTURE_SAMPLE_LABELLED, &json)) {
		ASSERT_BOOL_EQ(fixtureView_teamOverrides(buffer_string(&json), json.length, teams), true);
		ASSERT_BOOL_EQ(teams[0] == 0 && teams[1] == 0, true);
	}
	buffer_free(&json);
	if (sampleJson(FIXTURE_SAMPLE_VENUE, &json)) {
		ASSERT_BOOL_EQ(fixtureView_teamOverrides(buffer_string(&json), json.length, teams), true);
		ASSERT_BOOL_EQ(teams[0] == 678 && teams[1] == 677, true);
		replaceText(&json, "\"home_team\":678,", "");
		ASSERT_BOOL_EQ(fixtureView_teamOverrides(buffer_string(&json), json.length, teams), true);
		ASSERT_BOOL_EQ(teams[0] == 0 && teams[1] == 677, true);
	}
	buffer_free(&json);
	teams[0] = teams[1] = 1;
	ASSERT_BOOL_EQ(fixtureView_teamOverrides("{", 1, teams), false);
	ASSERT_BOOL_EQ(teams[0] == 0 && teams[1] == 0, true);
}

// The lineup sample (Liverpool v Newcastle, fixture_samples.h) with a stored pitch condition of
// 200, for venue saves.
static bool lineupJson(ByteBuffer *json) {
	buffer_clear(json);
	bool ok = sampleJson(FIXTURE_SAMPLE_LINEUP, json);
	if (ok) {
		replaceText(
			json,
			"{\"str\": \"Premier League\"}",
			"{\"str\": \"Premier League\"},\n    {\"name\": \"venue.pitch_condition\", \"u16\": 200}"
		);
	}
	return ok;
}

// Replaces every `from` in `json` with `to`.
static void replaceEvery(ByteBuffer *json, const char *from, const char *to) {
	while (strstr(buffer_string(json), from)) {
		replaceText(json, from, to);
	}
}

// Exchanges the names of two of a side's blocks, as a script that moves blocks without renaming
// them leaves them.
static void exchangeNames(ByteBuffer *json, const char *side, unsigned a, unsigned b) {
	char nameA[32];
	char nameB[32];
	char spare[32];
	(void)snprintf(nameA, sizeof nameA, "\"%s.player[%u]", side, a);
	(void)snprintf(nameB, sizeof nameB, "\"%s.player[%u]", side, b);
	(void)snprintf(spare, sizeof spare, "\"%s.player[moved]", side);
	replaceEvery(json, nameA, spare);
	replaceEvery(json, nameB, nameA);
	replaceEvery(json, spare, nameB);
}

// The order that exchanges places a and b (that moves nobody if they are the same).
static void exchanged(uint8_t from[FIXTURE_VIEW_BLOCKS], unsigned a, unsigned b) {
	for (unsigned place = 0; place < FIXTURE_VIEW_BLOCKS; ++place) {
		from[place] = (uint8_t)place;
	}
	from[a] = (uint8_t)b;
	from[b] = (uint8_t)a;
}

static void checkOrder(const FixtureView *view, unsigned side, const uint8_t *expected) {
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	fixtureView_order(view, side, from);
	ASSERT_INT_EQ(memcmp(from, expected, sizeof from), 0);
}

// A home row of `view` shows the slot columns of the same row of `original`, the view without an
// order, and the player columns of its row `playerRow`.
static void checkMovedRow(
	const FixtureView *view,
	const FixtureView *original,
	unsigned row,
	unsigned playerRow
) {
	for (unsigned column = 0; column < FIXTURE_VIEW_COLUMNS; ++column) {
		char shown[FIXTURE_VIEW_TEXT];
		char expected[FIXTURE_VIEW_TEXT];
		bool slotColumn = column == 0 || (column >= 2 && column <= 4);
		fixtureView_cell(view, 0, row, column, shown, sizeof shown);
		fixtureView_cell(original, 0, slotColumn ? row : playerRow, column, expected, sizeof expected);
		if (strcmp(shown, expected) != 0) {
			fprintf(
				stderr, "row %u column %u: expected \"%s\", got \"%s\"\n", row, column, expected, shown
			);
		}
		ASSERT_INT_EQ(strcmp(shown, expected), 0);
	}
}

TEST(shows_a_new_order_in_the_squad_lists) {
	ByteBuffer json = {0};
	FixtureView *view = lineupJson(&json) ? openJson(&json) : NULL;
	FixtureView *original = view ? openJson(&json) : NULL;
	if (!view || !original) {
		fixtureView_close(view);
		buffer_free(&json);
		return;
	}
	uint8_t identity[FIXTURE_VIEW_BLOCKS];
	exchanged(identity, 0, 0);
	ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 0), true);
	ASSERT_BOOL_EQ(fixtureView_lineupMarked(view, 0), false);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 0), false);
	ASSERT_BOOL_EQ(fixtureView_benchSize(view, 0) == 9, true);
	ASSERT_BOOL_EQ(fixtureView_benchSize(view, 1) == 2, true);
	ASSERT_BOOL_EQ(fixtureView_benchSize(view, 2) == 0, true);
	checkOrder(view, 0, identity);
	checkOrder(view, 2, identity);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 19), 19);

	// Wirtz (block 19) and Gravenberch (block 10) exchanged: each row keeps its slot and shows the
	// player moved there, whose own block the row still gives.
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	exchanged(from, 10, 19);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 0), true);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 1), false);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), true);
	checkOrder(view, 0, from);
	checkOrder(view, 1, identity);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 10), 19);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 19), 10);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 9), 9);
	checkCell(view, 0, 10, 0, "10");
	checkCell(view, 0, 10, 1, "Florian Wirtz");
	checkCell(view, 0, 10, 2, "ST");
	checkCell(view, 0, 10, 6, "90.19%");
	checkCell(view, 0, 19, 1, "Ryan Gravenberch");
	checkCell(view, 0, 19, 2, "Sub");
	checkCell(view, 0, 19, 6, "90.10%");
	for (unsigned row = 0; row < fixtureView_rowCount(view, 0); ++row) {
		unsigned player = row == 10 ? 19 : row == 19 ? 10 : row;
		checkMovedRow(view, original, row, player);
	}
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 19), 10);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 10), 19);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 21), 21);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 22), -1);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 2, 0), -1);
	// Blocks name the players, whatever their places.
	char text[FIXTURE_VIEW_TEXT];
	checkText(fixtureView_playerName(view, 0, 19, text, sizeof text), text, "Florian Wirtz");
	ASSERT_BOOL_EQ(fixtureView_personId(view, 0, 19) == 91193048, true);
	ASSERT_BOOL_EQ(fixtureView_personId(view, 0, 22) == -1, true);
	ASSERT_BOOL_EQ(fixtureView_personId(view, 2, 0) == -1, true);
	// A Max edit of Wirtz's block shows in his new row.
	bool selected[FIXTURE_VIEW_BLOCKS] = {false};
	selected[19] = true;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_CONDITION), true);
	checkCell(view, 0, 10, 6, "100.00%");
	checkCell(view, 0, 19, 6, "90.10%");

	// Orders that do not place each player once change nothing; places from the side's count on
	// are its own.
	uint8_t wrong[FIXTURE_VIEW_BLOCKS];
	exchanged(wrong, 10, 19);
	wrong[10] = 10;
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, wrong), false);
	exchanged(wrong, 3, 22);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, wrong), false);
	checkOrder(view, 0, from);
	exchanged(wrong, 2, 3);
	wrong[30] = 31;
	wrong[31] = 30;
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, wrong), true);
	wrong[30] = 30;
	wrong[31] = 31;
	checkOrder(view, 0, wrong);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 2), 3);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 10), 10);
	// An order that moves nobody clears it; the Max edit stays.
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, identity), true);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 0), false);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), true);
	fixtureView_clearPlayerEdits(view);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), false);
	// An order alone is a player edit, and forgetting the Max edits keeps it.
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
	fixtureView_clearPlayerEdits(view);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), true);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 0), true);

	// FM24's AI picks the away XI: its order cannot change, and its list is as before.
	ASSERT_BOOL_EQ(fixtureView_lineupKnown(view, 0), true);
	ASSERT_BOOL_EQ(fixtureView_lineupKnown(view, 1), false);
	ASSERT_BOOL_EQ(fixtureView_lineupKnown(view, 2), false);
	ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 1), false);
	exchanged(wrong, 1, 2);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 1, wrong), false);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 1), false);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 2, identity), false);
	ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 2), false);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 1), -1);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 12), 1);
	fixtureView_close(original);
	fixtureView_close(view);
	buffer_free(&json);

	// In the other samples too, only the human side's order can change.
	static const struct {
		int sample;
		bool editable[2];
	} sides[] = {
		{FIXTURE_SAMPLE_LABELLED, {true, false}},
		{FIXTURE_SAMPLE_SWAPPED, {false, true}},
	};
	for (size_t i = 0; i < sizeof sides / sizeof sides[0]; ++i) {
		view = openSample(sides[i].sample);
		for (unsigned side = 0; view && side < 2; ++side) {
			ASSERT_BOOL_EQ(fixtureView_lineupKnown(view, side), sides[i].editable[side]);
			ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, side), sides[i].editable[side]);
		}
		fixtureView_close(view);
	}
	// A block more than player_count says.
	if (lineupJson(&json)) {
		replaceText(&json, "\"home.player_count\", \"u8\": 22", "\"home.player_count\", \"u8\": 21");
		view = openJson(&json);
		if (view) {
			ASSERT_BOOL_EQ(fixtureView_lineupKnown(view, 0), true);
			ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 0), false);
			fixtureView_close(view);
		}
	}
	buffer_free(&json);
}

// Writes `json` with Max edits, orders and venue values (each may be NULL), as JSON, into `out`.
static bool writeOrdered(
	const ByteBuffer *json,
	const uint8_t *flags,
	const uint8_t *order,
	const FixtureViewNumber *venue,
	ByteBuffer *out
) {
	uint8_t *bytes = NULL;
	size_t length = 0;
	char error[256] = "";
	bool ok = fixtureView_write(
		buffer_string(json),
		json->length,
		flags,
		order,
		venue,
		false,
		&bytes,
		&length,
		error,
		sizeof error
	);
	if (!ok) {
		fprintf(stderr, "%s\n", error);
	}
	ok = ok && buffer_append(out, bytes, length);
	fixtureView_free(bytes);
	return ok;
}

// Away's slot 9 marking home place 10.
static const char wirtzMarking[] =
	"\"away.manager.tactic.plan[0].slot[9].player_instructions[winger].bits[1]\", \"flags\": "
	"[\"mark a specific player\"], \"u32\": 4},\n    {\"u8\": 10}";

TEST(writes_and_commits_a_new_order) {
	ByteBuffer json = {0};
	FixtureView *view = lineupJson(&json) ? openJson(&json) : NULL;
	if (!view) {
		buffer_free(&json);
		return;
	}
	// Max edits of Wirtz (19) and van Dijk (2), then Wirtz into Gravenberch's place (10).
	bool selected[FIXTURE_VIEW_BLOCKS] = {false};
	selected[19] = true;
	selected[2] = true;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_HAPPINESS), true);
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	exchanged(from, 10, 19);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
	checkWrittenEdits(view, &json);
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	uint8_t order[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	viewOrder(view, order);
	ASSERT_INT_EQ(flags[19], FIXTURE_VIEW_MAX_HAPPINESS);
	ASSERT_INT_EQ(flags[10], 0);

	// The bytes: the Max edits by the blocks' names, then the order, then the venue, then encoded.
	FixturePlayerEdits edits = {0};
	memcpy(edits.players, flags, sizeof edits.players);
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	FixtureViewNumber venue[FIXTURE_VIEW_VENUE_FIELDS] = {{0}};
	venue[10] = (FixtureViewNumber){.value = 150, .known = true};
	for (int binary = 0; binary < 2; ++binary) {
		checkWrite(&json, NULL, &lineup, NULL, binary != 0);
		checkWrite(&json, &edits, &lineup, venue, binary != 0);
	}
	ByteBuffer saved = {0};
	ASSERT_BOOL_EQ(writeOrdered(&json, flags, order, venue, &saved), true);
	FixtureView *reopened = saved.length ? openJson(&saved) : NULL;
	if (reopened) {
		ASSERT_BOOL_EQ(fixtureView_lineupMarked(reopened, 0), true);
		ASSERT_BOOL_EQ(fixtureView_lineupMarked(reopened, 1), false);
		ASSERT_BOOL_EQ(fixtureView_lineupEdited(reopened, 0), false);
		ASSERT_INT_EQ(fixtureView_rowBlock(reopened, 0, 10), 10);
		checkCell(reopened, 0, 10, 1, "Florian Wirtz");
		checkCell(reopened, 0, 10, 5, "20");
		checkCell(reopened, 0, 19, 5, "9");
		FixtureViewNumber values[FIXTURE_VIEW_VENUE_FIELDS];
		fixtureView_venue(reopened, values);
		ASSERT_BOOL_EQ(values[10].known && values[10].value == 150, true);
		fixtureView_close(reopened);
	}
	// Away's marking of Wirtz follows him to place 10, and away's line-up is not marked.
	ASSERT_BOOL_EQ(strstr(buffer_string(&saved), wirtzMarking) != NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&json), wirtzMarking) == NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&saved), "\"away_lineup\"") == NULL, true);

	// After the save the view takes the saved order: each player's block is its new place, with
	// its Max edits, and nothing but those edits is pending.
	fixtureView_commitOrder(view);
	uint8_t identity[FIXTURE_VIEW_BLOCKS];
	exchanged(identity, 0, 0);
	checkOrder(view, 0, identity);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 0), false);
	ASSERT_BOOL_EQ(fixtureView_lineupMarked(view, 0), true);
	ASSERT_BOOL_EQ(fixtureView_lineupMarked(view, 1), false);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 10), 10);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 0, 19), 19);
	char text[FIXTURE_VIEW_TEXT];
	checkText(fixtureView_playerName(view, 0, 10, text, sizeof text), text, "Florian Wirtz");
	checkText(fixtureView_playerName(view, 0, 19, text, sizeof text), text, "Ryan Gravenberch");
	ASSERT_BOOL_EQ(fixtureView_personId(view, 0, 10) == 91193048, true);
	checkCell(view, 0, 10, 5, "20");
	fixtureView_playerEdits(view, flags);
	ASSERT_INT_EQ(flags[10], FIXTURE_VIEW_MAX_HAPPINESS);
	ASSERT_INT_EQ(flags[19], 0);
	ASSERT_INT_EQ(flags[2], FIXTURE_VIEW_MAX_HAPPINESS);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), true);
	// The view now matches the saved file, edits and all.
	checkWrittenEdits(view, &saved);
	fixtureView_clearPlayerEdits(view);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), false);
	// Committing with nothing pending changes nothing.
	fixtureView_commitOrder(view);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 10), 10);
	ASSERT_BOOL_EQ(fixtureView_lineupMarked(view, 1), false);
	fixtureView_close(view);

	// A three-way move, committed: the cycles follow each player.
	view = openJson(&json);
	if (view) {
		exchanged(from, 0, 0);
		from[11] = 20;
		from[20] = 14;
		from[14] = 11;
		from[5] = 6;
		from[6] = 5;
		ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
		memset(selected, 0, sizeof selected);
		selected[14] = true;
		ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_CONDITION), true);
		checkWrittenEdits(view, &json);
		fixtureView_playerEdits(view, flags);
		viewOrder(view, order);
		buffer_clear(&saved);
		ASSERT_BOOL_EQ(writeOrdered(&json, flags, order, NULL, &saved), true);
		fixtureView_commitOrder(view);
		checkText(fixtureView_playerName(view, 0, 11, text, sizeof text), text, "Ben Doak");
		checkText(fixtureView_playerName(view, 0, 20, text, sizeof text), text, "Lewis Jones");
		checkText(fixtureView_playerName(view, 0, 14, text, sizeof text), text, "Caoimhin Kelleher");
		checkText(fixtureView_playerName(view, 0, 5, text, sizeof text), text, "Alexis Mac Allister");
		fixtureView_playerEdits(view, flags);
		ASSERT_INT_EQ(flags[20], FIXTURE_VIEW_MAX_CONDITION);
		ASSERT_INT_EQ(flags[14], 0);
		checkWrittenEdits(view, &saved);
		fixtureView_close(view);
	}
	buffer_free(&saved);
	buffer_free(&json);
}

// The lineup sample with the managers' flags exchanged: Newcastle's line-up (away: 13 players, a
// bench of 2) can be changed, and FM24's AI picks Liverpool's.
static bool awayLineupJson(ByteBuffer *json) {
	bool ok = lineupJson(json);
	if (ok) {
		replaceText(json, "\"home.manager\", \"u32\": 131848}", "\"home.manager\", \"u32\": 212992}");
		replaceText(json, "\"away.manager\", \"u32\": 212992}", "\"away.manager\", \"u32\": 131848}");
	}
	return ok;
}

TEST(writes_and_commits_an_away_order) {
	ByteBuffer json = {0};
	FixtureView *view = awayLineupJson(&json) ? openJson(&json) : NULL;
	if (!view) {
		buffer_free(&json);
		return;
	}
	ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 0), false);
	ASSERT_BOOL_EQ(fixtureView_lineupEditable(view, 1), true);
	// Miley's happiness maximised, then a cycle of three: Botman 2 to 1, Miley 12 to 2 and Trippier 1
	// to 12.
	bool selected[FIXTURE_VIEW_BLOCKS] = {false};
	selected[12] = true;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 1, selected, FIXTURE_VIEW_MAX_HAPPINESS), true);
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	exchanged(from, 0, 0);
	from[1] = 2;
	from[2] = 12;
	from[12] = 1;
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 1, from), true);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 1) && !fixtureView_lineupEdited(view, 0), true);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 2), 12);
	ASSERT_INT_EQ(fixtureView_placeOf(view, 1, 12), 2);
	checkCell(view, 1, 2, 1, "Lewis Miley");
	checkWrittenEdits(view, &json);
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	uint8_t order[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	viewOrder(view, order);
	ByteBuffer saved = {0};
	ASSERT_BOOL_EQ(writeOrdered(&json, flags, order, NULL, &saved), true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&saved), "\n  \"away_lineup\": true,\n") != NULL, true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&saved), "\"home_lineup\"") == NULL, true);
	ASSERT_BOOL_EQ(
		strstr(buffer_string(&saved), "{\"name\": \"away.player[2]\", \"player\": \"Lewis Miley\"") !=
			NULL,
		true
	);

	// After the save, each away player's block is its new place, with its Max edit, and the view
	// lists what the file holds.
	fixtureView_commitOrder(view);
	uint8_t identity[FIXTURE_VIEW_BLOCKS];
	exchanged(identity, 0, 0);
	checkOrder(view, 1, identity);
	ASSERT_BOOL_EQ(fixtureView_lineupEdited(view, 1), false);
	ASSERT_BOOL_EQ(fixtureView_lineupMarked(view, 1) && !fixtureView_lineupMarked(view, 0), true);
	char text[FIXTURE_VIEW_TEXT];
	checkText(fixtureView_playerName(view, 1, 1, text, sizeof text), text, "Sven Botman");
	checkText(fixtureView_playerName(view, 1, 2, text, sizeof text), text, "Lewis Miley");
	checkText(fixtureView_playerName(view, 1, 12, text, sizeof text), text, "Kieran Trippier");
	ASSERT_BOOL_EQ(fixtureView_personId(view, 1, 2) == fixtureSamples_lineupId(1, 12), true);
	ASSERT_INT_EQ(fixtureView_rowBlock(view, 1, 2), 2);
	fixtureView_playerEdits(view, flags);
	ASSERT_INT_EQ(flags[FIXTURE_VIEW_BLOCKS + 2], FIXTURE_VIEW_MAX_HAPPINESS);
	ASSERT_INT_EQ(flags[FIXTURE_VIEW_BLOCKS + 12], 0);
	checkWrittenEdits(view, &saved);
	// A second save of the saved file, its Max edit written, changes nothing: the order is not
	// applied again.
	fixtureView_clearPlayerEdits(view);
	ASSERT_BOOL_EQ(fixtureView_hasPlayerEdits(view), false);
	fixtureView_playerEdits(view, flags);
	viewOrder(view, order);
	ByteBuffer again = {0};
	ASSERT_BOOL_EQ(writeOrdered(&saved, flags, order, NULL, &again), true);
	ASSERT_BOOL_EQ(
		again.length == saved.length && memcmp(again.data, saved.data, saved.length) == 0, true
	);
	fixtureView_close(view);
	buffer_free(&again);
	buffer_free(&saved);
	buffer_free(&json);
}

TEST(refuses_blocks_moved_without_renaming) {
	ByteBuffer json = {0};
	static const struct {
		const char *side;
		unsigned a;
		unsigned b;
		const char *error;
	} moved[] = {
		{"home",
			10,
			19,
			"home.player[19] is in place 10: its player blocks were moved without renaming them. Use "
			"fm24ctl lineup to move players."},
		{"away",
			1,
			2,
			"away.player[2] is in place 1: its player blocks were moved without renaming them. Use "
			"fm24ctl lineup to move players."},
	};
	for (size_t i = 0; i < sizeof moved / sizeof moved[0]; ++i) {
		if (!lineupJson(&json)) {
			break;
		}
		exchangeNames(&json, moved[i].side, moved[i].a, moved[i].b);
		char error[256] = "";
		FixtureView *view = fixtureView_open(buffer_string(&json), json.length, error, sizeof error);
		ASSERT_BOOL_EQ(view == NULL, true);
		if (strcmp(error, moved[i].error) != 0) {
			fprintf(stderr, "%s\n", error);
		}
		ASSERT_INT_EQ(strcmp(error, moved[i].error), 0);
		fixtureView_close(view);
		// Renamed back, it opens.
		exchangeNames(&json, moved[i].side, moved[i].a, moved[i].b);
		view = openJson(&json);
		fixtureView_close(view);
	}
	buffer_free(&json);
}

TEST(rates_players_for_slots) {
	ByteBuffer json = {0};
	FixtureView *view = lineupJson(&json) ? openJson(&json) : NULL;
	if (!view) {
		buffer_free(&json);
		return;
	}
	// The sample's pos_gk is 20 for its goalkeepers and 1 for the others, whose rating k (in
	// docs/FIXTURE.md#ratings order) is 1 + (place + 3k) % 20.
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 0, 0), 20);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 0, 10), 1);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 0), 1);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 10), 7);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 19, 10), 16);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 2), 20);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 3), 20);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 5), 6);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 1, 9, 10), 1 + (9 + 36) % 20);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 11), -1);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 22, 10), -1);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 2, 0, 0), -1);
	// The rating is the player's, whatever the order.
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	exchanged(from, 10, 19);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
	ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 19, 10), 16);
	fixtureView_close(view);

	// Each position's rating, set in slot 1 (a DR, 4): docs/FIXTURE.md's position bits against the
	// order of its ratings. Block 0 is the goalkeeper; block 9's ratings all differ.
	static const struct {
		const char *position;
		unsigned block;
		int rating;
	} positions[] = {
		{"1", 0, 20},                         // GK: pos_gk
		{"4", 9, 1 + (9 + 3 * 4) % 20},       // DR: pos_dr
		{"8", 9, 1 + (9 + 3 * 2) % 20},       // DL: pos_dl
		{"16", 9, 1 + (9 + 3 * 3) % 20},      // DC: pos_dc
		{"32", 9, 1 + (9 + 3 * 14) % 20},     // WBR: pos_wbr
		{"64", 9, 1 + (9 + 3 * 13) % 20},     // WBL: pos_wbl
		{"128", 9, 1 + (9 + 3 * 5) % 20},     // DM: pos_dm
		{"256", 9, 1 + (9 + 3 * 8) % 20},     // MR: pos_mr
		{"512", 9, 1 + (9 + 3 * 6) % 20},     // ML: pos_ml
		{"1024", 9, 1 + (9 + 3 * 7) % 20},    // MC: pos_mc
		{"2048", 9, 1 + (9 + 3 * 11) % 20},   // AMR: pos_amr
		{"4096", 9, 1 + (9 + 3 * 9) % 20},    // AML: pos_aml
		{"8192", 9, 1 + (9 + 3 * 10) % 20},   // AMC: pos_amc
		{"16384", 9, 1 + (9 + 3 * 12) % 20},  // ST: pos_st
		{"1048704", 9, 1 + (9 + 3 * 5) % 20}, // DMCL
		{"2097280", 9, 1 + (9 + 3 * 5) % 20}, // DMCR
		{"3145856", 9, -1},                   // left and right
		{"2", 9, -1},                         // bit 1 names no position
		{"12", 9, -1},                        // DR and DL
		{"0", 9, -1},
		{"1", 9, 1}, // an outfield player's pos_gk
	};
	for (size_t i = 0; i < sizeof positions / sizeof positions[0]; ++i) {
		if (!lineupJson(&json)) {
			break;
		}
		char position[96];
		(void)snprintf(
			position,
			sizeof position,
			"\"home.manager.tactic.plan[0].slot[1].position\", \"u32\": %s}",
			positions[i].position
		);
		replaceText(&json, "\"home.manager.tactic.plan[0].slot[1].position\", \"u32\": 4}", position);
		view = openJson(&json);
		if (view) {
			int rating = fixtureView_positionRating(view, 0, positions[i].block, 1);
			if (rating != positions[i].rating) {
				fprintf(stderr, "position %s: %d\n", positions[i].position, rating);
			}
			ASSERT_INT_EQ(rating, positions[i].rating);
			fixtureView_close(view);
		}
	}
	// Ratings that are not all there are not known.
	if (lineupJson(&json)) {
		replaceText(
			&json,
			"\"home.player[10].ratings\", \"u8map\": {\"pos_gk\": 1, ",
			"\"home.player[10].ratings\", \"u8map\": {"
		);
		view = openJson(&json);
		if (view) {
			ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 10, 10), -1);
			ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 9, 10), 1 + (9 + 36) % 20);
			fixtureView_close(view);
		}
	}
	buffer_free(&json);
	// A sample without unique IDs, whose ratings are all 20.
	view = openSample(FIXTURE_SAMPLE_LABELLED);
	if (view) {
		ASSERT_BOOL_EQ(fixtureView_personId(view, 0, 0) == -1, true);
		ASSERT_INT_EQ(fixtureView_positionRating(view, 0, 0, 0), 20);
		fixtureView_close(view);
	}
}

TEST(warns_about_a_new_order) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_LINEUP);
	if (!view) {
		return;
	}
	static const struct {
		unsigned a;
		unsigned b;
		const char *warnings;
	} orders[] = {
		{0, 0, ""},
		{0, 11, ""},
		{0,
			10,
			"Ryan Gravenberch is in goal (slot 0) but is not a goalkeeper (GK 1).\n"
			"Alisson Becker, a goalkeeper, is in slot 10 (ST)."},
		{11, 20, "No goalkeeper among the substitutes."},
		{10, 19, ""},
	};
	char text[FIXTURE_VIEW_TEXT];
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	for (size_t i = 0; i < sizeof orders / sizeof orders[0]; ++i) {
		exchanged(from, orders[i].a, orders[i].b);
		checkText(
			fixtureView_lineupWarnings(view, 0, from, text, sizeof text), text, orders[i].warnings
		);
	}
	// Cut short, it still gives the full length.
	exchanged(from, 11, 20);
	ASSERT_BOOL_EQ(fixtureView_lineupWarnings(view, 0, from, text, 3) == 36, true);
	ASSERT_INT_EQ(strcmp(text, "No"), 0);
	ASSERT_BOOL_EQ(fixtureView_lineupWarnings(view, 0, from, NULL, 0) == 36, true);
	// Nothing for an order that is not one, or a side whose order cannot change.
	from[20] = 20;
	checkText(fixtureView_lineupWarnings(view, 0, from, text, sizeof text), text, "");
	exchanged(from, 0, 10);
	checkText(fixtureView_lineupWarnings(view, 1, from, text, sizeof text), text, "");
	checkText(fixtureView_lineupWarnings(view, 2, from, text, sizeof text), text, "");
	// The warnings are about the order given, not the one pending.
	exchanged(from, 11, 20);
	ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
	exchanged(from, 10, 19);
	checkText(fixtureView_lineupWarnings(view, 0, from, text, sizeof text), text, "");
	fixtureView_close(view);
}

// A line-up kept through a team replacement: the order putting the people given back in their
// places, which fixtureView_setOrder takes, with those the side lacks and who took their places.
TEST(orders_a_side_by_people) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_LINEUP);
	if (!view) {
		return;
	}
	// Wirtz and Gravenberch exchanged, Gravenberch by an ID the side does not have: the player no
	// one names, Gravenberch under his own ID, takes his place.
	uint32_t people[22];
	for (unsigned place = 0; place < 22; ++place) {
		people[place] = fixtureSamples_lineupId(0, place);
	}
	people[10] = fixtureSamples_lineupId(0, 19);
	people[19] = 2000040347;
	uint8_t from[FIXTURE_VIEW_BLOCKS];
	uint8_t expected[FIXTURE_VIEW_BLOCKS];
	bool found[22];
	int places[22];
	exchanged(expected, 10, 19);
	for (int pending = 0; pending < 2; ++pending) {
		ASSERT_BOOL_EQ(fixtureView_orderByPeople(view, 0, people, 22, from, found, places), true);
		ASSERT_INT_EQ(memcmp(from, expected, sizeof from), 0);
		ASSERT_BOOL_EQ(found[0] && found[10] && !found[19] && found[21], true);
		ASSERT_BOOL_EQ(places[0] == 0 && places[10] == 10 && places[19] == 19, true);
		// The people name players, not places: a pending order changes nothing.
		ASSERT_BOOL_EQ(fixtureView_setOrder(view, 0, from), true);
		ASSERT_INT_EQ(fixtureView_rowBlock(view, 0, 10), 19);
	}
	// None for a side whose order cannot change, or for more people than blocks.
	ASSERT_BOOL_EQ(fixtureView_orderByPeople(view, 1, people, 13, from, found, places), false);
	ASSERT_BOOL_EQ(fixtureView_orderByPeople(view, 2, people, 1, from, found, places), false);
	static uint32_t many[FIXTURE_VIEW_BLOCKS + 1];
	static bool manyFound[FIXTURE_VIEW_BLOCKS + 1];
	static int manyPlaces[FIXTURE_VIEW_BLOCKS + 1];
	ASSERT_BOOL_EQ(
		fixtureView_orderByPeople(
			view, 0, many, FIXTURE_VIEW_BLOCKS + 1, from, manyFound, manyPlaces
		),
		false
	);
	fixtureView_close(view);
}

// What a background thread does with a view, on a stack smaller than a worker thread's.
static void useView(void) {
	FixtureView *view = openSample(FIXTURE_SAMPLE_FAMILIARITY);
	if (!view) {
		return;
	}
	char text[FIXTURE_VIEW_TEXT];
	checkCell(view, 0, 1, 9, "42.00%");
	bool selected[FIXTURE_VIEW_BLOCKS] = {false};
	selected[1] = true;
	ASSERT_BOOL_EQ(fixtureView_applyEdit(view, 0, selected, FIXTURE_VIEW_MAX_FAMILIARITY), true);
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	ByteBuffer json = {0};
	if (sampleJson(FIXTURE_SAMPLE_FAMILIARITY, &json)) {
		uint8_t *bytes = NULL;
		size_t length = 0;
		ASSERT_BOOL_EQ(
			fixtureView_write(
				buffer_string(&json),
				json.length,
				flags,
				NULL,
				NULL,
				true,
				&bytes,
				&length,
				text,
				sizeof text
			),
			true
		);
		fixtureView_free(bytes);
	}
	buffer_free(&json);
	fixtureView_close(view);
}

#ifdef _WIN32
static DWORD WINAPI runView(LPVOID parameter) {
	(void)parameter;
	useView();
	return 0;
}
#else
static void *runView(void *parameter) {
	(void)parameter;
	useView();
	return NULL;
}
#endif

TEST(opens_and_writes_on_a_small_stack) {
	enum { STACK_SIZE = 256 * 1024 };
#ifdef _WIN32
	HANDLE thread =
		CreateThread(NULL, STACK_SIZE, runView, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	ASSERT_BOOL_EQ(thread != NULL, true);
	if (thread) {
		ASSERT_BOOL_EQ(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0, true);
		CloseHandle(thread);
	}
#else
	pthread_attr_t attributes;
	pthread_t thread;
	ASSERT_INT_EQ(pthread_attr_init(&attributes), 0);
	ASSERT_INT_EQ(pthread_attr_setstacksize(&attributes, STACK_SIZE), 0);
	ASSERT_INT_EQ(pthread_create(&thread, &attributes, runView, NULL), 0);
	ASSERT_INT_EQ(pthread_join(thread, NULL), 0);
	ASSERT_INT_EQ(pthread_attr_destroy(&attributes), 0);
#endif
}

void registerFixtureViewTests(void) {
	test_run("fixture view opens and summarises fixtures", test_opens_and_summarises_fixtures);
	test_run("fixture view lists squads as the Windows viewer", test_lists_squads_as_windows);
	test_run("fixture view shows invalid wellbeing values", test_shows_invalid_wellbeing_values);
	test_run("fixture view places slots on the pitch", test_places_slots_on_the_pitch);
	test_run(
		"fixture view edits players as saves write them", test_edits_players_as_saves_write_them
	);
	test_run(
		"fixture view reads, parses and validates venue values",
		test_reads_parses_and_validates_venue_values
	);
	test_run("fixture view writes the bytes Windows saves", test_writes_the_bytes_windows_saves);
	test_run(
		"fixture view keeps the save and date of exports", test_keeps_the_save_and_date_of_exports
	);
	test_run("fixture view names the save of exports", test_names_the_save_of_exports);
	test_run(
		"fixture view drops the save from binary copies", test_drops_the_save_from_binary_copies
	);
	test_run("fixture view names exports in UTF-8", test_names_exports_in_utf8);
	test_run("fixture view checks simulation input", test_checks_simulation_input);
	test_run("fixture view counts seeds as the harness does", test_counts_seeds_as_the_harness);
	test_run("fixture view reads results progress", test_reads_results_progress);
	test_run("fixture view lists team and player choices", test_lists_team_and_player_choices);
	test_run(
		"fixture view names swapped players and replaced teams",
		test_names_swapped_players_and_replaced_teams
	);
	test_run(
		"fixture view shows a new order in the squad lists", test_shows_a_new_order_in_the_squad_lists
	);
	test_run("fixture view writes and commits a new order", test_writes_and_commits_a_new_order);
	test_run("fixture view writes and commits an away order", test_writes_and_commits_an_away_order);
	test_run(
		"fixture view refuses blocks moved without renaming", test_refuses_blocks_moved_without_renaming
	);
	test_run("fixture view rates players for slots", test_rates_players_for_slots);
	test_run("fixture view warns about a new order", test_warns_about_a_new_order);
	test_run("fixture view orders a side by people", test_orders_a_side_by_people);
	test_run(
		"fixture view opens and writes on a small stack", test_opens_and_writes_on_a_small_stack
	);
}
