// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Player and team statistics of played matches: summed over a run's seeds (run --detailed), and
// one match's rows (run --per-match). They come from the engine's per-player
// GAME_MATCH_PLAYER_STATS objects (MATCH_PLAYER+5856), the data the game's own match stats screens
// show; offsets and names are in docs/PLAN.md Phase 18.
#pragma once

#include "buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>


enum {
	MATCH_STATS_MAX_PLAYERS = 40,
	MATCH_STATS_PLAYER_VALUES = 46,
	MATCH_STATS_TEAM_VALUES = 38,
	MATCH_STATS_NAME_LENGTH = 128,
};

typedef struct PlayerTotals {
	uint32_t uniqueId;
	unsigned appearances; // matches played in (from the start, or brought on)
	unsigned starts;      // appearances from the start
	double minutes;
	int64_t ratingSum; // rating x 100, over appearances (an integer, so the sum is exact)
	double values[MATCH_STATS_PLAYER_VALUES]; // sums are exact: see exactValue in match_stats.c
} PlayerTotals;

typedef struct TeamTotals {
	char name[MATCH_STATS_NAME_LENGTH];
	size_t playerCount;
	size_t playerCapacity;
	PlayerTotals *players;
	double values[MATCH_STATS_TEAM_VALUES];
} TeamTotals;

typedef struct MatchStatsTotals {
	unsigned matches;
	TeamTotals teams[2]; // home, away
} MatchStatsTotals;

// One squad slot of a played match, as the engine left it.
typedef struct MatchPlayerStats {
	uint32_t uniqueId; // the person's unique ID
	uint32_t flags;    // the stats object's flags (0x20: a substitute)
	uint8_t slot;      // the place in the team's list
	uint8_t onMinute;  // the minutes of the player's changes; 0 for none
	uint8_t offMinute;
	uint8_t injuredMinute;
	uint8_t sentOffMinute;
	bool inSquad;
	bool started;
	bool appeared;
	int16_t rating; // x 100
	int minutes;
	float values[MATCH_STATS_PLAYER_VALUES]; // raw: u8 counts, f32 xg/xa, distance in metres
} MatchPlayerStats;

typedef struct MatchTeamStats {
	const uint8_t *databaseTeam; // the db::TEAM, for its club's name in --detailed
	uint32_t teamUid;            // its unique ID, 0 if it cannot be read
	int goals;                   // MATCH_TEAM_GOALS, own goals included
	int squad;                   // the slots in `players`
	int used;                    // the players who appeared
	double possession;           // a whole percent
	MatchPlayerStats players[MATCH_STATS_MAX_PLAYERS];
} MatchTeamStats;

typedef struct MatchStats {
	int matchLength; // minutes
	MatchTeamStats teams[2];
} MatchStats;

// The header lines of the per-match player and team rows (matchStats_formatRows), with "\n".
extern const char matchStats_playerRowsHeader[];
extern const char matchStats_teamRowsHeader[];

// Reads a played match's statistics; allocates nothing and takes no lock.
void matchStats_read(const uint8_t *match, MatchStats *out);

// Adds a match's statistics to `totals`, matching players by unique ID across rebuilt line-ups.
// False if memory runs out.
bool matchStats_accumulate(MatchStatsTotals *totals, const MatchStats *match);

// matchStats_read and matchStats_accumulate in one.
bool matchStats_add(MatchStatsTotals *totals, const uint8_t *match);

void matchStats_free(MatchStatsTotals *totals);

// Writes the averages per match next to the run's CSV: "<run output without .csv>-players.csv" and
// "-teams.csv". Returns false if a file cannot be written.
bool matchStats_write(const MatchStatsTotals *totals, const wchar_t *runOutput);

// Appends a match's rows, under matchStats_playerRowsHeader and matchStats_teamRowsHeader: one per
// squad slot, home then away, and one per team. False if memory runs out.
bool matchStats_formatRows(
	const MatchStats *match,
	uint64_t index,
	uint32_t seed,
	ByteBuffer *players,
	ByteBuffer *teams
);

// The run output's path without ".csv", plus `suffix` ("results.csv" -> "results-players.csv").
// False if it does not fit in `size`.
bool matchStats_siblingPath(
	const wchar_t *runOutput,
	const wchar_t *suffix,
	wchar_t *out,
	size_t size
);
