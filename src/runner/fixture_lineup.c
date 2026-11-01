// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_lineup.h"

#include "json_reader.h"
#include "match_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum {
	HEADER_FIELDS = 15,     // a block's fields before .ratings: the serialiser's header
	RECORD_FIELDS = 10,     // a part_4240 record: four u32, then six u8
	MAX_RECORDS = 4096,     // more part_4240 records than this is not the expected structure
	GOALKEEPER_RATING = 10, // the pos_gk from which a player counts as a goalkeeper
	CANDIDATES = 8,         // the matches an ambiguous player's message lists
};

static const char *const sideNames[2] = {"home", "away"};

// A block field's value token, which the block's header compares.
typedef struct HeaderField {
	ConfigValueKind kind;
	size_t offset;
	size_t length;
} HeaderField;

// One player block: its fields' objects span [start, end).
typedef struct Block {
	size_t start;
	size_t end;
	size_t firstName; // its names' entries in the side's `numbers`
	size_t names;
	unsigned headerFields;
	bool headerDone;
	HeaderField header[HEADER_FIELDS];
} Block;

// An opponent's place that the side marks: the u8 after a "...player_instructions[<role>].bits[1]".
typedef struct Marking {
	size_t offset; // its value token
	size_t length;
	unsigned value;
} Marking;

// What a visit of the fixture found about one side.
typedef struct Side {
	const char *name;
	char prefix[16]; // "home.player["
	size_t prefixLength;
	Block blocks[FIXTURE_PLAYERS];
	unsigned count;
	bool inBlocks; // the field being visited belongs to the last block
	bool ended;    // a named field after the blocks ended them
	FixtureNumber playerCount;
	ByteBuffer numbers;  // size_t offsets: where the number of each name in the blocks starts
	ByteBuffer markings; // Marking entries
	bool markingNext;
	bool recordsCountNext;
	size_t recordsLeft; // fields of the side's part_4240 records still to read
	size_t recordIndex;
	uint64_t record[RECORD_FIELDS]; // the first record, which the others must repeat
	ConfigValueKind recordKinds[RECORD_FIELDS];
	bool recordsUneven;
	char error[256];
} Side;

typedef struct Scan {
	const char *json;
	Side sides[2];
	bool outOfMemory;
} Scan;

// Where a marker goes: before the top-level "fields" key, after the whitespace that precedes it.
typedef struct Header {
	size_t fieldsKey;
	size_t fieldsSpace;
	size_t marker[2]; // an existing "<side>_lineup" value token, if markerLength is not 0
	size_t markerLength[2];
	bool markerTrue[2];
} Header;

typedef enum EditKind { EDIT_MARKER, EDIT_TRUE, EDIT_NUMBER, EDIT_BLOCKS } EditKind;

// A replacement of json[offset, offset + length): a marker (`value` is the side), "true", a number,
// or a side's blocks in their new order.
typedef struct Edit {
	size_t offset;
	size_t length;
	EditKind kind;
	unsigned value;
	unsigned order;
} Edit;

typedef struct Writer {
	const char *json;
	const Scan *scan;
	const Header *header;
	const FixtureLineup *lineup;
	ByteBuffer *out;
} Writer;

static void setError(char *error, size_t errorSize, const char *format, ...);
static void appendText(char *out, size_t size, size_t *used, const char *format, ...);
static unsigned playerCount(const FixtureTeam *team);
static void updateChanged(FixtureLineup *lineup, unsigned side);
static bool isPermutation(const uint8_t *from, unsigned count);
static bool isIdentity(const uint8_t *from, unsigned count);
static char lowerAscii(char c);
static bool sameText(const char *a, const char *b);
static bool containsText(const char *text, const char *part);
static bool trimText(const char *text, char *out, size_t size, char *error, size_t errorSize);
static unsigned matchIds(const FixtureTeam *team, const char *text, unsigned *matches);
static unsigned matchNames(const FixtureTeam *team, const char *text, unsigned *matches);
static bool findMatches(
	const FixtureTeam *team,
	unsigned side,
	const char *text,
	bool isId,
	const unsigned *matches,
	unsigned found,
	unsigned *place,
	char *error,
	size_t errorSize
);
static bool isGoalkeeper(const FixturePlayer *player);
static void goalkeeperWarnings(
	const FixtureTeam *team,
	const uint8_t *from,
	char *warnings,
	size_t size,
	size_t *used
);
static uint64_t fieldValue(const ConfigField *field);
static void scanField(void *context, const ConfigField *field);
static bool scanHeader(const char *json, size_t length, Header *header);
static bool
checkSide(const char *json, const Side *side, const uint8_t *from, char *error, size_t errorSize);
static bool collectEdits(
	const Scan *scan,
	const Header *header,
	const FixtureLineup *lineup,
	const bool sides[2],
	const bool marks[2],
	ByteBuffer *edits,
	FixtureLineupResult *result
);
static bool writeEdits(const Writer *writer, ByteBuffer *edits, size_t length);
static bool verify(
	const char *json,
	size_t length,
	const ByteBuffer *out,
	const FixtureLineup *lineup,
	const bool sides[2],
	const bool marks[2],
	char *error,
	size_t errorSize
);

void fixtureLineup_identity(FixtureLineup *lineup) {
	for (unsigned side = 0; side < 2; ++side) {
		lineup->changed[side] = false;
		lineup->mark[side] = false;
		for (unsigned place = 0; place < FIXTURE_PLAYERS; ++place) {
			lineup->from[side][place] = (uint8_t)place;
		}
	}
}

int fixtureLineup_readPlace(
	const char *text,
	unsigned side,
	unsigned *place,
	char *error,
	size_t errorSize
) {
	const char *rest = text;
	unsigned named = side;
	for (unsigned s = 0; s < 2; ++s) {
		size_t length = strlen(sideNames[s]);
		if (strncmp(text, sideNames[s], length) == 0 && strncmp(text + length, ".player[", 8) == 0) {
			named = s;
			rest = text + length + 1;
		}
	}
	if (strncmp(rest, "player[", 7) != 0) {
		return 0;
	}
	const char *digits = rest + 7;
	size_t length = strspn(digits, "0123456789");
	if (!length || length > 9 || strcmp(digits + length, "]") != 0) {
		setError(error, errorSize, "'%s' is not a place: use player[N], counting from 0", text);
		return -1;
	}
	if (side > 1) {
		setError(error, errorSize, "no such side");
		return -1;
	}
	if (named != side) {
		setError(error, errorSize, "%s is not on the %s side", text, sideNames[side]);
		return -1;
	}
	*place = (unsigned)strtoul(digits, NULL, 10);
	return 1;
}

bool fixtureLineup_find(
	const FixtureData *data,
	unsigned side,
	const char *text,
	unsigned *place,
	char *error,
	size_t errorSize
) {
	char trimmed[FIXTURE_TEXT];
	if (side > 1) {
		setError(error, errorSize, "no such side");
		return false;
	}
	if (!trimText(text, trimmed, sizeof trimmed, error, errorSize)) {
		return false;
	}
	const FixtureTeam *team = &data->teams[side];
	int isPlace = fixtureLineup_readPlace(trimmed, side, place, error, errorSize);
	if (isPlace > 0 && *place >= playerCount(team)) {
		setError(error, errorSize, "the fixture has no %s.player[%u]", sideNames[side], *place);
		return false;
	}
	if (isPlace) {
		return isPlace > 0;
	}
	unsigned matches[FIXTURE_PLAYERS];
	bool isId = !trimmed[strspn(trimmed, "0123456789")];
	unsigned found = isId ? matchIds(team, trimmed, matches) : matchNames(team, trimmed, matches);
	return findMatches(team, side, trimmed, isId, matches, found, place, error, errorSize);
}

bool fixtureLineup_exchange(
	FixtureLineup *lineup,
	unsigned side,
	unsigned count,
	unsigned a,
	unsigned b
) {
	if (side > 1 || count > FIXTURE_PLAYERS || a >= count || b >= count || a == b) {
		return false;
	}
	uint8_t *from = lineup->from[side];
	uint8_t moved = from[a];
	from[a] = from[b];
	from[b] = moved;
	updateChanged(lineup, side);
	return true;
}

bool fixtureLineup_promote(
	FixtureLineup *lineup,
	unsigned side,
	unsigned count,
	const unsigned *listed,
	unsigned listedCount,
	char *error,
	size_t errorSize
) {
	if (side > 1 || count > FIXTURE_PLAYERS) {
		setError(error, errorSize, "no such side");
		return false;
	}
	if (listedCount < FIXTURE_SLOTS || listedCount > count) {
		setError(
			error,
			errorSize,
			listedCount < FIXTURE_SLOTS ? "give the XI in slot order, then any substitutes"
																	: "%s has only %u players",
			sideNames[side],
			count
		);
		return false;
	}
	bool isListed[FIXTURE_PLAYERS] = {false};
	for (unsigned i = 0; i < listedCount; ++i) {
		if (listed[i] >= count || isListed[listed[i]]) {
			setError(
				error,
				errorSize,
				listed[i] >= count ? "the fixture has no %s.player[%u]" : "%s.player[%u] is listed twice",
				sideNames[side],
				listed[i]
			);
			return false;
		}
		isListed[listed[i]] = true;
	}
	uint8_t *from = lineup->from[side];
	uint8_t next[FIXTURE_PLAYERS];
	memcpy(next, from, sizeof next);
	for (unsigned j = 0; j < listedCount; ++j) {
		next[j] = from[listed[j]];
	}
	// The listed players' places from listedCount on are left, and the displaced take them in order.
	unsigned left = listedCount;
	for (unsigned p = 0; p < listedCount; ++p) {
		if (isListed[p]) {
			continue;
		}
		while (left < count && !isListed[left]) {
			++left;
		}
		if (left < count) {
			next[left++] = from[p];
		}
	}
	memcpy(from, next, sizeof next);
	updateChanged(lineup, side);
	return true;
}

bool fixtureLineup_check(
	const FixtureData *data,
	unsigned side,
	const FixtureLineup *lineup,
	bool allowAiSide,
	char *error,
	size_t errorSize,
	char *warnings,
	size_t warningsSize
) {
	size_t used = 0;
	if (warnings && warningsSize) {
		warnings[0] = 0;
	}
	if (side > 1) {
		setError(error, errorSize, "no such side");
		return false;
	}
	const FixtureTeam *team = &data->teams[side];
	const char *name = sideNames[side];
	unsigned count = playerCount(team);
	if (!team->lineupKnown && !allowAiSide) {
		setError(
			error,
			errorSize,
			"%s is managed by FM24's AI, which picks its own XI at kick-off; use --allow-ai-side to "
			"reorder it anyway",
			name
		);
	} else if (!team->blocksInOrder) {
		setError(
			error,
			errorSize,
			"%s.player[%u] is in place %u: the blocks were moved without renaming them",
			name,
			team->misplacedBlock,
			team->misplacedPlace
		);
	} else if (count < FIXTURE_SLOTS) {
		setError(error, errorSize, "%s has %u players; a line-up needs 11", name, count);
	} else if (team->blockCount != count) {
		setError(
			error,
			errorSize,
			"%s has %u player blocks, but its player_count is %u",
			name,
			team->blockCount,
			count
		);
	} else if (!isPermutation(lineup->from[side], count)) {
		setError(error, errorSize, "the new %s line-up does not place each player once", name);
	} else {
		if (!team->lineupKnown) {
			appendText(
				warnings,
				warningsSize,
				&used,
				"%s is managed by FM24's AI, which picks its own XI at kick-off\n",
				name
			);
		}
		goalkeeperWarnings(team, lineup->from[side], warnings, warningsSize, &used);
		return true;
	}
	return false;
}

bool fixtureLineup_apply(
	const char *json,
	size_t length,
	const FixtureLineup *lineup,
	ByteBuffer *out,
	FixtureLineupResult *result,
	char *error,
	size_t errorSize
) {
	buffer_clear(out);
	*result = (FixtureLineupResult){0};
	Scan *scan = calloc(1, sizeof *scan);
	if (!scan) {
		setError(error, errorSize, "out of memory");
		return false;
	}
	scan->json = json;
	for (unsigned s = 0; s < 2; ++s) {
		Side *side = &scan->sides[s];
		side->name = sideNames[s];
		(void)snprintf(side->prefix, sizeof side->prefix, "%s.player[", side->name);
		side->prefixLength = strlen(side->prefix);
	}
	ConfigVisitor visitor = {.context = scan, .field = scanField};
	Header header = {0};
	bool ok = matchConfig_visit(json, length, &visitor, error, errorSize);
	if (ok && !scanHeader(json, length, &header)) {
		setError(error, errorSize, "the fixture has no \"fields\"");
		ok = false;
	}
	bool sides[2] = {false, false}; // reordered
	bool marks[2] = {false, false}; // given the marker
	if (ok && scan->outOfMemory) {
		setError(error, errorSize, "out of memory");
		ok = false;
	}
	for (unsigned s = 0; ok && s < 2; ++s) {
		sides[s] = lineup->changed[s] && !isIdentity(lineup->from[s], FIXTURE_PLAYERS);
		marks[s] = sides[s] || lineup->mark[s];
		ok = !sides[s] || checkSide(json, &scan->sides[s], lineup->from[s], error, errorSize);
	}
	ByteBuffer edits = {0};
	if (ok && !collectEdits(scan, &header, lineup, sides, marks, &edits, result)) {
		setError(error, errorSize, "out of memory");
		ok = false;
	}
	Writer writer = {.json = json, .scan = scan, .header = &header, .lineup = lineup, .out = out};
	if (ok && !writeEdits(&writer, &edits, length)) {
		setError(error, errorSize, "out of memory");
		ok = false;
	}
	ok = ok && verify(json, length, out, lineup, sides, marks, error, errorSize);
	buffer_free(&edits);
	for (unsigned s = 0; s < 2; ++s) {
		buffer_free(&scan->sides[s].numbers);
		buffer_free(&scan->sides[s].markings);
	}
	free(scan);
	if (!ok) {
		buffer_clear(out);
		*result = (FixtureLineupResult){0};
	}
	return ok;
}

bool fixtureLineup_byPeople(
	const FixtureData *data,
	unsigned side,
	const uint32_t *people,
	unsigned count,
	FixtureLineup *lineup,
	FixtureLineupPeople *matched
) {
	*matched = (FixtureLineupPeople){0};
	if (side > 1 || count > FIXTURE_PLAYERS) {
		return false;
	}
	const FixtureTeam *team = &data->teams[side];
	unsigned players = playerCount(team);
	bool used[FIXTURE_PLAYERS] = {false};
	unsigned at[FIXTURE_PLAYERS]; // each person's place in the file, or `players` if not found
	for (unsigned i = 0; i < count; ++i) {
		unsigned found = 0;
		while (found < players && (used[found] || !team->players[found].personUid.known ||
																team->players[found].personUid.value != people[i])) {
			++found;
		}
		at[i] = found;
		matched->found[i] = found < players;
		if (found < players) {
			used[found] = true;
		} else {
			++matched->missing;
		}
	}
	uint8_t *from = lineup->from[side];
	unsigned place = 0;
	unsigned spare = 0; // the next player no person names
	for (unsigned i = 0; i < count; ++i) {
		unsigned player = at[i];
		if (player == players) {
			while (spare < players && used[spare]) {
				++spare;
			}
			if (spare == players) {
				matched->places[i] = -1;
				continue;
			}
			player = spare;
			used[spare] = true;
		}
		matched->places[i] = (int)place;
		from[place++] = (uint8_t)player;
	}
	for (unsigned i = 0; i < players; ++i) {
		if (!used[i]) {
			from[place++] = (uint8_t)i;
		}
	}
	for (; place < FIXTURE_PLAYERS; ++place) {
		from[place] = (uint8_t)place;
	}
	updateChanged(lineup, side);
	return true;
}

static void setError(char *error, size_t errorSize, const char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(error, errorSize, format, arguments);
	va_end(arguments);
}

// Appends to `out`, keeping `used` within it when the text is cut short.
static void appendText(char *out, size_t size, size_t *used, const char *format, ...) {
	if (!out || *used + 1 >= size) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	int written = vsnprintf(out + *used, size - *used, format, arguments);
	va_end(arguments);
	if (written > 0) {
		*used += (size_t)written < size - *used ? (size_t)written : size - *used - 1;
	}
}

static unsigned playerCount(const FixtureTeam *team) {
	return team->playerCount.known ? (unsigned)team->playerCount.value : 0;
}

static void updateChanged(FixtureLineup *lineup, unsigned side) {
	lineup->changed[side] = !isIdentity(lineup->from[side], FIXTURE_PLAYERS);
}

static bool isPermutation(const uint8_t *from, unsigned count) {
	bool seen[FIXTURE_PLAYERS] = {false};
	for (unsigned place = 0; place < count; ++place) {
		if (from[place] >= count || seen[from[place]]) {
			return false;
		}
		seen[from[place]] = true;
	}
	return true;
}

static bool isIdentity(const uint8_t *from, unsigned count) {
	for (unsigned place = 0; place < count; ++place) {
		if (from[place] != place) {
			return false;
		}
	}
	return true;
}

static char lowerAscii(char c) {
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool sameText(const char *a, const char *b) {
	while (*a && lowerAscii(*a) == lowerAscii(*b)) {
		++a;
		++b;
	}
	return !*a && !*b;
}

static bool containsText(const char *text, const char *part) {
	for (; *text; ++text) {
		size_t i = 0;
		while (part[i] && lowerAscii(text[i]) == lowerAscii(part[i])) {
			++i;
		}
		if (!part[i]) {
			return true;
		}
	}
	return false;
}

// `text` without its leading and trailing spaces and tabs, if that is not empty and fits.
static bool trimText(const char *text, char *out, size_t size, char *error, size_t errorSize) {
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
		--length;
	}
	if (!length || length >= size) {
		setError(error, errorSize, length ? "the player is too long" : "give a player");
		return false;
	}
	memcpy(out, text, length);
	out[length] = 0;
	return true;
}

// The places whose unique ID is `text`, a decimal number.
static unsigned matchIds(const FixtureTeam *team, const char *text, unsigned *matches) {
	unsigned long long id = strtoull(text, NULL, 10);
	unsigned found = 0;
	for (unsigned i = 0; i < playerCount(team); ++i) {
		if (team->players[i].personUid.known && team->players[i].personUid.value == id) {
			matches[found++] = i;
		}
	}
	return found;
}

// The places whose name is `text`, or failing that contains it, ASCII case ignored.
static unsigned matchNames(const FixtureTeam *team, const char *text, unsigned *matches) {
	unsigned found = 0;
	for (unsigned pass = 0; pass < 2 && !found; ++pass) {
		for (unsigned i = 0; i < playerCount(team); ++i) {
			const char *name = team->players[i].name;
			if (name[0] && (pass ? containsText(name, text) : sameText(name, text))) {
				matches[found++] = i;
			}
		}
	}
	return found;
}

// The one match of a name or ID, or a message naming none or several.
static bool findMatches(
	const FixtureTeam *team,
	unsigned side,
	const char *text,
	bool isId,
	const unsigned *matches,
	unsigned found,
	unsigned *place,
	char *error,
	size_t errorSize
) {
	if (found == 1) {
		*place = matches[0];
		return true;
	}
	if (!found) {
		setError(
			error,
			errorSize,
			isId ? "no %s player has the unique ID %s" : "no %s player matches '%s'",
			sideNames[side],
			text
		);
		return false;
	}
	size_t used = 0;
	appendText(
		error, errorSize, &used, "'%s' matches more than one %s player:", text, sideNames[side]
	);
	for (unsigned i = 0; i < found && i < CANDIDATES; ++i) {
		const FixturePlayer *player = &team->players[matches[i]];
		appendText(error, errorSize, &used, "%s player[%u] %s", i ? "," : "", matches[i], player->name);
		if (player->personUid.known) {
			appendText(error, errorSize, &used, " (%llu)", (unsigned long long)player->personUid.value);
		}
	}
	appendText(
		error,
		errorSize,
		&used,
		"%s; use %s",
		found > CANDIDATES ? ", ..." : "",
		isId ? "player[N]" : "more of the name, the ID or player[N]"
	);
	return false;
}

static bool isGoalkeeper(const FixturePlayer *player) {
	return player->positionRatingsKnown && player->positionRatings[0] >= GOALKEEPER_RATING;
}

// Goalkeepers out of goal, outfield players in it, and a bench that loses its goalkeeper.
static void goalkeeperWarnings(
	const FixtureTeam *team,
	const uint8_t *from,
	char *warnings,
	size_t size,
	size_t *used
) {
	for (unsigned slot = 0; slot < FIXTURE_SLOTS; ++slot) {
		const FixturePlayer *player = &team->players[from[slot]];
		if (!player->positionRatingsKnown) {
			continue;
		}
		if ((int)slot == team->goalkeeperSlot && !isGoalkeeper(player)) {
			appendText(
				warnings,
				size,
				used,
				"%s is in goal (slot %u) but is not a goalkeeper (GK %u)\n",
				player->name,
				slot,
				player->positionRatings[0]
			);
		} else if ((int)slot != team->goalkeeperSlot && isGoalkeeper(player)) {
			char position[16] = "Unknown";
			if (team->slots[slot].position.known) {
				fixtureData_position(team->slots[slot].position.value, position, sizeof position);
			}
			appendText(
				warnings, size, used, "%s, a goalkeeper, is in slot %u (%s)\n", player->name, slot, position
			);
		}
	}
	bool before = false;
	bool after = false;
	for (unsigned place = FIXTURE_SLOTS; place < FIXTURE_SLOTS + team->benchSize; ++place) {
		before = before || isGoalkeeper(&team->players[place]);
		after = after || isGoalkeeper(&team->players[from[place]]);
	}
	if (before && !after) {
		appendText(warnings, size, used, "no goalkeeper among the substitutes\n");
	}
}

static uint64_t fieldValue(const ConfigField *field) {
	uint64_t value = 0;
	for (size_t i = 0; i < field->length && i < 8; ++i) {
		value |= (uint64_t)field->bytes[i] << (8 * i);
	}
	return value;
}

static void sideError(Side *side, const char *format, ...) {
	if (side->error[0]) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(side->error, sizeof side->error, format, arguments);
	va_end(arguments);
}

static bool isNumber(const ConfigField *field) {
	return field->kind == CONFIG_U8 || field->kind == CONFIG_U16 || field->kind == CONFIG_U32 ||
				 field->kind == CONFIG_U64;
}

// One field of the side's part_4240 records, which must repeat the first record.
static void scanRecord(Side *side, const ConfigField *field) {
	--side->recordsLeft;
	size_t k = side->recordIndex++ % RECORD_FIELDS;
	if (field->name[0] || !isNumber(field)) {
		side->recordsUneven = true;
		side->recordsLeft = 0;
		return;
	}
	uint64_t value = fieldValue(field);
	if (side->recordIndex <= RECORD_FIELDS) {
		side->record[k] = value;
		side->recordKinds[k] = field->kind;
	} else if (side->record[k] != value || side->recordKinds[k] != field->kind) {
		side->recordsUneven = true;
	}
}

// The values that follow a named field: a marking's place and the part_4240 records.
static void scanFollowing(Scan *scan, Side *side, const ConfigField *field) {
	if (side->markingNext) {
		side->markingNext = false;
		Marking marking = {field->valueOffset, field->valueLength, (unsigned)fieldValue(field)};
		if (!field->name[0] && field->kind == CONFIG_U8 &&
				!buffer_append(&side->markings, &marking, sizeof marking)) {
			scan->outOfMemory = true;
		}
	}
	if (side->recordsCountNext) {
		side->recordsCountNext = false;
		bool counted = field->kind == CONFIG_U32 && !field->name[0];
		uint64_t records = counted ? fieldValue(field) : 0;
		if (!counted || records > MAX_RECORDS) {
			side->recordsUneven = true;
		} else {
			side->recordsLeft = (size_t)records * RECORD_FIELDS;
			side->recordIndex = 0;
		}
	} else if (side->recordsLeft) {
		scanRecord(side, field);
	}
}

// A block name's number and what follows it: "<side>.player[N]..." without the prefix.
static bool blockNumber(const char *text, unsigned *number, const char **rest) {
	size_t length = strspn(text, "0123456789");
	if (!length || length > 3 || text[length] != ']') {
		return false;
	}
	*number = (unsigned)strtoul(text, NULL, 10);
	*rest = text + length + 1;
	return true;
}

// Records where the number in a block name starts, which the name token must spell literally.
static void recordName(Scan *scan, Side *side, const ConfigField *field, unsigned number) {
	char expected[32];
	int length = snprintf(expected, sizeof expected, "\"%s%u]", side->prefix, number);
	size_t offset = field->nameOffset + 1 + side->prefixLength;
	if (length <= 0 || field->nameLength < (size_t)length ||
			memcmp(scan->json + field->nameOffset, expected, (size_t)length) != 0) {
		sideError(side, "%s is written with escapes; lineup cannot rename it", field->name);
	} else if (!buffer_append(&side->numbers, &offset, sizeof offset)) {
		scan->outOfMemory = true;
	} else {
		++side->blocks[side->count - 1].names;
	}
}

static void startBlock(Side *side, const ConfigField *field, unsigned number) {
	if (side->ended) {
		sideError(
			side, "%s.player[%u] is apart from the other %s player blocks", side->name, number, side->name
		);
	} else if (number != side->count) {
		sideError(
			side,
			"%s.player[%u] is in place %u: the blocks were moved without renaming them",
			side->name,
			number,
			side->count
		);
	} else if (side->count == FIXTURE_PLAYERS) {
		sideError(side, "%s has too many player blocks", side->name);
	} else {
		Block *block = &side->blocks[side->count++];
		block->start = field->objectOffset;
		block->firstName = side->numbers.length / sizeof(size_t);
		side->inBlocks = true;
	}
}

// A field named "<side>.player[...": a block's first field, or one of the last block's own.
static void scanBlockName(Scan *scan, Side *side, const ConfigField *field) {
	unsigned number = 0;
	const char *rest = NULL;
	if (!blockNumber(field->name + side->prefixLength, &number, &rest)) {
		sideError(side, "unexpected %s among the %s player blocks", field->name, side->name);
		return;
	}
	if (!*rest) {
		startBlock(side, field, number);
	} else if (*rest != '.' || !side->inBlocks || number + 1 != side->count) {
		if (side->inBlocks) {
			sideError(
				side,
				"unexpected %s in the block of %s.player[%u]",
				field->name,
				side->name,
				side->count - 1
			);
		} else {
			sideError(side, "unexpected %s outside the %s player blocks", field->name, side->name);
		}
	}
	if (!side->error[0]) {
		recordName(scan, side, field, number);
	}
}

static bool isMarkingName(const char *name) {
	static const char suffix[] = ".bits[1]";
	size_t length = strlen(name);
	return strncmp(name, "manager.tactic.plan[", 20) == 0 && strstr(name, ".slot[") &&
				 strstr(name, ".player_instructions[") && length > sizeof suffix &&
				 strcmp(name + length - (sizeof suffix - 1), suffix) == 0;
}

static void scanName(Scan *scan, Side *side, const ConfigField *field) {
	const char *name = field->name;
	bool block = strncmp(name, side->prefix, side->prefixLength) == 0;
	if (side->inBlocks && !block) {
		side->inBlocks = false;
		side->ended = true;
	}
	size_t length = strlen(side->name);
	if (strncmp(name, side->name, length) == 0 && name[length] == '.') {
		const char *rest = name + length + 1;
		if (strcmp(rest, "player_count") == 0 && field->kind == CONFIG_U8) {
			side->playerCount = (FixtureNumber){.value = fieldValue(field), .known = true};
		} else if (strcmp(rest, "manager.tactic.part_4240") == 0) {
			side->recordsCountNext = true;
		} else if (isMarkingName(rest)) {
			side->markingNext = true;
		}
	}
	if (block && !side->error[0]) {
		scanBlockName(scan, side, field);
	}
}

// Extends the last block over a field of it, collecting the block's header.
static void scanBlockField(Side *side, const ConfigField *field) {
	Block *block = &side->blocks[side->count - 1];
	block->end = field->objectOffset + field->objectLength;
	if (block->headerDone) {
		return;
	}
	const char *bracket = field->name[0] ? strchr(field->name + side->prefixLength, ']') : NULL;
	if (bracket && strcmp(bracket + 1, ".ratings") == 0) {
		block->headerDone = true;
		return;
	}
	block->header[block->headerFields++] =
		(HeaderField){.kind = field->kind, .offset = field->valueOffset, .length = field->valueLength};
	block->headerDone = block->headerFields == HEADER_FIELDS;
}

static void scanField(void *context, const ConfigField *field) {
	Scan *scan = context;
	for (unsigned s = 0; s < 2; ++s) {
		Side *side = &scan->sides[s];
		scanFollowing(scan, side, field);
		if (field->name[0]) {
			scanName(scan, side, field);
		}
		if (side->inBlocks && !side->error[0]) {
			scanBlockField(side, field);
		}
	}
}

static size_t skipSpace(JsonReader *reader) {
	while (
		reader->position < reader->end && *reader->position && strchr(" \t\r\n", *reader->position)) {
		++reader->position;
	}
	return (size_t)(reader->position - reader->begin);
}

static bool isKey(const ByteBuffer *key, const char *name) {
	size_t length = strlen(name);
	return key->length == length && memcmp(key->data, name, length) == 0;
}

// Finds the top-level "fields" key and any markers.
static bool scanHeader(const char *json, size_t length, Header *header) {
	*header = (Header){0};
	JsonReader reader;
	jsonReader_init(&reader, json, length);
	ByteBuffer key = {0};
	bool found = false;
	bool ok = jsonReader_consume(&reader, '{');
	while (ok) {
		size_t space = (size_t)(reader.position - reader.begin);
		size_t keyOffset = skipSpace(&reader);
		ok = jsonReader_readString(&reader, &key) && jsonReader_consume(&reader, ':');
		size_t value = skipSpace(&reader);
		ok = ok && jsonReader_skipValue(&reader);
		size_t valueLength = (size_t)(reader.position - reader.begin) - value;
		if (ok && isKey(&key, "fields")) {
			found = true;
			header->fieldsKey = keyOffset;
			header->fieldsSpace = space;
		}
		for (unsigned s = 0; ok && s < 2; ++s) {
			if (isKey(&key, s ? "away_lineup" : "home_lineup")) {
				header->marker[s] = value;
				header->markerLength[s] = valueLength;
				header->markerTrue[s] = valueLength == 4 && memcmp(json + value, "true", 4) == 0;
			}
		}
		if (!ok || !jsonReader_consume(&reader, ',')) {
			break;
		}
	}
	buffer_free(&key);
	return ok && found;
}

static bool sameHeader(const char *json, const Block *a, const Block *b) {
	if (a->headerFields != b->headerFields) {
		return false;
	}
	for (unsigned i = 0; i < a->headerFields; ++i) {
		const HeaderField *x = &a->header[i];
		const HeaderField *y = &b->header[i];
		if (x->kind != y->kind || x->length != y->length ||
				memcmp(json + x->offset, json + y->offset, x->length) != 0) {
			return false;
		}
	}
	return true;
}

// The refusals of a side that is to be reordered.
static bool
checkSide(const char *json, const Side *side, const uint8_t *from, char *error, size_t errorSize) {
	unsigned count = side->playerCount.known ? (unsigned)side->playerCount.value : 0;
	if (side->error[0]) {
		setError(error, errorSize, "%s", side->error);
	} else if (!side->playerCount.known) {
		setError(error, errorSize, "the fixture has no %s.player_count", side->name);
	} else if (side->count != count) {
		setError(
			error,
			errorSize,
			"%s has %u player blocks, but its player_count is %u",
			side->name,
			side->count,
			count
		);
	} else if (!isPermutation(from, count)) {
		setError(error, errorSize, "the new %s line-up does not place each player once", side->name);
	} else {
		const Block *first = NULL;
		unsigned firstPlace = 0;
		for (unsigned place = 0; place < count; ++place) {
			const Block *block = &side->blocks[from[place]];
			if (from[place] == place) {
				continue;
			}
			if (!first) {
				first = block;
				firstPlace = from[place];
			} else if (!sameHeader(json, first, block)) {
				setError(
					error,
					errorSize,
					"%s.player[%u] and player[%u] start with different values; lineup cannot move them",
					side->name,
					firstPlace < from[place] ? firstPlace : from[place],
					firstPlace < from[place] ? from[place] : firstPlace
				);
				return false;
			}
		}
		return true;
	}
	return false;
}

static bool
addEdit(ByteBuffer *edits, size_t offset, size_t length, EditKind kind, unsigned value) {
	Edit edit = {offset, length, kind, value, (unsigned)(edits->length / sizeof edit)};
	return buffer_append(edits, &edit, sizeof edit);
}

// The markers, the re-pointed markings and the reordered blocks, in no particular order.
static bool collectEdits(
	const Scan *scan,
	const Header *header,
	const FixtureLineup *lineup,
	const bool sides[2],
	const bool marks[2],
	ByteBuffer *edits,
	FixtureLineupResult *result
) {
	bool ok = true;
	for (unsigned s = 0; ok && s < 2; ++s) {
		const Side *side = &scan->sides[s];
		const Side *opponent = &scan->sides[1 - s];
		if (!marks[s] || (sides[s] && !side->count)) {
			continue;
		}
		if (!header->markerLength[s]) {
			ok = addEdit(edits, header->fieldsKey, 0, EDIT_MARKER, s);
		} else if (!header->markerTrue[s]) {
			ok = addEdit(edits, header->marker[s], header->markerLength[s], EDIT_TRUE, 0);
		}
		if (!sides[s]) {
			continue;
		}
		uint8_t placeOf[FIXTURE_PLAYERS];
		for (unsigned place = 0; place < side->count; ++place) {
			placeOf[lineup->from[s][place]] = (uint8_t)place;
		}
		size_t markings = opponent->markings.length / sizeof(Marking);
		for (size_t i = 0; ok && i < markings; ++i) {
			Marking marking;
			memcpy(&marking, opponent->markings.data + i * sizeof marking, sizeof marking);
			if (marking.value < side->count && placeOf[marking.value] != marking.value) {
				ok = addEdit(edits, marking.offset, marking.length, EDIT_NUMBER, placeOf[marking.value]);
				++result->markings[s];
				result->marked[s][placeOf[marking.value]] = true;
			}
		}
		result->opponentRecords[s] = opponent->recordsUneven;
		const Block *blocks = side->blocks;
		ok =
			ok && addEdit(
							edits, blocks[0].start, blocks[side->count - 1].end - blocks[0].start, EDIT_BLOCKS, s
						);
	}
	return ok;
}

static int compareEdits(const void *a, const void *b) {
	const Edit *left = a;
	const Edit *right = b;
	if (left->offset != right->offset) {
		return left->offset < right->offset ? -1 : 1;
	}
	return (left->order > right->order) - (left->order < right->order);
}

// Block `from` of a side as block `place`: its text with each name's number replaced.
static bool writeBlock(const Writer *writer, const Side *side, unsigned from, unsigned place) {
	const Block *block = &side->blocks[from];
	char number[8];
	char old[8];
	int numberLength = snprintf(number, sizeof number, "%u", place);
	int oldLength = snprintf(old, sizeof old, "%u", from);
	size_t at = block->start;
	bool ok = numberLength > 0 && oldLength > 0;
	for (size_t i = 0; ok && i < block->names; ++i) {
		size_t offset;
		memcpy(&offset, side->numbers.data + (block->firstName + i) * sizeof offset, sizeof offset);
		ok = buffer_append(writer->out, writer->json + at, offset - at) &&
				 buffer_append(writer->out, number, (size_t)numberLength);
		at = offset + (size_t)oldLength;
	}
	return ok && buffer_append(writer->out, writer->json + at, block->end - at);
}

// A side's blocks in their new order, with the text between blocks where it was.
static bool writeBlocks(const Writer *writer, unsigned s) {
	const Side *side = &writer->scan->sides[s];
	bool ok = true;
	for (unsigned place = 0; ok && place < side->count; ++place) {
		ok = writeBlock(writer, side, writer->lineup->from[s][place], place);
		if (ok && place + 1 < side->count) {
			size_t end = side->blocks[place].end;
			ok = buffer_append(writer->out, writer->json + end, side->blocks[place + 1].start - end);
		}
	}
	return ok;
}

static bool writeEdit(const Writer *writer, const Edit *edit) {
	ByteBuffer *out = writer->out;
	const Header *header = writer->header;
	char text[32];
	switch (edit->kind) {
		case EDIT_MARKER:
			(void)snprintf(text, sizeof text, "\"%s_lineup\": true,", sideNames[edit->value]);
			return buffer_append(out, text, strlen(text)) &&
						 buffer_append(
							 out, writer->json + header->fieldsSpace, header->fieldsKey - header->fieldsSpace
						 );
		case EDIT_TRUE: return buffer_append(out, "true", 4);
		case EDIT_NUMBER:
			(void)snprintf(text, sizeof text, "%u", edit->value);
			return buffer_append(out, text, strlen(text));
		case EDIT_BLOCKS: return writeBlocks(writer, edit->value);
	}
	return false;
}

static bool writeEdits(const Writer *writer, ByteBuffer *edits, size_t length) {
	size_t count = edits->length / sizeof(Edit);
	if (count) {
		qsort(edits->data, count, sizeof(Edit), compareEdits);
	}
	size_t copied = 0;
	bool ok = true;
	for (size_t i = 0; ok && i < count; ++i) {
		Edit edit;
		memcpy(&edit, edits->data + i * sizeof edit, sizeof edit);
		ok = edit.offset >= copied &&
				 buffer_append(writer->out, writer->json + copied, edit.offset - copied) &&
				 writeEdit(writer, &edit);
		copied = edit.offset + edit.length;
	}
	return ok && buffer_append(writer->out, writer->json + copied, length - copied);
}

static bool samePlayer(const FixturePlayer *a, const FixturePlayer *b) {
	return strcmp(a->name, b->name) == 0 && a->personUid.known == b->personUid.known &&
				 a->personUid.value == b->personUid.value;
}

// The result encodes to as many bytes, each place holds the player the order put there, and each
// side marked has its marker.
static bool verify(
	const char *json,
	size_t length,
	const ByteBuffer *out,
	const FixtureLineup *lineup,
	const bool sides[2],
	const bool marks[2],
	char *error,
	size_t errorSize
) {
	ByteBuffer before = {0};
	ByteBuffer after = {0};
	FixtureData *data = malloc(2 * sizeof *data);
	char problem[192] = "out of memory";
	const char *text = buffer_string(out);
	bool ok = data && matchConfig_encode(json, length, &before, NULL, problem, sizeof problem) &&
						matchConfig_encode(text, out->length, &after, NULL, problem, sizeof problem) &&
						fixtureData_read(json, length, &data[0], problem, sizeof problem) &&
						fixtureData_read(text, out->length, &data[1], problem, sizeof problem);
	if (ok && before.length != after.length) {
		(void)snprintf(problem, sizeof problem, "it encodes to another length");
		ok = false;
	}
	ConfigOrigin origin;
	ok = ok && matchConfig_readOrigin(text, out->length, &origin);
	for (unsigned s = 0; ok && s < 2; ++s) {
		const FixtureTeam *was = &data[0].teams[s];
		const FixtureTeam *now = &data[1].teams[s];
		unsigned count = playerCount(was);
		ok = (!marks[s] || origin.lineup[s]) &&
				 (!sides[s] || (now->blocksInOrder && playerCount(now) == count));
		for (unsigned place = 0; ok && sides[s] && place < count; ++place) {
			ok = samePlayer(&was->players[lineup->from[s][place]], &now->players[place]);
		}
		if (!ok) {
			(void)snprintf(
				problem, sizeof problem, "the %s players are not in the new order", sideNames[s]
			);
		}
	}
	if (!ok) {
		setError(error, errorSize, "lineup's result failed its check: %s", problem);
	}
	free(data);
	buffer_free(&before);
	buffer_free(&after);
	return ok;
}
