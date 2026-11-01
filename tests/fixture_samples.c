// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_samples.h"

#include "buffer.h"
#include "fixture_data.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


bool fixtureSamples_append(ByteBuffer *out, const char *format, ...) {
	char text[2048];
	va_list arguments;
	va_start(arguments, format);
	int length = vsnprintf(text, sizeof text, format, arguments);
	va_end(arguments);
	return length >= 0 && (size_t)length < sizeof text && buffer_append(out, text, (size_t)length);
}

// One player block: its ratings, the 14 wellbeing values in file order, its names and, if
// `labelled`, the named wellbeing fields.
static bool
appendPlayer(ByteBuffer *out, const char *name, unsigned i, bool labelled, unsigned flags) {
	bool ok = fixtureSamples_append(
		out,
		",{\"name\":\"%s.player[%u]\"%s,\"u8\":255},"
		"{\"name\":\"%s.player[%u].ratings\",\"u8map\":{",
		name,
		i,
		// player[0] has no note; the others are named by it, as after inject-player.
		i ? ",\"player\":\"Injected player\"" : "",
		name,
		i
	);
	for (unsigned rating = 0; ok && rating < 69; ++rating) {
		ok = fixtureSamples_append(out, "%s\"r%u\":20", rating ? "," : "", rating);
	}
	ok = ok && fixtureSamples_append(out, "}}");
	static const unsigned values[14] = {
		10000,
		5600,
		5650,
		1,
		26,
		255,
		65396,
		0,
		7750,
		7850,
		7000,
		157,
		157,
		15,
	};
	static const char *const types[14] = {
		"u16",
		"u16",
		"u16",
		"u8",
		"u8",
		"u8",
		"u16",
		"u8",
		"u16",
		"u16",
		"u16",
		"u16",
		"u16",
		"u8",
	};
	for (unsigned field = 0; ok && field < 14; ++field) {
		unsigned value = values[field];
		if ((flags & FIXTURE_MAX_CONDITION) &&
				(field == 2 || (!labelled && (field == 0 || field == 1 || field == 6)))) {
			value = field == 6 ? 0 : 10000;
		}
		if (!labelled && field == 13 && (flags & FIXTURE_MAX_HAPPINESS)) {
			value = 20;
		}
		ok = fixtureSamples_append(out, ",{\"%s\":%u}", types[field], value);
	}
	ok = ok && fixtureSamples_append(
							 out,
							 ",{\"str\":\"Emiliano\"},{\"str\":\"Mart\\u00ednez\"},"
							 "{\"str\":\"Emiliano Mart\\u00ednez\"}"
						 );
	if (ok && labelled) {
		ok = fixtureSamples_append(
			out,
			",{\"name\":\"%s.player[%u].condition\",\"u16\":%u},"
			"{\"name\":\"%s.player[%u].sharpness[0]\",\"u16\":%u},"
			"{\"name\":\"%s.player[%u].morale\",\"u8\":%u},"
			"{\"name\":\"%s.player[%u].fatigue\",\"u16\":%u}",
			name,
			i,
			flags & FIXTURE_MAX_CONDITION ? 10000U : 9876U,
			name,
			i,
			flags & FIXTURE_MAX_CONDITION ? 10000U : 8125U,
			name,
			i,
			flags & FIXTURE_MAX_HAPPINESS ? 20U : 12U,
			name,
			i,
			flags & FIXTURE_MAX_CONDITION ? 0U : 65535U
		);
	}
	return ok;
}

bool fixtureSamples_config(
	ByteBuffer *out,
	bool labelled,
	bool swapControl,
	const char *source,
	const FixturePlayerEdits *edits
) {
	bool ok = fixtureSamples_append(
		out,
		"{\"format\":\"fm24-match-config/1\",\"fields\":["
		"{\"name\":\"fixture\",\"u8\":24},{\"str\":\"Friendly\"},"
		"{\"name\":\"venue\",\"u32\":0},{\"str\":\"Villa Park\"},"
		"{\"name\":\"official[0]\",\"u8\":0},"
		"{\"name\":\"referee\",\"u8\":8},{\"str\":\"Aimee\"},{\"str\":\"Keir\"},"
		"{\"str\":\"Aimee Keir\"}"
	);
	static const unsigned positions[11] = {
		1,
		2097168,
		1048704,
		1048592,
		4,
		4096,
		2113536,
		2097280,
		8,
		1064960,
		2048,
	};
	static const unsigned long long roles[11] = {
		2101248,
		18874368,
		4194320,
		18874368,
		8388612,
		138412032,
		2151677952,
		4194320,
		8388612,
		8390656,
		142606336,
	};
	for (unsigned side = 0; ok && side < 2; ++side) {
		const char *name = side ? "away" : "home";
		ok = fixtureSamples_append(
			out,
			",{\"name\":\"%s.team\",\"u8\":2},{\"str\":\"%s\"},"
			"{\"name\":\"%s.manager\",\"u32\":%u},"
			"{\"name\":\"%s.manager.tactic.plan[0]\",\"u8\":34},{\"str\":\"Custom 4-2-2-2\"},"
			"{\"name\":\"%s.manager.tactic.plan[0].mentality\",\"u8\":6}",
			name,
			side ? "Bristol City" : "Aston Villa",
			name,
			(side == (unsigned)swapControl) ? 135942U : 81920U,
			name,
			name
		);
		for (unsigned i = 0; ok && i < 11; ++i) {
			ok = fixtureSamples_append(
				out,
				",{\"name\":\"%s.manager.tactic.plan[0].slot[%u].position\","
				"\"flags\":[\"deliberately wrong\"],\"u32\":%u},"
				"{\"name\":\"%s.manager.tactic.plan[0].slot[%u].role\",\"u64\":%llu}",
				name,
				i,
				positions[i],
				name,
				i,
				roles[i]
			);
		}
		ok = ok && fixtureSamples_append(out, ",{\"name\":\"%s.player_count\",\"u8\":12}", name);
		for (unsigned i = 0; ok && i < 12; ++i) {
			ok = appendPlayer(out, name, i, labelled, edits ? edits->players[side][i] : 0);
		}
	}
	ok = ok && fixtureSamples_append(out, "]");
	if (ok && source) {
		ok = fixtureSamples_append(out, ",\"source\":\"%s\"", source);
	}
	return ok && fixtureSamples_append(out, "}");
}

static const char *const venueNames[VENUE_FIELD_COUNT] = {
	"venue.pitch.width",
	"venue",
	"venue.city_longitude",
	"venue.city_latitude",
	"venue.capacity",
	"venue.seating_capacity",
	"venue.home_fans",
	"venue.away_fans",
	"venue.season_ticket_holders",
	"venue.surface",
	"venue.pitch_condition",
};
static const char *const venuePoints[] = {
	"centre_spot",
	"end[0].goal",
	"end[0].post[0]",
	"end[0].post[1]",
	"end[0].corner[0]",
	"end[0].corner[1]",
	"end[0].penalty_spot",
	"end[1].goal",
	"end[1].post[0]",
	"end[1].post[1]",
	"end[1].corner[0]",
	"end[1].corner[1]",
	"end[1].penalty_spot",
};
static const char *const attendanceNames[] = {
	"venue.used_capacity",
	"venue.attendance",
	"fixture.attendance",
	"fixture.away_fans",
	"fixture_extra",
	"fixture_extra.home_fans",
	"fixture_extra.away_fans",
};

bool fixtureSamples_venue(ByteBuffer *json) {
	bool ok = fixtureSamples_append(
		json, "{\"format\":\"fm24-match-config/1\",\"home_team\":678,\"away_team\":677,\"fields\":["
	);
	const uint32_t values[] = {
		1116156723,
		1121071923,
		3211914117,
		1112666544,
		42777,
		42000,
		30000,
		2000,
		30000,
		8,
		200,
	};
	for (unsigned i = 0; ok && i < VENUE_FIELD_COUNT; ++i) {
		ok = fixtureSamples_append(
			json,
			"%s{\"u%u\": %u,\"name\":\"%s\",\"note\":\"preserve me\"}",
			i ? "," : "",
			i == VENUE_SURFACE     ? 8
			: i == VENUE_CONDITION ? 16
														 : 32,
			values[i],
			venueNames[i]
		);
	}
	static const char *const fractions[] = {
		"half_width", "half_length", "quarter_width", "quarter_length"};
	for (unsigned i = 0; ok && i < 4; ++i) {
		ok = fixtureSamples_append(json, ",{\"name\":\"venue.pitch.%s\",\"u32\":123}", fractions[i]);
	}
	for (unsigned p = 0; ok && p < 13; ++p) {
		for (unsigned axis = 0; ok && axis < 3; ++axis) {
			ok = fixtureSamples_append(
				json, ",{\"name\":\"venue.pitch.%s.%c\",\"u32\":123}", venuePoints[p], "xyz"[axis]
			);
		}
	}
	for (unsigned i = 0; ok && i < 7; ++i) {
		ok = fixtureSamples_append(json, ",{\"name\":\"%s\",\"u32\":123}", attendanceNames[i]);
	}
	for (unsigned side = 0; ok && side < 2; ++side) {
		const char *team = side ? "away" : "home";
		ok = fixtureSamples_append(
			json,
			",{\"name\":\"%s.team\",\"u8\":0},{\"str\":\"Test team\"},"
			"{\"name\":\"%s.player_count\",\"u8\":11}",
			team,
			team
		);
		for (unsigned player = 0; ok && player < 11; ++player) {
			ok = fixtureSamples_append(json, ",{\"name\":\"%s.player[%u]\",\"u8\":0}", team, player);
		}
	}
	return ok && fixtureSamples_append(
								 json,
								 ",{\"u32\": 987654321, \"name\":\"unrelated\", \"strange\":{\"keep\":[true,null]}}"
								 "],\"source\":\"unchanged source annotation\"}"
							 );
}

// Two Aston Villa players' {setting, amount} entries, read from the game, and the values its tactic
// familiarity panel showed for them against a tactic with settings {16, 5, 10, 20, 10, 15, 5}.
static const struct {
	unsigned count[FIXTURE_FAMILIARITY_CATEGORIES];
	unsigned entries[FIXTURE_FAMILIARITY_CATEGORIES][3][2];
	unsigned shown[FIXTURE_FAMILIARITY_CATEGORIES];
} familiarityPlayers[] = {
	{{2, 2, 2, 2, 1, 2, 2},
		{{{10, 8200}, {12, 8200}},
			{{9, 8200}, {8, 8200}},
			{{9, 8200}, {8, 8200}},
			{{10, 8200}, {15, 8200}},
			{{0, 8300}},
			{{10, 8500}, {13, 8500}},
			{{0, 8500}, {14, 8500}}},
		{4200, 5200, 7200, 3200, 1500, 6500, 3500}},
	{{3, 3, 3, 3, 2, 3, 3},
		{{{10, 10000}, {15, 10000}, {12, 8000}},
			{{9, 10000}, {3, 10000}, {8, 9000}},
			{{8, 10000}, {7, 10000}, {9, 9000}},
			{{9, 10000}, {15, 10000}, {10, 9000}},
			{{20, 10000}, {0, 1500}},
			{{10, 10000}, {3, 10000}, {13, 7000}},
			{{0, 10000}, {7, 10000}, {14, 3000}}},
		{9000, 8000, 8000, 5000, 1500, 5000, 8000}},
};

unsigned fixtureSamples_familiarityShown(unsigned player, unsigned category) {
	return player < 2 && category < FIXTURE_FAMILIARITY_CATEGORIES
					 ? familiarityPlayers[player].shown[category]
					 : 0;
}

// Puts `text` before the first `marker` in `json`.
static bool insertBefore(ByteBuffer *json, const char *marker, const ByteBuffer *text) {
	const char *found = strstr(buffer_string(json), marker);
	if (!found) {
		return false;
	}
	size_t at = (size_t)(found - (const char *)json->data);
	ByteBuffer out = {0};
	bool ok = buffer_append(&out, json->data, at) && buffer_append(&out, text->data, text->length) &&
						buffer_append(&out, json->data + at, json->length - at);
	if (ok) {
		buffer_free(json);
		*json = out;
	} else {
		buffer_free(&out);
	}
	return ok;
}

// Appends player `player`'s familiarity fields from familiarityPlayers[p].
static bool appendFamiliarity(ByteBuffer *text, unsigned player, unsigned p) {
	static const char *const categories[] = {
		"mentality", "passing_style", "creative_freedom", "trigger_press", "marking", "tempo", "width"};
	bool ok = true;
	for (unsigned k = 0; ok && k < FIXTURE_FAMILIARITY_CATEGORIES; ++k) {
		ok = fixtureSamples_append(
			text,
			",{\"name\":\"home.player[%u].tactical_familiarity.%s.count\",\"u32\":%u}",
			player,
			categories[k],
			familiarityPlayers[p].count[k]
		);
		for (unsigned e = 0; ok && e < familiarityPlayers[p].count[k]; ++e) {
			ok = fixtureSamples_append(
				text,
				",{\"name\":\"home.player[%u].tactical_familiarity.%s[%u].setting\",\"u8\":%u},"
				"{\"name\":\"home.player[%u].tactical_familiarity.%s[%u].amount\",\"u32\":%u}",
				player,
				categories[k],
				e,
				familiarityPlayers[p].entries[k][e][0],
				player,
				categories[k],
				e,
				familiarityPlayers[p].entries[k][e][1]
			);
		}
	}
	return ok;
}

bool fixtureSamples_familiarity(ByteBuffer *json) {
	ByteBuffer text = {0};
	bool ok = fixtureSamples_config(json, false, false, "familiarity", NULL) &&
						fixtureSamples_append(
							&text,
							"{\"name\":\"home.manager.tactic.plan[0].team_instructions[0]\",\"u32\":%u},"
							"{\"name\":\"home.manager.tactic.plan[0].team_instructions[1]\",\"u32\":%u},"
							"{\"name\":\"home.manager.tactic.plan[0].team_instructions[2]\",\"u32\":%u},"
							"{\"u8\":2},",
							0x100C0C28U,
							0x44230280U,
							0x1032CU
						) &&
						insertBefore(json, "{\"name\":\"home.manager.tactic.plan[0].mentality\"", &text);
	for (unsigned p = 0; ok && p < 2; ++p) {
		buffer_clear(&text);
		char marker[64];
		(void)snprintf(marker, sizeof marker, ",{\"name\":\"home.player[%u]\"", p + 2);
		ok = appendFamiliarity(&text, p + 1, p) && insertBefore(json, marker, &text);
	}
	buffer_free(&text);
	return ok;
}

// The lineup sample's squads: each player's note, unique ID and pos_gk.
static const struct {
	const char *name;
	unsigned id;
	unsigned goalkeeper;
} lineupPlayers[2][22] = {
	{
		{"Alisson Becker", 19058734, 20},
		{"Conor Bradley", 28127166, 1},
		{"Virgil van Dijk", 37036372, 1},
		{"Ibrahima Konate", 85028014, 1},
		{"Andrew Robertson", 28044813, 1},
		{"Wataru Endo", 29056345, 1},
		{"Alexis Mac Allister", 43219372, 1},
		{"Dominik Szoboszlai", 67236018, 1},
		{"Mohamed Salah", 98028753, 1},
		{"Cody Gakpo", 37060815, 1},
		{"Ryan Gravenberch", 37071197, 1},
		{"Caoimhin Kelleher", 28104130, 20},
		{"Joe Gomez", 28076016, 1},
		{"Trent Alexander-Arnold", 28104124, 1},
		{"Lewis Jones", 28100917, 1},
		{"Harvey Elliott", 29233143, 1},
		{"Jarell Quansah", 2000049283, 1},
		{"Curtis Jones", 28115788, 1},
		{"Luis D\\u00edaz", 76049803, 1},
		{"Florian Wirtz", 91193048, 1},
		{"Ben Doak", 2000164850, 1},
		{"Erling Haaland", 29179241, 1},
	},
	{
		{"Nick Pope", 29083750, 20},
		{"Kieran Trippier", 28018613, 1},
		{"Sven Botman", 37063649, 1},
		{"Dan Burn", 28057781, 1},
		{"Tino Livramento", 28124569, 1},
		{"Bruno Guimaraes", 19297055, 1},
		{"Sandro Tonali", 43298481, 1},
		{"Joelinton", 19184436, 1},
		{"Anthony Gordon", 28116386, 1},
		{"Alexander Isak", 93070286, 1},
		{"Harvey Barnes", 28100207, 1},
		{"Martin Dubravka", 3020153, 20},
		{"Lewis Miley", 2000189871, 1},
	},
};
static const unsigned lineupCounts[2] = {22, 13};

unsigned fixtureSamples_lineupId(unsigned side, unsigned place) {
	return side < 2 && place < lineupCounts[side] ? lineupPlayers[side][place].id : 0;
}

// A side's manager and tactic: its flags, part_4240, plan[0]'s slots with one role's instructions
// each (the u8 after bits[1] is the marked opponent's place) and formation[0..25].
static bool appendLineupTactic(ByteBuffer *out, unsigned side) {
	static const unsigned positions[11] = {
		1, 4, 2097168, 1048592, 8, 2097280, 1048704, 2048, 8192, 4096, 16384};
	static const char *const roles[11] = {
		"goalkeeper",
		"full_back",
		"central_defender",
		"central_defender",
		"full_back",
		"defensive_midfielder",
		"central_midfielder",
		"winger",
		"attacking_midfielder",
		"winger",
		"advanced_forward",
	};
	static const unsigned bench[9] = {1, 28, 1792, 114688, 1792, 28, 114688, 1792, 114688};
	const char *name = side ? "away" : "home";
	bool ok = fixtureSamples_append(
		out,
		",\n    {\"name\": \"%s.team\", \"u8\": 2},\n    {\"str\": \"%s\"},"
		"\n    {\"name\": \"%s.manager\", \"u32\": %u},"
		"\n    {\"name\": \"%s.manager.tactic.part_4240\", \"u16\": 3},\n    {\"u32\": 26}",
		name,
		side ? "Newcastle" : "Liverpool",
		name,
		side ? 0x34000U : 0x20308U,
		name
	);
	for (unsigned record = 0; ok && record < 26; ++record) {
		ok = fixtureSamples_append(
			out,
			",\n    {\"u32\": 0},\n    {\"u32\": 0},\n    {\"u32\": 0},\n    {\"u32\": 0},"
			"\n    {\"u8\": 0},\n    {\"u8\": 0},\n    {\"u8\": 0},\n    {\"u8\": 0},\n    {\"u8\": 0},"
			"\n    {\"u8\": 1}"
		);
	}
	ok = ok && fixtureSamples_append(
							 out,
							 ",\n    {\"name\": \"%s.manager.tactic.plan[0]\", \"u8\": 34},"
							 "\n    {\"str\": \"4-2-3-1 Wide\"},"
							 "\n    {\"name\": \"%s.manager.tactic.plan[0].mentality\", \"u8\": 3}",
							 name,
							 name
						 );
	for (unsigned slot = 0; ok && slot < 11; ++slot) {
		bool marking = side == 1 && slot == 9;
		ok = fixtureSamples_append(
			out,
			",\n    {\"name\": \"%s.manager.tactic.plan[0].slot[%u].position\", \"u32\": %u},"
			"\n    {\"name\": \"%s.manager.tactic.plan[0].slot[%u].role\", \"u64\": %u},"
			"\n    {\"name\": \"%s.manager.tactic.plan[0].slot[%u].player_instructions[%s]\","
			" \"u64\": %u},",
			name,
			slot,
			positions[slot],
			name,
			slot,
			1U << slot,
			name,
			slot,
			roles[slot],
			1U << slot
		);
		ok = ok && fixtureSamples_append(
								 out,
								 "\n    {\"name\": \"%s.manager.tactic.plan[0].slot[%u].player_instructions[%s]"
								 ".bits[0]\", \"flags\": [], \"u32\": 0},"
								 "\n    {\"name\": \"%s.manager.tactic.plan[0].slot[%u].player_instructions[%s]"
								 ".bits[1]\", \"flags\": [%s], \"u32\": %u},"
								 "\n    {\"u8\": %u},\n    {\"u32\": 0}",
								 name,
								 slot,
								 roles[slot],
								 name,
								 slot,
								 roles[slot],
								 marking ? "\"mark a specific player\"" : "",
								 marking ? 4U : 0U,
								 marking ? 19U : 255U
							 );
	}
	for (unsigned k = 0; ok && k < 26; ++k) {
		unsigned position = k < 11 ? positions[k] : k - 11 < (side ? 2U : 9U) ? bench[k - 11] : 0;
		ok = fixtureSamples_append(
			out,
			",\n    {\"name\": \"%s.manager.tactic.formation[%u]\", \"u8\": 2},\n    {\"u32\": %u}",
			name,
			k,
			position
		);
	}
	return ok;
}

// One player block: the serialiser's 15 header fields, ratings, the named wellbeing values, the
// squad number, the unique ID and its unnamed copy, and 0-3 more fields by place.
static bool appendLineupPlayer(ByteBuffer *out, unsigned side, unsigned place) {
	static const char *const positions[15] = {
		"gk", "sw", "dl", "dc", "dr", "dm", "ml", "mc", "mr", "aml", "amc", "amr", "st", "wbl", "wbr"};
	const char *name = side ? "away" : "home";
	unsigned goalkeeper = lineupPlayers[side][place].goalkeeper;
	bool ok = fixtureSamples_append(
		out,
		",\n    {\"name\": \"%s.player[%u]\", \"player\": \"%s\", \"u8\": 255},"
		"\n    {\"u16\": 0},\n    {\"u8\": 0},\n    {\"u8\": 0},\n    {\"u16\": 58},\n    {\"u8\": 2},"
		"\n    {\"u32\": 0},\n    {\"u64\": 0},\n    {\"u8\": 1},\n    {\"u8\": 1},\n    {\"u32\": 0},"
		"\n    {\"u32\": 0},\n    {\"u8\": 255},\n    {\"u8\": 0},\n    {\"u8\": 1},"
		"\n    {\"name\": \"%s.player[%u].ratings\", \"u8map\": {",
		name,
		place,
		lineupPlayers[side][place].name,
		name,
		place
	);
	for (unsigned i = 0; ok && i < 15; ++i) {
		unsigned rating = i == 0 ? goalkeeper : goalkeeper > 1 ? 1 : 1 + (place + 3 * i) % 20;
		ok = fixtureSamples_append(out, "%s\"pos_%s\": %u", i ? ", " : "", positions[i], rating);
	}
	for (unsigned i = 0; ok && i < 54; ++i) {
		ok = fixtureSamples_append(out, ", \"a%u\": %u", i, 20 + (place * 7 + i) % 80);
	}
	ok = ok && fixtureSamples_append(
							 out,
							 "}},\n    {\"name\": \"%s.player[%u].condition\", \"u16\": %u},"
							 "\n    {\"name\": \"%s.player[%u].sharpness[0]\", \"u16\": 9500},"
							 "\n    {\"name\": \"%s.player[%u].sharpness[1]\", \"u16\": 9500},"
							 "\n    {\"name\": \"%s.player[%u].fatigue\", \"u16\": 65469},"
							 "\n    {\"name\": \"%s.player[%u].morale\", \"u8\": 9}",
							 name,
							 place,
							 9000 + place,
							 name,
							 place,
							 name,
							 place,
							 name,
							 place,
							 name,
							 place
						 );
	ok = ok && fixtureSamples_append(
							 out,
							 ",\n    {\"name\": \"%s.player[%u].squad_number\", \"u8\": %u},"
							 "\n    {\"name\": \"%s.player[%u].person_uid\", \"u32\": %u}",
							 name,
							 place,
							 place + 1,
							 name,
							 place,
							 lineupPlayers[side][place].id
						 );
	for (unsigned i = 0; ok && i < place % 4; ++i) {
		ok = fixtureSamples_append(out, ",\n    {\"u8\": %u}", place * 10 + i);
	}
	return ok && fixtureSamples_append(
								 out, ",\n    {\"u32\": %u},\n    {\"u8\": 7}", lineupPlayers[side][place].id
							 );
}

bool fixtureSamples_lineup(ByteBuffer *out) {
	bool ok = fixtureSamples_append(
		out,
		"{\n  \"format\": \"fm24-match-config/1\",\n  \"game_build\": 2081827,"
		"\n  \"source\": \"Liverpool vs Newcastle, 2026 day 25 15:00, seed 42\","
		"\n  \"fields\": [\n    {\"name\": \"fixture\", \"u8\": 24},\n    {\"str\": \"Premier League\"}"
	);
	for (unsigned side = 0; ok && side < 2; ++side) {
		const char *name = side ? "away" : "home";
		ok = appendLineupTactic(out, side) &&
				 fixtureSamples_append(
					 out, ",\n    {\"name\": \"%s.player_count\", \"u8\": %u}", name, lineupCounts[side]
				 );
		for (unsigned place = 0; ok && place < lineupCounts[side]; ++place) {
			ok = appendLineupPlayer(out, side, place);
		}
		ok = ok && fixtureSamples_append(
								 out, ",\n    {\"name\": \"%s.has_team_extra\", \"u8\": 0},\n    {\"u8\": 9}", name
							 );
	}
	return ok && fixtureSamples_append(out, "\n  ]\n}\n");
}

char *fixtureSamples_json(int sample, size_t *length) {
	ByteBuffer json = {0};
	bool ok = false;
	switch (sample) {
		case FIXTURE_SAMPLE_LABELLED:
		case FIXTURE_SAMPLE_UNLABELLED:
		case FIXTURE_SAMPLE_SWAPPED:
			ok = fixtureSamples_config(
				&json,
				sample != FIXTURE_SAMPLE_UNLABELLED,
				sample == FIXTURE_SAMPLE_SWAPPED,
				"Aston Villa vs Bristol City, 2023 day 186 19:30, seed 42",
				NULL
			);
			break;
		case FIXTURE_SAMPLE_VENUE: ok = fixtureSamples_venue(&json); break;
		case FIXTURE_SAMPLE_FAMILIARITY: ok = fixtureSamples_familiarity(&json); break;
		case FIXTURE_SAMPLE_LINEUP: ok = fixtureSamples_lineup(&json); break;
		default: break;
	}
	if (!ok) {
		buffer_free(&json);
		*length = 0;
		return NULL;
	}
	*length = json.length;
	return (char *)json.data;
}

char *fixtureSamples_savedJson(int sample, size_t *length) {
	static const char format[] = "{\"format\":\"fm24-match-config/1\",";
	size_t prefix = sizeof format - 1;
	size_t plainLength = 0;
	char *plain = fixtureSamples_json(sample, &plainLength);
	ByteBuffer json = {0};
	bool ok =
		plain && plainLength > prefix && memcmp(plain, format, prefix) == 0 &&
		buffer_append(&json, format, prefix) &&
		fixtureSamples_append(
			&json, "\"save\":\"%s\",\"game_date\":\"%s\",", FIXTURE_SAMPLE_SAVE, FIXTURE_SAMPLE_GAME_DATE
		) &&
		buffer_append(&json, plain + prefix, plainLength - prefix);
	free(plain);
	if (!ok) {
		buffer_free(&json);
		*length = 0;
		return NULL;
	}
	*length = json.length;
	return (char *)json.data;
}
