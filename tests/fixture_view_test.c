// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/dll/job.h"
#include "buffer.h"
#include "fixture_data.h"
#include "fixture_edit.h"
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

// The cells of every row after a save of the view's edits, read back, are those it shows.
static void checkWrittenEdits(const FixtureView *view, const ByteBuffer *json) {
	uint8_t flags[2 * FIXTURE_VIEW_BLOCKS];
	fixtureView_playerEdits(view, flags);
	uint8_t *bytes = NULL;
	size_t length = 0;
	char error[256] = "";
	bool ok = fixtureView_write(
		buffer_string(json), json->length, flags, NULL, false, &bytes, &length, error, sizeof error
	);
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

// fixtureView_write's bytes against fixture_data.h and match_config.h's, step by step.
static void checkWrite(
	const ByteBuffer *json,
	const FixturePlayerEdits *edits,
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
		checkWrite(&json, NULL, NULL, binary != 0);
		checkWrite(&json, &edits, NULL, binary != 0);
	}
	// Nothing to apply: the JSON as it is.
	uint8_t *bytes = NULL;
	size_t length = 0;
	char error[256] = "";
	ASSERT_BOOL_EQ(
		fixtureView_write(
			buffer_string(&json), json.length, NULL, NULL, false, &bytes, &length, error, sizeof error
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
			checkWrite(&json, NULL, venue, binary != 0);
		}
		ASSERT_BOOL_EQ(
			fixtureView_write(
				buffer_string(&json), json.length, NULL, venue, false, &bytes, &length, error, sizeof error
			),
			true
		);
		ASSERT_BOOL_EQ(length != json.length || memcmp(bytes, json.data, length) != 0, true);
		fixtureView_free(bytes);
		venue[FIXTURE_VIEW_SURFACE].value = 2;
		ASSERT_BOOL_EQ(
			fixtureView_write(
				buffer_string(&json), json.length, NULL, venue, false, &bytes, &length, error, sizeof error
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
			buffer_string(&json), json.length, NULL, NULL, false, &out, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(out.length == json.length && memcmp(out.data, json.data, json.length) == 0, true);
	ASSERT_BOOL_EQ(fixtureEdit_apply("{", 1, NULL, NULL, true, &out, error, sizeof error), false);
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
	bool ok =
		fixtureView_write(json, length, players, venue, binary, &bytes, &written, error, sizeof error);
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
				buffer_string(&json), json.length, flags, NULL, true, &bytes, &length, text, sizeof text
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
		"fixture view opens and writes on a small stack", test_opens_and_writes_on_a_small_stack
	);
}
