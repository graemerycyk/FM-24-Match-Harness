// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_lineup.h"
#include "fixture_data.h"
#include "fixture_edit.h"
#include "fixture_samples.h"
#include "match_config.h"
#include "test.h"
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { SEPARATOR = 6 }; // the length of ",\n    ", between the lineup sample's fields
static const char wirtzMarked[] = "\"flags\": [\"mark a specific player\"], \"u32\": 4},\n    {\"u8\": ";
static const char fieldsKey[] = "\n  \"fields\": [";
static const char homeMarker[] = "\n  \"home_lineup\": true,\n  \"fields\": [";

static bool lineupSample(ByteBuffer *json) {
	buffer_clear(json);
	bool ok = fixtureSamples_lineup(json);
	ASSERT_BOOL_EQ(ok, true);
	return ok;
}

// The fixture read into a new FixtureData (NULL if it does not read).
static FixtureData *readData(const ByteBuffer *json) {
	FixtureData *data = malloc(sizeof *data);
	char error[256] = "";
	if (data && !fixtureData_read(buffer_string(json), json->length, data, error, sizeof error)) {
		fprintf(stderr, "%s\n", error);
		free(data);
		data = NULL;
	}
	ASSERT_BOOL_EQ(data != NULL, true);
	return data;
}

// Replaces the first `count` (0: every) occurrences of `old` in `text`; how many it replaced.
static unsigned replaceText(ByteBuffer *text, const char *old, const char *new, unsigned count) {
	ByteBuffer out = {0};
	const char *at = buffer_string(text);
	const char *end = at + text->length;
	size_t oldLength = strlen(old);
	unsigned replaced = 0;
	const char *found = NULL;
	while ((!count || replaced < count) && (found = strstr(at, old)) != NULL) {
		(void)buffer_append(&out, at, (size_t)(found - at));
		(void)buffer_append(&out, new, strlen(new));
		at = found + oldLength;
		++replaced;
	}
	(void)buffer_append(&out, at, (size_t)(end - at));
	buffer_free(text);
	*text = out;
	return replaced;
}

static void replaceOnce(ByteBuffer *text, const char *old, const char *new) {
	ASSERT_INT_EQ((int)replaceText(text, old, new, 1), 1);
}

static unsigned occurrences(const ByteBuffer *text, const char *part) {
	unsigned count = 0;
	for (const char *at = strstr(buffer_string(text), part); at; at = strstr(at + 1, part)) {
		++count;
	}
	return count;
}

// Where block `place` of the sample's `side` starts and ends (without the separator after it).
static bool blockSpan(const ByteBuffer *json, const char *side, unsigned place, size_t span[2]) {
	char start[64];
	char next[64];
	(void)snprintf(start, sizeof start, "{\"name\": \"%s.player[%u]\", \"player\"", side, place);
	(void)snprintf(next, sizeof next, "{\"name\": \"%s.player[%u]\", \"player\"", side, place + 1);
	const char *text = buffer_string(json);
	const char *from = strstr(text, start);
	const char *to = strstr(text, next);
	if (!to) {
		(void)snprintf(next, sizeof next, "{\"name\": \"%s.has_team_extra\"", side);
		to = strstr(text, next);
	}
	bool ok = from && to && to > from;
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		span[0] = (size_t)(from - text);
		span[1] = (size_t)(to - text) - SEPARATOR;
	}
	return ok;
}

// The text with `side`'s blocks a < b exchanged, renamed if `rename`, as blocks.py swap leaves them
// if not.
static bool exchangedText(
	const ByteBuffer *json,
	const char *side,
	unsigned a,
	unsigned b,
	bool rename,
	ByteBuffer *out
) {
	size_t spans[2][2];
	buffer_clear(out);
	if (!blockSpan(json, side, a, spans[0]) || !blockSpan(json, side, b, spans[1])) {
		return false;
	}
	ByteBuffer moved[2] = {{0}, {0}};
	for (unsigned i = 0; i < 2; ++i) {
		char old[32];
		char new[32];
		(void)snprintf(old, sizeof old, "\"%s.player[%u]", side, i ? b : a);
		(void)snprintf(new, sizeof new, "\"%s.player[%u]", side, i ? a : b);
		(void)buffer_append(&moved[i], json->data + spans[i][0], spans[i][1] - spans[i][0]);
		if (rename) {
			ASSERT_BOOL_EQ(replaceText(&moved[i], old, new, 0) > 5, true);
		}
	}
	bool ok = buffer_append(out, json->data, spans[0][0]) &&
						buffer_append(out, moved[1].data, moved[1].length) &&
						buffer_append(out, json->data + spans[0][1], spans[1][0] - spans[0][1]) &&
						buffer_append(out, moved[0].data, moved[0].length) &&
						buffer_append(out, json->data + spans[1][1], json->length - spans[1][1]);
	buffer_free(&moved[0]);
	buffer_free(&moved[1]);
	return ok;
}

static bool apply(
	const ByteBuffer *json,
	const FixtureLineup *lineup,
	ByteBuffer *out,
	FixtureLineupResult *result,
	char *error,
	size_t errorSize
) {
	error[0] = 0;
	bool ok = fixtureLineup_apply(
		buffer_string(json), json->length, lineup, out, result, error, errorSize
	);
	if (!ok) {
		ASSERT_BOOL_EQ(out->length == 0, true);
	}
	return ok;
}

static bool exchange(
	const ByteBuffer *json,
	unsigned side,
	unsigned a,
	unsigned b,
	ByteBuffer *out,
	FixtureLineupResult *result,
	char *error,
	size_t errorSize
) {
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, side, side ? 13 : 22, a, b), true);
	return apply(json, &lineup, out, result, error, errorSize);
}

static void countField(void *context, const ConfigField *field) {
	(void)field;
	++*(size_t *)context;
}

static size_t fieldCount(const ByteBuffer *json) {
	size_t count = 0;
	ConfigVisitor visitor = {.context = &count, .field = countField};
	char error[256] = "";
	bool ok = matchConfig_visit(buffer_string(json), json->length, &visitor, error, sizeof error);
	ASSERT_BOOL_EQ(ok, true);
	return count;
}

// Where the encoded bytes of each home block start (then where the blocks end), and of the u8 after
// away's marking of slot 9.
typedef struct Layout {
	size_t at;
	size_t blocks[FIXTURE_PLAYERS + 1];
	unsigned count;
	size_t marking;
	bool markingNext;
} Layout;

static void layoutField(void *context, const ConfigField *field) {
	Layout *layout = context;
	if (layout->markingNext) {
		layout->marking = layout->at;
		layout->markingNext = false;
	}
	unsigned place = 0;
	int length = 0;
	if (sscanf(field->name, "home.player[%u]%n", &place, &length) == 1 && !field->name[length]) {
		layout->blocks[layout->count++] = layout->at;
	} else if (strcmp(field->name, "home.has_team_extra") == 0) {
		layout->blocks[layout->count] = layout->at;
	}
	layout->markingNext = strncmp(field->name, "away.manager.tactic.plan[0].slot[9].", 36) == 0 &&
												strstr(field->name, ".bits[1]") != NULL;
	layout->at += field->length;
}

TEST(reads_person_ids_ratings_and_the_bench) {
	ByteBuffer json = {0};
	FixtureData *data = lineupSample(&json) ? readData(&json) : NULL;
	if (data) {
		const FixtureTeam *home = &data->teams[0];
		const FixtureTeam *away = &data->teams[1];
		ASSERT_BOOL_EQ(home->lineupKnown && !away->lineupKnown, true);
		ASSERT_INT_EQ((int)home->benchSize, 9);
		ASSERT_INT_EQ((int)away->benchSize, 2);
		ASSERT_INT_EQ(home->goalkeeperSlot, 0);
		ASSERT_BOOL_EQ(home->blocksInOrder && away->blocksInOrder, true);
		ASSERT_INT_EQ((int)home->blockCount, 22);
		ASSERT_INT_EQ((int)away->blockCount, 13);
		ASSERT_INT_EQ(strcmp(home->players[18].name, "Luis D\xC3\xAD" "az"), 0);
		ASSERT_BOOL_EQ(home->players[19].personUid.known, true);
		ASSERT_BOOL_EQ(home->players[19].personUid.value == 91193048, true);
		ASSERT_BOOL_EQ(away->players[12].personUid.value == fixtureSamples_lineupId(1, 12), true);
		ASSERT_BOOL_EQ(home->players[11].positionRatingsKnown, true);
		ASSERT_INT_EQ(home->players[11].positionRatings[0], 20);
		ASSERT_INT_EQ(home->players[10].positionRatings[0], 1);
		ASSERT_INT_EQ(home->players[10].positionRatings[4], 1 + (10 + 3 * 4) % 20);
	}
	free(data);
	// Without a formation every player after the 11th is a substitute. The older samples' first slot
	// is the goalkeeper's, and their blocks have ratings but no unique IDs.
	size_t length = 0;
	char *labelled = fixtureSamples_json(FIXTURE_SAMPLE_LABELLED, &length);
	ByteBuffer text = {0};
	if (labelled) {
		(void)buffer_append(&text, labelled, length);
	}
	free(labelled);
	data = readData(&text);
	if (data) {
		ASSERT_INT_EQ((int)data->teams[0].benchSize, 1);
		ASSERT_INT_EQ(data->teams[0].goalkeeperSlot, 0);
		ASSERT_BOOL_EQ(data->teams[0].blocksInOrder, true);
		ASSERT_BOOL_EQ(data->teams[0].players[0].personUid.known, false);
		ASSERT_BOOL_EQ(data->teams[0].players[0].positionRatingsKnown, true);
	}
	free(data);
	// A bench template of 0 is no substitute, and there are never more than the players after 11.
	replaceOnce(
		&json,
		"\"home.manager.tactic.formation[19]\", \"u8\": 2},\n    {\"u32\": 114688}",
		"\"home.manager.tactic.formation[19]\", \"u8\": 2},\n    {\"u32\": 0}"
	);
	replaceOnce(&json, "\"away.player_count\", \"u8\": 13", "\"away.player_count\", \"u8\": 12");
	replaceOnce(&json, "{\"name\": \"away.player[12]\"", "{\"name\": \"away.extra\"");
	// A goalkeeper's slot that is not the first.
	replaceOnce(
		&json,
		"\"home.manager.tactic.plan[0].slot[0].position\", \"u32\": 1}",
		"\"home.manager.tactic.plan[0].slot[0].position\", \"u32\": 4}"
	);
	replaceOnce(
		&json,
		"\"home.manager.tactic.plan[0].slot[1].position\", \"u32\": 4}",
		"\"home.manager.tactic.plan[0].slot[1].position\", \"u32\": 1}"
	);
	data = readData(&json);
	if (data) {
		ASSERT_INT_EQ((int)data->teams[0].benchSize, 8);
		ASSERT_INT_EQ((int)data->teams[1].benchSize, 1);
		ASSERT_INT_EQ(data->teams[0].goalkeeperSlot, 1);
	}
	free(data);
	buffer_free(&text);
	buffer_free(&json);
}

typedef struct SpanCollector {
	size_t count;
	size_t object[2][2];
	size_t name[2][2];
} SpanCollector;

static void collectSpans(void *context, const ConfigField *field) {
	SpanCollector *spans = context;
	if (spans->count < 2) {
		spans->object[spans->count][0] = field->objectOffset;
		spans->object[spans->count][1] = field->objectLength;
		spans->name[spans->count][0] = field->nameOffset;
		spans->name[spans->count][1] = field->nameLength;
	}
	++spans->count;
}

TEST(reads_field_spans_and_lineup_markers) {
	static const char config[] = "{\"format\": \"fm24-match-config/1\", \"fields\": [\n"
															 "  { \"u8\": 1 , \"name\" : \"home.player[3]\" } ,\n"
															 "  {\"u16\": 2}\n"
															 "], \"home_lineup\": true, \"away_lineup\": false}";
	static const char first[] = "{ \"u8\": 1 , \"name\" : \"home.player[3]\" }";
	static const char second[] = "{\"u16\": 2}";
	static const char name[] = "\"home.player[3]\"";
	SpanCollector spans = {0};
	ConfigVisitor visitor = {.context = &spans, .field = collectSpans};
	char error[256] = "";
	ASSERT_BOOL_EQ(matchConfig_visit(config, sizeof config - 1, &visitor, error, sizeof error), true);
	ASSERT_INT_EQ((int)spans.count, 2);
	ASSERT_INT_EQ((int)spans.object[0][0], (int)(strstr(config, first) - config));
	ASSERT_INT_EQ((int)spans.object[0][1], (int)(sizeof first - 1));
	ASSERT_INT_EQ((int)spans.name[0][0], (int)(strstr(config, name) - config));
	ASSERT_INT_EQ((int)spans.name[0][1], (int)(sizeof name - 1));
	ASSERT_INT_EQ((int)spans.object[1][0], (int)(strstr(config, second) - config));
	ASSERT_INT_EQ((int)spans.object[1][1], (int)(sizeof second - 1));
	ASSERT_INT_EQ((int)spans.name[1][1], 0);
	ConfigOrigin origin;
	ASSERT_BOOL_EQ(matchConfig_readOrigin(config, sizeof config - 1, &origin), true);
	ASSERT_BOOL_EQ(origin.lineup[0] && !origin.lineup[1], true);
	// Only a top-level true is a marker, and the last of a key counts.
	static const struct {
		const char *text;
		bool home;
		bool away;
	} markers[] = {
		{"{\"away_lineup\":\ttrue , \"home_lineup\": 1}", false, true},
		{"{\"home_lineup\": \"true\", \"away_lineup\": [true]}", false, false},
		{"{\"home_lineup\": true, \"home_lineup\": false}", false, false},
		{"{\"home_lineup\": false, \"home_lineup\": true}", true, false},
		{"{\"fields\": [{\"home_lineup\": true, \"u8\": 1}]}", false, false},
	};
	for (size_t i = 0; i < sizeof markers / sizeof markers[0]; ++i) {
		ASSERT_BOOL_EQ(matchConfig_readOrigin(markers[i].text, strlen(markers[i].text), &origin), true);
		ASSERT_BOOL_EQ(origin.lineup[0], markers[i].home);
		ASSERT_BOOL_EQ(origin.lineup[1], markers[i].away);
	}
}

TEST(exchanges_whole_blocks_renamed_to_their_places) {
	ByteBuffer json = {0};
	ByteBuffer out = {0};
	ByteBuffer expected = {0};
	ByteBuffer back = {0};
	FixtureLineupResult result;
	char error[512];
	if (!lineupSample(&json)) {
		return;
	}
	ASSERT_BOOL_EQ(exchange(&json, 0, 10, 19, &out, &result, error, sizeof error), true);
	if (error[0]) {
		fprintf(stderr, "%s\n", error);
	}
	// Every other byte is the input's: only the two blocks, their names' numbers, away's marking of
	// Florian Wirtz and the marker change.
	ASSERT_BOOL_EQ(exchangedText(&json, "home", 10, 19, true, &expected), true);
	char marked[sizeof wirtzMarked + 8];
	char remarked[sizeof wirtzMarked + 8];
	(void)snprintf(marked, sizeof marked, "%s19}", wirtzMarked);
	(void)snprintf(remarked, sizeof remarked, "%s10}", wirtzMarked);
	replaceOnce(&expected, marked, remarked);
	replaceOnce(&expected, fieldsKey, homeMarker);
	ASSERT_INT_EQ((int)out.length, (int)expected.length);
	ASSERT_INT_EQ(strcmp(buffer_string(&out), buffer_string(&expected)), 0);
	ASSERT_BOOL_EQ(fieldCount(&out) == fieldCount(&json), true);
	ASSERT_INT_EQ((int)result.markings[0], 1);
	ASSERT_INT_EQ((int)result.markings[1], 0);
	// The marked player's new place.
	for (unsigned place = 0; place < FIXTURE_PLAYERS; ++place) {
		ASSERT_BOOL_EQ(result.marked[0][place], place == 10);
		ASSERT_BOOL_EQ(result.marked[1][place], false);
	}
	ASSERT_BOOL_EQ(result.opponentRecords[0] || result.opponentRecords[1], false);
	ASSERT_INT_EQ((int)occurrences(&out, "\"home_lineup\""), 1);
	ASSERT_INT_EQ((int)occurrences(&out, "\"away_lineup\""), 0);
	FixtureData *data = readData(&out);
	if (data) {
		const FixtureTeam *home = &data->teams[0];
		ASSERT_BOOL_EQ(home->blocksInOrder, true);
		ASSERT_INT_EQ(strcmp(home->players[10].name, "Florian Wirtz"), 0);
		ASSERT_BOOL_EQ(home->players[10].personUid.value == 91193048, true);
		ASSERT_INT_EQ(strcmp(home->players[19].name, "Ryan Gravenberch"), 0);
		ASSERT_INT_EQ(strcmp(home->players[9].name, "Cody Gakpo"), 0);
	}
	free(data);
	ConfigOrigin origin;
	ASSERT_BOOL_EQ(matchConfig_readOrigin(buffer_string(&out), out.length, &origin), true);
	ASSERT_BOOL_EQ(origin.lineup[0] && !origin.lineup[1], true);
	// Exchanging back restores the input but for the marker, which is not added again.
	ASSERT_BOOL_EQ(exchange(&out, 0, 19, 10, &back, &result, error, sizeof error), true);
	ByteBuffer variant = {0};
	(void)buffer_append(&variant, json.data, json.length);
	replaceOnce(&variant, fieldsKey, homeMarker);
	ASSERT_INT_EQ(strcmp(buffer_string(&back), buffer_string(&variant)), 0);
	ASSERT_INT_EQ((int)result.markings[0], 1);
	// A marker that is not true becomes true where it is.
	buffer_clear(&variant);
	(void)buffer_append(&variant, json.data, json.length);
	replaceOnce(&variant, "\n  ]\n}\n", "\n  ],\n  \"home_lineup\": false\n}\n");
	ASSERT_BOOL_EQ(exchange(&variant, 0, 10, 19, &back, &result, error, sizeof error), true);
	ASSERT_INT_EQ((int)occurrences(&back, "\"home_lineup\""), 1);
	ASSERT_BOOL_EQ(strstr(buffer_string(&back), "\n  ],\n  \"home_lineup\": true\n}\n") != NULL, true);
	// Both sides at once: each is marked, and away's marking follows home's player.
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 1, 13, 1, 2), true);
	ASSERT_BOOL_EQ(apply(&json, &lineup, &back, &result, error, sizeof error), true);
	ASSERT_BOOL_EQ(exchangedText(&expected, "away", 1, 2, true, &variant), true);
	replaceOnce(&variant, homeMarker, "\n  \"home_lineup\": true,\n  \"away_lineup\": true,\n  \"fields\": [");
	ASSERT_INT_EQ(strcmp(buffer_string(&back), buffer_string(&variant)), 0);
	ASSERT_BOOL_EQ(result.markings[0] == 1 && result.markings[1] == 0, true);
	buffer_free(&variant);
	buffer_free(&json);
	buffer_free(&out);
	buffer_free(&expected);
	buffer_free(&back);
}

TEST(encodes_the_exchange_as_the_blocks_bytes_exchanged) {
	ByteBuffer json = {0};
	ByteBuffer out = {0};
	ByteBuffer before = {0};
	ByteBuffer after = {0};
	ByteBuffer expected = {0};
	FixtureLineupResult result;
	char error[512];
	if (!lineupSample(&json) || !exchange(&json, 0, 10, 19, &out, &result, error, sizeof error)) {
		ASSERT_BOOL_EQ(false, true);
		buffer_free(&json);
		return;
	}
	Layout layout = {0};
	ConfigVisitor visitor = {.context = &layout, .field = layoutField};
	ASSERT_BOOL_EQ(matchConfig_visit(buffer_string(&json), json.length, &visitor, error, sizeof error), true);
	ASSERT_INT_EQ((int)layout.count, 22);
	ASSERT_BOOL_EQ(
		matchConfig_encode(buffer_string(&json), json.length, &before, NULL, error, sizeof error), true
	);
	ASSERT_BOOL_EQ(
		matchConfig_encode(buffer_string(&out), out.length, &after, NULL, error, sizeof error), true
	);
	const size_t *at = layout.blocks;
	bool ok = layout.count == 22 && layout.marking > at[22] && layout.marking < before.length &&
						before.data[layout.marking] == 19 &&
						buffer_append(&expected, before.data, at[10]) &&
						buffer_append(&expected, before.data + at[19], at[20] - at[19]) &&
						buffer_append(&expected, before.data + at[11], at[19] - at[11]) &&
						buffer_append(&expected, before.data + at[10], at[11] - at[10]) &&
						buffer_append(&expected, before.data + at[20], before.length - at[20]);
	ASSERT_BOOL_EQ(ok, true);
	if (ok) {
		expected.data[layout.marking] = 10;
		ASSERT_BOOL_EQ(
			after.length == expected.length && memcmp(after.data, expected.data, after.length) == 0, true
		);
	}
	buffer_free(&json);
	buffer_free(&out);
	buffer_free(&before);
	buffer_free(&after);
	buffer_free(&expected);
}

// Where the value of `side`'s marking for `slot` (the u8 after its bits[1], the place of the
// opponent's player it marks) starts in the sample's text, or 0 if it has none.
static size_t markingAt(const ByteBuffer *json, const char *side, unsigned slot) {
	char name[80];
	(void)snprintf(
		name, sizeof name, "\"%s.manager.tactic.plan[0].slot[%u].player_instructions[", side, slot
	);
	const char *text = buffer_string(json);
	const char *at = strstr(text, name);
	at = at ? strstr(at, ".bits[1]\"") : NULL;
	at = at ? strstr(at, "{\"u8\": ") : NULL;
	ASSERT_BOOL_EQ(at != NULL, true);
	return at ? (size_t)(at - text) + 7 : 0;
}

static unsigned marking(const ByteBuffer *json, const char *side, unsigned slot) {
	size_t at = markingAt(json, side, slot);
	return at ? (unsigned)strtoul(buffer_string(json) + at, NULL, 10) : 999;
}

static void setMarking(ByteBuffer *json, const char *side, unsigned slot, unsigned value) {
	size_t at = markingAt(json, side, slot);
	if (!at) {
		return;
	}
	ByteBuffer out = {0};
	char number[8];
	(void)snprintf(number, sizeof number, "%u", value);
	size_t end = at + strspn(buffer_string(json) + at, "0123456789");
	bool ok = buffer_append(&out, json->data, at) && buffer_append(&out, number, strlen(number)) &&
						buffer_append(&out, json->data + end, json->length - end);
	ASSERT_BOOL_EQ(ok, true);
	buffer_free(json);
	*json = out;
}

// The sides' places of the players marked in `result`, as a bit per place below 32.
static unsigned markedPlaces(const FixtureLineupResult *result, unsigned side) {
	unsigned places = 0;
	for (unsigned place = 0; place < FIXTURE_PLAYERS; ++place) {
		if (result->marked[side][place]) {
			places |= place < 32 ? 1U << place : 1U << 31;
		}
	}
	return places;
}

// A marking follows the player it marks to the place the order gives that player, whichever side
// moves; a marking of no place the side has, or of a player who stays, is left as it is.
TEST(repoints_markings_to_where_each_player_moves) {
	ByteBuffer json = {0};
	ByteBuffer out = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512] = "";
	if (!lineupSample(&json)) {
		return;
	}
	// Away marks Gravenberch (10) with slot 0, Haaland (21) with slot 1, Wirtz (19) with slot 9 as
	// the sample has it, and place 22, which home lacks, with slot 2. Home marks Trippier (1) with
	// slot 0, Miley (12) with slot 1, place 13, which away lacks, with slot 2, and Guimaraes (5) with
	// slot 3.
	static const unsigned awayMarks[4][2] = {{0, 10}, {1, 21}, {2, 22}, {9, 19}};
	static const unsigned homeMarks[4][2] = {{0, 1}, {1, 12}, {2, 13}, {3, 5}};
	for (unsigned i = 0; i < 4; ++i) {
		setMarking(&json, "away", awayMarks[i][0], awayMarks[i][1]);
		setMarking(&json, "home", homeMarks[i][0], homeMarks[i][1]);
	}
	// A cycle of three, which no exchange can stand for: Wirtz 19 to 10, Haaland 21 to 19 and
	// Gravenberch 10 to 21.
	fixtureLineup_identity(&lineup);
	lineup.from[0][10] = 19;
	lineup.from[0][19] = 21;
	lineup.from[0][21] = 10;
	lineup.changed[0] = true;
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	static const unsigned homeMoved[4] = {21, 19, 22, 10};
	for (unsigned i = 0; i < 4; ++i) {
		ASSERT_INT_EQ((int)marking(&out, "away", awayMarks[i][0]), (int)homeMoved[i]);
		ASSERT_INT_EQ((int)marking(&out, "home", homeMarks[i][0]), (int)homeMarks[i][1]);
	}
	ASSERT_INT_EQ((int)marking(&out, "away", 3), 255);
	ASSERT_BOOL_EQ(result.markings[0] == 3 && result.markings[1] == 0, true);
	ASSERT_INT_EQ((int)markedPlaces(&result, 0), (1 << 10) | (1 << 19) | (1 << 21));
	ASSERT_INT_EQ((int)markedPlaces(&result, 1), 0);
	FixtureData *data = readData(&out);
	if (data) {
		ASSERT_INT_EQ(strcmp(data->teams[0].players[10].name, "Florian Wirtz"), 0);
		ASSERT_INT_EQ(strcmp(data->teams[0].players[19].name, "Erling Haaland"), 0);
		ASSERT_INT_EQ(strcmp(data->teams[0].players[21].name, "Ryan Gravenberch"), 0);
	}
	free(data);
	ASSERT_BOOL_EQ(fieldCount(&out) == fieldCount(&json), true);

	// Away reordered in a cycle (Botman 2 to 1, Miley 12 to 2, Trippier 1 to 12): home's markings
	// follow, and away's of home stay.
	fixtureLineup_identity(&lineup);
	lineup.from[1][1] = 2;
	lineup.from[1][2] = 12;
	lineup.from[1][12] = 1;
	lineup.changed[1] = true;
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	static const unsigned awayMoved[4] = {12, 2, 13, 5};
	for (unsigned i = 0; i < 4; ++i) {
		ASSERT_INT_EQ((int)marking(&out, "home", homeMarks[i][0]), (int)awayMoved[i]);
		ASSERT_INT_EQ((int)marking(&out, "away", awayMarks[i][0]), (int)awayMarks[i][1]);
	}
	ASSERT_BOOL_EQ(result.markings[0] == 0 && result.markings[1] == 2, true);
	ASSERT_INT_EQ((int)markedPlaces(&result, 0), 0);
	ASSERT_INT_EQ((int)markedPlaces(&result, 1), (1 << 2) | (1 << 12));
	ASSERT_BOOL_EQ(strstr(buffer_string(&out), "\"away_lineup\": true") != NULL, true);

	// Both at once: each side's markings follow the other's players.
	lineup.from[0][10] = 19;
	lineup.from[0][19] = 21;
	lineup.from[0][21] = 10;
	lineup.changed[0] = true;
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	for (unsigned i = 0; i < 4; ++i) {
		ASSERT_INT_EQ((int)marking(&out, "away", awayMarks[i][0]), (int)homeMoved[i]);
		ASSERT_INT_EQ((int)marking(&out, "home", homeMarks[i][0]), (int)awayMoved[i]);
	}
	ASSERT_BOOL_EQ(result.markings[0] == 3 && result.markings[1] == 2, true);
	if (error[0]) {
		fprintf(stderr, "%s\n", error);
	}
	buffer_free(&json);
	buffer_free(&out);
}

TEST(promotes_a_list_with_least_movement) {
	FixtureLineup lineup;
	FixtureLineup exchanged;
	char error[256] = "";
	unsigned listed[22];
	for (unsigned i = 0; i < 22; ++i) {
		listed[i] = i;
	}
	// Florian Wirtz for Ryan Gravenberch: the exchange.
	fixtureLineup_identity(&lineup);
	fixtureLineup_identity(&exchanged);
	listed[10] = 19;
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), true);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&exchanged, 0, 22, 10, 19), true);
	ASSERT_BOOL_EQ(memcmp(&lineup, &exchanged, sizeof lineup) == 0, true);
	ASSERT_BOOL_EQ(lineup.changed[0] && !lineup.changed[1], true);
	// A reserve promoted: the displaced starter takes the reserve's place, where nobody comes on.
	fixtureLineup_identity(&lineup);
	listed[10] = 21;
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), true);
	ASSERT_INT_EQ(lineup.from[0][10], 21);
	ASSERT_INT_EQ(lineup.from[0][21], 10);
	ASSERT_INT_EQ(lineup.from[0][19], 19);
	// The XI and the first substitutes: displaced players fill the left places in old-place order.
	fixtureLineup_identity(&lineup);
	unsigned longer[13] = {1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 19, 21, 12};
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, longer, 13, error, sizeof error), true);
	static const unsigned order[22] = {
		1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 19, 21, 12, 13, 14, 15, 16, 17, 18, 10, 20, 11};
	for (unsigned place = 0; place < 22; ++place) {
		ASSERT_INT_EQ(lineup.from[0][place], (int)order[place]);
	}
	// On an order already changed, the list names the places as they are.
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 0, 1), true);
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), true);
	ASSERT_INT_EQ(lineup.from[0][0], 0);
	ASSERT_INT_EQ(lineup.from[0][10], 11);
	ASSERT_INT_EQ(lineup.from[0][21], 19);
	// Too short, a place twice or out of range: unchanged.
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 10, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "give the XI in slot order, then any substitutes"), 0);
	listed[10] = 3;
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "home.player[3] is listed twice"), 0);
	listed[10] = 22;
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "the fixture has no home.player[22]"), 0);
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 1, 13, listed, 14, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "away has only 13 players"), 0);
	ASSERT_BOOL_EQ(lineup.changed[0] || lineup.changed[1], false);
	// Exchanges need two places of the side.
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 4, 4), false);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 4, 22), false);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 2, 22, 4, 5), false);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 1, 13, 4, 5), true);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 1, 13, 5, 4), true);
	ASSERT_BOOL_EQ(lineup.changed[1], false);
}

static int find(const FixtureData *data, unsigned side, const char *text, char *error, size_t size) {
	unsigned place = 999;
	error[0] = 0;
	return fixtureLineup_find(data, side, text, &place, error, size) ? (int)place : -1;
}

TEST(finds_players_by_place_id_and_name) {
	ByteBuffer json = {0};
	FixtureData *data = lineupSample(&json) ? readData(&json) : NULL;
	char error[512];
	if (!data) {
		buffer_free(&json);
		return;
	}
	ASSERT_INT_EQ(find(data, 0, "player[3]", error, sizeof error), 3);
	ASSERT_INT_EQ(find(data, 0, " home.player[21] ", error, sizeof error), 21);
	ASSERT_INT_EQ(find(data, 1, "away.player[12]", error, sizeof error), 12);
	ASSERT_INT_EQ(find(data, 0, "away.player[3]", error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "away.player[3] is not on the home side"), 0);
	ASSERT_INT_EQ(find(data, 1, "player[13]", error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "the fixture has no away.player[13]"), 0);
	ASSERT_INT_EQ(find(data, 0, "player[1x]", error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "'player[1x]' is not a place: use player[N], counting from 0"), 0);
	ASSERT_INT_EQ(find(data, 0, "91193048", error, sizeof error), 19);
	ASSERT_INT_EQ(find(data, 1, "91193048", error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "no away player has the unique ID 91193048"), 0);
	ASSERT_INT_EQ(find(data, 0, "florian WIRTZ", error, sizeof error), 19);
	ASSERT_INT_EQ(find(data, 0, "Wirtz", error, sizeof error), 19);
	ASSERT_INT_EQ(find(data, 0, "gravenb", error, sizeof error), 10);
	ASSERT_INT_EQ(find(data, 0, "Luis D\xC3\xAD" "az", error, sizeof error), 18);
	ASSERT_INT_EQ(find(data, 0, "D\xC3\x8D" "AZ", error, sizeof error), -1);
	ASSERT_INT_EQ(find(data, 0, "Diaz", error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "no home player matches 'Diaz'"), 0);
	ASSERT_INT_EQ(find(data, 0, "Jones", error, sizeof error), -1);
	ASSERT_INT_EQ(
		strcmp(
			error,
			"'Jones' matches more than one home player: player[14] Lewis Jones (28100917), "
			"player[17] Curtis Jones (28115788); use more of the name, the ID or player[N]"
		),
		0
	);
	ASSERT_INT_EQ(find(data, 0, "curtis jones", error, sizeof error), 17);
	ASSERT_INT_EQ(find(data, 0, "", error, sizeof error), -1);
	// A place's syntax alone, without the fixture, as `fm24ctl lineup` checks it first.
	unsigned place = 0;
	ASSERT_INT_EQ(fixtureLineup_readPlace("home.player[30]", 0, &place, error, sizeof error), 1);
	ASSERT_INT_EQ((int)place, 30);
	ASSERT_INT_EQ(fixtureLineup_readPlace("player[7]", 1, &place, error, sizeof error), 1);
	ASSERT_INT_EQ((int)place, 7);
	ASSERT_INT_EQ(fixtureLineup_readPlace("Wirtz", 0, &place, error, sizeof error), 0);
	ASSERT_INT_EQ(fixtureLineup_readPlace("91193048", 0, &place, error, sizeof error), 0);
	ASSERT_INT_EQ(fixtureLineup_readPlace("home.player", 0, &place, error, sizeof error), 0);
	ASSERT_INT_EQ(fixtureLineup_readPlace("player[]", 0, &place, error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "'player[]' is not a place: use player[N], counting from 0"), 0);
	ASSERT_INT_EQ(fixtureLineup_readPlace("player[1234567890]", 0, &place, error, sizeof error), -1);
	ASSERT_INT_EQ(fixtureLineup_readPlace("away.player[1]", 0, &place, error, sizeof error), -1);
	ASSERT_INT_EQ(strcmp(error, "away.player[1] is not on the home side"), 0);
	ASSERT_INT_EQ(find(data, 0, "Joelinton", error, sizeof error), -1);
	ASSERT_INT_EQ(find(data, 1, "Joelinton", error, sizeof error), 7);
	free(data);
	// A whole name before a part of another, and an ID an --allow-duplicate copy shares.
	replaceOnce(&json, "\"player\": \"Ben Doak\"", "\"player\": \"Florian Wirtz Jr\"");
	ASSERT_INT_EQ((int)replaceText(&json, "\"u32\": 2000164850}", "\"u32\": 91193048}", 0), 2);
	data = readData(&json);
	if (data) {
		ASSERT_INT_EQ(find(data, 0, "Florian Wirtz", error, sizeof error), 19);
		ASSERT_INT_EQ(find(data, 0, "jr", error, sizeof error), 20);
		ASSERT_INT_EQ(find(data, 0, "91193048", error, sizeof error), -1);
		ASSERT_INT_EQ(
			strcmp(
				error,
				"'91193048' matches more than one home player: player[19] Florian Wirtz (91193048), "
				"player[20] Florian Wirtz Jr (91193048); use player[N]"
			),
			0
		);
	}
	free(data);
	buffer_free(&json);
}

static bool check(
	const FixtureData *data,
	unsigned side,
	const FixtureLineup *lineup,
	bool allowAiSide,
	char *error,
	char *warnings
) {
	error[0] = 0;
	return fixtureLineup_check(data, side, lineup, allowAiSide, error, 512, warnings, 512);
}

TEST(refuses_ai_sides_and_moved_blocks) {
	ByteBuffer json = {0};
	ByteBuffer moved = {0};
	ByteBuffer out = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512];
	char warnings[512];
	FixtureData *data = lineupSample(&json) ? readData(&json) : NULL;
	if (!data) {
		buffer_free(&json);
		return;
	}
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 1, 13, 1, 2), true);
	ASSERT_BOOL_EQ(check(data, 1, &lineup, false, error, warnings), false);
	ASSERT_INT_EQ(
		strcmp(
			error,
			"away is managed by FM24's AI, which picks its own XI at kick-off; use --allow-ai-side to "
			"reorder it anyway"
		),
		0
	);
	ASSERT_BOOL_EQ(check(data, 1, &lineup, true, error, warnings), true);
	ASSERT_INT_EQ(
		strcmp(warnings, "away is managed by FM24's AI, which picks its own XI at kick-off\n"), 0
	);
	ASSERT_BOOL_EQ(check(data, 0, &lineup, false, error, warnings), true);
	ASSERT_INT_EQ(warnings[0], 0);
	free(data);
	// An away exchange keeps home's blocks, and re-points nothing: home marks nobody.
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	ASSERT_BOOL_EQ(exchangedText(&json, "away", 1, 2, true, &moved), true);
	replaceOnce(&moved, fieldsKey, "\n  \"away_lineup\": true,\n  \"fields\": [");
	ASSERT_INT_EQ(strcmp(buffer_string(&out), buffer_string(&moved)), 0);
	ASSERT_INT_EQ((int)result.markings[1], 0);
	// Blocks that blocks.py moved without renaming them.
	ASSERT_BOOL_EQ(exchangedText(&json, "home", 10, 19, false, &moved), true);
	data = readData(&moved);
	if (data) {
		ASSERT_BOOL_EQ(data->teams[0].blocksInOrder, false);
		ASSERT_INT_EQ((int)data->teams[0].misplacedBlock, 19);
		ASSERT_INT_EQ((int)data->teams[0].misplacedPlace, 10);
		ASSERT_BOOL_EQ(data->teams[1].blocksInOrder, true);
		fixtureLineup_identity(&lineup);
		ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 3, 4), true);
		ASSERT_BOOL_EQ(check(data, 0, &lineup, false, error, warnings), false);
		ASSERT_INT_EQ(
			strcmp(error, "home.player[19] is in place 10: the blocks were moved without renaming them"),
			0
		);
	}
	free(data);
	ASSERT_BOOL_EQ(apply(&moved, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(
		strcmp(error, "home.player[19] is in place 10: the blocks were moved without renaming them"), 0
	);
	// The other side can still be reordered.
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 1, 13, 1, 2), true);
	ASSERT_BOOL_EQ(apply(&moved, &lineup, &out, &result, error, sizeof error), true);
	buffer_free(&json);
	buffer_free(&moved);
	buffer_free(&out);
}

TEST(refuses_blocks_it_cannot_move) {
	ByteBuffer json = {0};
	ByteBuffer changed = {0};
	ByteBuffer out = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512];
	char warnings[512];
	if (!lineupSample(&json)) {
		return;
	}
	// More blocks than player_count says.
	(void)buffer_append(&changed, json.data, json.length);
	replaceOnce(&changed, "\"home.player_count\", \"u8\": 22", "\"home.player_count\", \"u8\": 21");
	FixtureData *data = readData(&changed);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 21, 3, 4), true);
	if (data) {
		ASSERT_BOOL_EQ(check(data, 0, &lineup, false, error, warnings), false);
		ASSERT_INT_EQ(strcmp(error, "home has 22 player blocks, but its player_count is 21"), 0);
	}
	free(data);
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "home has 22 player blocks, but its player_count is 21"), 0);
	// Blocks whose header differs move only if neither is moved.
	buffer_clear(&changed);
	(void)buffer_append(&changed, json.data, json.length);
	replaceOnce(
		&changed,
		"\"home.player[19]\", \"player\": \"Florian Wirtz\", \"u8\": 255},\n    {\"u16\": 0},\n    {\"u8\": 0},"
		"\n    {\"u8\": 0},\n    {\"u16\": 58}",
		"\"home.player[19]\", \"player\": \"Florian Wirtz\", \"u8\": 255},\n    {\"u16\": 0},\n    {\"u8\": 0},"
		"\n    {\"u8\": 0},\n    {\"u16\": 59}"
	);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(
		strcmp(
			error,
			"home.player[10] and player[19] start with different values; lineup cannot move them"
		),
		0
	);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 3, 4), true);
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), true);
	// A name inside a block that is not the block's.
	buffer_clear(&changed);
	(void)buffer_append(&changed, json.data, json.length);
	replaceOnce(&changed, "\"home.player[12].squad_number\"", "\"home.player[13].squad_number\"");
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(
		strcmp(error, "unexpected home.player[13].squad_number in the block of home.player[12]"), 0
	);
	// A name written with escapes, which the text does not spell.
	buffer_clear(&changed);
	(void)buffer_append(&changed, json.data, json.length);
	replaceOnce(&changed, "\"home.player[4].morale\"", "\"home.pl\\u0061yer[4].morale\"");
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "home.player[4].morale is written with escapes; lineup cannot rename it"), 0);
	// A block apart from the others.
	buffer_clear(&changed);
	(void)buffer_append(&changed, json.data, json.length);
	replaceOnce(
		&changed, ",\n    {\"name\": \"home.player[21]\"", ",\n    {\"name\": \"home.break\", \"u8\": 0},\n    {\"name\": \"home.player[21]\""
	);
	ASSERT_BOOL_EQ(apply(&changed, &lineup, &out, &result, error, sizeof error), false);
	ASSERT_INT_EQ(strcmp(error, "home.player[21] is apart from the other home player blocks"), 0);
	buffer_free(&json);
	buffer_free(&changed);
	buffer_free(&out);
}

TEST(warns_about_goalkeepers_and_opposition_records) {
	ByteBuffer json = {0};
	ByteBuffer out = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512];
	char warnings[512];
	FixtureData *data = lineupSample(&json) ? readData(&json) : NULL;
	if (!data) {
		buffer_free(&json);
		return;
	}
	static const struct {
		unsigned a;
		unsigned b;
		const char *warnings;
	} cases[] = {
		{10, 19, ""},
		{0, 1,
			"Conor Bradley is in goal (slot 0) but is not a goalkeeper (GK 1)\n"
			"Alisson Becker, a goalkeeper, is in slot 1 (DR)\n"},
		{11, 20, "no goalkeeper among the substitutes\n"},
		{11, 12, ""},
		{5, 11,
			"Caoimhin Kelleher, a goalkeeper, is in slot 5 (DMCR)\nno goalkeeper among the substitutes\n"},
		{0, 11, ""},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		fixtureLineup_identity(&lineup);
		ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, cases[i].a, cases[i].b), true);
		ASSERT_BOOL_EQ(check(data, 0, &lineup, false, error, warnings), true);
		ASSERT_INT_EQ(strcmp(warnings, cases[i].warnings), 0);
	}
	free(data);
	// An opponent's part_4240 records that are not all alike are reported, and left as they are.
	ASSERT_INT_EQ(
		(int)replaceText(
			&json,
			"{\"u32\": 26},\n    {\"u32\": 0}",
			"{\"u32\": 26},\n    {\"u32\": 2}",
			0
		),
		2
	);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	ASSERT_BOOL_EQ(result.opponentRecords[0] && !result.opponentRecords[1], true);
	ASSERT_INT_EQ((int)occurrences(&out, "{\"u32\": 26},\n    {\"u32\": 2}"), 2);
	buffer_clear(&json);
	ASSERT_BOOL_EQ(lineupSample(&json), true);
	replaceOnce(
		&json,
		"{\"u8\": 1},\n    {\"name\": \"away.manager.tactic.plan[0]\"",
		"{\"u8\": 2},\n    {\"name\": \"away.manager.tactic.plan[0]\""
	);
	ASSERT_BOOL_EQ(apply(&json, &lineup, &out, &result, error, sizeof error), true);
	ASSERT_BOOL_EQ(result.opponentRecords[0], true);
	buffer_free(&json);
	buffer_free(&out);
}

static void placeIds(const ByteBuffer *json, unsigned side, uint32_t *ids, unsigned count) {
	FixtureData *data = readData(json);
	for (unsigned i = 0; i < count; ++i) {
		ids[i] = data ? (uint32_t)data->teams[side].players[i].personUid.value : 0;
	}
	free(data);
}

TEST(orders_players_by_people) {
	ByteBuffer json = {0};
	ByteBuffer edited = {0};
	ByteBuffer rebuilt = {0};
	ByteBuffer replayed = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512];
	if (!lineupSample(&json)) {
		return;
	}
	// An edit, and a rebuild in another order (the game's own: 13-19 rotated).
	unsigned listed[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 21};
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_promote(&lineup, 0, 22, listed, 11, error, sizeof error), true);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 12, 14), true);
	ASSERT_BOOL_EQ(apply(&json, &lineup, &edited, &result, error, sizeof error), true);
	fixtureLineup_identity(&lineup);
	for (unsigned place = 13; place < 20; ++place) {
		lineup.from[0][place] = (uint8_t)(place == 19 ? 13 : place + 1);
	}
	lineup.changed[0] = true;
	ASSERT_BOOL_EQ(apply(&json, &lineup, &rebuilt, &result, error, sizeof error), true);
	uint32_t people[22];
	uint32_t ids[22];
	placeIds(&edited, 0, people, 22);
	FixtureData *data = readData(&rebuilt);
	FixtureLineupPeople matched;
	ASSERT_BOOL_EQ(data && fixtureLineup_byPeople(data, 0, people, 22, &lineup, &matched), true);
	ASSERT_INT_EQ((int)matched.missing, 0);
	ASSERT_BOOL_EQ(matched.found[0] && matched.found[21] && matched.places[21] == 21, true);
	ASSERT_BOOL_EQ(apply(&rebuilt, &lineup, &replayed, &result, error, sizeof error), true);
	placeIds(&replayed, 0, ids, 22);
	ASSERT_BOOL_EQ(memcmp(ids, people, sizeof ids) == 0, true);
	// An order kept by people that moves nobody marks its side all the same, and changes nothing else.
	ByteBuffer marked = {0};
	ASSERT_BOOL_EQ(buffer_append(&marked, json.data, json.length), true);
	replaceOnce(&marked, fieldsKey, homeMarker);
	for (unsigned header = 0; header < 3; ++header) {
		ByteBuffer input = {0};
		ASSERT_BOOL_EQ(buffer_append(&input, json.data, json.length), true);
		if (header) {
			replaceOnce(
				&input, fieldsKey, header == 1 ? homeMarker : "\n  \"home_lineup\": false,\n  \"fields\": ["
			);
		}
		fixtureLineup_identity(&lineup);
		lineup.mark[0] = true;
		ASSERT_BOOL_EQ(apply(&input, &lineup, &replayed, &result, error, sizeof error), true);
		ASSERT_BOOL_EQ(strcmp(buffer_string(&replayed), buffer_string(&marked)) == 0, true);
		ASSERT_BOOL_EQ(result.markings[0] == 0 && !result.marked[0][10], true);
		buffer_free(&input);
	}
	buffer_free(&marked);
	// A person not found leaves the place to the first player no person names, so the people after
	// keep theirs; the others follow in file order.
	uint32_t some[4] = {29179241, 12345, 19058734, 28100917};
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(data && fixtureLineup_byPeople(data, 0, some, 4, &lineup, &matched), true);
	ASSERT_INT_EQ((int)matched.missing, 1);
	ASSERT_BOOL_EQ(matched.found[0] && !matched.found[1] && matched.found[2] && matched.found[3], true);
	for (int i = 0; i < 4; ++i) {
		ASSERT_INT_EQ(matched.places[i], i);
	}
	static const uint8_t expected[22] = {
		21, 1, 0, 13, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20};
	ASSERT_BOOL_EQ(memcmp(lineup.from[0], expected, sizeof expected) == 0, true);
	ASSERT_BOOL_EQ(lineup.changed[0] && !lineup.changed[1], true);
	// The kept order of a side whose rebuild wrote one player's unique ID back (Wirtz's, at place 19):
	// that player's block, which no person names, takes the place of the person not found.
	people[19] = 2000040347;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(data && fixtureLineup_byPeople(data, 0, people, 22, &lineup, &matched), true);
	ASSERT_BOOL_EQ(matched.missing == 1 && !matched.found[19] && matched.places[19] == 19, true);
	ASSERT_BOOL_EQ(apply(&rebuilt, &lineup, &replayed, &result, error, sizeof error), true);
	placeIds(&replayed, 0, ids, 22);
	people[19] = fixtureSamples_lineupId(0, 19);
	ASSERT_BOOL_EQ(memcmp(ids, people, sizeof ids) == 0, true);
	// With no player left to take it, the place goes to the next person.
	uint32_t extra[23] = {12345};
	memcpy(extra + 1, people, sizeof people);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(data && fixtureLineup_byPeople(data, 0, extra, 23, &lineup, &matched), true);
	ASSERT_BOOL_EQ(matched.missing == 1 && !matched.found[0] && matched.places[0] == -1, true);
	ASSERT_BOOL_EQ(matched.places[1] == 0 && matched.places[22] == 21, true);
	ASSERT_BOOL_EQ(apply(&rebuilt, &lineup, &replayed, &result, error, sizeof error), true);
	placeIds(&replayed, 0, ids, 22);
	ASSERT_BOOL_EQ(memcmp(ids, extra + 1, sizeof ids) == 0, true);
	uint32_t many[FIXTURE_PLAYERS + 1] = {0};
	ASSERT_BOOL_EQ(
		data && fixtureLineup_byPeople(data, 0, many, FIXTURE_PLAYERS + 1, &lineup, &matched), false
	);
	free(data);
	// Two blocks of one person, matched in turn.
	ASSERT_INT_EQ((int)replaceText(&json, "\"u32\": 2000164850}", "\"u32\": 91193048}", 0), 2);
	data = readData(&json);
	uint32_t twins[2] = {91193048, 91193048};
	ASSERT_BOOL_EQ(data && fixtureLineup_byPeople(data, 0, twins, 2, &lineup, &matched), true);
	ASSERT_INT_EQ((int)matched.missing, 0);
	ASSERT_INT_EQ(lineup.from[0][0], 19);
	ASSERT_INT_EQ(lineup.from[0][1], 20);
	ASSERT_INT_EQ(lineup.from[0][2], 0);
	free(data);
	buffer_free(&json);
	buffer_free(&edited);
	buffer_free(&rebuilt);
	buffer_free(&replayed);
}

// fixtureEdit_apply's text, or "" if it fails (with the reason in `error`).
static bool editApply(
	const ByteBuffer *json,
	const FixturePlayerEdits *edits,
	const FixtureLineup *lineup,
	const FixtureNumber *venue,
	bool binary,
	ByteBuffer *out,
	char *error
) {
	error[0] = 0;
	bool ok = fixtureEdit_apply(
		buffer_string(json), json->length, edits, lineup, venue, binary, out, error, 512
	);
	if (!ok) {
		ASSERT_BOOL_EQ(out->length == 0, true);
	}
	return ok;
}

static bool sameBytes(const ByteBuffer *a, const ByteBuffer *b) {
	return a->length == b->length && (!a->length || memcmp(a->data, b->data, a->length) == 0);
}

TEST(max_flags_set_before_an_order_follow_the_player) {
	ByteBuffer json = {0};
	ByteBuffer patched = {0};
	ByteBuffer ordered = {0};
	ByteBuffer expected = {0};
	ByteBuffer out = {0};
	FixtureLineupResult result;
	FixtureLineup lineup;
	char error[512];
	if (!lineupSample(&json)) {
		return;
	}
	// A stored pitch condition, which a save writes after the order.
	replaceOnce(
		&json,
		"{\"str\": \"Premier League\"}",
		"{\"str\": \"Premier League\"},\n    {\"name\": \"venue.pitch_condition\", \"u16\": 200}"
	);
	FixtureNumber venue[VENUE_FIELD_COUNT] = {{0}};
	venue[VENUE_CONDITION] = (FixtureNumber){.value = 150, .known = true};
	// The Max patches name the blocks as they were, so they come first: Wirtz's (19) go with him.
	FixturePlayerEdits edits = {0};
	edits.players[0][19] = FIXTURE_MAX_CONDITION | FIXTURE_MAX_HAPPINESS;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	ASSERT_BOOL_EQ(editApply(&json, &edits, &lineup, venue, false, &out, error), true);
	bool ok = fixtureData_writePlayers(
							buffer_string(&json), json.length, &edits, &patched, error, sizeof error
						) &&
						apply(&patched, &lineup, &ordered, &result, error, sizeof error) &&
						fixtureData_writeVenue(
							buffer_string(&ordered), ordered.length, venue, &expected, error, sizeof error
						);
	ASSERT_BOOL_EQ(ok, true);
	ASSERT_BOOL_EQ(sameBytes(&out, &expected), true);
	FixtureData *data = readData(&out);
	if (data) {
		const FixturePlayer *wirtz = &data->teams[0].players[10];
		const FixturePlayer *gravenberch = &data->teams[0].players[19];
		ASSERT_INT_EQ(strcmp(wirtz->name, "Florian Wirtz"), 0);
		ASSERT_BOOL_EQ(wirtz->condition.value == 10000 && wirtz->morale.value == 20, true);
		ASSERT_BOOL_EQ(gravenberch->condition.value == 9010 && gravenberch->morale.value == 9, true);
		ASSERT_BOOL_EQ(data->venueValues[VENUE_CONDITION].value == 150, true);
	}
	free(data);
	ASSERT_INT_EQ((int)occurrences(&out, homeMarker), 1);
	// Encoded, the same bytes as the JSON's.
	ByteBuffer encoded = {0};
	ASSERT_BOOL_EQ(editApply(&json, &edits, &lineup, venue, true, &out, error), true);
	ASSERT_BOOL_EQ(
		matchConfig_encode(
			buffer_string(&expected), expected.length, &encoded, NULL, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(sameBytes(&out, &encoded), true);
	// Without an order, or with one that moves nobody, the blocks and header stay.
	ASSERT_BOOL_EQ(
		fixtureData_writeVenue(
			buffer_string(&patched), patched.length, venue, &expected, error, sizeof error
		),
		true
	);
	ASSERT_BOOL_EQ(editApply(&json, &edits, NULL, venue, false, &out, error), true);
	ASSERT_BOOL_EQ(sameBytes(&out, &expected), true);
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(editApply(&json, &edits, &lineup, venue, false, &out, error), true);
	ASSERT_BOOL_EQ(sameBytes(&out, &expected), true);
	// An order lineup refuses: the viewer's sentence, and no bytes.
	replaceOnce(&json, "\"home.player_count\", \"u8\": 22", "\"home.player_count\", \"u8\": 21");
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 21, 3, 4), true);
	ASSERT_BOOL_EQ(editApply(&json, &edits, &lineup, NULL, true, &out, error), false);
	ASSERT_INT_EQ(
		strcmp(
			error, "Cannot write the new line-up: home has 22 player blocks, but its player_count is 21."
		),
		0
	);
	buffer_free(&json);
	buffer_free(&patched);
	buffer_free(&ordered);
	buffer_free(&expected);
	buffer_free(&encoded);
	buffer_free(&out);
}

void registerFixtureLineupTests(void) {
	test_run(
		"fixture line-up reads person IDs, position ratings and the bench",
		test_reads_person_ids_ratings_and_the_bench
	);
	test_run(
		"fixture line-up reads field spans and line-up markers",
		test_reads_field_spans_and_lineup_markers
	);
	test_run(
		"fixture line-up exchanges whole blocks renamed to their places",
		test_exchanges_whole_blocks_renamed_to_their_places
	);
	test_run(
		"fixture line-up encodes an exchange as the blocks' bytes exchanged",
		test_encodes_the_exchange_as_the_blocks_bytes_exchanged
	);
	test_run(
		"fixture line-up re-points markings to where each player moves",
		test_repoints_markings_to_where_each_player_moves
	);
	test_run(
		"fixture line-up promotes a list with least movement", test_promotes_a_list_with_least_movement
	);
	test_run(
		"fixture line-up finds players by place, ID and name", test_finds_players_by_place_id_and_name
	);
	test_run(
		"fixture line-up refuses AI sides and moved blocks", test_refuses_ai_sides_and_moved_blocks
	);
	test_run("fixture line-up refuses blocks it cannot move", test_refuses_blocks_it_cannot_move);
	test_run(
		"fixture line-up warns about goalkeepers and opposition records",
		test_warns_about_goalkeepers_and_opposition_records
	);
	test_run("fixture line-up orders players by people", test_orders_players_by_people);
	test_run(
		"fixture line-up saves Max flags set before an order with the player, then the venue",
		test_max_flags_set_before_an_order_follow_the_player
	);
}
