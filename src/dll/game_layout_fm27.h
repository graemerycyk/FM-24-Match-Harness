// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The object layouts of the FM27 edition's builds, which game.h includes in place of FM24's. Every
// value is the one tools/macos/steam-27.0.0.2486716.json records with the instructions that show it
// in both slices; tools/macos/gen_tables.py refuses tables for a build whose layouts differ from
// these, or a mapping that lists a layout these leave out. Offsets are those of the same objects as
// FM24's (game.h), unless a comment says otherwise.
//
// What has not been located in these builds is left out, and the harness does without it
// (GAME_UNLOCATED_*): whether the game is processing, the loaded save's name and the clubs' short
// names. These builds have no start screen: their mapping gives RVA_MACOS_START_SCREEN the GAME
// singleton's address, so the game counts as started once its startup has created GAME. That is
// before the startup reads the auto-play file, so a game in progress counts as loaded only once it
// has settled (game_state.h).
#pragma once

#include <stdint.h>


#define GAME_UNLOCATED_PROCESSING 1 // GAME_PROCESSING and GAME_PROCESSING_KIND
#define GAME_UNLOCATED_SAVE_NAME 1  // SAVE_SYSTEM_NAME
#define GAME_UNLOCATED_CLUB_NAME 1  // CLUB_NAME_SHORT

// A player record holds a body type (PLAYER_BODY_TYPE) where FM24's held a weight (PLAYER_WEIGHT).
#define GAME_WEIGHT_FROM_BODY_TYPE 1

// GAME_MATCH layout.
enum MatchLayout {
	MATCH_SIZE = 0x15590,       // what the GAME_MATCH factory clears
	MATCH_SEED = 0x18,          // u32
	MATCH_CLOCK = 0x154DC,      // u32, 4 ticks per second
	MATCH_FLAGS = 0x154F0,      // u32, bit 2 = finished
	MATCH_TEAM_HOME = 0x15370,  // MATCH_TEAM *
	MATCH_TEAM_AWAY = 0x15378,  // MATCH_TEAM *
	MATCH_RNG_STATE = 0x152A0,  // u64
	MATCH_BUILD_FLAG = 0x15588, // byte the factory clears after the constructor
	MATCH_TEAM_GOALS = 0x733,   // u8 in MATCH_TEAM
	// The deleting destructor's vtable slot (Itanium: complete destructor, then deleting).
	MATCH_DELETING_DESTRUCTOR_SLOT = 1,
};

// A team's manager and its control flags. The flags' values are FM24's, observed there; these
// builds use the field in the same way, but the values have not been checked in them.
enum ManagerLayout {
	MATCH_TEAM_MANAGER = 0x640,
	MANAGER_FLAGS = 0x30D0,
	MANAGER_HUMAN_IN_CONTROL = 0x1,
	MANAGER_AI_MANAGED = 0x14000,
	MANAGER_HUMAN_NO_PLAN = 0x21306,
};

// Database layouts (game.h explains PERSON_FIELDS_*). The game gives a player in a match the weight
// game_playerWeight computes from their body type.
enum DatabaseLayout {
	PERSON_UNIQUE_ID = 0x0C,           // in db::PERSON itself
	PERSON_FIELDS_GETTER_SLOT = 0x140, // vtable byte offset of RVA_MACOS_PERSON_FORENAME
	PERSON_FIELDS_OFFSET = -0x150,     // vtable byte offset of the ptrdiff_t to ACTUAL_PERSON_BASE
	PERSON_DATE_OF_BIRTH = 0x58,
	PERSON_FORENAME = 0x20,
	PERSON_SURNAME = 0x28,
	PERSON_COMMON_NAME = 0x30,
	PERSON_NATION = 0x38,
	PERSON_PERSONALITY = 0x40,
	PERSON_CONTRACT = 0x78,
	PERSON_TRAITS = 0x90,
	NATION_UNIQUE_ID = 0x0C,
	// Player record members, relative to the complete db::ACTUAL_PLAYER(_AND_NON_PLAYER).
	PLAYER_BODY_TYPE = 0x228, // u8, 1-5
	PLAYER_HEIGHT = 0x226,    // u16, centimetres
	PLAYER_LAST_TRANSFER_FEE = 0x23C,
	PLAYER_SHARPNESS = 0x250,
	PLAYER_FATIGUE = 0x252,
	PLAYER_CONDITION = 0x254,
	PLAYER_HOME_REPUTATION = 0x256,
	PLAYER_CURRENT_ABILITY = 0x25C,
	PLAYER_RATINGS = 0x148,
	PLAYER_MORALE = 0x264,
	PLAYER_PREFERRED_POSITION = 0x26E,
	CONTRACT_TEAM = 0x10,
	TEAM_UNIQUE_ID = 0x0C,
	TEAM_CLUB = 0x30,
	TEAM_IN_GAME_DATA = 0xA0,
	NEXT_MATCH_OPPONENT = 0x38,
	CLUB_TEAMS = 0x18,
	FIXTURE_HOME = 0x08,
	FIXTURE_AWAY = 0x10,
	FIXTURE_DATE = 0x4C,
	PEOPLE_LIST_HOLDER = 0x88,
	HUMAN_MATCHES = 0x148,
};

// The game's state (game_state.h), as the game's own code reads and writes it.
enum GameStateLayout {
	GAME_OPTIONS_IN_PROGRESS = 0x67, // u8
	GAME_AUTO_PLAY = 0x80,           // u8
	GAME_AUTO_PLAY_FILE = 0xD0,      // the auto-play file's parsed JSON, once it has been read
};

// The thread-local PCG32: the accessor returns the object (+8 state, +16 increment).
enum ThreadRngLayout { TLS_PCG = 0 };

// What a detailed run reads of a played match (match_stats.c), which match_stats.c otherwise has
// for FM24. Its statistics object is GAME_MATCH_PLAYER_STATS, through MATCH_PLAYER's pointer; the
// statistics are its members (STATS_*), each a u8 count unless a comment says otherwise.
#define GAME_MATCH_STATS_LAYOUT 1

enum MatchStatsLayout {
	MATCH_RESULT = 0x14E90,       // GAME_MATCH: the match result object
	RESULT_MATCH_LENGTH = 0xAC,   // match result: u16 minutes (90)
	TEAM_DATABASE_TEAM = 0xB8,    // MATCH_TEAM: the database team
	TEAM_PLAYERS = 0x140,         // MATCH_TEAM: MATCH_PLAYER *[TEAM_PLAYER_COUNT]
	TEAM_PLAYER_COUNT = 0x73D,    // MATCH_TEAM: i8, the matchday squad including substitutes
	PLAYER_UNIQUE_ID = 0x48,      // MATCH_PLAYER: u32 database person unique ID
	PLAYER_STATS = 0x1880,        // MATCH_PLAYER: GAME_MATCH_PLAYER_STATS *
	STATS_PERSON = 0x18,          // i32 database person ID, -1 if not in the squad
	STATS_FLAGS = 0x5C,           // u32: 0x20 substitute
	STATS_RATING = 0x82,          // i16 rating x 100
	STATS_OFF_MINUTE = 0xAA,      // minute substituted off
	STATS_ON_MINUTE = 0xAF,       // minute substituted on
	STATS_OUT_MINUTE = 0xB0,      // minute off injured
	STATS_SENT_OFF_MINUTE = 0xB2, // minute sent off
	STATS_XG = 0x1C,              // f32
	STATS_XA = 0x24,              // f32
	STATS_DISTANCE = 0x68,        // f32, metres
	STATS_GOALS = 0x8B,
	STATS_GOALS_CONCEDED = 0x8C,
	STATS_SHOTS = 0x90,
	STATS_SHOTS_ON_TARGET = 0x91,
	STATS_BLOCKED_SHOTS = 0x92,
	STATS_LONG_SHOTS = 0x93,
	STATS_CLEAR_CUT_CHANCES = 0x94,
	STATS_HALF_CHANCES = 0x95,
	STATS_SAVES_HELD = 0x99,
	STATS_SAVES_PARRIED = 0x9A,
	STATS_SAVES_TIPPED = 0x9B,
	STATS_SHOTS_BLOCKED = 0x9C,
	STATS_ASSISTS = 0x9F,
	STATS_DRIBBLES = 0xA1,
	STATS_FOULS = 0xA3,
	STATS_FOULED = 0xA4,
	STATS_OFFSIDES = 0xA5,
	STATS_YELLOW_CARDS = 0xA6,
	STATS_RED_CARDS = 0xA7,
	STATS_CROSSES_ATTEMPTED = 0xB5,
	STATS_CROSSES_COMPLETED = 0xB6,
	STATS_PASSES_ATTEMPTED = 0xBA,
	STATS_PASSES_COMPLETED = 0xBB,
	STATS_KEY_PASSES = 0xBC,
	STATS_TACKLES_ATTEMPTED = 0xC6,
	STATS_TACKLES_WON = 0xC7,
	STATS_KEY_TACKLES = 0xC8,
	STATS_HEADERS_ATTEMPTED = 0xCA,
	STATS_HEADERS_WON = 0xCB,
	STATS_KEY_HEADERS = 0xCE,
	STATS_INTERCEPTIONS = 0xCF,
	STATS_THROW_INS = 0xD3,
	STATS_CORNERS = 0xD4,
	STATS_DEFENSIVE_FREE_KICKS = 0xD5,
	STATS_ATTACKING_FREE_KICKS = 0xD6,
	STATS_POSSESSION_WON = 0xD7,
	STATS_POSSESSION_LOST = 0xD8,
	STATS_CLEARANCES = 0xD9,
	STATS_BLOCKS = 0xDA,
	STATS_PRESSURES_ATTEMPTED = 0xEC,
	STATS_PRESSURES_COMPLETED = 0xED,
	STATS_PROGRESSIVE_PASSES = 0xF1,
	STATS_HIGH_INTENSITY_SPRINTS = 0xF5,
};

enum { PLAYER_BODY_TYPES = 5 };

// The weight in kilograms the game's match builder gives a player: their body type's BMI times
// their height in metres squared, truncated, as the builder computes it in single precision; 0 for
// a body type outside 1-5.
static inline uint8_t game_playerWeight(uint8_t bodyType, uint16_t height) {
	static const float bmi[PLAYER_BODY_TYPES] = {19.0F, 20.5F, 23.5F, 26.5F, 29.5F};
	if (bodyType < 1 || bodyType > PLAYER_BODY_TYPES) {
		return 0;
	}
	float metres = (float)height / 100.0F;
	int32_t weight = (int32_t)(bmi[bodyType - 1] * (metres * metres));
	return (uint8_t)(weight < 0 ? -weight : weight); // the builder stores the low byte
}
