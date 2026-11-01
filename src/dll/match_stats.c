// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "match_stats.h"

#include "database.h"
#include "game.h"
#include "memory.h"
#include "platform.h"
#include "protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum {
#ifdef __APPLE__
	TEAM_DATABASE_TEAM = 0x50,
	PLAYER_UNIQUE_ID = 0x48,
	TEAM_PLAYERS = 0x180,
	TEAM_PLAYER_COUNT = 0x3D2,
	PLAYER_STATS = 0x16E0,
	MATCH_RESULT = 0x920,
#else
	TEAM_DATABASE_TEAM = 72, // MATCH_TEAM: the database team (club name via database.h)
	PLAYER_UNIQUE_ID = 80,   // MATCH_PLAYER: u32 database person unique ID
	TEAM_PLAYERS = 376,      // MATCH_TEAM: MATCH_PLAYER *[TEAM_PLAYER_COUNT]
	TEAM_PLAYER_COUNT = 962, // MATCH_TEAM: i8, the matchday squad including substitutes
	PLAYER_STATS = 5856,     // MATCH_PLAYER: GAME_MATCH_PLAYER_STATS *
	MATCH_RESULT = 2352,     // GAME_MATCH: the match result object
#endif
	RESULT_MATCH_LENGTH = 148, // match result: u16 minutes (90)
	// GAME_MATCH_PLAYER_STATS members (docs/PLAN.md Phase 18)
	STATS_PERSON = 16,      // i32 database person ID, -1 if not in the squad
	STATS_FLAGS = 88,       // u32: 0x20 substitute
	STATS_RATING = 118,     // i16 rating x 100
	STATS_OFF_MINUTE = 158, // u8 minute substituted off
	STATS_ON_MINUTE = 163,  // u8 minute substituted on
	STATS_OUT_MINUTE = 164, // u8 minute off injured
	STATS_SENT_OFF_MINUTE = 166,
	FLAG_SUBSTITUTE = 0x20,
	NO_PERSON = -1,
};

enum { EXACT_FRACTION_BITS = 20 };

typedef enum ValueKind { VALUE_U8, VALUE_F32, VALUE_F32_THOUSANDTHS } ValueKind;

typedef struct PlayerValue {
	const char *name;
	ValueKind kind;
	uint8_t offset;
} PlayerValue;

// Player values, in CSV column order.
typedef enum PlayerValueIndex {
	V_GOALS,
	V_ASSISTS,
	V_SHOTS,
	V_SHOTS_ON_TARGET,
	V_BLOCKED_SHOTS,
	V_LONG_SHOTS,
	V_XG,
	V_XA,
	V_KEY_PASSES,
	V_CLEAR_CUT_CHANCES,
	V_HALF_CHANCES,
	V_PASSES_ATTEMPTED,
	V_PASSES_COMPLETED,
	V_PROGRESSIVE_PASSES,
	V_CROSSES_ATTEMPTED,
	V_CROSSES_COMPLETED,
	V_DRIBBLES,
	V_TACKLES_ATTEMPTED,
	V_TACKLES_WON,
	V_KEY_TACKLES,
	V_INTERCEPTIONS,
	V_CLEARANCES,
	V_BLOCKS,
	V_SHOTS_BLOCKED,
	V_PRESSURES_ATTEMPTED,
	V_PRESSURES_COMPLETED,
	V_HEADERS_ATTEMPTED,
	V_HEADERS_WON,
	V_KEY_HEADERS,
	V_FOULS,
	V_FOULED,
	V_OFFSIDES,
	V_YELLOW_CARDS,
	V_RED_CARDS,
	V_CORNERS,
	V_THROW_INS,
	V_DEFENSIVE_FREE_KICKS,
	V_ATTACKING_FREE_KICKS,
	V_SAVES_HELD,
	V_SAVES_PARRIED,
	V_SAVES_TIPPED,
	V_GOALS_CONCEDED,
	V_POSSESSION_WON,
	V_POSSESSION_LOST,
	V_HIGH_INTENSITY_SPRINTS,
	V_DISTANCE_KM,
} PlayerValueIndex;
_Static_assert(V_DISTANCE_KM + 1 == MATCH_STATS_PLAYER_VALUES, "one index per player value");

// Members identified by matching one match's sums with the game's own Teams statistics screen
// (docs/PLAN.md Phase 18).
static const PlayerValue playerValues[] = {
	[V_GOALS] = {"goals", VALUE_U8, 127},
	[V_ASSISTS] = {"assists", VALUE_U8, 147},
	[V_SHOTS] = {"shots", VALUE_U8, 132},
	[V_SHOTS_ON_TARGET] = {"shots_on_target", VALUE_U8, 133},
	[V_BLOCKED_SHOTS] = {"blocked_shots", VALUE_U8, 134},
	[V_LONG_SHOTS] = {"long_shots", VALUE_U8, 135},
	[V_XG] = {"xg", VALUE_F32, 48},
	[V_XA] = {"xa", VALUE_F32, 56},
	[V_KEY_PASSES] = {"key_passes", VALUE_U8, 176},
	[V_CLEAR_CUT_CHANCES] = {"clear_cut_chances", VALUE_U8, 136},
	[V_HALF_CHANCES] = {"half_chances", VALUE_U8, 137},
	[V_PASSES_ATTEMPTED] = {"passes_attempted", VALUE_U8, 174},
	[V_PASSES_COMPLETED] = {"passes_completed", VALUE_U8, 175},
	[V_PROGRESSIVE_PASSES] = {"progressive_passes", VALUE_U8, 221},
	[V_CROSSES_ATTEMPTED] = {"crosses_attempted", VALUE_U8, 169},
	[V_CROSSES_COMPLETED] = {"crosses_completed", VALUE_U8, 170},
	[V_DRIBBLES] = {"dribbles", VALUE_U8, 149},
	[V_TACKLES_ATTEMPTED] = {"tackles_attempted", VALUE_U8, 178},
	[V_TACKLES_WON] = {"tackles_won", VALUE_U8, 179},
	[V_KEY_TACKLES] = {"key_tackles", VALUE_U8, 180},
	[V_INTERCEPTIONS] = {"interceptions", VALUE_U8, 187},
	[V_CLEARANCES] = {"clearances", VALUE_U8, 197},
	[V_BLOCKS] = {"blocks", VALUE_U8, 198},
	[V_SHOTS_BLOCKED] = {"shots_blocked", VALUE_U8, 144},
	[V_PRESSURES_ATTEMPTED] = {"pressures_attempted", VALUE_U8, 216},
	[V_PRESSURES_COMPLETED] = {"pressures_completed", VALUE_U8, 217},
	[V_HEADERS_ATTEMPTED] = {"headers_attempted", VALUE_U8, 182},
	[V_HEADERS_WON] = {"headers_won", VALUE_U8, 183},
	[V_KEY_HEADERS] = {"key_headers", VALUE_U8, 186},
	[V_FOULS] = {"fouls", VALUE_U8, 151},
	[V_FOULED] = {"fouled", VALUE_U8, 152},
	[V_OFFSIDES] = {"offsides", VALUE_U8, 153},
	[V_YELLOW_CARDS] = {"yellow_cards", VALUE_U8, 154},
	[V_RED_CARDS] = {"red_cards", VALUE_U8, 155},
	[V_CORNERS] = {"corners", VALUE_U8, 192},
	[V_THROW_INS] = {"throw_ins", VALUE_U8, 191},
	[V_DEFENSIVE_FREE_KICKS] = {"defensive_free_kicks", VALUE_U8, 193},
	[V_ATTACKING_FREE_KICKS] = {"attacking_free_kicks", VALUE_U8, 194},
	[V_SAVES_HELD] = {"saves_held", VALUE_U8, 141},
	[V_SAVES_PARRIED] = {"saves_parried", VALUE_U8, 142},
	[V_SAVES_TIPPED] = {"saves_tipped", VALUE_U8, 143},
	[V_GOALS_CONCEDED] = {"goals_conceded", VALUE_U8, 128},
	[V_POSSESSION_WON] = {"possession_won", VALUE_U8, 195},
	[V_POSSESSION_LOST] = {"possession_lost", VALUE_U8, 196},
	[V_HIGH_INTENSITY_SPRINTS] = {"high_intensity_sprints", VALUE_U8, 232},
	[V_DISTANCE_KM] = {"distance_km", VALUE_F32_THOUSANDTHS, 96},
};

// A team value: the sum of a player value, or one of these.
enum { TEAM_GOALS = -1, TEAM_POSSESSION = -2, TEAM_SAVES = -3, TEAM_RATING = -4 };

typedef struct TeamValue {
	const char *name;
	int source; // player value index, or TEAM_*
} TeamValue;

static const TeamValue teamValues[] = {
	{"goals", TEAM_GOALS},
	{"possession", TEAM_POSSESSION},
	{"average_rating", TEAM_RATING},
	{"xg", V_XG},
	{"shots", V_SHOTS},
	{"shots_on_target", V_SHOTS_ON_TARGET},
	{"blocked_shots", V_BLOCKED_SHOTS},
	{"long_shots", V_LONG_SHOTS},
	{"clear_cut_chances", V_CLEAR_CUT_CHANCES},
	{"half_chances", V_HALF_CHANCES},
	{"corners", V_CORNERS},
	{"fouls", V_FOULS},
	{"fouled", V_FOULED},
	{"offsides", V_OFFSIDES},
	{"passes_attempted", V_PASSES_ATTEMPTED},
	{"passes_completed", V_PASSES_COMPLETED},
	{"progressive_passes", V_PROGRESSIVE_PASSES},
	{"crosses_attempted", V_CROSSES_ATTEMPTED},
	{"crosses_completed", V_CROSSES_COMPLETED},
	{"tackles_attempted", V_TACKLES_ATTEMPTED},
	{"tackles_won", V_TACKLES_WON},
	{"headers_attempted", V_HEADERS_ATTEMPTED},
	{"headers_won", V_HEADERS_WON},
	{"interceptions", V_INTERCEPTIONS},
	{"clearances", V_CLEARANCES},
	{"blocks", V_BLOCKS},
	{"shots_blocked", V_SHOTS_BLOCKED},
	{"pressures_attempted", V_PRESSURES_ATTEMPTED},
	{"pressures_completed", V_PRESSURES_COMPLETED},
	{"yellow_cards", V_YELLOW_CARDS},
	{"red_cards", V_RED_CARDS},
	{"saves", TEAM_SAVES},
	{"distance_km", V_DISTANCE_KM},
	{"key_passes", V_KEY_PASSES},
	{"dribbles", V_DRIBBLES},
	{"possession_won", V_POSSESSION_WON},
	{"possession_lost", V_POSSESSION_LOST},
	{"high_intensity_sprints", V_HIGH_INTENSITY_SPRINTS},
};
_Static_assert(
	sizeof teamValues / sizeof teamValues[0] == MATCH_STATS_TEAM_VALUES,
	"one entry per team value"
);

static const char *const sideNames[] = {"home", "away"};

enum { ROW_LENGTH = 2048 }; // longer than any row: 62 fields of at most 20 characters

// One CSV row being formatted.
typedef struct Row {
	char text[ROW_LENGTH];
	size_t length;
} Row;

const char matchStats_playerRowsHeader[] =
	"index,seed,side,team_uid,slot,person_uid,in_squad,started,appeared,minutes,on_minute,"
	"off_minute,injured_minute,sent_off_minute,stats_flags,rating,goals,assists,shots,"
	"shots_on_target,blocked_shots,long_shots,xg,xa,key_passes,clear_cut_chances,half_chances,"
	"passes_attempted,passes_completed,progressive_passes,crosses_attempted,crosses_completed,"
	"dribbles,tackles_attempted,tackles_won,key_tackles,interceptions,clearances,blocks,"
	"shots_blocked,pressures_attempted,pressures_completed,headers_attempted,headers_won,"
	"key_headers,fouls,fouled,offsides,yellow_cards,red_cards,corners,throw_ins,"
	"defensive_free_kicks,attacking_free_kicks,saves_held,saves_parried,saves_tipped,"
	"goals_conceded,possession_won,possession_lost,high_intensity_sprints,distance_km\n";

const char matchStats_teamRowsHeader[] =
	"index,seed,side,team_uid,squad,used,goals,possession,average_rating,xg,shots,shots_on_target,"
	"blocked_shots,long_shots,clear_cut_chances,half_chances,corners,fouls,fouled,offsides,"
	"passes_attempted,passes_completed,progressive_passes,crosses_attempted,crosses_completed,"
	"tackles_attempted,tackles_won,headers_attempted,headers_won,interceptions,clearances,blocks,"
	"shots_blocked,pressures_attempted,pressures_completed,yellow_cards,red_cards,saves,distance_km,"
	"key_passes,dribbles,possession_won,possession_lost,high_intensity_sprints\n";

static double exactValue(double value);
static double rowValue(float raw, ValueKind kind);
static double summedValue(float raw, ValueKind kind);
static bool appeared(const uint8_t *stats, int minutes);
static int minutesPlayed(const uint8_t *stats, int matchLength);
static int squadSize(const uint8_t *team);
static unsigned completedPasses(const MatchTeamStats *team);
static double possession(unsigned own, unsigned other);
static void readTeam(const uint8_t *team, int matchLength, MatchTeamStats *out);
static void
readPlayer(const uint8_t *matchPlayer, int slot, int matchLength, MatchPlayerStats *out);
static void computeTeamValues(const MatchTeamStats *team, bool rounded, double *out);
static PlayerTotals *findPlayer(TeamTotals *team, uint32_t uniqueId);
static bool addTeam(TeamTotals *totals, const MatchTeamStats *team, bool first);
static void putText(Row *row, const char *text);
static void putUnsigned(Row *row, uint64_t value);
static void putFloat(Row *row, double value);
static void putRating(Row *row, long long hundredths);
static void putIdentity(Row *row, uint64_t index, uint32_t seed, int side, uint32_t teamUid);
static bool endRow(Row *row, ByteBuffer *out);
static bool formatPlayerRow(
	const MatchPlayerStats *player,
	uint64_t index,
	uint32_t seed,
	int side,
	uint32_t teamUid,
	ByteBuffer *out
);
static bool
formatTeamRow(const MatchTeamStats *team, uint64_t index, uint32_t seed, int side, ByteBuffer *out);
static void writeQuoted(FILE *file, const char *text);
static void writePlayers(FILE *file, const MatchStatsTotals *totals, const PeopleIndex *people);
static void writeTeams(FILE *file, const MatchStatsTotals *totals);

void matchStats_read(const uint8_t *match, MatchStats *out) {
	const uint8_t *teams[2] = {
		*(const uint8_t *const *)(match + MATCH_TEAM_HOME),
		*(const uint8_t *const *)(match + MATCH_TEAM_AWAY),
	};
	const uint8_t *result = *(const uint8_t *const *)(match + MATCH_RESULT);
	out->matchLength = memory_loadU16(result + RESULT_MATCH_LENGTH);
	for (int side = 0; side < 2; ++side) {
		readTeam(teams[side], out->matchLength, &out->teams[side]);
	}
	unsigned passes[2] = {completedPasses(&out->teams[0]), completedPasses(&out->teams[1])};
	out->teams[0].possession = possession(passes[0], passes[1]);
	out->teams[1].possession = possession(passes[1], passes[0]);
}

bool matchStats_accumulate(MatchStatsTotals *totals, const MatchStats *match) {
	bool first = totals->matches == 0;
	if (!addTeam(&totals->teams[0], &match->teams[0], first) ||
			!addTeam(&totals->teams[1], &match->teams[1], first)) {
		return false;
	}
	++totals->matches;
	return true;
}

bool matchStats_add(MatchStatsTotals *totals, const uint8_t *match) {
	MatchStats *stats = malloc(sizeof *stats);
	if (!stats) {
		return false;
	}
	matchStats_read(match, stats);
	bool ok = matchStats_accumulate(totals, stats);
	free(stats);
	return ok;
}

void matchStats_free(MatchStatsTotals *totals) {
	free(totals->teams[0].players);
	free(totals->teams[1].players);
	*totals = (MatchStatsTotals){0};
}

bool matchStats_write(const MatchStatsTotals *totals, const wchar_t *runOutput) {
	wchar_t playersPath[HARNESS_PATH_LENGTH];
	wchar_t teamsPath[HARNESS_PATH_LENGTH];
	if (!matchStats_siblingPath(runOutput, L"-players.csv", playersPath, HARNESS_PATH_LENGTH) ||
			!matchStats_siblingPath(runOutput, L"-teams.csv", teamsPath, HARNESS_PATH_LENGTH)) {
		return false;
	}
	FILE *players = platform_openFile(playersPath, "wb");
	FILE *teams = platform_openFile(teamsPath, "wb");
	bool ok = players && teams;
	if (ok) {
		PeopleIndex people = {0};
		(void)database_loadPeople(&people); // without it the players have no names
		writePlayers(players, totals, &people);
		writeTeams(teams, totals);
		ok = !ferror(players) && !ferror(teams);
		database_freePeople(&people);
	}
	if (players) {
		ok = fclose(players) == 0 && ok;
	}
	if (teams) {
		ok = fclose(teams) == 0 && ok;
	}
	return ok;
}

bool matchStats_formatRows(
	const MatchStats *match,
	uint64_t index,
	uint32_t seed,
	ByteBuffer *players,
	ByteBuffer *teams
) {
	for (int side = 0; side < 2; ++side) {
		const MatchTeamStats *team = &match->teams[side];
		for (int i = 0; i < team->squad; ++i) {
			if (!formatPlayerRow(&team->players[i], index, seed, side, team->teamUid, players)) {
				return false;
			}
		}
	}
	return formatTeamRow(&match->teams[0], index, seed, 0, teams) &&
				 formatTeamRow(&match->teams[1], index, seed, 1, teams);
}

bool matchStats_siblingPath(
	const wchar_t *runOutput,
	const wchar_t *suffix,
	wchar_t *out,
	size_t size
) {
	size_t length = wcslen(runOutput);
	size_t stem = length > 4 && platform_compareWideIgnoringCase(runOutput + length - 4, L".csv") == 0
									? length - 4
									: length;
	int written = swprintf(out, size, L"%.*ls%ls", (int)stem, runOutput, suffix);
	return written >= 0 && (size_t)written < size;
}

// A value as a per-match row has it: the count, xG or xA, or the distance in km.
static double rowValue(float raw, ValueKind kind) {
	return kind == VALUE_F32_THOUSANDTHS ? (double)raw / 1000.0 : (double)raw;
}

// The same, rounded to be summed exactly (see exactValue): what --detailed averages.
static double summedValue(float raw, ValueKind kind) {
	switch (kind) {
		case VALUE_U8: return raw;
		case VALUE_F32: return exactValue(raw);
		case VALUE_F32_THOUSANDTHS: return exactValue(raw / 1000.0);
		default: return 0;
	}
}

// Minutes played, as the game's average-rating code (sub_1419ECBC0) counts them.
static int minutesPlayed(const uint8_t *stats, int matchLength) {
	if ((int32_t)memory_loadU32(stats + STATS_PERSON) == NO_PERSON) {
		return 0;
	}
	int off = stats[STATS_OFF_MINUTE] ? stats[STATS_OFF_MINUTE] : stats[STATS_OUT_MINUTE];
	int sentOff = stats[STATS_SENT_OFF_MINUTE];
	int on = stats[STATS_ON_MINUTE];
	int minutes = 0;
	if (memory_loadU32(stats + STATS_FLAGS) & FLAG_SUBSTITUTE) {
		if (off) {
			minutes = off - on;
		} else if (sentOff) {
			minutes = sentOff - on;
		} else if (on) {
			minutes = matchLength - on;
		}
	} else {
		minutes = off ? off : (sentOff ? sentOff : matchLength);
	}
	return minutes > 0 ? minutes : 0;
}

static int squadSize(const uint8_t *team) {
	int count = team[TEAM_PLAYER_COUNT];
	if (count > INT8_MAX) {
		return 0; // the game reads the count as signed: a "negative" count means no players
	}
	return count < MATCH_STATS_MAX_PLAYERS ? count : MATCH_STATS_MAX_PLAYERS;
}

// Every listed player's completed passes, whether or not the player appeared.
static unsigned completedPasses(const MatchTeamStats *team) {
	unsigned total = 0;
	for (int i = 0; i < team->squad; ++i) {
		total += (unsigned)team->players[i].values[V_PASSES_COMPLETED];
	}
	return total;
}

// Possession as the match stats panel shows it (sub_151827710): the team's share of the completed
// passes, rounded to a whole percent.
static double possession(unsigned own, unsigned other) {
	if (own + other == 0) {
		return 50;
	}
	return floor(100.0 * own / (own + other) + 0.5);
}

static void readTeam(const uint8_t *team, int matchLength, MatchTeamStats *out) {
	out->databaseTeam = *(const uint8_t *const *)(team + TEAM_DATABASE_TEAM);
	out->teamUid = out->databaseTeam ? database_teamUniqueId(out->databaseTeam) : 0;
	out->goals = team[MATCH_TEAM_GOALS];
	out->squad = squadSize(team);
	out->used = 0;
	for (int i = 0; i < out->squad; ++i) {
		const uint8_t *matchPlayer = *(const uint8_t *const *)(team + TEAM_PLAYERS + 8 * (size_t)i);
		readPlayer(matchPlayer, i, matchLength, &out->players[i]);
		out->used += out->players[i].appeared;
	}
}

static void
readPlayer(const uint8_t *matchPlayer, int slot, int matchLength, MatchPlayerStats *out) {
	const uint8_t *stats = *(const uint8_t *const *)(matchPlayer + PLAYER_STATS);
	out->uniqueId = memory_loadU32(matchPlayer + PLAYER_UNIQUE_ID);
	out->flags = memory_loadU32(stats + STATS_FLAGS);
	out->slot = (uint8_t)slot;
	out->onMinute = stats[STATS_ON_MINUTE];
	out->offMinute = stats[STATS_OFF_MINUTE];
	out->injuredMinute = stats[STATS_OUT_MINUTE];
	out->sentOffMinute = stats[STATS_SENT_OFF_MINUTE];
	out->inSquad = (int32_t)memory_loadU32(stats + STATS_PERSON) != NO_PERSON;
	out->started = out->inSquad && !(out->flags & FLAG_SUBSTITUTE);
	out->minutes = minutesPlayed(stats, matchLength);
	out->appeared = appeared(stats, out->minutes);
	out->rating = (int16_t)memory_loadU16(stats + STATS_RATING);
	for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
		const PlayerValue *value = &playerValues[v];
		if (value->kind == VALUE_U8) {
			out->values[v] = stats[value->offset];
		} else {
			memcpy(&out->values[v], stats + value->offset, sizeof out->values[v]);
		}
	}
}

// A team's values (teamValues) from its players who appeared: rounded as --detailed sums them, or
// as the per-match rows have them.
static void computeTeamValues(const MatchTeamStats *team, bool rounded, double *out) {
	double sums[MATCH_STATS_PLAYER_VALUES] = {0};
	long long ratingMinutes = 0; // rating x 100 x minutes
	long long teamMinutes = 0;
	for (int i = 0; i < team->squad; ++i) {
		const MatchPlayerStats *player = &team->players[i];
		if (!player->appeared) {
			continue;
		}
		ratingMinutes += (long long)player->rating * player->minutes;
		teamMinutes += player->minutes;
		for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
			ValueKind kind = playerValues[v].kind;
			sums[v] += rounded ? summedValue(player->values[v], kind) : rowValue(player->values[v], kind);
		}
	}
	for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES; ++v) {
		int source = teamValues[v].source;
		if (source == TEAM_GOALS) {
			out[v] = team->goals;
		} else if (source == TEAM_POSSESSION) {
			out[v] = team->possession;
		} else if (source == TEAM_RATING) {
			// Minutes-weighted, truncated to hundredths as the match statistics screen shows it.
			long long hundredths = teamMinutes ? ratingMinutes / teamMinutes : 0;
			out[v] = rounded ? exactValue((double)hundredths / 100.0) : (double)hundredths / 100.0;
		} else if (source == TEAM_SAVES) {
			out[v] = sums[V_SAVES_HELD] + sums[V_SAVES_PARRIED] + sums[V_SAVES_TIPPED];
		} else {
			out[v] = sums[source];
		}
	}
}

static PlayerTotals *findPlayer(TeamTotals *team, uint32_t uniqueId) {
	size_t low = 0;
	size_t high = team->playerCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (team->players[middle].uniqueId < uniqueId) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	if (low < team->playerCount && team->players[low].uniqueId == uniqueId) {
		return &team->players[low];
	}
	if (team->playerCount == team->playerCapacity) {
		size_t capacity = team->playerCapacity ? team->playerCapacity * 2 : MATCH_STATS_MAX_PLAYERS;
		if (capacity < team->playerCapacity || capacity > SIZE_MAX / sizeof *team->players) {
			return NULL;
		}
		PlayerTotals *players = realloc(team->players, capacity * sizeof *players);
		if (!players) {
			return NULL;
		}
		team->players = players;
		team->playerCapacity = capacity;
	}
	memmove(
		team->players + low + 1, team->players + low, (team->playerCount - low) * sizeof *team->players
	);
	++team->playerCount;
	PlayerTotals *player = &team->players[low];
	*player = (PlayerTotals){.uniqueId = uniqueId};
	return player;
}

// Every listed player gets totals, so one who never appears is still reported.
static bool addTeam(TeamTotals *totals, const MatchTeamStats *team, bool first) {
	if (first) {
		database_clubShortName(team->databaseTeam, totals->name, sizeof totals->name);
	}
	for (int i = 0; i < team->squad; ++i) {
		const MatchPlayerStats *stats = &team->players[i];
		PlayerTotals *player = findPlayer(totals, stats->uniqueId);
		if (!player) {
			return false;
		}
		if (!stats->appeared) {
			continue;
		}
		++player->appearances;
		player->starts += stats->started;
		player->minutes += stats->minutes;
		player->ratingSum += stats->rating;
		for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
			player->values[v] += summedValue(stats->values[v], playerValues[v].kind);
		}
	}
	double values[MATCH_STATS_TEAM_VALUES];
	computeTeamValues(team, true, values);
	for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES; ++v) {
		totals->values[v] += values[v];
	}
	return true;
}

static void putText(Row *row, const char *text) {
	size_t length = strlen(text);
	if (length < ROW_LENGTH - row->length) {
		memcpy(row->text + row->length, text, length);
		row->length += length;
	} else {
		row->length = ROW_LENGTH; // too long: endRow refuses it
	}
}

static void putUnsigned(Row *row, uint64_t value) {
	char digits[24];
	size_t at = sizeof digits;
	digits[--at] = 0;
	do {
		digits[--at] = (char)('0' + value % 10);
		value /= 10;
	} while (value);
	putText(row, digits + at);
}

// Nine significant digits, which tell every f32 apart; 0 for either zero.
static void putFloat(Row *row, double value) {
	char text[32];
	(void)snprintf(text, sizeof text, "%.9g", value == 0 ? 0.0 : value);
	putText(row, text);
}

// A rating in hundredths as a decimal: 7.12, -0.05.
static void putRating(Row *row, long long hundredths) {
	unsigned long long magnitude =
		hundredths < 0 ? 0ULL - (unsigned long long)hundredths : (unsigned long long)hundredths;
	if (hundredths < 0) {
		putText(row, "-");
	}
	putUnsigned(row, magnitude / 100);
	char fraction[4] = {'.', (char)('0' + magnitude % 100 / 10), (char)('0' + magnitude % 10), 0};
	putText(row, fraction);
}

static void putIdentity(Row *row, uint64_t index, uint32_t seed, int side, uint32_t teamUid) {
	putUnsigned(row, index);
	putText(row, ",");
	putUnsigned(row, seed);
	putText(row, side ? ",away," : ",home,");
	putUnsigned(row, teamUid);
}

static bool endRow(Row *row, ByteBuffer *out) {
	putText(row, "\n");
	return row->length < ROW_LENGTH && buffer_append(out, row->text, row->length);
}

// One squad slot; a player who did not appear has no rating or values, as the averages leave such
// players out.
static bool formatPlayerRow(
	const MatchPlayerStats *player,
	uint64_t index,
	uint32_t seed,
	int side,
	uint32_t teamUid,
	ByteBuffer *out
) {
	Row row;
	row.length = 0;
	putIdentity(&row, index, seed, side, teamUid);
	const uint64_t fields[] = {
		player->slot,
		player->uniqueId,
		player->inSquad,
		player->started,
		player->appeared,
		(uint64_t)player->minutes,
		player->onMinute,
		player->offMinute,
		player->injuredMinute,
		player->sentOffMinute,
		player->flags,
	};
	for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
		putText(&row, ",");
		putUnsigned(&row, fields[i]);
	}
	putText(&row, ",");
	if (player->appeared) {
		putRating(&row, player->rating);
	}
	for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
		putText(&row, ",");
		if (!player->appeared) {
			continue;
		}
		ValueKind kind = playerValues[v].kind;
		if (kind == VALUE_U8) {
			putUnsigned(&row, (uint64_t)player->values[v]);
		} else {
			putFloat(&row, rowValue(player->values[v], kind));
		}
	}
	return endRow(&row, out);
}

static bool formatTeamRow(
	const MatchTeamStats *team,
	uint64_t index,
	uint32_t seed,
	int side,
	ByteBuffer *out
) {
	double values[MATCH_STATS_TEAM_VALUES];
	computeTeamValues(team, false, values);
	Row row;
	row.length = 0;
	putIdentity(&row, index, seed, side, team->teamUid);
	putText(&row, ",");
	putUnsigned(&row, (uint64_t)team->squad);
	putText(&row, ",");
	putUnsigned(&row, (uint64_t)team->used);
	for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES; ++v) {
		int source = teamValues[v].source;
		putText(&row, ",");
		if (source == TEAM_RATING) {
			putRating(&row, llround(values[v] * 100));
		} else if (source == V_XG || source == V_DISTANCE_KM) {
			putFloat(&row, values[v]);
		} else {
			putUnsigned(&row, (uint64_t)values[v]);
		}
	}
	return endRow(&row, out);
}

static void writeQuoted(FILE *file, const char *text) {
	fputc('"', file);
	for (const char *c = text; *c; ++c) {
		if (*c == '"') {
			fputc('"', file);
		}
		fputc(*c, file);
	}
	fputc('"', file);
}
static void writePlayers(FILE *file, const MatchStatsTotals *totals, const PeopleIndex *people) {
	fprintf(file, "side,team,player,person_uid,matches,appearances,starts,minutes,rating");
	for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
		fprintf(file, ",%s", playerValues[v].name);
	}
	fputc('\n', file);
	double matches = totals->matches ? totals->matches : 1;
	for (int side = 0; side < 2; ++side) {
		const TeamTotals *team = &totals->teams[side];
		for (size_t i = 0; i < team->playerCount; ++i) {
			const PlayerTotals *player = &team->players[i];
			char name[MATCH_STATS_NAME_LENGTH] = "?";
			const uint8_t *person = database_findPerson(people, player->uniqueId);
			if (person) {
				(void)database_personName(person, name, sizeof name);
			}
			fprintf(file, "%s,", sideNames[side]);
			writeQuoted(file, team->name);
			fputc(',', file);
			writeQuoted(file, name);
			fprintf(
				file,
				",%u,%u,%u,%u,%.1f,",
				player->uniqueId,
				totals->matches,
				player->appearances,
				player->starts,
				player->minutes / matches
			);
			if (player->appearances) {
				fprintf(file, "%.2f", (double)player->ratingSum / 100.0 / player->appearances);
			}
			for (size_t v = 0; v < MATCH_STATS_PLAYER_VALUES; ++v) {
				fprintf(file, ",%.3f", player->values[v] / matches);
			}
			fputc('\n', file);
		}
	}
}

static void writeTeams(FILE *file, const MatchStatsTotals *totals) {
	fprintf(file, "side,team,matches");
	for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES; ++v) {
		fprintf(file, ",%s", teamValues[v].name);
	}
	fputc('\n', file);
	double matches = totals->matches ? totals->matches : 1;
	for (int side = 0; side < 2; ++side) {
		const TeamTotals *team = &totals->teams[side];
		fprintf(file, "%s,", sideNames[side]);
		writeQuoted(file, team->name);
		fprintf(file, ",%u", totals->matches);
		for (size_t v = 0; v < MATCH_STATS_TEAM_VALUES; ++v) {
			fprintf(file, ",%.3f", team->values[v] / matches);
		}
		fputc('\n', file);
	}
}

// `value` rounded to a multiple of 2^-20. Sums of such values are exact in a double (well within
// its 53 bits for match statistics), so totals don't depend on the order the workers finish matches
// in.
static double exactValue(double value) {
	return ldexp(nearbyint(ldexp(value, EXACT_FRACTION_BITS)), -EXACT_FRACTION_BITS);
}

// Whether the player took part: minutes played, or brought on (a substitute brought on in stoppage
// time at 90 has no whole minutes in the game's count, but did play).
static bool appeared(const uint8_t *stats, int minutes) {
	if ((int32_t)memory_loadU32(stats + STATS_PERSON) == NO_PERSON) {
		return false;
	}
	return minutes > 0 || stats[STATS_ON_MINUTE] != 0;
}
