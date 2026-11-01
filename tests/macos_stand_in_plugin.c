// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for the game's engine plugin in the plugin test (macos_test.c): a bundle the stand-in
// game loads and unloads, whose engine bytes and state the stand-in harness's tables point at (see
// macos_stand_in_plugin.h; tests/macos_stand_in_offsets.cmake finds them after the link). Built a
// second time with STAND_IN_OTHER_BUILD, it is another build of it, with another UUID, which the
// stand-in harness does not support.
//
// It also plays matches, as far as a run with keep_build asks the engine to: a stand-in for each
// call such a run makes (DATA_BUFFER's constructor, the thread's PCG32, create and the GAME_MATCH
// factory, deserialise, prepare, run and delete), on objects in the edition's layout (game.h).
// Deserialise reads a serialised match's header and the seed after it, then two u32s: the home and
// away managers' flags. Run plays seed n to n % 4 goals against n / 4 % 3, each team's first player
// scoring them and its second coming off after 60 minutes. The calls are noted, a line each, in the
// file FMNN_TEST_ENGINE_LOG names.
//
// As it loads, it starts up as the game's engine does: it creates the GAME object and reads the
// auto-play file the game's command line names (--auto_play_game_file), unless
// FMNN_TEST_NO_COMMAND_LINE is set, as if the game did not pass its command line on. A file whose
// LOAD_GAME names a save that exists has it load the save (standInLoadSave), as loading one in the
// game by hand does (the stand-in game's `play`). A load takes a while, in steps, as the game's does:
// at once the game is in progress at a date, with no people, then, LOAD_STEP_MS apart, the people
// list has a human manager of a team, and then a player of it too, with a club the team scan finds
// ("loaded").

#include "macos_stand_in_plugin.h"

#include "edition.h"
#include "game.h"
#include "stream.h"

#include <crt_externs.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STAND_IN_EXPORT __attribute__((visibility("default"), used))

enum {
	PLAYERS = 2, // per team
	FORMAT_VERSION = 0x004C,
	HEADER_SIZE = 12,          // the game's version, the format version, 0, 0x02 and a u32
	PREPARED = MATCH_SIZE - 1, // a byte of the stand-in's own: prepare has been called
	TEAM_SIZE = 0x800,
	STATS_SIZE = 0x100,
	MATCH_LENGTH = 90,
	SUBSTITUTED = 60,
	RATING = 700,
	OBJECT_SIZE = 0x400, // of the GAME object and the database's
	TEAM_ID = 0x21E4,    // the human manager's team
	PERSON_ID = 0x7A11,  // the human manager
	PLAYER_ID = 0x7A12,  // a player in the team
	NATION_ID = 793,     // the player's nation
	PLAYER_CONDITION_VALUE = 9000,
	PLAYER_HEIGHT_CM = 180,
	PLAYER_BODY = 3,       // average: the game's BMI 23.5
	PLAYER_BORN_DAY = 100, // 26 on the loaded save's date
	PLAYER_BORN_YEAR = 2000,
	DATE_DAY = 200,      // the loaded save's date: 2026 day 200 15:00
	DATE_TIME_CODE = 37, // 15:00, a quarter-hour slot after 06:00 (game_state.c)
	DATE_YEAR = 2026,
	DATE_TIME_SHIFT = 9,
	SAVE_PATH_LENGTH = 1024,
	LOAD_STEP_MS = 300, // between the steps of a load (macos_test.c's STAND_IN_LOAD_STEP_MS)
};
_Static_assert(GAME_OPTIONS_IN_PROGRESS < 256, "the options");
_Static_assert(GAME_AUTO_PLAY_FILE + sizeof(void *) <= OBJECT_SIZE, "the GAME object");
_Static_assert(PERSON_CONTRACT + sizeof(void *) <= OBJECT_SIZE, "the person");
_Static_assert(
	PLAYER_PREFERRED_POSITION < OBJECT_SIZE && PLAYER_MORALE < OBJECT_SIZE,
	"the player"
);
_Static_assert(PERSON_TRAITS + sizeof(uint64_t) <= OBJECT_SIZE, "the player's person");
_Static_assert(PEOPLE_LIST_HOLDER + sizeof(void *) <= OBJECT_SIZE, "the people list's holder");
_Static_assert(TEAM_CLUB + sizeof(void *) <= OBJECT_SIZE, "the team");
_Static_assert(CLUB_TEAMS + 2 * sizeof(void *) <= OBJECT_SIZE, "the club");
_Static_assert(TEAM_PLAYER_COUNT < TEAM_SIZE && MATCH_TEAM_GOALS < TEAM_SIZE, "the team's counts");
_Static_assert(TEAM_PLAYERS + PLAYERS * sizeof(void *) <= TEAM_SIZE, "the team's players");
_Static_assert(MATCH_TEAM_MANAGER + sizeof(void *) <= TEAM_SIZE, "the team's manager");
_Static_assert(STATS_HIGH_INTENSITY_SPRINTS < STATS_SIZE, "the statistics");

STAND_IN_EXPORT void *standInDataBufferCtor(void *self);
STAND_IN_EXPORT uint8_t *standInThreadRng(void);
STAND_IN_EXPORT uint8_t *standInMatchFactory(void);
STAND_IN_EXPORT uint8_t *standInMatchCreate(void);
STAND_IN_EXPORT char
standInMatchDeserialize(uint8_t *match, void **stream, void *context, unsigned flags);
STAND_IN_EXPORT void standInMatchPrepare(uint8_t *match, char unknown);
STAND_IN_EXPORT int standInMatchRun(uint8_t *match, unsigned mode, int untilTick);
STAND_IN_EXPORT void standInMatchDtor(uint8_t *match);
STAND_IN_EXPORT void standInMatchPoolFree(uint8_t *match);
STAND_IN_EXPORT void standInLoadSave(const char *path);

static void deleteMatch(uint8_t *match);
static void note(const char *format, ...) __attribute__((format(printf, 1, 2)));
static uint8_t *pointerAt(const uint8_t *object, size_t offset);
static void setPointer(uint8_t *object, size_t offset, const void *value);
static uint32_t u32At(const uint8_t *object, size_t offset);
static void setU32(uint8_t *object, size_t offset, uint32_t value);
static bool readMatch(uint8_t *match, const uint8_t *bytes, size_t size);
static void playTeam(uint8_t *team, unsigned goals, uint32_t seed);
static void startUp(void);
static bool autoplaySave(const char *file, char *save, size_t size);
static uint8_t *object(void);
static uint8_t *nameObject(const char *name);
static void *loadPeople(void *parameter);
static void finishLoad(void);

static int standInContext; // the engine's serialisation context
static atomic_int matchesAlive;
// The objects startUp and standInLoadSave make, freed as the plugin unloads.
static uint8_t *objects[32];
static size_t objectCount;
// The load under way, whose people loadPeople adds on a thread of its own.
static struct {
	pthread_t thread;
	bool started; // the thread has not been joined
	uint8_t *holder;
	uint8_t *list;
	uint8_t *people;
	char path[SAVE_PATH_LENGTH];
} load;

STAND_IN_EXPORT const uint8_t standInEngine[32] = STAND_IN_ENGINE_BYTES;
STAND_IN_EXPORT StandInState standInState = {.matchContext = &standInContext};

// A human manager's vtable: its fields are the person's own (offset 0), and its forename getter is
// the one the harness checks for. A player's is the same, with the RTTI of a db::ACTUAL_PLAYER that
// is the complete object (Itanium: offset-to-top, then the type_info, whose name is at +8).
STAND_IN_EXPORT const void *const standInPersonVtable[STAND_IN_PERSON_SLOTS] = {
	[STAND_IN_PERSON_VPTR + PERSON_FIELDS_GETTER_SLOT / (int)sizeof(void *)] = standInEngine,
};
static const void *const playerTypeInfo[2] = {NULL, "N2db13ACTUAL_PLAYERE"};
static const void *const playerVtable[STAND_IN_PERSON_SLOTS] = {
	[STAND_IN_PERSON_VPTR - 1] = playerTypeInfo,
	[STAND_IN_PERSON_VPTR + PERSON_FIELDS_GETTER_SLOT / (int)sizeof(void *)] = standInEngine,
};

// The GAME_MATCH vtable: the complete, then the deleting destructor.
STAND_IN_EXPORT void (*const standInGameMatchVtable[4])(
	uint8_t *match
) = {standInMatchDtor, deleteMatch, NULL, NULL};

#ifdef STAND_IN_OTHER_BUILD
STAND_IN_EXPORT const char standInBuild[] = "another build";
#endif

// DATA_BUFFER: the shared buffer and the two streams over it, each with the error code at +16 and
// the shared buffer at +24.
void *standInDataBufferCtor(void *self) {
	static void *vtable[4];
	DataBuffer *buffer = self;
	buffer->vtable = vtable;
	buffer->shared = calloc(1, sizeof *buffer->shared);
	buffer->input = calloc(1, 32);
	buffer->output = calloc(1, 32);
	if (buffer->input && buffer->output) {
		setPointer(buffer->input, 24, buffer->shared);
		setPointer(buffer->output, 24, buffer->shared);
	}
	return self;
}

// One PCG32 for every thread: a run does not draw from it. (A bundle with thread-local variables
// is never unloaded.)
uint8_t *standInThreadRng(void) {
	static uint8_t pcg[24];
	return pcg;
}

uint8_t *standInMatchFactory(void) {
	uint8_t *match = calloc(1, MATCH_SIZE);
	if (match) {
		setPointer(match, 0, (const void *)standInGameMatchVtable);
		atomic_fetch_add(&matchesAlive, 1);
	}
	return match;
}

// As create does with the factory registered.
uint8_t *standInMatchCreate(void) {
	return standInMatchFactory();
}

char standInMatchDeserialize(uint8_t *match, void **stream, void *context, unsigned flags) {
	const SharedBuffer *shared = (const SharedBuffer *)pointerAt(*stream, 24);
	size_t size = shared && shared->begin ? (size_t)(shared->end - shared->begin) : 0;
	bool ours = context == standInState.matchContext;
	bool ok = ours && flags == 0 && readMatch(match, shared ? shared->begin : NULL, size);
	note(
		"deserialize seed=%u context=%s flags=%u ok=%d",
		u32At(match, MATCH_SEED),
		ours ? "engine" : "other",
		flags,
		ok
	);
	return (char)ok;
}

void standInMatchPrepare(uint8_t *match, char unknown) {
	(void)unknown;
	match[PREPARED] = 1;
}

int standInMatchRun(uint8_t *match, unsigned mode, int untilTick) {
	uint32_t seed = u32At(match, MATCH_SEED);
	uint8_t *teams[2] = {pointerAt(match, MATCH_TEAM_HOME), pointerAt(match, MATCH_TEAM_AWAY)};
	note(
		"run seed=%u home=%#x away=%#x prepared=%d mode=%u until=%d",
		seed,
		u32At(pointerAt(teams[0], MATCH_TEAM_MANAGER), MANAGER_FLAGS),
		u32At(pointerAt(teams[1], MATCH_TEAM_MANAGER), MANAGER_FLAGS),
		match[PREPARED],
		mode,
		untilTick
	);
	playTeam(teams[0], seed % 4, seed);
	playTeam(teams[1], seed / 4 % 3, seed);
	setU32(match, MATCH_CLOCK, MATCH_LENGTH * 240 + 4 * (seed % 300));
	setU32(match, MATCH_FLAGS, u32At(match, MATCH_FLAGS) | 4);
	uint64_t state = (uint64_t)seed * 0x5851F42D4C957F2DULL + 1;
	memcpy(match + MATCH_RNG_STATE, &state, sizeof state);
	return 0;
}

void standInMatchDtor(uint8_t *match) {
	free(pointerAt(match, MATCH_RESULT));
	for (int side = 0; side < 2; ++side) {
		uint8_t *team = pointerAt(match, side ? MATCH_TEAM_AWAY : MATCH_TEAM_HOME);
		if (!team) {
			continue;
		}
		free(pointerAt(team, MATCH_TEAM_MANAGER));
		for (size_t i = 0; i < PLAYERS; ++i) {
			uint8_t *player = pointerAt(team, TEAM_PLAYERS + i * sizeof(void *));
			if (player) {
				free(pointerAt(player, PLAYER_STATS));
				free(player);
			}
		}
		free(team);
	}
}

void standInMatchPoolFree(uint8_t *match) {
	free(match);
	atomic_fetch_sub(&matchesAlive, 1);
}

// As the game loads a save: its date and a game in progress at once, then (loadPeople) the people
// list, with a human manager and a player whose contracts are with a team, whose club's first team
// it is.
void standInLoadSave(const char *path) {
	finishLoad();
	uint8_t *holder = object();
	uint8_t *list = object();
	uint8_t *people = object();
	uint8_t *person = object();
	uint8_t *player = object();
	uint8_t *contract = object();
	uint8_t *team = object();
	uint8_t *club = object();
	uint8_t *clubTeams = object();
	uint8_t *nation = object();
	uint8_t *forename = nameObject("Ada");
	uint8_t *surname = nameObject("Stand-in");
	const uint8_t *const made[] = {holder,
		list,
		people,
		person,
		player,
		contract,
		team,
		club,
		clubTeams,
		nation,
		forename,
		surname};
	for (size_t i = 0; i < sizeof made / sizeof made[0]; ++i) {
		if (!made[i]) {
			note("cannot load %s", path);
			return;
		}
	}
	setPointer(person, 0, &standInPersonVtable[STAND_IN_PERSON_VPTR]);
	setU32(person, PERSON_UNIQUE_ID, PERSON_ID);
	setPointer(person, PERSON_CONTRACT, contract);
	setPointer(player, 0, &playerVtable[STAND_IN_PERSON_VPTR]);
	setU32(player, PERSON_UNIQUE_ID, PLAYER_ID);
	setPointer(player, PERSON_CONTRACT, contract);
	setPointer(player, PERSON_FORENAME, forename);
	setPointer(player, PERSON_SURNAME, surname);
	setPointer(player, PERSON_NATION, nation);
	setU32(nation, NATION_UNIQUE_ID, NATION_ID);
	uint16_t born[2] = {PLAYER_BORN_DAY, PLAYER_BORN_YEAR};
	memcpy(player + PERSON_DATE_OF_BIRTH, born, sizeof born);
	for (unsigned k = 0; k < PLAYER_RATING_COUNT; ++k) {
		player[PLAYER_RATINGS + k] = (uint8_t)(1 + k % 20);
	}
	uint16_t condition = PLAYER_CONDITION_VALUE;
	uint16_t height = PLAYER_HEIGHT_CM;
	memcpy(player + PLAYER_CONDITION, &condition, sizeof condition);
	memcpy(player + PLAYER_HEIGHT, &height, sizeof height);
	player[PLAYER_BODY_TYPE] = PLAYER_BODY;
	setPointer(contract, CONTRACT_TEAM, team);
	setPointer(team, 0, &standInState.vtables[1]);
	setU32(team, TEAM_UNIQUE_ID, TEAM_ID);
	setPointer(team, TEAM_CLUB, club);
	setPointer(clubTeams, 0, team);
	setPointer(club, CLUB_TEAMS, clubTeams);
	setPointer(club, CLUB_TEAMS + sizeof(void *), clubTeams + sizeof(void *));
	setPointer(people, 0, person);
	setPointer(people, sizeof(void *), player);
	setPointer(list, 0, people);
	setPointer(list, sizeof(void *), people + sizeof(void *)); // the human manager, to start with
	setPointer(holder, PEOPLE_LIST_HOLDER, list);
	standInState.peopleList = NULL;
	standInState.date[0] = DATE_DAY | DATE_TIME_CODE << DATE_TIME_SHIFT;
	standInState.date[1] = DATE_YEAR;
	standInState.options[GAME_OPTIONS_IN_PROGRESS] = 1;
	note("loading %s", path);
	load.holder = holder;
	load.list = list;
	load.people = people;
	(void)snprintf(load.path, sizeof load.path, "%s", path);
	load.started = pthread_create(&load.thread, NULL, loadPeople, NULL) == 0;
	if (!load.started) {
		note("cannot load %s", path);
	}
}

__attribute__((constructor)) static void loaded(void) {
	startUp();
}

__attribute__((destructor)) static void unloaded(void) {
	finishLoad();
	note("unloaded with %d matches alive", atomic_load(&matchesAlive));
	standInState.game = NULL;
	standInState.peopleList = NULL;
	for (size_t i = 0; i < objectCount; ++i) {
		free(objects[i]);
	}
	objectCount = 0;
}

static void deleteMatch(uint8_t *match) {
	note("delete seed=%u", u32At(match, MATCH_SEED));
	standInMatchDtor(match);
	standInMatchPoolFree(match);
}

static void note(const char *format, ...) {
	const char *path = getenv(EDITION_ENV_PREFIX "TEST_ENGINE_LOG");
	char line[256];
	va_list arguments;
	va_start(arguments, format);
	int length = vsnprintf(line, sizeof line - 1, format, arguments);
	va_end(arguments);
	if (!path || length <= 0 || (size_t)length >= sizeof line - 1) {
		return;
	}
	line[length++] = '\n';
	int file = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
	if (file >= 0) {
		(void)write(file, line, (size_t)length); // one write: whole lines, whatever the thread
		close(file);
	}
}

static uint8_t *pointerAt(const uint8_t *object, size_t offset) {
	uint8_t *value = NULL;
	if (object) {
		memcpy(&value, object + offset, sizeof value);
	}
	return value;
}

static void setPointer(uint8_t *object, size_t offset, const void *value) {
	memcpy(object + offset, &value, sizeof value);
}

static uint32_t u32At(const uint8_t *object, size_t offset) {
	uint32_t value = 0;
	if (object) {
		memcpy(&value, object + offset, sizeof value);
	}
	return value;
}

static void setU32(uint8_t *object, size_t offset, uint32_t value) {
	memcpy(object + offset, &value, sizeof value);
}

// The match a serialised match describes: its seed and the managers' flags; the rest as built.
static bool readMatch(uint8_t *match, const uint8_t *bytes, size_t size) {
	if (!bytes || size < HEADER_SIZE + sizeof(uint32_t) || bytes[4] != (FORMAT_VERSION & 0xFF) ||
			bytes[5] != FORMAT_VERSION >> 8 || bytes[6] != 0 || bytes[7] != 0x02) {
		return false;
	}
	size_t seedAt = HEADER_SIZE + sizeof(uint32_t) + u32At(bytes, HEADER_SIZE) + sizeof(uint32_t);
	if (seedAt + 3 * sizeof(uint32_t) > size) {
		return false;
	}
	setU32(match, MATCH_SEED, u32At(bytes, seedAt));
	uint8_t *result = calloc(1, RESULT_MATCH_LENGTH + sizeof(uint16_t));
	if (!result) {
		return false;
	}
	uint16_t length = MATCH_LENGTH;
	memcpy(result + RESULT_MATCH_LENGTH, &length, sizeof length);
	setPointer(match, MATCH_RESULT, result);
	for (int side = 0; side < 2; ++side) {
		uint8_t *team = calloc(1, TEAM_SIZE);
		uint8_t *manager = team ? calloc(1, MANAGER_FLAGS + sizeof(uint32_t)) : NULL;
		setPointer(match, side ? MATCH_TEAM_AWAY : MATCH_TEAM_HOME, team);
		if (!manager) {
			return false;
		}
		setPointer(team, MATCH_TEAM_MANAGER, manager);
		setU32(manager, MANAGER_FLAGS, u32At(bytes, seedAt + (1 + (size_t)side) * sizeof(uint32_t)));
		team[TEAM_PLAYER_COUNT] = PLAYERS;
		for (size_t i = 0; i < PLAYERS; ++i) {
			uint8_t *player = calloc(1, PLAYER_STATS + sizeof(void *));
			uint8_t *stats = player ? calloc(1, STATS_SIZE) : NULL;
			setPointer(team, TEAM_PLAYERS + i * sizeof(void *), player);
			if (!stats) {
				return false;
			}
			setPointer(player, PLAYER_STATS, stats);
			uint32_t uniqueId = 100 * (1 + (uint32_t)side) + 1 + (uint32_t)i;
			setU32(player, PLAYER_UNIQUE_ID, uniqueId);
			setU32(stats, STATS_PERSON, uniqueId);
		}
	}
	return true;
}

// The team's first player scores `goals`; its second comes off after SUBSTITUTED minutes.
static void playTeam(uint8_t *team, unsigned goals, uint32_t seed) {
	team[MATCH_TEAM_GOALS] = (uint8_t)goals;
	for (size_t i = 0; i < PLAYERS; ++i) {
		uint8_t *stats = pointerAt(pointerAt(team, TEAM_PLAYERS + i * sizeof(void *)), PLAYER_STATS);
		stats[STATS_GOALS] = i == 0 ? (uint8_t)goals : 0;
		stats[STATS_PASSES_COMPLETED] = (uint8_t)(10 + i);
		stats[STATS_YELLOW_CARDS] = (uint8_t)(seed % 2);
		stats[STATS_OFF_MINUTE] = i == 1 ? SUBSTITUTED : 0;
		int16_t rating = RATING;
		memcpy(stats + STATS_RATING, &rating, sizeof rating);
	}
}

// The engine's startup: the GAME object, then the auto-play file the game's command line names, if
// the game passes it on, and the save it loads.
static void startUp(void) {
	uint8_t *game = object();
	if (!game) {
		return;
	}
	standInState.game = game;
	static const char option[] = "--auto_play_game_file=";
	char **arguments = *_NSGetArgv();
	int count = *_NSGetArgc();
	if (getenv(EDITION_ENV_PREFIX "TEST_NO_COMMAND_LINE")) {
		note("started without the game's command line");
		return;
	}
	for (int i = 1; i < count; ++i) {
		if (strncmp(arguments[i], option, sizeof option - 1) != 0) {
			continue;
		}
		static char save[SAVE_PATH_LENGTH];
		struct stat status;
		game[GAME_AUTO_PLAY] = 1;
		setPointer(game, GAME_AUTO_PLAY_FILE, game);
		if (!autoplaySave(arguments[i] + sizeof option - 1, save, sizeof save) ||
				stat(save, &status) != 0) {
			note("read the auto-play file %s, but loaded no save", arguments[i] + sizeof option - 1);
			return;
		}
		standInLoadSave(save);
		return;
	}
	note("started without an auto-play file");
}

// The save {"LOAD_GAME": "<path>"} in the auto-play file `file` names, unescaped.
static bool autoplaySave(const char *file, char *save, size_t size) {
	static const char key[] = "\"LOAD_GAME\": \"";
	char text[2 * SAVE_PATH_LENGTH];
	FILE *input = fopen(file, "rb");
	size_t length = input ? fread(text, 1, sizeof text - 1, input) : 0;
	if (input) {
		(void)fclose(input);
	}
	text[length] = 0;
	const char *at = strstr(text, key);
	size_t used = 0;
	for (at = at ? at + sizeof key - 1 : NULL; at && *at && *at != '"' && used + 1 < size; ++at) {
		if (*at == '\\' && at[1]) {
			++at;
		}
		save[used++] = *at;
	}
	save[used] = 0;
	return at && *at == '"' && used > 0;
}

// A person's name object, as the database has it: a pointer to a string object (a u32 byte length,
// then the bytes), here in the same object. NULL if there is no room.
static uint8_t *nameObject(const char *name) {
	uint8_t *made = object();
	uint32_t length = (uint32_t)strlen(name);
	if (made) {
		setPointer(made, 0, made + 2 * sizeof(void *));
		setU32(made, 2 * sizeof(void *), length);
		memcpy(made + 2 * sizeof(void *) + sizeof length, name, length);
	}
	return made;
}

// The rest of a load: the people list with the human manager, then with the player as well. All
// they point to was made before this thread started.
static void *loadPeople(void *parameter) {
	(void)parameter;
	(void)usleep(LOAD_STEP_MS * 1000);
	standInState.peopleList = load.holder;
	(void)usleep(LOAD_STEP_MS * 1000);
	setPointer(load.list, sizeof(void *), load.people + 2 * sizeof(void *));
	note("loaded %s", load.path);
	return NULL;
}

// Waits for a load under way to end.
static void finishLoad(void) {
	if (load.started) {
		(void)pthread_join(load.thread, NULL);
		load.started = false;
	}
}

// A zeroed object the plugin frees as it unloads, or NULL.
static uint8_t *object(void) {
	uint8_t *made = objectCount < sizeof objects / sizeof objects[0] ? calloc(1, OBJECT_SIZE) : NULL;
	if (made) {
		objects[objectCount++] = made;
	}
	return made;
}
