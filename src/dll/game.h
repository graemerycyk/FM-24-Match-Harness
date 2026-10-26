// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Everything the harness knows about fm.exe (Football Manager 24, build 24.2): code addresses,
// object layouts and the signatures of the game functions it calls. Addresses are RVAs
// (offsets from the module base); the image has no ASLR, so the base is
// always 0x140000000, but nothing here relies on that.
//
// On macOS the same RVAs name the macOS build's counterparts (game_macos.h translates them), and
// the object layouts below are the macOS build's: clang's Itanium C++ ABI and libc++ lay many
// objects out differently from MSVC (docs/MACOS.md).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// PE timestamps of the builds these addresses are valid for. The Epic Games build is a separate
// link of the same source; `fm24ctl probe` found every function and data address below at the
// same RVA in it (docs/PLAN.md, Phase 20).
enum {
	GAME_PE_TIMESTAMP_STEAM = 1744804480,
	GAME_PE_TIMESTAMP_EPIC = 1744804949,
	// An older Steam build (March 2024). Its code sits at other addresses; game.c translates the
	// Steam RVAs used throughout the harness using the shifts `fm24ctl probe` reported for it.
	GAME_PE_TIMESTAMP_STEAM_OLD = 1710868029,
};

// Game functions.
enum GameFunction {
	RVA_GAME_MATCH_CREATE = 0x1A8FAF0,
	RVA_GAME_MATCH_DESERIALIZE = 0x1AC7840,
	RVA_GAME_MATCH_PREPARE = 0x1A92070, // jmp thunk into the relocated (.arch) implementation
	RVA_GAME_MATCH_RUN = 0x1A98930,
	RVA_GAME_MATCH_SERIALIZE = 0x1AC76A0,
	RVA_GAME_MATCH_SERIALIZE_END = 0x1AC7837,
	RVA_DATA_BUFFER_CTOR = 0x19253A0,
	RVA_ALLOC_FROM_POOL = 0x34F81D0,
	RVA_MATCH_CTOR = 0x1A8FC30,
	RVA_FIXTURE_PREPARE_TEAMS = 0x34F3610,
	RVA_INIT_FROM_FIXTURE = 0x3AB8350,
	RVA_GAME_MATCH_DTOR = 0x1A90D30,
	RVA_GAME_MATCH_POOL_FREE = 0x34F82E0,
	RVA_TLS_ON_DEMAND_INIT = 0x54E8E38, // TlsCallback_0(0, DLL_THREAD_ATTACH)
};

// Game data.
enum GameData {
	RVA_GAME_MATCH_VTABLE = 0x56083C8,
	RVA_HUMAN_NON_PLAYER_VTABLE = 0x5A6A388, // db::HUMAN_NON_PLAYER
	RVA_FIXTURE_VTABLE = 0x5A25F58,          // sicomps::FIXTURE
	RVA_TEAM_VTABLE = 0x5A78848,             // db::TEAM
	RVA_PEOPLE_LIST = 0x642ED38,             // [p] + PEOPLE_LIST_HOLDER -> {start, end}
	RVA_CURRENT_DATE = 0x631D5BC,            // u16 day | time << 9, u16 year
	// [p] + HUMAN_MATCHES -> {start, end} of the matches the human manager attends: each points to
	// a copy of its sicomps::FIXTURE. The match builder gives a team's manager human control
	// (MANAGER_HUMAN_IN_CONTROL, the human's line-up and tactic) only for a fixture whose home and
	// away teams match one of these.
	RVA_HUMAN_MATCHES_HOLDER = 0x6374488,
	RVA_TLS_INDEX = 0x638EEC0, // fm.exe's _tls_index
};

#ifdef __APPLE__
// macOS game code with no Windows counterpart, named by IDs beyond the Windows image.
enum MacosGameFunction {
	// The thread-local PCG32's accessor: returns this thread's copy, initialising it on first use.
	RVA_MACOS_THREAD_RNG = 0x7F000001,
	// db::PERSON's forename getter of ACTUAL_PERSON_BASE-based people (PERSON_FIELDS_GETTER_SLOT).
	RVA_MACOS_PERSON_FORENAME = 0x7F000002,
};

// macOS game data the harness has not located on Windows yet: what game_state.h reads.
enum MacosGameData {
	RVA_MACOS_GAME = 0x7F000003,         // [p]: the GAME singleton (GameStateLayout GAME_*)
	RVA_MACOS_SAVE_SYSTEM = 0x7F000004,  // [p]: the SAVE_SYSTEM singleton
	RVA_MACOS_GAME_OPTIONS = 0x7F000005, // the game options object itself
	RVA_MACOS_START_SCREEN = 0x7F000006, // [p], set as the start screen's first entry ends
};
#endif

#ifdef __APPLE__
// GAME_MATCH layout.
enum MatchLayout {
	MATCH_SIZE = 0xE630,       // what MatchBuilder_Create allocates
	MATCH_SEED = 0x18,         // u32
	MATCH_CLOCK = 0x9D8,       // u32, 4 ticks per second
	MATCH_FLAGS = 0xA94,       // u32, bit 2 = finished
	MATCH_TEAM_HOME = 0xCDE8,  // MATCH_TEAM *
	MATCH_TEAM_AWAY = 0xCDF0,  // MATCH_TEAM *
	MATCH_RNG_STATE = 0xE3C0,  // u64
	MATCH_BUILD_FLAG = 0xE620, // byte MatchBuilder_Create clears after the constructor
	MATCH_TEAM_GOALS = 0x250,  // u8 in MATCH_TEAM
	// The deleting destructor's vtable slot (Itanium: complete destructor, then deleting).
	MATCH_DELETING_DESTRUCTOR_SLOT = 1,
};

// A team's manager and its control flags (docs/PLAN.md Phase 18).
enum ManagerLayout {
	MATCH_TEAM_MANAGER = 0x140,
	MANAGER_FLAGS = 0x1DC0,
	MANAGER_HUMAN_IN_CONTROL = 0x1,
	MANAGER_AI_MANAGED = 0x14000,
	MANAGER_HUMAN_NO_PLAN = 0x21306,
};

// Database layouts. The people list holds db::PERSON pointers, the virtual base the game's classes
// share. Itanium places it after the class's own members, so a person's names, birth date,
// nationality, personality, contract and traits sit before it, at a distance that depends on the
// class: PERSON_FIELDS_* below are relative to that ACTUAL_PERSON_BASE part, which
// database_personFields finds through the person's vtable the way the game's getters do.
enum DatabaseLayout {
	PERSON_UNIQUE_ID = 0x0C,           // in db::PERSON itself
	PERSON_FIELDS_GETTER_SLOT = 0x1A8, // vtable byte offset of RVA_MACOS_PERSON_FORENAME
	PERSON_FIELDS_OFFSET = -0x1B8,     // vtable byte offset of the ptrdiff_t to ACTUAL_PERSON_BASE
	PERSON_DATE_OF_BIRTH = 0x14,
	PERSON_FORENAME = 0x28,
	PERSON_SURNAME = 0x30,
	PERSON_COMMON_NAME = 0x38,
	PERSON_NATION = 0x40,
	PERSON_PERSONALITY = 0x48,
	PERSON_CONTRACT = 0x98,
	PERSON_TRAITS = 0xC0,
	NATION_UNIQUE_ID = 0x0C,
	// Player record members, relative to the complete db::ACTUAL_PLAYER(_AND_NON_PLAYER).
	PLAYER_WEIGHT = 0x144, // u16 here; its low byte is the weight in kilograms
	PLAYER_HEIGHT = 0x146,
	PLAYER_LAST_TRANSFER_FEE = 0x1D8,
	PLAYER_SHARPNESS = 0x1EC,
	PLAYER_FATIGUE = 0x1EE,
	PLAYER_CONDITION = 0x1F0,
	PLAYER_HOME_REPUTATION = 0x1F2,
	PLAYER_CURRENT_ABILITY = 0x1F8,
	PLAYER_RATINGS = 0x200,
	PLAYER_MORALE = 0x257,
	PLAYER_PREFERRED_POSITION = 0x25E,
	CONTRACT_TEAM = 0x10,
	TEAM_UNIQUE_ID = 0x0C,
	TEAM_CLUB = 0x30,
	TEAM_IN_GAME_DATA = 0xA0,
	NEXT_MATCH_OPPONENT = 0x38,
	CLUB_TEAMS = 0x18,
	CLUB_NAME_SHORT = 0xC8,
	FIXTURE_HOME = 0x08,
	FIXTURE_AWAY = 0x10,
	FIXTURE_DATE = 0x4C,
	PEOPLE_LIST_HOLDER = 0x88,
	HUMAN_MATCHES = 0x130,
};

// The game's state (game_state.h), as the game's own code reads and writes it.
enum GameStateLayout {
	GAME_OPTIONS_IN_PROGRESS = 0x5D, // u8: set by a completed load or new game, cleared on leaving it
	GAME_AUTO_PLAY = 0x64,           // u8: auto play is on (cleared when there is no auto-play file)
	GAME_AUTO_PLAY_FILE = 0x80,      // the auto-play file's parsed JSON, once it has been read
	GAME_PROCESSING = 0xB8,          // u8: set from GAME::start_processing to end_processing
	GAME_PROCESSING_KIND = 0xBC,     // i32: what start_processing was given, -1 after it ends
	SAVE_SYSTEM_NAME = 0x28,         // string object *: the loaded save's name
};

// The thread-local PCG32 the pre-match setup uses when no RNG is passed in: game_initThread returns
// the object itself.
enum ThreadRngLayout { TLS_PCG = 0 };
#else
// GAME_MATCH layout.
enum MatchLayout {
	MATCH_SEED = 32,          // u32
	MATCH_CLOCK = 2536,       // u32, 4 ticks per second
	MATCH_FLAGS = 2724,       // u32, bit 2 = finished
	MATCH_TEAM_HOME = 52728,  // MATCH_TEAM *
	MATCH_TEAM_AWAY = 52736,  // MATCH_TEAM *
	MATCH_RNG_STATE = 58320,  // u64
	MATCH_BUILD_FLAG = 58928, // byte MatchBuilder_Create clears after the constructor
	MATCH_TEAM_GOALS = 0x248, // u8 in MATCH_TEAM
};

// A team's manager and its control flags (docs/PLAN.md Phase 18).
enum ManagerLayout {
	// MATCH_TEAM: the manager (its tactic etc.; "<side>.manager" in a config).
	MATCH_TEAM_MANAGER = 312,
	// u32 in the manager: the first field of "<side>.manager".
	MANAGER_FLAGS = 7600,
	// A human makes the team's in-match changes (substitutions etc.).
	MANAGER_HUMAN_IN_CONTROL = 0x1,
	// Set on AI-managed teams' managers, never on a human's.
	MANAGER_AI_MANAGED = 0x14000,
	// A human manager's flags at kick-off with no match plan chosen at the pre-match screen (the
	// screen clears MANAGER_HUMAN_IN_CONTROL; choosing a plan clears 0x100).
	MANAGER_HUMAN_NO_PLAN = 0x21306,
};

// Database layouts.
enum DatabaseLayout {
	PERSON_UNIQUE_ID = 0x0C,
	PERSON_FORENAME = 0x58,    // doubly indirect string
	PERSON_SURNAME = 0x60,     // doubly indirect string
	PERSON_COMMON_NAME = 0x68, // doubly indirect string; absent for most people
	PERSON_CONTRACT = 0xC8,
	PERSON_DATE_OF_BIRTH = 0x44, // u16 day of year, u16 year
	PERSON_NATION = 0x70,        // NATION *
	PERSON_PERSONALITY = 0x78,   // 7 u8 (1-20): adaptability, ambition, loyalty, pressure,
															 // professionalism, sportsmanship, temperament
	PERSON_TRAITS = 0xF0,        // u64, one bit per player trait
	NATION_UNIQUE_ID = 0x0C,
	// Player record (database_playerRecord) members copied into a match (docs/FIXTURE.md).
	PLAYER_WEIGHT = 0x14C,             // u8, kilograms
	PLAYER_HEIGHT = 0x14E,             // u16, centimetres
	PLAYER_LAST_TRANSFER_FEE = 0x1E0,  // u32
	PLAYER_SHARPNESS = 0x1F4,          // u16, 0-10000
	PLAYER_FATIGUE = 0x1F6,            // u16 holding a signed value, 0 is fresh
	PLAYER_CONDITION = 0x1F8,          // u16, 0-10000
	PLAYER_HOME_REPUTATION = 0x1FA,    // u16, then current and world reputation
	PLAYER_CURRENT_ABILITY = 0x200,    // u16
	PLAYER_RATINGS = 0x208,            // PLAYER_RATING_COUNT u8: positions, then attributes
	PLAYER_MORALE = 0x25F,             // u8
	PLAYER_PREFERRED_POSITION = 0x266, // u8
	CONTRACT_TEAM = 0x10,
	TEAM_UNIQUE_ID = 0x0C,
	TEAM_CLUB = 0x30,
	TEAM_IN_GAME_DATA = 0xA0, // TEAM_IN_GAME_DATA *
	// TEAM_IN_GAME_DATA: the team's next match (competition +0x28, the team, its opponent, the date
	// +0x40).
	NEXT_MATCH_OPPONENT = 0x38,
	CLUB_TEAMS = 0x18,      // vector<TEAM *> {start, end}: the first team, then B, youth etc.
	CLUB_NAME_SHORT = 0xC8, // singly indirect string
	FIXTURE_HOME = 0x08,
	FIXTURE_AWAY = 0x10,
	FIXTURE_DATE = 0x4C, // u16 day | time << 9, u16 year
	PEOPLE_LIST_HOLDER = 0x80,
	HUMAN_MATCHES = 0x130,
};

// Thread-local PCG32 the pre-match setup uses when no RNG is passed in.
enum ThreadRngLayout {
	TLS_INIT_FLAG = 60, // u8, set once the thread's TLS is initialised
	TLS_PCG = 352,      // object: +8 state, +16 increment
};
#endif

enum { PLAYER_RATING_COUNT = 69, STAFF_ATTRIBUTE_COUNT = 52 };

// Second argument of GAME_MATCH__init_from_fixture, as laid out on MatchBuilder_Create's stack.
typedef struct InitArgs {
	uint8_t *fixture;
	uint32_t seed;
	uint32_t padding0;
	void *scratch; // 16 bytes filled by fixture_prepare_teams
	uint8_t mode;
	uint8_t padding1[7];
	uint64_t zero1;
	uint64_t zero2;
	uint8_t zero3;
	uint8_t padding2[7];
} InitArgs;

typedef uint8_t *(*MatchCreateFn)(void);
typedef uint8_t *(*MatchAllocFn)(size_t size); // macOS: GAME_MATCH's operator new
typedef char (*MatchStreamFn)(uint8_t *match, void **streamHandle, unsigned flags);
typedef void (*MatchPrepareFn)(uint8_t *match, char unknown);
typedef int (*MatchRunFn)(uint8_t *match, unsigned mode, int untilTick);
typedef void (*MatchFn)(uint8_t *match);
typedef void (*MatchDeleteFn)(uint8_t *match, int flags);
typedef void (*FixturePrepareFn)(uint8_t *fixture, void *scratch);
typedef void (*MatchInitFn)(uint8_t *match, InitArgs *args, uint8_t *flags);
typedef void *(*DataBufferCtorFn)(void *self);
typedef void (*VoidFn)(void);

// Finds fm.exe in this process and checks it is the expected build; logs the reason if not.
bool game_init(void);

uintptr_t game_base(void);

// The loaded fm.exe's PE timestamp (one of GAME_PE_TIMESTAMP_*, once game_init succeeded); on
// macOS the game's bundle version (macos_builds.h). Configs record it as their game_build.
uint32_t game_timestamp(void);

// The address of an RVA in the loaded image. RVAs, here and in game_rva, are those of the Steam
// build; the older build's are translated.
void *game_address(uint32_t rva);

// A game function at an RVA, as a generic function pointer; cast it to the function's type.
VoidFn game_function(uint32_t rva);

// The same conversion for a code address read from memory (e.g. a vtable slot).
VoidFn game_functionAt(const void *address);

// The RVA of an address inside fm.exe, or 0 if it is outside the image.
uint32_t game_rva(const void *address);

// The RVAs of a stack's return addresses, inner first, as game_rva gives them: at most `capacity`,
// returning how many. On macOS a frame translates into the frames the Windows build has for the
// same code, which may be several or none (game_macos.h).
size_t game_translateFrames(void *const *frames, size_t count, uint32_t *out, size_t capacity);

// Runs the calling thread's thread_local constructors in fm.exe if the game has not done so yet
// (every thread that calls into the engine needs them); returns the thread's TLS block. On macOS,
// where thread-locals start on first use one by one, it starts the thread-local PCG32 and returns
// that object.
uint8_t *game_initThread(void);
