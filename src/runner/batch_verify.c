// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_verify.h"

#include <stdlib.h>
#include <string.h>


enum {
	LINE_LIMIT = 4096, // longer than any row the harness writes
	BLOCK_SIZE = 1 << 16,
	RESULT_FIELDS = 7, // seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms
	TEAM_FIELDS = 44,
	PLAYER_FIELDS = 62,
	FAILED_FIELDS = 3,
	MOST_FIELDS = PLAYER_FIELDS + 1, // more than any file's row has
	SQUAD_LIMIT = 40,                // MATCH_STATS_MAX_PLAYERS
	TEAM_IDENTITY = 6,               // index,seed,side,team_uid,squad,used
	PLAYER_RATING = 15,              // the rating's column; the values follow it
	MINUTE_LIMIT = 255,
};

const char batchVerify_partResultsHeader[] =
	"seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n";
const char batchVerify_chunkResultsHeader[] =
	"index,seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n";
// As the harness's matchStats_playerRowsHeader and matchStats_teamRowsHeader (the batch suite
// checks that they agree).
const char batchVerify_playersHeader[] =
	"index,seed,side,team_uid,slot,person_uid,in_squad,started,appeared,minutes,on_minute,"
	"off_minute,injured_minute,sent_off_minute,stats_flags,rating,goals,assists,shots,"
	"shots_on_target,blocked_shots,long_shots,xg,xa,key_passes,clear_cut_chances,half_chances,"
	"passes_attempted,passes_completed,progressive_passes,crosses_attempted,crosses_completed,"
	"dribbles,tackles_attempted,tackles_won,key_tackles,interceptions,clearances,blocks,"
	"shots_blocked,pressures_attempted,pressures_completed,headers_attempted,headers_won,"
	"key_headers,fouls,fouled,offsides,yellow_cards,red_cards,corners,throw_ins,"
	"defensive_free_kicks,attacking_free_kicks,saves_held,saves_parried,saves_tipped,"
	"goals_conceded,possession_won,possession_lost,high_intensity_sprints,distance_km\n";
const char batchVerify_teamsHeader[] =
	"index,seed,side,team_uid,squad,used,goals,possession,average_rating,xg,shots,shots_on_target,"
	"blocked_shots,long_shots,clear_cut_chances,half_chances,corners,fouls,fouled,offsides,"
	"passes_attempted,passes_completed,progressive_passes,crosses_attempted,crosses_completed,"
	"tackles_attempted,tackles_won,headers_attempted,headers_won,interceptions,clearances,blocks,"
	"shots_blocked,pressures_attempted,pressures_completed,yellow_cards,red_cards,saves,distance_km,"
	"key_passes,dribbles,possession_won,possession_lost,high_intensity_sprints\n";
const char batchVerify_failedHeader[] = "index,seed,stage\n";

static const char *const sideNames[] = {"home", "away"};
static const char digits[] = "0123456789";

typedef enum LineKind {
	LINE_NONE,     // the end of the file
	LINE_COMPLETE, // ended by a newline
	LINE_TORN,     // the file ends inside it
} LineKind;

// Reads an input a line at a time, hashing everything read if asked.
typedef struct LineReader {
	FILE *file;
	size_t start; // the unread bytes of `block`
	size_t end;
	bool ended;
	bool failed; // a read error
	bool hashing;
	bool hashContent;
	Sha256 raw;
	Sha256 content;
	uint64_t bytes;
	uint64_t lines; // complete lines
	size_t length;  // of `line`, which holds at most LINE_LIMIT bytes of it
	bool overlong;
	uint8_t block[BLOCK_SIZE];
	char line[LINE_LIMIT];
} LineReader;

// A line's comma-separated fields.
typedef struct Fields {
	const char *text[MOST_FIELDS];
	size_t length[MOST_FIELDS];
	size_t count; // MOST_FIELDS if there are that many or more
} Fields;

// A verification under way.
typedef struct Walk {
	const BatchVerify *request;
	BatchVerified *out;
	LineReader *readers[BATCH_ROLES]; // NULL where there is no input
	ByteBuffer pending[BATCH_ROLES];  // the rows of the index being verified
	ResultsSummary match;             // its result
	ByteBuffer failedRow;             // the next failed row, read ahead
	bool hasFailed;
	uint64_t failedIndex;
	uint64_t lastFailed; // the previous failed row's index
	bool anyFailed;
	bool failedRejected; // a failed row could not be used, which ends them
	size_t excluded;     // the next of request->excluded
	bool failed;         // memory, or a read or write
} Walk;

static bool openReaders(Walk *walk);
static bool checkHeaders(Walk *walk);
static void walkIndices(Walk *walk);
static bool coverIndex(Walk *walk, uint64_t index);
static bool readMatch(Walk *walk, uint64_t index, uint32_t seed);
static bool readTeam(Walk *walk, uint64_t index, uint32_t seed, size_t side, unsigned *squad);
static bool readPlayer(
	Walk *walk,
	uint64_t index,
	uint32_t seed,
	size_t side,
	const Fields *team,
	uint64_t slot
);
static bool checkPlayerValues(const Fields *fields);
static bool readResultFields(const Fields *fields, size_t at, uint32_t seed, ResultsSummary *match);
static void readNextFailed(Walk *walk);
static bool readRow(Walk *walk, BatchRole role, Fields *fields, size_t count);
static void keepRow(Walk *walk, BatchRole role, const char *prefix);
static void commitIndex(Walk *walk);
static void finishInputs(Walk *walk);
static LineKind readLine(LineReader *reader);
static bool fill(LineReader *reader);
static void endLine(LineReader *reader, bool complete);
static void hashContentLine(Sha256 *hash, const char *line, size_t length, bool complete);
static void split(const char *line, size_t length, Fields *fields);
static bool isWord(const Fields *fields, size_t at, const char *word);
static bool readNumber(const Fields *fields, size_t at, uint64_t max, uint64_t *value);
static bool isNumber(const Fields *fields, size_t at, uint64_t max);
static bool isIndex(const Fields *fields, size_t at, uint64_t index);
static bool isFlag(const Fields *fields, size_t at);
static bool isValue(const Fields *fields, size_t at);
static bool isRating(const Fields *fields, size_t at);
static bool isHex16(const Fields *fields, size_t at);
static bool isMilliseconds(const Fields *fields, size_t at);
static size_t spanOf(const char *text, size_t length, const char *set);
static double milliseconds(const Fields *fields, size_t at);
static void finishHash(Sha256 *hash, char hex[SHA256_HEX_LENGTH]);

bool batchVerify_run(const BatchVerify *request, BatchVerified *out) {
	*out = (BatchVerified){0};
	Walk walk = {.request = request, .out = out};
	if (openReaders(&walk) && checkHeaders(&walk)) {
		readNextFailed(&walk);
		walkIndices(&walk);
	}
	if (request->hashInputs) {
		finishInputs(&walk);
	}
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		walk.failed = walk.failed || (walk.readers[role] && walk.readers[role]->failed);
		free(walk.readers[role]);
		buffer_free(&walk.pending[role]);
	}
	buffer_free(&walk.failedRow);
	for (size_t role = 0; request->outputs && role < BATCH_ROLES; ++role) {
		walk.failed = walk.failed || request->outputs[role].failed;
	}
	return !walk.failed;
}

void batchSink_start(BatchSink *sink, FILE *file, const char *header, bool hashContent) {
	*sink = (BatchSink){.file = file, .hashContent = hashContent};
	sha256_start(&sink->raw);
	sha256_start(&sink->content);
	batchSink_line(sink, header, strlen(header));
	sink->rows = 0;
}

void batchSink_line(BatchSink *sink, const char *line, size_t length) {
	if (sink->file && !sink->failed && fwrite(line, 1, length, sink->file) != length) {
		sink->failed = true;
	}
	sha256_add(&sink->raw, line, length);
	if (sink->hashContent) {
		hashContentLine(&sink->content, line, length - 1, true);
	}
	sink->bytes += length;
	++sink->rows;
}

void batchSink_finish(
	BatchSink *sink,
	char sha256[SHA256_HEX_LENGTH],
	char contentSha256[SHA256_HEX_LENGTH]
) {
	finishHash(&sink->raw, sha256);
	if (sink->hashContent) {
		finishHash(&sink->content, contentSha256);
	} else {
		contentSha256[0] = 0;
	}
}

// A reader for each input that is there. False if an expected file is missing or memory runs out.
static bool openReaders(Walk *walk) {
	const BatchVerify *request = walk->request;
	bool expected[BATCH_ROLES] = {true, request->perMatch, request->perMatch, request->failed};
	bool complete = true;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		FILE *file = expected[role] ? request->inputs[role] : NULL;
		complete = complete && (!expected[role] || file);
		if (!file) {
			continue;
		}
		LineReader *reader = malloc(sizeof *reader);
		if (!reader) {
			walk->failed = true;
			return false;
		}
		reader->file = file;
		reader->start = reader->end = 0;
		reader->ended = reader->failed = false;
		reader->hashing = request->hashInputs;
		reader->hashContent = request->hashInputs && role == BATCH_RESULTS;
		reader->bytes = reader->lines = 0;
		reader->length = 0;
		reader->overlong = false;
		sha256_start(&reader->raw);
		sha256_start(&reader->content);
		walk->readers[role] = reader;
	}
	return complete;
}

// Each input starts with its header.
static bool checkHeaders(Walk *walk) {
	const char *headers[BATCH_ROLES] = {
		walk->request->chunk ? batchVerify_chunkResultsHeader : batchVerify_partResultsHeader,
		batchVerify_playersHeader,
		batchVerify_teamsHeader,
		batchVerify_failedHeader,
	};
	bool ok = true;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		LineReader *reader = walk->readers[role];
		if (reader) {
			size_t length = strlen(headers[role]) - 1;
			ok = readLine(reader) == LINE_COMPLETE && !reader->overlong && reader->length == length &&
					 memcmp(reader->line, headers[role], length) == 0 && ok;
		}
	}
	return ok;
}

static void walkIndices(Walk *walk) {
	const BatchVerify *request = walk->request;
	for (uint64_t i = 0; i < request->count && !walk->failed; ++i) {
		for (size_t role = 0; role < BATCH_ROLES; ++role) {
			buffer_clear(&walk->pending[role]);
		}
		walk->match = (ResultsSummary){0};
		if (!coverIndex(walk, request->first + i)) {
			return;
		}
		commitIndex(walk);
		++walk->out->verified;
	}
}

// Whether `index` is covered: excluded, failed or played with its rows in every file.
static bool coverIndex(Walk *walk, uint64_t index) {
	const BatchVerify *request = walk->request;
	if (walk->excluded < request->excludedCount && request->excluded[walk->excluded] == index) {
		++walk->excluded;
		return true;
	}
	if (walk->hasFailed && walk->failedIndex == index) {
		if (!buffer_append(
					&walk->pending[BATCH_FAILED], walk->failedRow.data, walk->failedRow.length
				)) {
			walk->failed = true;
			return false;
		}
		readNextFailed(walk);
		return true;
	}
	return readMatch(walk, index, batchSeeds_at(request->seeds, index));
}

// The result row of a played index, then with per-match rows its two team rows and its players'.
static bool readMatch(Walk *walk, uint64_t index, uint32_t seed) {
	bool chunk = walk->request->chunk;
	Fields fields;
	size_t at = chunk ? 1 : 0;
	if (!readRow(walk, BATCH_RESULTS, &fields, RESULT_FIELDS + at) ||
			(chunk && !isIndex(&fields, 0, index)) ||
			!readResultFields(&fields, at, seed, &walk->match)) {
		return false;
	}
	char prefix[24];
	(void)snprintf(prefix, sizeof prefix, "%llu,", (unsigned long long)index);
	keepRow(walk, BATCH_RESULTS, chunk ? "" : prefix);
	if (!walk->request->perMatch) {
		return true;
	}
	for (size_t side = 0; side < 2; ++side) {
		unsigned squad = 0;
		if (!readTeam(walk, index, seed, side, &squad)) {
			return false;
		}
	}
	return true;
}

// A team row and its squad's player rows, in slot order.
static bool readTeam(Walk *walk, uint64_t index, uint32_t seed, size_t side, unsigned *squad) {
	Fields team;
	uint64_t size = 0;
	uint64_t used = 0;
	uint32_t expected = walk->request->teams[side];
	bool ok = readRow(walk, BATCH_TEAMS, &team, TEAM_FIELDS) && isIndex(&team, 0, index) &&
						isIndex(&team, 1, seed) && isWord(&team, 2, sideNames[side]) &&
						isNumber(&team, 3, UINT32_MAX) && (!expected || isIndex(&team, 3, expected)) &&
						readNumber(&team, 4, SQUAD_LIMIT, &size) && readNumber(&team, 5, size, &used);
	for (size_t i = TEAM_IDENTITY; ok && i < TEAM_FIELDS; ++i) {
		ok = isValue(&team, i);
	}
	if (!ok) {
		return false;
	}
	keepRow(walk, BATCH_TEAMS, "");
	*squad = (unsigned)size;
	for (uint64_t slot = 0; slot < size; ++slot) {
		if (!readPlayer(walk, index, seed, side, &team, slot)) {
			return false;
		}
	}
	return true;
}

static bool readPlayer(
	Walk *walk,
	uint64_t index,
	uint32_t seed,
	size_t side,
	const Fields *team,
	uint64_t slot
) {
	Fields player;
	bool ok = readRow(walk, BATCH_PLAYERS, &player, PLAYER_FIELDS) && isIndex(&player, 0, index) &&
						isIndex(&player, 1, seed) && isWord(&player, 2, sideNames[side]) &&
						player.length[3] == team->length[3] &&
						memcmp(player.text[3], team->text[3], team->length[3]) == 0 &&
						isIndex(&player, 4, slot) && isNumber(&player, 5, UINT32_MAX) && isFlag(&player, 6) &&
						isFlag(&player, 7) && isFlag(&player, 8) && isNumber(&player, 9, UINT32_MAX) &&
						isNumber(&player, 10, MINUTE_LIMIT) && isNumber(&player, 11, MINUTE_LIMIT) &&
						isNumber(&player, 12, MINUTE_LIMIT) && isNumber(&player, 13, MINUTE_LIMIT) &&
						isNumber(&player, 14, UINT32_MAX) && checkPlayerValues(&player);
	if (ok) {
		keepRow(walk, BATCH_PLAYERS, "");
	}
	return ok;
}

// The rating and values: all empty for a player who did not appear, all there for one who did.
static bool checkPlayerValues(const Fields *fields) {
	bool appeared = fields->text[8][0] == '1';
	for (size_t i = PLAYER_RATING; i < PLAYER_FIELDS; ++i) {
		bool ok = !appeared            ? fields->length[i] == 0
							: i == PLAYER_RATING ? isRating(fields, i)
																	 : isValue(fields, i);
		if (!ok) {
			return false;
		}
	}
	return true;
}

// A results row's fields from `at`: the planned seed, the goals and clock, finished, the final RNG
// state and the time, with the match's result in `match`.
static bool
readResultFields(const Fields *fields, size_t at, uint32_t seed, ResultsSummary *match) {
	uint64_t home = 0;
	uint64_t away = 0;
	if (!isIndex(fields, at, seed) || !readNumber(fields, at + 1, UINT32_MAX, &home) ||
			!readNumber(fields, at + 2, UINT32_MAX, &away) || !isNumber(fields, at + 3, UINT32_MAX) ||
			!isFlag(fields, at + 4) || !isHex16(fields, at + 5) || !isMilliseconds(fields, at + 6)) {
		return false;
	}
	*match = (ResultsSummary){
		.matches = 1,
		.home = home > away,
		.draw = home == away,
		.away = home < away,
		.homeGoals = home,
		.awayGoals = away,
		.unfinished = fields->text[at + 4][0] == '0',
		.totalMs = milliseconds(fields, at + 6),
	};
	return true;
}

// Reads ahead to the next failed row: a complete line for an index after the last one, within
// the indices being verified, with that index's seed. Anything else ends the failed rows.
static void readNextFailed(Walk *walk) {
	walk->hasFailed = false;
	const BatchVerify *request = walk->request;
	LineReader *reader = walk->readers[BATCH_FAILED];
	LineKind kind = reader ? readLine(reader) : LINE_NONE;
	if (kind == LINE_NONE) {
		return;
	}
	Fields fields;
	split(reader->line, reader->length, &fields);
	uint64_t index = 0;
	if (kind != LINE_COMPLETE || reader->overlong || fields.count != FAILED_FIELDS ||
			!readNumber(&fields, 0, UINT64_MAX, &index) || index < request->first ||
			index - request->first >= request->count || (walk->anyFailed && index <= walk->lastFailed) ||
			!isIndex(&fields, 1, batchSeeds_at(request->seeds, index)) ||
			!isWord(&fields, 2, "rebuild")) {
		walk->failedRejected = true;
		return;
	}
	buffer_clear(&walk->failedRow);
	if (!buffer_append(&walk->failedRow, reader->line, reader->length) ||
			!buffer_append(&walk->failedRow, "\n", 1)) {
		walk->failed = true;
		return;
	}
	walk->hasFailed = true;
	walk->failedIndex = index;
	walk->lastFailed = index;
	walk->anyFailed = true;
}

// The next complete line of a file, as exactly `count` fields.
static bool readRow(Walk *walk, BatchRole role, Fields *fields, size_t count) {
	LineReader *reader = walk->readers[role];
	if (!reader || readLine(reader) != LINE_COMPLETE || reader->overlong) {
		return false;
	}
	split(reader->line, reader->length, fields);
	return fields->count == count;
}

// Keeps the line just read for the index being verified, after `prefix`.
static void keepRow(Walk *walk, BatchRole role, const char *prefix) {
	const LineReader *reader = walk->readers[role];
	ByteBuffer *pending = &walk->pending[role];
	if (!buffer_append(pending, prefix, strlen(prefix)) ||
			!buffer_append(pending, reader->line, reader->length) || !buffer_append(pending, "\n", 1)) {
		walk->failed = true;
	}
}

// The index is covered: its rows go to the outputs and count.
static void commitIndex(Walk *walk) {
	BatchVerified *out = walk->out;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		const ByteBuffer *pending = &walk->pending[role];
		const char *line = (const char *)pending->data;
		const char *end = line + pending->length;
		while (line < end) {
			const char *newline = memchr(line, '\n', (size_t)(end - line));
			size_t length = (size_t)(newline - line) + 1;
			if (walk->request->outputs) {
				batchSink_line(&walk->request->outputs[role], line, length);
			}
			++out->rows[role];
			line += length;
		}
	}
	out->summary.matches += walk->match.matches;
	out->summary.home += walk->match.home;
	out->summary.draw += walk->match.draw;
	out->summary.away += walk->match.away;
	out->summary.homeGoals += walk->match.homeGoals;
	out->summary.awayGoals += walk->match.awayGoals;
	out->summary.unfinished += walk->match.unfinished;
	out->summary.totalMs += walk->match.totalMs;
}

// Notes whether anything follows the verified rows, then reads every input to its end for its
// hashes.
static void finishInputs(Walk *walk) {
	BatchVerified *out = walk->out;
	out->trailing = walk->hasFailed || walk->failedRejected;
	for (size_t role = 0; role < BATCH_ROLES; ++role) {
		LineReader *reader = walk->readers[role];
		if (!reader) {
			continue;
		}
		while (readLine(reader) != LINE_NONE) {
			out->trailing = true;
		}
		BatchInput *input = &out->inputs[role];
		input->bytes = reader->bytes;
		input->rows = reader->lines ? reader->lines - 1 : 0;
		finishHash(&reader->raw, input->sha256);
		if (reader->hashContent) {
			finishHash(&reader->content, input->contentSha256);
		}
	}
}

static LineKind readLine(LineReader *reader) {
	reader->length = 0;
	reader->overlong = false;
	bool any = false;
	for (;;) {
		if (reader->start == reader->end && !fill(reader)) {
			if (!any) {
				return LINE_NONE;
			}
			endLine(reader, false);
			return LINE_TORN;
		}
		const uint8_t *from = reader->block + reader->start;
		size_t available = reader->end - reader->start;
		const uint8_t *newline = memchr(from, '\n', available);
		size_t take = newline ? (size_t)(newline - from) : available;
		size_t room = LINE_LIMIT - reader->length;
		memcpy(reader->line + reader->length, from, take < room ? take : room);
		reader->overlong = reader->overlong || take > room;
		reader->length += take < room ? take : room;
		reader->start += take + (newline != NULL);
		any = true;
		if (newline) {
			endLine(reader, true);
			return LINE_COMPLETE;
		}
	}
}

static bool fill(LineReader *reader) {
	if (reader->ended) {
		return false;
	}
	size_t got = fread(reader->block, 1, sizeof reader->block, reader->file);
	if (!got) {
		reader->ended = true;
		reader->failed = ferror(reader->file) != 0;
		return false;
	}
	if (reader->hashing) {
		sha256_add(&reader->raw, reader->block, got);
	}
	reader->bytes += got;
	reader->start = 0;
	reader->end = got;
	return true;
}

static void endLine(LineReader *reader, bool complete) {
	reader->lines += complete;
	if (reader->hashContent) {
		hashContentLine(&reader->content, reader->line, reader->length, complete);
	}
}

// A line without its last field (and the comma before it) as a run manifest's content_sha256 has
// it, with its newline if it had one.
static void hashContentLine(Sha256 *hash, const char *line, size_t length, bool complete) {
	size_t kept = length;
	while (kept && line[kept - 1] != ',') {
		--kept;
	}
	sha256_add(hash, line, kept ? kept - 1 : length);
	if (complete) {
		sha256_add(hash, "\n", 1);
	}
}

static void split(const char *line, size_t length, Fields *fields) {
	fields->count = 0;
	size_t start = 0;
	for (size_t i = 0; i <= length; ++i) {
		if (i < length && line[i] != ',') {
			continue;
		}
		if (fields->count == MOST_FIELDS) {
			return;
		}
		fields->text[fields->count] = line + start;
		fields->length[fields->count++] = i - start;
		start = i + 1;
	}
}

static bool isWord(const Fields *fields, size_t at, const char *word) {
	size_t length = strlen(word);
	return fields->length[at] == length && memcmp(fields->text[at], word, length) == 0;
}

// A decimal number as printf writes it (no sign or leading zero), at most `max`.
static bool readNumber(const Fields *fields, size_t at, uint64_t max, uint64_t *value) {
	const char *text = fields->text[at];
	size_t length = fields->length[at];
	*value = 0;
	if (!length || (length > 1 && text[0] == '0')) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned digit = (unsigned)(text[i] - '0');
		if (text[i] < '0' || text[i] > '9' || *value > (max - digit) / 10) {
			return false;
		}
		*value = *value * 10 + digit;
	}
	return true;
}

static bool isNumber(const Fields *fields, size_t at, uint64_t max) {
	uint64_t value = 0;
	return readNumber(fields, at, max, &value);
}

static bool isIndex(const Fields *fields, size_t at, uint64_t index) {
	uint64_t value = 0;
	return readNumber(fields, at, UINT64_MAX, &value) && value == index;
}

static bool isFlag(const Fields *fields, size_t at) {
	return isWord(fields, at, "0") || isWord(fields, at, "1");
}

// A statistic as the harness prints it: a count, or "%.9g" of a float ("0.412337601", "1e-05").
static bool isValue(const Fields *fields, size_t at) {
	size_t length = fields->length[at];
	return length &&
				 spanOf(fields->text[at], length, "0123456789abcdefghijklmnopqrstuvwxyz.+-") == length;
}

// Hundredths as "-0.05" or "7.12".
static bool isRating(const Fields *fields, size_t at) {
	const char *text = fields->text[at];
	size_t length = fields->length[at];
	size_t sign = length && text[0] == '-';
	return length >= sign + 4 && spanOf(text + sign, length - sign, digits) == length - sign - 3 &&
				 text[length - 3] == '.' && spanOf(text + length - 2, 2, digits) == 2;
}

static bool isHex16(const Fields *fields, size_t at) {
	return fields->length[at] == 16 && spanOf(fields->text[at], 16, "0123456789abcdef") == 16;
}

// "%.1f" of a time: digits, a point and one digit.
static bool isMilliseconds(const Fields *fields, size_t at) {
	const char *text = fields->text[at];
	size_t length = fields->length[at];
	return length >= 3 && spanOf(text, length, digits) == length - 2 && text[length - 2] == '.' &&
				 spanOf(text + length - 1, 1, digits) == 1;
}

// How many of the first `length` characters of `text`, from the start, are each one of `set`.
static size_t spanOf(const char *text, size_t length, const char *set) {
	size_t span = 0;
	while (span < length && text[span] && strchr(set, text[span])) {
		++span;
	}
	return span;
}

// A time that isMilliseconds accepted.
static double milliseconds(const Fields *fields, size_t at) {
	const char *text = fields->text[at];
	size_t length = fields->length[at];
	double value = 0;
	for (size_t i = 0; i + 2 < length; ++i) {
		value = value * 10 + (text[i] - '0');
	}
	return value + (text[length - 1] - '0') / 10.0;
}

static void finishHash(Sha256 *hash, char hex[SHA256_HEX_LENGTH]) {
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_finish(hash, digest);
	sha256_hex(digest, hex);
}
