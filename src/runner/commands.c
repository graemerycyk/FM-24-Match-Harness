// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "commands.h"

#include "build_info.h"
#include "client.h"
#include "files.h"
#include "fixture_data.h"
#include "fixture_lineup.h"
#include "host_info.h"
#include "manifest.h"
#include "match_config.h"
#include "paths.h"
#include "platform.h"
#include "player_inject.h"
#include "protocol.h"
#include "run_job.h"
#include "saves.h"
#include "sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include "macos_game.h"
#endif


enum {
	EXIT_OK = 0,
	EXIT_FAILED = 1,
	EXIT_USAGE = 2,
	EXIT_WRONG_STATE = 3, // the game is not in the state the command needs
	UTF8_PATH_LENGTH = HARNESS_PATH_LENGTH * 3,
	JOB_LENGTH = 4 * UTF8_PATH_LENGTH + 512,
	SUMMARY_SHOWN = 10,        // how many individual results `run` prints
	SLEEP_TOLERANCE_MS = 1000, // less than this between the two clocks is their drift, not sleep
	SEED_PATTERN_LENGTH = 255,
	MAX_SEED_RANGE = 10000000, // as job.c allows
};

// The options after a command's fixed arguments (see parseOptions).
enum {
	OPTION_FULL = 1 << 0,                                   // --full
	OPTION_ALLOW_DUPLICATE = 1 << 1,                        // --allow-duplicate
	OPTION_SAVE = 1 << 2,                                   // --save <name>
	OPTION_NO_LAUNCH = 1 << 3,                              // --no-launch
	OPTION_ANY_SAVE = 1 << 4,                               // --any-save
	OPTION_SAVE_FLAGS = OPTION_NO_LAUNCH | OPTION_ANY_SAVE, // how a job's save is checked
};

// The options that have the game's save checked before a job (see runJob_ensureSave), and their
// usage text. Windows, whose harness has no status job yet, has none of them.
#ifdef _WIN32
enum { SAVE_OPTIONS = 0 };
#define SAVE_FLAGS_USAGE ""
#define SAVE_OPTIONS_USAGE ""
#else
enum { SAVE_OPTIONS = OPTION_SAVE | OPTION_SAVE_FLAGS };
#define SAVE_FLAGS_USAGE " [--no-launch] [--any-save]"
#define SAVE_OPTIONS_USAGE " [--save <name>] [--no-launch] [--any-save]"
#endif

typedef struct Path {
	wchar_t wide[HARNESS_PATH_LENGTH];
	char utf8[UTF8_PATH_LENGTH];
} Path;

typedef struct Totals {
	uint64_t runs;
	uint64_t homeWins;
	uint64_t draws;
	uint64_t awayWins;
	uint64_t homeGoals;
	uint64_t awayGoals;
} Totals;

typedef struct RunOptions {
	const wchar_t *input;
	const wchar_t *seeds;  // default "keep"
	const wchar_t *output; // default: the input with a .csv extension
	unsigned long threads;
	const wchar_t *profile;
	bool detailed;
	bool humanControl;
	bool keepBuild;
	bool perMatch;
	bool keepGoing;  // seed_errors=skip
	bool manifest;   // report=1, and the manifest
	SaveCheck check; // the fixture's save, and --no-launch and --any-save
} RunOptions;

// What `lineup` is asked: to show a side, or to write its new order.
typedef struct LineupRequest {
	const wchar_t *input;
	const wchar_t *output; // NULL to show the side
	unsigned side;
	bool allowAiSide;
	bool list;  // the XI, then any substitutes; otherwise two players to exchange
	char *text; // the players' UTF-8 text (malloc'd), which `players` point into
	char *players[FIXTURE_PLAYERS];
	unsigned count;
} LineupRequest;

// The order in which replace-home or replace-away puts the other side's players back, when the
// input marks that side's line-up as changed: their unique IDs in the input's places.
typedef struct KeptLineup {
	unsigned side;
	unsigned count;
	uint32_t people[FIXTURE_PLAYERS];
	FixtureData input; // the input's players, for the names of those the rebuilt side lacks
	char problem[512]; // why the order cannot be kept, or ""
} KeptLineup;

// What `run` did, for its manifest.
typedef struct RunRecord {
	Path fixture; // the input as given, made absolute
	Path input;   // what the job plays: the fixture, or its encoded copy
	Path profile;
	char seeds[SEED_PATTERN_LENGTH + 1];
	RunJob job; // its texts are the fields above and the output's
	FixtureFacts facts;
	char date[SAVES_DATE_LENGTH]; // the save's in-game date, from the save check
	bool refused;                 // an output was refused before the job: no manifest either
	bool submitted;
	time_t startedAt;
	time_t finishedAt;
	uint64_t sleptMs;
	ClientWatch watch;
	ClientOutcome outcome;
	char result[CLIENT_RESULT_LENGTH];
} RunRecord;

// The files a run's manifest lists.
typedef struct RunOutputs {
	ManifestOutput list[6];
	char names[6][UTF8_PATH_LENGTH];
	size_t count;
	ResultsSummary summary;
	bool summarised;
} RunOutputs;

static bool absolutePath(const wchar_t *argument, Path *out);
static bool outputPath(const wchar_t *argument, Path *out, bool opened);
static bool checkOutput(const Path *out, PathsFolder folder, bool opened);
static bool jobInputPath(Path *out, const wchar_t *extension);
static void removeJobInput(const Path *path);
static int submitAndReport(const char *job);
static int
playRun(RunOptions *options, const Path *output, uintptr_t progressWindow, RunRecord *record);
static void submitRun(const char *job, RunRecord *record);
static int writeRunManifest(
	int argc,
	wchar_t **argv,
	const RunOptions *options,
	const Path *output,
	RunRecord *record,
	int status
);
static void
listOutputs(const RunOptions *options, const Path *output, RunOutcome outcome, RunOutputs *outputs);
static void warnIfOtherHarness(const RunReport *report);
static bool siblingPath(const Path *output, const char *suffix, Path *out);
static bool encodeConfig(
	const Path *json,
	ByteBuffer *bytes,
	uint32_t teams[2],
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2],
	ByteBuffer *config
);
static bool encodeToFile(
	const Path *json,
	const Path *output,
	size_t *size,
	uint32_t teams[2],
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2],
	ByteBuffer *config
);
static void readBinaryFacts(const Path *input, FixtureFacts *facts);
static void addResult(Totals *totals, unsigned seed, unsigned home, unsigned away);
static bool parseCsvNumbers(const char *line, unsigned long *values, size_t count);
static int printSummary(const Path *csvPath);
static bool prepareInput(
	Path *input,
	uint32_t teams[2],
	bool *encoded,
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2]
);
static void warnIfLineupLost(const RunOptions *options, const bool lineup[2]);
static bool binaryFound(const Path *input);
static bool isSeedList(const wchar_t *text);
static bool seedNumber(const char *text, const char **end, unsigned long long *value);
static bool checkSeedPattern(const wchar_t *pattern);
static bool parseRunOption(int argc, wchar_t **argv, int *i, RunOptions *options);
static bool placeRunArgument(const wchar_t *argument, RunOptions *options);
static bool parseRunArguments(int argc, wchar_t **argv, RunOptions *options);
static bool defaultOutput(const Path *input, Path *output);
static bool checkSiblingPaths(const RunOptions *options, const Path *output);
static bool checkManifestPath(const Path *output);
static int siblingStem(const Path *output);
static void printSibling(const char *label, const Path *output, const char *suffix);
static void printSkippedSeeds(const char *result, const Path *output);
static bool parseSeedValue(const wchar_t *value, char *seed, size_t size);
static bool parseFixtureOptions(
	int argc,
	wchar_t **argv,
	char *seed,
	size_t size,
	bool *full,
	SaveCheck *check
);
static bool parseDecodeArguments(int argc, wchar_t **argv, bool *full);
static bool parsePersonId(const wchar_t *text, char *out, size_t size);
static bool parsePlayerBlock(const wchar_t *text, char *out, size_t size);
static int fetchPlayer(const char *person, const Path *output, char *result, size_t resultSize);
static int fetchForFixture(
	const ByteBuffer *config,
	SaveCheck *check,
	const char *person,
	const Path *player,
	char *result,
	size_t resultSize
);
static bool parseLineupArguments(int argc, wchar_t **argv, LineupRequest *request);
static bool readLineupPlayers(const wchar_t *const *texts, unsigned count, LineupRequest *request);
static char *trimSpaces(char *text);
static bool readLineupFixture(const Path *input, ByteBuffer *config, FixtureData **data);
static int showLineup(const FixtureData *data, unsigned side);
static int writeLineup(
	const LineupRequest *request,
	const ByteBuffer *config,
	const FixtureData *data,
	const Path *output
);
static bool givenTwice(
	const FixtureTeam *team,
	const unsigned *places,
	unsigned count,
	char *error,
	size_t size
);
static KeptLineup *readKeptLineup(const ByteBuffer *json, unsigned side);
static int keepLineup(const KeptLineup *kept, const Path *output);
static void warnLineupNotKept(unsigned side, const char *problem);
static void warnMissingPlayers(
	const KeptLineup *kept,
	const FixtureTeam *rebuilt,
	const uint8_t *from,
	const FixtureLineupPeople *matched
);
static void warnOpponentRecords(const char *command, unsigned side);
static void placeLabel(const FixtureTeam *team, unsigned place, char *out, size_t size);
static void printLineup(
	const FixtureTeam *team,
	unsigned side,
	const uint8_t *from,
	const FixtureLineupResult *result,
	const Path *output,
	bool kept
);
static void printPlayers(
	const char *label,
	const FixtureTeam *team,
	const uint8_t *from,
	const bool *listed,
	unsigned count
);
static unsigned optionNamed(const wchar_t *argument);
static bool parseOptions(
	int argc,
	wchar_t **argv,
	int first,
	unsigned allowed,
	unsigned *given,
	SaveCheck *check
);
static bool checkSaveOptions(const SaveCheck *check);
static bool parseSaveName(const wchar_t *text, char *out, size_t size);
static void versionLine(char *out, size_t size);

int commands_run(int argc, wchar_t **argv) {
	return commands_runWithProgress(argc, argv, 0);
}

int commands_runWithProgress(int argc, wchar_t **argv, uintptr_t progressWindow) {
	RunOptions options;
	if (!parseRunArguments(argc, argv, &options)) {
		fprintf(
			stderr,
			"usage: fm24ctl run <input.bin|.json> [seeds] [output.csv] [--threads N]"
			" [--profile <samples.txt>] [--detailed] [--human-control] [--keep-build] [--per-match]"
			" [--keep-going] [--manifest]" SAVE_FLAGS_USAGE "\n"
		);
		return EXIT_USAGE;
	}
	RunRecord record = {0};
	Path output;
	// The default results, beside the input, are checked once it has been read (see playRun).
	if (!absolutePath(options.input, &record.fixture) ||
			!(options.output ? outputPath(options.output, &output, true)
											 : defaultOutput(&record.fixture, &output)) ||
			(options.profile && !outputPath(options.profile, &record.profile, false))) {
		return EXIT_FAILED;
	}
	int status = playRun(&options, &output, progressWindow, &record);
	return options.manifest && !record.refused
					 ? writeRunManifest(argc, argv, &options, &output, &record, status)
					 : status;
}

int commands_getNextFixture(int argc, wchar_t **argv) {
	char seed[32] = "random";
	bool full = false;
	SaveCheck check = {0};
	if (argc < 3 || !parseFixtureOptions(argc, argv, seed, sizeof seed, &full, &check)) {
		fprintf(
			stderr,
			"usage: fm24ctl get-next-fixture <output.json|.bin> [--seed N] [--full]" SAVE_OPTIONS_USAGE
			"\n"
		);
		return EXIT_USAGE;
	}
	Path output;
	if (!outputPath(argv[2], &output, false)) {
		return EXIT_FAILED;
	}
	char job[JOB_LENGTH];
	(void)snprintf(
		job, sizeof job, "command=build_next\noutput=%s\nseed=%s\nfull=%d\n", output.utf8, seed, full
	);
	int ready = runJob_ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	return ready == EXIT_OK ? submitAndReport(job) : ready;
}

int commands_replaceTeam(int argc, wchar_t **argv) {
	char result[CLIENT_RESULT_LENGTH];
	return commands_replaceTeamWithResult(argc, argv, result, sizeof result);
}

int commands_replaceTeamWithResult(int argc, wchar_t **argv, char *result, size_t resultSize) {
	bool home = argc > 1 && wcscmp(argv[1], L"replace-home") == 0;
	unsigned options = 0;
	SaveCheck check = {0};
	if (argc < 5 || !parseOptions(argc, argv, 5, OPTION_FULL | OPTION_SAVE_FLAGS, &options, &check) ||
			!files_hasExtension(argv[2], L".json") || !files_hasExtension(argv[4], L".json")) {
		(void)snprintf(result, resultSize, "Team replacement requires a .json fixture and output.");
		fprintf(
			stderr,
			"usage: fm24ctl replace-%s <input.json> <team ID or club name> <output.json>"
			" [--full]" SAVE_FLAGS_USAGE "\n",
			home ? "home" : "away"
		);
		return EXIT_USAGE;
	}
	bool full = (options & OPTION_FULL) != 0;
	Path input;
	Path output;
	Path bin;
	char team[256];
	size_t size = 0;
	uint32_t teams[2] = {0};
	if (!paths_toUtf8(argv[3], team, sizeof team) || !team[0] || strpbrk(team, "\r\n") ||
			wcsspn(argv[3], L" \t") == wcslen(argv[3])) {
		(void)snprintf(
			result, resultSize, "Enter a team ID or club short name (up to 255 UTF-8 bytes)."
		);
		fprintf(stderr, "%s\n", result);
		return EXIT_FAILED;
	}
	(void)snprintf(result, resultSize, "Cannot prepare replacement; see the console for details.");
	if (!absolutePath(argv[2], &input) || !outputPath(argv[4], &output, false) ||
			!jobInputPath(&bin, L".bin")) {
		return EXIT_FAILED;
	}
	// The other side's line-up, which the rebuild lists in the game's order, is kept if the input
	// marks it as changed: read before the job, whose output may be the input. fm24ctl then reads
	// and writes the output itself, so it checks it as its own.
	unsigned other = home ? 1 : 0;
	bool lineup[2] = {false, false};
	ByteBuffer json = {0};
	bool encoded = encodeToFile(&input, &bin, &size, teams, &check, NULL, lineup, &json);
	KeptLineup *kept = encoded && lineup[other] ? readKeptLineup(&json, other) : NULL;
	buffer_free(&json);
	if (!encoded || (lineup[other] && !kept) ||
			(kept && !checkOutput(&output, paths_parentFolder(output.wide), true))) {
		free(kept);
		removeJobInput(&bin);
		return EXIT_FAILED;
	}
	int ready = runJob_ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have the fixture's save loaded and idle.");
		removeJobInput(&bin);
		free(kept);
		return ready;
	}
	char job[JOB_LENGTH];
	(void)snprintf(
		job,
		sizeof job,
		"command=replace_%s\ninput=%s\nteam=%s\nhome_team=%.0u\naway_team=%.0u\noutput=%s\nfull=%d\n",
		home ? "home" : "away",
		bin.utf8,
		team,
		teams[0],
		teams[1],
		output.utf8,
		full
	);
	(void)snprintf(
		result,
		resultSize,
		"Cannot replace team; FM24 must be running and idle. See the console and harness.log."
	);
	bool submitted = client_submit(job, CLIENT_SHORT_TIMEOUT_MS, result, resultSize);
	removeJobInput(&bin);
	if (!submitted) {
		free(kept);
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	int status = client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
	if (status == EXIT_OK && kept) {
		status = keepLineup(kept, &output);
		if (status != EXIT_OK) {
			(void)snprintf(
				result,
				resultSize,
				"Cannot keep the %s line-up in the replacement; see the console for details.",
				other ? "away" : "home"
			);
		}
	}
	free(kept);
	return status;
}

int commands_listTeams(int argc, wchar_t **argv) {
	char result[CLIENT_RESULT_LENGTH];
	return commands_listTeamsWithResult(argc, argv, result, sizeof result);
}

int commands_listTeamsWithResult(int argc, wchar_t **argv, char *result, size_t resultSize) {
	unsigned options = 0;
	SaveCheck check = {0};
	if (argc < 3 || !parseOptions(argc, argv, 3, SAVE_OPTIONS, &options, &check)) {
		(void)snprintf(result, resultSize, "List teams requires an output file.");
		fprintf(stderr, "usage: fm24ctl list-teams <output.txt>" SAVE_OPTIONS_USAGE "\n");
		return EXIT_USAGE;
	}
	Path output;
	if (!outputPath(argv[2], &output, false)) {
		(void)snprintf(result, resultSize, "Cannot resolve the team list path.");
		return EXIT_FAILED;
	}
	int ready = runJob_ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have that save loaded and idle.");
		return ready;
	}
	char job[JOB_LENGTH];
	(void)snprintf(job, sizeof job, "command=list_teams\noutput=%s\n", output.utf8);
	(void)snprintf(
		result, resultSize, "Cannot list teams; FM24 must be running and idle. See harness.log."
	);
	if (!client_submit(job, CLIENT_SHORT_TIMEOUT_MS, result, resultSize)) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	return client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
}

int commands_listPlayers(int argc, wchar_t **argv) {
	char result[CLIENT_RESULT_LENGTH];
	return commands_listPlayersWithResult(argc, argv, result, sizeof result);
}

int commands_listPlayersWithResult(int argc, wchar_t **argv, char *result, size_t resultSize) {
	unsigned options = 0;
	SaveCheck check = {0};
	if (argc < 3 || !parseOptions(argc, argv, 3, SAVE_OPTIONS, &options, &check)) {
		(void)snprintf(result, resultSize, "List players requires an output file.");
		fprintf(stderr, "usage: fm24ctl list-players <output.txt>" SAVE_OPTIONS_USAGE "\n");
		return EXIT_USAGE;
	}
	Path output;
	if (!outputPath(argv[2], &output, false)) {
		(void)snprintf(result, resultSize, "Cannot resolve the player list path.");
		return EXIT_FAILED;
	}
	int ready = runJob_ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have that save loaded and idle.");
		return ready;
	}
	char job[JOB_LENGTH];
	(void)snprintf(job, sizeof job, "command=list_players\noutput=%s\n", output.utf8);
	(void)snprintf(
		result, resultSize, "Cannot list players; FM24 must be running and idle. See harness.log."
	);
	if (!client_submit(job, CLIENT_SHORT_TIMEOUT_MS, result, resultSize)) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	return client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
}

int commands_getPlayer(int argc, wchar_t **argv) {
	char person[16];
	Path output;
	unsigned options = 0;
	SaveCheck check = {0};
	if (argc < 4 || !parsePersonId(argv[2], person, sizeof person) ||
			!parseOptions(argc, argv, 4, SAVE_OPTIONS, &options, &check)) {
		fprintf(
			stderr, "usage: fm24ctl get-player <person unique ID> <output.json>" SAVE_OPTIONS_USAGE "\n"
		);
		return EXIT_USAGE;
	}
	if (!outputPath(argv[3], &output, false)) {
		return EXIT_FAILED;
	}
	int ready = runJob_ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	char result[CLIENT_RESULT_LENGTH];
	return ready == EXIT_OK ? fetchPlayer(person, &output, result, sizeof result) : ready;
}

int commands_injectPlayer(int argc, wchar_t **argv) {
	char result[CLIENT_RESULT_LENGTH];
	return commands_injectPlayerWithResult(argc, argv, result, sizeof result);
}

int commands_injectPlayerWithResult(int argc, wchar_t **argv, char *result, size_t resultSize) {
	unsigned options = 0;
	SaveCheck check = {0};
	char block[64];
	char person[16] = "";
	bool fromFile = argc >= 5 && files_hasExtension(argv[4], L".json");
	if (argc < 6 ||
			!parseOptions(argc, argv, 6, OPTION_ALLOW_DUPLICATE | OPTION_SAVE_FLAGS, &options, &check) ||
			!files_hasExtension(argv[2], L".json") || !files_hasExtension(argv[5], L".json") ||
			!parsePlayerBlock(argv[3], block, sizeof block) ||
			(!fromFile && !parsePersonId(argv[4], person, sizeof person))) {
		(void)snprintf(result, resultSize, "Player injection requires a .json fixture and output.");
		fprintf(
			stderr,
			"usage: fm24ctl inject-player <input.json> <home|away>.player[N]"
			" <person unique ID|player.json> <output.json> [--allow-duplicate]" SAVE_FLAGS_USAGE "\n"
		);
		return EXIT_USAGE;
	}
	bool allowDuplicate = (options & OPTION_ALLOW_DUPLICATE) != 0;
	Path input;
	Path output;
	Path player;
	(void)snprintf(result, resultSize, "Cannot prepare the injection; see the console for details.");
	if (!absolutePath(argv[2], &input) || !outputPath(argv[5], &output, true) ||
			!(fromFile ? absolutePath(argv[4], &player) : jobInputPath(&player, L".json"))) {
		return EXIT_FAILED;
	}
	ByteBuffer config = {0};
	if (!files_read(input.wide, &config)) {
		(void)snprintf(result, resultSize, "cannot read %s", input.utf8);
		fprintf(stderr, "%s\n", result);
		buffer_free(&config);
		return EXIT_FAILED;
	}
	if (!fromFile) {
		int fetched = fetchForFixture(&config, &check, person, &player, result, resultSize);
		if (fetched != EXIT_OK) {
			removeJobInput(&player);
			buffer_free(&config);
			return fetched;
		}
	}
	ByteBuffer values = {0};
	ByteBuffer injected = {0};
	PlayerInjection injection;
	char error[512] = "";
	int status = EXIT_FAILED;
	if (!files_read(player.wide, &values)) {
		(void)snprintf(result, resultSize, "cannot read %s", player.utf8);
	} else if (!playerInject_apply(
							 buffer_string(&config),
							 config.length,
							 buffer_string(&values),
							 values.length,
							 block,
							 allowDuplicate,
							 &injected,
							 &injection,
							 error,
							 sizeof error
						 )) {
		(void)snprintf(result, resultSize, "error: %s", error);
	} else if (!files_write(output.wide, injected.data, injected.length)) {
		(void)snprintf(result, resultSize, "cannot write %s", output.utf8);
	} else {
		(void)snprintf(
			result,
			resultSize,
			"ok: %zu values (and %zu unique ID copies) of %s replaced; wrote %s",
			injection.replaced,
			injection.uidCopies,
			block,
			output.utf8
		);
		status = EXIT_OK;
	}
	if (status != EXIT_OK) {
		fprintf(stderr, "%s\n", result);
	} else {
		printf("%s\n", result);
		if (injection.missing) {
			printf("not in the block, left out: %s\n", injection.missingNames);
		}
	}
	if (!fromFile) {
		removeJobInput(&player);
	}
	buffer_free(&config);
	buffer_free(&values);
	buffer_free(&injected);
	return status;
}

// clang-format off: one usage line per source line
static const char lineupUsage[] =
	"usage: fm24ctl lineup <input.json> <home|away>\n"
	"       fm24ctl lineup <input.json> <home|away> <player> <player> <output.json>"
	" [--allow-ai-side]\n"
	"       fm24ctl lineup <input.json> <home|away> <player,player,...> <output.json>"
	" [--allow-ai-side]\n";
// clang-format on

// Shows a side's players, or writes the fixture with two of them exchanged or with a new XI,
// offline: see docs/CLI.md.
int commands_lineup(int argc, wchar_t **argv) {
	LineupRequest request;
	if (!parseLineupArguments(argc, argv, &request)) {
		free(request.text);
		fputs(lineupUsage, stderr);
		return EXIT_USAGE;
	}
	Path input;
	Path output;
	ByteBuffer config = {0};
	FixtureData *data = NULL;
	int status = EXIT_FAILED;
	if (absolutePath(request.input, &input) &&
			(!request.output || outputPath(request.output, &output, true)) &&
			readLineupFixture(&input, &config, &data)) {
		status = request.output ? writeLineup(&request, &config, data, &output)
														: showLineup(data, request.side);
	}
	free(data);
	buffer_free(&config);
	free(request.text);
	return status;
}

int commands_encode(int argc, wchar_t **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: fm24ctl encode <input.json> <output.bin>\n");
		return EXIT_USAGE;
	}
	Path input;
	Path output;
	size_t size = 0;
	if (!absolutePath(argv[2], &input) || !absolutePath(argv[3], &output) ||
			!encodeToFile(&input, &output, &size, NULL, NULL, NULL, NULL, NULL)) {
		return EXIT_FAILED;
	}
	printf("wrote %zu bytes\n", size);
	return EXIT_OK;
}

int commands_decode(int argc, wchar_t **argv) {
	bool full = false;
	if (!parseDecodeArguments(argc, argv, &full)) {
		fprintf(stderr, "usage: fm24ctl decode <input.bin> <output.json> [--full]\n");
		return EXIT_USAGE;
	}
	Path input;
	Path output;
	if (!absolutePath(argv[2], &input) || !outputPath(argv[3], &output, false)) {
		return EXIT_FAILED;
	}
	char job[JOB_LENGTH];
	(void)snprintf(
		job,
		sizeof job,
		"command=inspect\ninput=%s\noutput=%s\nfull=%d\n",
		input.utf8,
		output.utf8,
		full
	);
	return submitAndReport(job);
}

// Prints the harness's status line: success only if a game is loaded and idle.
int commands_status(int argc, wchar_t **argv) {
	(void)argv;
	if (argc != 2) {
		fprintf(stderr, "usage: fm24ctl status\n");
		return EXIT_USAGE;
	}
	char result[CLIENT_RESULT_LENGTH];
	if (!client_submit("command=status\n", CLIENT_STATUS_TIMEOUT_MS, result, sizeof result)) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	if (!client_succeeded(result)) {
		return EXIT_FAILED;
	}
	bool ready = strncmp(result, "ok: ready", 9) == 0 && (result[9] == ' ' || result[9] == 0);
	return ready ? EXIT_OK : EXIT_WRONG_STATE;
}

// Prints the build's protocol, commit, whether it had uncommitted changes, and its slice.
int commands_version(int argc, wchar_t **argv) {
	(void)argv;
	if (argc != 2) {
		fprintf(stderr, "usage: fm24ctl version\n");
		return EXIT_USAGE;
	}
	char line[128];
	versionLine(line, sizeof line);
	printf("%s\n", line);
	return EXIT_OK;
}

// A command-line path made absolute (the DLL runs with fm.exe's working directory).
static bool absolutePath(const wchar_t *argument, Path *out) {
	bool ok = paths_absolute(argument, out->wide, HARNESS_PATH_LENGTH) &&
						paths_toUtf8(out->wide, out->utf8, sizeof out->utf8) && !strpbrk(out->utf8, "\r\n");
	if (!ok) {
		fprintf(stderr, "bad path: %ls\n", argument);
	}
	return ok;
}

// absolutePath for a file the game or fm24ctl writes: false, with why, also if it is a folder, if
// its folder is missing or is a file, or if the file cannot be written there, before the game is
// started or given the job (which would only report a failed write). macOS's privacy protection
// judges each app on its own: a folder it keeps the app fm24ctl runs in (such as Terminal) out of
// may still be open to the game, so it stops only a file fm24ctl `opened` itself (run's results,
// which it reads back; inject-player's and lineup's output, which it writes; and the output of a
// replacement that keeps a changed line-up, which it reads and writes after the job). Otherwise,
// as a folder that could not be checked, it is left to the game.
static bool outputPath(const wchar_t *argument, Path *out, bool opened) {
	return absolutePath(argument, out) && checkOutput(out, paths_parentFolder(out->wide), opened);
}

// outputPath's check of absolute path `out`, whose folder paths_parentFolder found as `folder`.
static bool checkOutput(const Path *out, PathsFolder folder, bool opened) {
	const char *problem = NULL;
	switch (folder) {
		case PATHS_FILE_IS_FOLDER: problem = "it is a folder (name a file in it)"; break;
		case PATHS_FOLDER_MISSING: problem = "its folder does not exist (create it first)"; break;
		case PATHS_FOLDER_NOT_FOLDER: problem = "its folder, or one above it, is a file"; break;
		case PATHS_FOLDER_DENIED:
			problem =
				"you have no permission to open its folder, or one above it (choose another folder, or "
				"change the permissions)";
			break;
		case PATHS_FOLDER_NOT_WRITABLE:
			problem =
				"you have no permission to write in its folder (choose another folder, or change its "
				"permissions)";
			break;
		case PATHS_FOLDER_LOCKED:
			problem =
				"its folder is locked (choose another folder, or unlock it with Get Info in Finder)";
			break;
		case PATHS_FILE_NOT_WRITABLE:
			problem =
				"you have no permission to write it (choose another file, or change its permissions)";
			break;
		case PATHS_FILE_LOCKED:
			problem = "it is locked (choose another file, or unlock it with Get Info in Finder)";
			break;
		case PATHS_FOLDER_PRIVACY:
			if (opened) {
				problem =
					"macOS does not let fm24ctl into its folder (operation not permitted): choose another "
					"folder or, for a folder macOS protects, allow the app fm24ctl runs in (such as "
					"Terminal) under System Settings > Privacy & Security";
			}
			break;
		case PATHS_FOLDER_FOUND:
		case PATHS_FOLDER_UNCHECKED: break;
	}
	if (problem) {
		fprintf(stderr, "cannot write %s: %s\n", out->utf8, problem);
		return false;
	}
	return true;
}

// A new file in the job directory (next to fm24ctl.exe on Windows) for one job's encoded input (or
// output): jobs wait in line for the DLL, so each needs its own (several runners, or a viewer's
// simulation and replacement, may be queued).
static bool jobInputPath(Path *out, const wchar_t *extension) {
	static atomic_long count;
	wchar_t name[64];
	wchar_t directory[HARNESS_PATH_LENGTH];
	(void)swprintf(
		name,
		sizeof name / sizeof name[0],
		L"_job_input-%lu-%ld%ls",
		platform_processId(),
		atomic_fetch_add(&count, 1) + 1,
		extension
	);
	bool ok = paths_jobDirectory(directory, HARNESS_PATH_LENGTH) &&
						paths_join(out->wide, HARNESS_PATH_LENGTH, directory, name) &&
						paths_toUtf8(out->wide, out->utf8, sizeof out->utf8);
	if (!ok) {
		fprintf(stderr, "cannot resolve the working file %ls\n", name);
	}
	return ok;
}

static void removeJobInput(const Path *path) {
	(void)platform_removeFile(path->wide);
}

// Submits a job that reports one result line, prints it, and turns it into an exit code.
static int submitAndReport(const char *job) {
	char result[CLIENT_RESULT_LENGTH];
	if (!client_submit(job, CLIENT_SHORT_TIMEOUT_MS, result, sizeof result)) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	return client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
}

// `run` once its paths are known: prepares its input, has the game's save checked, submits the job
// and prints what it did. Returns the exit code, with what the manifest records in `record`.
static int
playRun(RunOptions *options, const Path *output, uintptr_t progressWindow, RunRecord *record) {
	record->input = record->fixture;
	record->job = (RunJob){
		.input = record->input.utf8,
		.seeds = record->seeds,
		.output = output->utf8,
		.threads = options->threads,
		.profile = record->profile.utf8,
		.detailed = options->detailed,
		.humanControl = options->humanControl,
		.keepBuild = options->keepBuild,
		.progressWindow = progressWindow,
		.perMatch = options->perMatch,
		.skipSeedErrors = options->keepGoing,
		.report = options->manifest,
	};
	bool encoded = false;
	bool lineup[2] = {false, false};
	if (!paths_toUtf8(options->seeds, record->seeds, sizeof record->seeds)) {
		record->seeds[0] = 0;
		return EXIT_FAILED;
	}
	if (!prepareInput(
				&record->input,
				record->job.teams,
				&encoded,
				&options->check,
				options->manifest ? &record->facts : NULL,
				lineup
			)) {
		return EXIT_FAILED;
	}
	// The default results go beside the input, once it has been read: a missing input says so first.
	// The files beside the results are checked then too, before the save is checked.
	if ((!options->output && !checkOutput(output, paths_parentFolder(output->wide), true)) ||
			!checkSiblingPaths(options, output) || (options->manifest && !checkManifestPath(output))) {
		record->refused = true;
		if (encoded) {
			removeJobInput(&record->input);
		}
		return EXIT_FAILED;
	}
	warnIfLineupLost(options, lineup);
	char job[HARNESS_JOB_LENGTH];
	int ready = EXIT_FAILED;
	if (!runJob_format(&record->job, job, sizeof job)) {
		fprintf(stderr, "the job is too long\n");
	} else {
		ready =
			runJob_ensureSave(&options->check, CLIENT_LONG_TIMEOUT_MS, record->date, sizeof record->date);
	}
	if (ready != EXIT_OK) {
		if (encoded) {
			removeJobInput(&record->input);
		}
		return ready;
	}
	runJob_warnIfDated(&options->check, record->date);
	printf("results: %s\n", output->utf8);
	printf("job submitted; waiting...\n");
	submitRun(job, record);
	if (encoded) {
		removeJobInput(&record->input);
	}
	if (record->outcome != CLIENT_REPLIED) {
		return EXIT_FAILED;
	}
	printf("%s\n", record->result);
	if (!client_succeeded(record->result)) {
		return EXIT_FAILED;
	}
	if (options->detailed) {
		printSibling("player statistics", output, "-players.csv");
		printSibling("team statistics", output, "-teams.csv");
	}
	if (options->perMatch) {
		printSibling("per-match player rows", output, "-match-players.csv");
		printSibling("per-match team rows", output, "-match-teams.csv");
	}
	if (options->keepGoing) {
		printSkippedSeeds(record->result, output);
	}
	return printSummary(output);
}

// Submits the run's job, noting when, how it ended and how long the computer slept meanwhile: the
// monotonic clock counts sleep, the uptime clock does not.
static void submitRun(const char *job, RunRecord *record) {
	uint64_t perMs = platform_tickFrequency() / 1000;
	uint64_t ticks = platform_ticks();
	uint64_t uptime = platform_uptimeTicks();
	record->submitted = true;
	record->startedAt = time(NULL);
	record->outcome = client_submitWatched(
		job, CLIENT_LONG_TIMEOUT_MS, &record->watch, record->result, sizeof record->result
	);
	record->finishedAt = time(NULL);
	uint64_t passed = (platform_ticks() - ticks) / perMs;
	uint64_t awake = (platform_uptimeTicks() - uptime) / perMs;
	record->sleptMs = passed > awake + SLEEP_TOLERANCE_MS ? passed - awake : 0;
}

// Writes `run`'s manifest, <stem>-manifest.json, once it has ended with exit code `status`, and
// returns the exit code: `status`, or EXIT_FAILED if the manifest cannot be written.
static int writeRunManifest(
	int argc,
	wchar_t **argv,
	const RunOptions *options,
	const Path *output,
	RunRecord *record,
	int status
) {
	RunReport report;
	bool reported = record->submitted && record->outcome == CLIENT_REPLIED &&
									runReport_parse(record->result, &report);
	RunOutcome outcome = manifest_runOutcome(
		record->submitted ? &record->watch : NULL,
		record->outcome,
		record->result,
		reported ? &report : NULL
	);
	if (reported) {
		warnIfOtherHarness(&report);
	}
	RunOutputs outputs = {0};
	if (reported) {
		listOutputs(options, output, outcome, &outputs);
	}
	HostInfo host;
	hostInfo_read(&host);
	size_t count = argc > 1 ? (size_t)argc - 1 : 0;
	char **command = calloc(count ? count : 1, sizeof *command);
	for (size_t i = 0; command && i < count; ++i) {
		size_t size = wcslen(argv[i + 1]) * 4 + 1;
		command[i] = malloc(size);
		if (command[i] && !platform_toUtf8(argv[i + 1], command[i], size)) {
			free(command[i]);
			command[i] = NULL;
		}
	}
#ifdef _WIN32
	const GameFacts *game = NULL;
#else
	char gamePath[4096];
	pid_t pid = macosGame_findRunning(gamePath, sizeof gamePath);
	GameFacts facts = {
		.pid = pid > 0 ? (uint32_t)pid : 0,
		.underRosetta = pid > 0 && macosGame_underRosetta(pid),
		.residentBytes = pid > 0 ? macosGame_residentBytes(pid) : 0,
		.save = options->check.save,
		.date = record->date,
	};
	const GameFacts *game = &facts;
#endif
	RunManifest manifest = {
		.createdAt = time(NULL),
		.outcome = outcome,
		.exitCode = status,
		.submitted = record->submitted,
		.clientOutcome = record->outcome,
		.result = record->submitted ? record->result : NULL,
		.command = (const char *const *)command,
		.commandCount = command ? count : 0,
		.commit = buildInfo_commit,
		.dirty = buildInfo_dirty,
		.slice = BUILD_INFO_SLICE,
		.host = &host,
		.report = reported ? &report : NULL,
		.game = game,
		.fixturePath = record->fixture.utf8,
		.fixture = &record->facts,
		.job = &record->job,
		.noLaunch = options->check.noLaunch,
		.anySave = options->check.anySave,
		.startedAt = record->startedAt,
		.finishedAt = record->finishedAt,
		.watch = &record->watch,
		.sleptMs = record->sleptMs,
		.outputs = outputs.list,
		.outputCount = outputs.count,
		.summary = outputs.summarised ? &outputs.summary : NULL,
	};
	Path path;
	ByteBuffer text = {0};
	bool written = command && siblingPath(output, "-manifest.json", &path) &&
								 manifest_formatRun(&manifest, &text) &&
								 files_replace(path.wide, text.data, text.length);
	buffer_free(&text);
	for (size_t i = 0; command && i < count; ++i) {
		free(command[i]);
	}
	free(command);
	if (!written) {
		fprintf(stderr, "cannot write the run's manifest\n");
		return status == EXIT_OK ? EXIT_FAILED : status;
	}
	printf("manifest: %s\n", path.utf8);
	return status;
}

// The files the harness wrote for the job, which it opened once it had read the job: the results,
// and the per-match rows, failed seeds and (if the run succeeded) detailed statistics it was asked
// for.
static void listOutputs(
	const RunOptions *options,
	const Path *output,
	RunOutcome outcome,
	RunOutputs *outputs
) {
	const struct {
		bool listed;
		const char *role;
		const char *suffix; // NULL: the output itself
	} files[] = {
		{true, "results", NULL},
		{options->perMatch, "match_players", "-match-players.csv"},
		{options->perMatch, "match_teams", "-match-teams.csv"},
		{options->keepGoing, "failed", "-failed.csv"},
		{options->detailed && outcome == RUN_OK, "detailed_players", "-players.csv"},
		{options->detailed && outcome == RUN_OK, "detailed_teams", "-teams.csv"},
	};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		Path path = *output;
		if (!files[i].listed || (files[i].suffix && !siblingPath(output, files[i].suffix, &path))) {
			continue;
		}
		ManifestOutput *file = &outputs->list[outputs->count];
		char *name = outputs->names[outputs->count++];
		const char *separator = strrchr(path.utf8, (char)PLATFORM_PATH_SEPARATOR);
		(void)snprintf(name, UTF8_PATH_LENGTH, "%s", separator ? separator + 1 : path.utf8);
		file->role = files[i].role;
		file->name = name;
		bool results = !files[i].suffix;
		(void)manifest_readOutput(path.wide, file, results ? &outputs->summary : NULL);
		outputs->summarised = outputs->summarised || (results && file->read);
	}
}

// A harness of this protocol may be an older build, which the game keeps until it restarts.
static void warnIfOtherHarness(const RunReport *report) {
	if (report->dirty < 0 || buildInfo_dirty < 0 ||
			(strcmp(report->commit, buildInfo_commit) == 0 && report->dirty == buildInfo_dirty)) {
		return;
	}
	fprintf(
		stderr,
		"warning: the game's harness was built from %.12s%s, fm24ctl from %.12s%s; restart the game "
		"to use this build's harness\n",
		report->commit,
		report->dirty ? " with changes" : "",
		buildInfo_commit,
		buildInfo_dirty ? " with changes" : ""
	);
}

// A file the harness writes next to the output: the output's stem (siblingStem) and `suffix`.
static bool siblingPath(const Path *output, const char *suffix, Path *out) {
	int written =
		snprintf(out->utf8, sizeof out->utf8, "%.*s%s", siblingStem(output), output->utf8, suffix);
	return written > 0 && (size_t)written < sizeof out->utf8 &&
				 platform_fromUtf8(out->utf8, out->wide, HARNESS_PATH_LENGTH);
}

// Encodes a JSON config into match bytes, and reads the save check its header asks for into
// `check`, the file's and its encoding's sizes, hashes and header into `facts`, its line-up
// markers (ConfigOrigin.lineup) into `lineup` and the JSON itself into `config`, if given.
static bool encodeConfig(
	const Path *json,
	ByteBuffer *bytes,
	uint32_t teams[2],
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2],
	ByteBuffer *config
) {
	ByteBuffer text = {0};
	char error[256] = "";
	bool ok = files_read(json->wide, &text);
	if (ok && facts) {
		facts->read = true;
		facts->bytes = text.length;
		sha256_hexOf(buffer_string(&text), text.length, facts->sha256);
		facts->hasHeader = matchConfig_readHeader(buffer_string(&text), text.length, &facts->header);
	}
	if (!ok) {
		fprintf(stderr, "cannot read %s\n", json->utf8);
	} else if (!(ok = matchConfig_encode(
								 buffer_string(&text), text.length, bytes, teams, error, sizeof error
							 ))) {
		fprintf(stderr, "%s: %s\n", json->utf8, error);
	} else {
		if (check) {
			runJob_readFixtureSave(&text, check);
		}
		ConfigOrigin origin;
		if (lineup && matchConfig_readOrigin(buffer_string(&text), text.length, &origin)) {
			lineup[0] = origin.lineup[0];
			lineup[1] = origin.lineup[1];
		}
		if (facts) {
			facts->encoded = true;
			facts->inputBytes = bytes->length;
			sha256_hexOf(buffer_string(bytes), bytes->length, facts->inputSha256);
		}
	}
	if (ok && config) {
		buffer_free(config);
		*config = text;
	} else {
		buffer_free(&text);
	}
	return ok;
}

static bool encodeToFile(
	const Path *json,
	const Path *output,
	size_t *size,
	uint32_t teams[2],
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2],
	ByteBuffer *config
) {
	ByteBuffer bytes = {0};
	bool ok = encodeConfig(json, &bytes, teams, check, facts, lineup, config);
	if (ok && !(ok = files_write(output->wide, bytes.data, bytes.length))) {
		fprintf(stderr, "cannot write %s\n", output->utf8);
	}
	*size = bytes.length;
	buffer_free(&bytes);
	return ok;
}

static void addResult(Totals *totals, unsigned seed, unsigned home, unsigned away) {
	++totals->runs;
	totals->homeGoals += home;
	totals->awayGoals += away;
	totals->homeWins += home > away;
	totals->draws += home == away;
	totals->awayWins += home < away;
	if (totals->runs <= SUMMARY_SHOWN) {
		printf("  seed %u: %u-%u\n", seed, home, away);
	}
}

// The first `count` comma-separated decimal numbers of a CSV line.
static bool parseCsvNumbers(const char *line, unsigned long *values, size_t count) {
	const char *position = line;
	for (size_t i = 0; i < count; ++i) {
		if (*position < '0' || *position > '9') {
			return false;
		}
		char *end = NULL;
		errno = 0;
		values[i] = strtoul(position, &end, 10);
		if (errno != 0 || values[i] > UINT32_MAX ||
				(i + 1 < count ? *end != ',' : *end && !strchr(",\r\n", *end))) {
			return false;
		}
		position = *end == ',' ? end + 1 : end;
	}
	return true;
}

// Prints the first results and the win/draw/loss totals of a results CSV.
static int printSummary(const Path *csvPath) {
	FILE *csv = platform_openFile(csvPath->wide, "rb");
	if (!csv) {
		fprintf(stderr, "no output written; see harness.log\n");
		return EXIT_FAILED;
	}
	char line[256];
	Totals totals = {0};
	bool ok =
		fgets(line, sizeof line, csv) != NULL && strncmp(line, "seed,home_goals,away_goals,", 27) == 0;
	while (ok && fgets(line, sizeof line, csv)) {
		unsigned long values[3] = {0};
		ok = parseCsvNumbers(line, values, 3);
		if (ok) {
			addResult(&totals, (unsigned)values[0], (unsigned)values[1], (unsigned)values[2]);
		}
	}
	ok = !ferror(csv) && ok;
	ok = fclose(csv) == 0 && ok;
	if (!ok) {
		fprintf(stderr, "cannot read valid results from %s\n", csvPath->utf8);
		return EXIT_FAILED;
	}
	if (totals.runs == 0) {
		printf("0 runs completed; see harness.log\n");
		return EXIT_FAILED;
	}
	printf(
		"%" PRIu64 " runs: home %" PRIu64 " / draw %" PRIu64 " / away %" PRIu64 ", avg %.2f-%.2f\n",
		totals.runs,
		totals.homeWins,
		totals.draws,
		totals.awayWins,
		(double)totals.homeGoals / (double)totals.runs,
		(double)totals.awayGoals / (double)totals.runs
	);
	return EXIT_OK;
}

// The size and hash of a .bin input, which is played as it is (for the manifest; the harness says
// if it cannot read it).
static void readBinaryFacts(const Path *input, FixtureFacts *facts) {
	ByteBuffer bytes = {0};
	if (files_read(input->wide, &bytes)) {
		facts->read = true;
		facts->encoded = true;
		facts->bytes = bytes.length;
		facts->inputBytes = bytes.length;
		sha256_hexOf(buffer_string(&bytes), bytes.length, facts->sha256);
		memcpy(facts->inputSha256, facts->sha256, sizeof facts->sha256);
	}
	buffer_free(&bytes);
}

// Turns a .json config into the .bin the DLL plays, and reads its team overrides, the save it asks
// for and its line-up markers, and what a manifest records of it into `facts` if given; a .bin
// (which has no markers) is played as it is, once fm24ctl has seen that it is there.
static bool prepareInput(
	Path *input,
	uint32_t teams[2],
	bool *encoded,
	SaveCheck *check,
	FixtureFacts *facts,
	bool lineup[2]
) {
	teams[0] = 0;
	teams[1] = 0;
	*encoded = false;
	if (!files_hasExtension(input->wide, L".json")) {
		if (!binaryFound(input)) {
			return false;
		}
		if (facts) {
			readBinaryFacts(input, facts);
		}
		return true;
	}
	Path bin;
	size_t size = 0;
	if (!jobInputPath(&bin, L".bin")) {
		return false;
	}
	if (!encodeToFile(input, &bin, &size, teams, check, facts, lineup, NULL)) {
		removeJobInput(&bin);
		return false;
	}
	*input = bin;
	*encoded = true;
	return true;
}

// The game reads a .bin input itself, but one that is not there (in a mistyped folder, say) is
// reported here, as a JSON input is, and not as its default results' folder. macOS's privacy
// protection may keep fm24ctl out of a file the game can still read ("operation not permitted"):
// that is left to the game.
static bool binaryFound(const Path *input) {
	errno = 0;
	FILE *file = platform_openFile(input->wide, "rb");
	if (file) {
		(void)fclose(file);
		return true;
	}
	if (errno == EPERM) {
		return true;
	}
	fprintf(stderr, "cannot read %s\n", input->utf8);
	return false;
}

// A side whose players `lineup` reordered (its "<side>_lineup" marker) plays its new order only as
// the file has it: each seed's rebuild puts the game's own back, and with --human-control a
// managed side may play its manager's selection. Warns, before the save check, if the run may not
// play it.
static void warnIfLineupLost(const RunOptions *options, const bool lineup[2]) {
	bool rebuilt = !options->keepBuild && wcscmp(options->seeds, L"keep") != 0;
	for (unsigned side = 0; side < 2; ++side) {
		const char *name = side ? "away" : "home";
		if (lineup[side] && rebuilt) {
			fprintf(
				stderr,
				"warning: %s line-up was edited (%s_lineup); rebuilding each seed puts FM24's own line-up "
				"back; add --keep-build to play it\n",
				name,
				name
			);
		}
		if (lineup[side] && options->humanControl) {
			fprintf(
				stderr,
				"warning: %s line-up was edited (%s_lineup); with --human-control FM24 may play the "
				"manager's own selection instead\n",
				name,
				name
			);
		}
	}
}

// One worker per logical processor, less one left for the game itself.
unsigned long commands_defaultThreads(void) {
	unsigned long processors = platform_processorCount();
	unsigned long threads = processors > 1 ? processors - 1 : 1;
	return threads < HARNESS_MAX_THREADS ? threads : HARNESS_MAX_THREADS;
}

// "keep", or only digits, commas and dashes ("42", "1-1000", "1,5,10-20"). The DLL checks the rest.
static bool isSeedList(const wchar_t *text) {
	if (wcscmp(text, L"keep") == 0) {
		return true;
	}
	return text[0] != 0 && wcsspn(text, L"0123456789,-") == wcslen(text);
}

// A decimal number at `text` (job.c's parseNumber); `end` is left after it. One too large for
// `value` reads as ULLONG_MAX.
static bool seedNumber(const char *text, const char **end, unsigned long long *value) {
	if (*text < '0' || *text > '9') {
		return false;
	}
	char *after = NULL;
	*value = strtoull(text, &after, 10);
	*end = after;
	return after != text;
}

// True if the harness takes seed pattern `pattern` (an isSeedList), as job.c's parseSeeds would;
// otherwise prints why, so that a typo stops fm24ctl before it starts the game or submits the job.
static bool checkSeedPattern(const wchar_t *pattern) {
	size_t length = wcslen(pattern);
	if (length > SEED_PATTERN_LENGTH) {
		fprintf(
			stderr,
			"the seed pattern must fit in %d characters (this one has %zu)\n",
			SEED_PATTERN_LENGTH,
			length
		);
		return false;
	}
	if (wcscmp(pattern, L"keep") == 0) {
		return true;
	}
	char text[SEED_PATTERN_LENGTH + 1];
	for (size_t i = 0; i <= length; ++i) {
		text[i] = (char)pattern[i]; // digits, commas and hyphens only
	}
	for (const char *part = text;;) {
		const char *comma = strchr(part, ',');
		int partLength = (int)(comma ? (size_t)(comma - part) : strlen(part));
		const char *end = part;
		unsigned long long first = 0;
		unsigned long long last = 0;
		bool read = partLength > 0 && seedNumber(part, &end, &first);
		last = first;
		if (read && *end == '-') {
			read = seedNumber(end + 1, &end, &last);
		}
		if (partLength == 0) {
			fprintf(stderr, "a seed is missing next to a comma in %s\n", text);
			return false;
		}
		if (!read || end != part + partLength) {
			fprintf(stderr, "%.*s is not a seed or a range of seeds (first-last)\n", partLength, part);
			return false;
		}
		if (first > UINT32_MAX || last > UINT32_MAX) {
			fprintf(stderr, "seeds must be numbers from 0 to 4294967295, not %.*s\n", partLength, part);
			return false;
		}
		if (last < first) {
			fprintf(
				stderr,
				"the seed range %.*s runs backwards: write %llu-%llu\n",
				partLength,
				part,
				last,
				first
			);
			return false;
		}
		if (last - first > MAX_SEED_RANGE) {
			fprintf(
				stderr,
				"the seed range %.*s has more than %d seeds: split it\n",
				partLength,
				part,
				MAX_SEED_RANGE + 1
			);
			return false;
		}
		if (!comma) {
			return true;
		}
		part = comma + 1;
	}
}

// `--name [value]` at argv[*i]; advances *i past a value. False (with a message) if invalid.
static bool parseRunOption(int argc, wchar_t **argv, int *i, RunOptions *options) {
	const wchar_t *name = argv[*i];
	bool hasValue = *i + 1 < argc;
	if (wcscmp(name, L"--profile") == 0 && hasValue) {
		options->profile = argv[++*i];
	} else if (wcscmp(name, L"--detailed") == 0) {
		options->detailed = true;
	} else if (wcscmp(name, L"--human-control") == 0) {
		options->humanControl = true;
	} else if (wcscmp(name, L"--keep-build") == 0) {
		options->keepBuild = true;
	} else if (wcscmp(name, L"--per-match") == 0) {
		options->perMatch = true;
	} else if (wcscmp(name, L"--keep-going") == 0) {
		options->keepGoing = true;
	} else if (wcscmp(name, L"--manifest") == 0) {
		options->manifest = true;
	} else if (optionNamed(name) == OPTION_NO_LAUNCH) {
		options->check.noLaunch = true;
	} else if (optionNamed(name) == OPTION_ANY_SAVE) {
		options->check.anySave = true;
	} else if (wcscmp(name, L"--threads") == 0 && hasValue) {
		wchar_t *end = NULL;
		errno = 0;
		options->threads = wcstoul(argv[++*i], &end, 10);
		if (errno || argv[*i][0] < L'0' || argv[*i][0] > L'9' || *end != 0 || options->threads < 1 ||
				options->threads > HARNESS_MAX_THREADS) {
			fprintf(stderr, "--threads must be 1-%d\n", HARNESS_MAX_THREADS);
			return false;
		}
	} else {
		fprintf(stderr, "unknown option: %ls\n", name);
		return false;
	}
	return true;
}

// The input, then optionally a seed list and/or an output file (told apart by isSeedList).
static bool placeRunArgument(const wchar_t *argument, RunOptions *options) {
	if (!options->input) {
		options->input = argument;
	} else if (!options->seeds && !options->output && isSeedList(argument)) {
		options->seeds = argument;
	} else if (!options->output) {
		options->output = argument;
	} else {
		fprintf(stderr, "unexpected argument: %ls\n", argument);
		return false;
	}
	return true;
}

// Everything after `run`; false (with a message) if something is missing or invalid.
static bool parseRunArguments(int argc, wchar_t **argv, RunOptions *options) {
	*options = (RunOptions){.threads = commands_defaultThreads()};
	for (int i = 2; i < argc; ++i) {
		bool ok = wcsncmp(argv[i], L"--", 2) == 0 ? parseRunOption(argc, argv, &i, options)
																							: placeRunArgument(argv[i], options);
		if (!ok) {
			return false;
		}
	}
	if (!options->seeds) {
		options->seeds = L"keep";
	}
	return options->input != NULL && checkSeedPattern(options->seeds);
}

// The input path with its extension replaced by .csv ("next.json" -> "next.csv"), which playRun
// checks as an explicit output is once the input has been read.
static bool defaultOutput(const Path *input, Path *output) {
	wchar_t path[HARNESS_PATH_LENGTH];
	wcscpy(path, input->wide);
	wchar_t *name = wcsrchr(path, PLATFORM_PATH_SEPARATOR);
	wchar_t *dot = wcsrchr(name ? name : path, L'.');
	if (dot) {
		*dot = 0;
	}
	if (wcslen(path) + wcslen(L".csv") >= HARNESS_PATH_LENGTH) {
		fprintf(stderr, "the default results path is too long\n");
		return false;
	}
	wcscat(path, L".csv");
	return absolutePath(path, output);
}

// The files the DLL writes beside the results (see siblingPath) for --per-match, --keep-going and
// --detailed, checked as outputs the game writes: beside results that already exist, they may be
// new files (a path too long for them is left to the DLL).
static bool checkSiblingPaths(const RunOptions *options, const Path *output) {
	const struct {
		bool written;
		const char *suffix;
	} siblings[] = {
		{options->perMatch, "-match-players.csv"},
		{options->perMatch, "-match-teams.csv"},
		{options->keepGoing, "-failed.csv"},
		{options->detailed, "-players.csv"},
		{options->detailed, "-teams.csv"},
	};
	for (size_t i = 0; i < sizeof siblings / sizeof siblings[0]; ++i) {
		Path path;
		if (siblings[i].written && siblingPath(output, siblings[i].suffix, &path) &&
				!checkOutput(&path, paths_parentFolder(path.wide), false)) {
			return false;
		}
	}
	return true;
}

// The manifest beside the results, which fm24ctl writes as a new file in its folder and then
// renames over it (files_replace): checked as that new file, whether or not the manifest exists,
// and as itself only for being a folder or locked, which the rename cannot replace (an existing
// one you may not write is replaced all the same). An append-only folder takes the new file but
// not the rename. A path too long is left to writeRunManifest.
static bool checkManifestPath(const Path *output) {
	char temporary[64];
	(void)snprintf(temporary, sizeof temporary, "-manifest.json.%lu.tmp", platform_processId());
	Path manifest;
	Path written;
	if (!siblingPath(output, "-manifest.json", &manifest) ||
			!siblingPath(output, temporary, &written)) {
		return true;
	}
	PathsFolder folder = paths_parentFolder(manifest.wide);
	if (folder != PATHS_FILE_IS_FOLDER && folder != PATHS_FILE_LOCKED) {
		folder = paths_parentFolder(written.wide);
	}
	if (folder == PATHS_FOLDER_FOUND && paths_folderAppendOnly(written.wide)) {
		folder = PATHS_FOLDER_LOCKED;
	}
	return checkOutput(&manifest, folder, true);
}

// The output path without ".csv": the stem of the files the DLL writes next to it (see
// matchStats_siblingPath).
static int siblingStem(const Path *output) {
	size_t length = strlen(output->utf8);
	return (int)(length > 4 && platform_compareIgnoringCase(output->utf8 + length - 4, ".csv") == 0
								 ? length - 4
								 : length);
}

static void printSibling(const char *label, const Path *output, const char *suffix) {
	printf("%s: %.*s%s\n", label, siblingStem(output), output->utf8, suffix);
}

// With --keep-going: how many seeds the result line says were skipped (if any), and where they are
// listed.
static void printSkippedSeeds(const char *result, const Path *output) {
	static const char runs[] = " runs, ";
	const char *after = strstr(result, runs);
	if (!after) {
		return;
	}
	after += sizeof runs - 1;
	char *end = NULL;
	unsigned long long skipped = strtoull(after, &end, 10);
	if (end == after || strncmp(end, " skipped (", 10) != 0 || !skipped) {
		return;
	}
	printf(
		"%llu seed%s skipped; see %.*s-failed.csv\n",
		skipped,
		skipped == 1 ? "" : "s",
		siblingStem(output),
		output->utf8
	);
}

// A `--seed` value: a decimal u32, copied to `seed` as text. Prints why and returns false if not.
static bool parseSeedValue(const wchar_t *value, char *seed, size_t size) {
	wchar_t *end = NULL;
	errno = 0;
	unsigned long long number = wcstoull(value, &end, 10);
	if (errno != 0 || value[0] < L'0' || value[0] > L'9' || *end != 0 || number > UINT32_MAX) {
		fprintf(stderr, "--seed must be a number from 0 to %u\n", UINT32_MAX);
		return false;
	}
	(void)snprintf(seed, size, "%llu", number);
	return true;
}

// get-next-fixture's options after the output path, in any order: `--seed N` (a decimal u32),
// `--full` and the save options.
static bool parseFixtureOptions(
	int argc,
	wchar_t **argv,
	char *seed,
	size_t size,
	bool *full,
	SaveCheck *check
) {
	for (int i = 3; i < argc; ++i) {
		unsigned option = optionNamed(argv[i]);
		bool hasValue = i + 1 < argc;
		bool ok = true;
		if (option == OPTION_FULL) {
			*full = true;
		} else if (option == OPTION_NO_LAUNCH) {
			check->noLaunch = true;
		} else if (option == OPTION_ANY_SAVE) {
			check->anySave = true;
		} else if (option == OPTION_SAVE && hasValue) {
			ok = parseSaveName(argv[++i], check->save, sizeof check->save);
		} else if (wcscmp(argv[i], L"--seed") == 0 && hasValue) {
			ok = parseSeedValue(argv[++i], seed, size);
		} else {
			fprintf(stderr, "unknown option or missing value: %ls\n", argv[i]);
			ok = false;
		}
		if (!ok) {
			return false;
		}
	}
	return checkSaveOptions(check);
}

// decode's arguments: the binary input, the JSON output, then optionally `--full`. False if they
// are not; a wrong extension also prints why.
static bool parseDecodeArguments(int argc, wchar_t **argv, bool *full) {
	*full = argc == 5 && wcscmp(argv[4], L"--full") == 0;
	if (argc != 4 && !*full) {
		return false;
	}
	if (files_hasExtension(argv[2], L".json")) {
		fprintf(stderr, "%ls is JSON already; decode reads a binary fixture (.bin)\n", argv[2]);
		return false;
	}
	if (!files_hasExtension(argv[3], L".json")) {
		fprintf(stderr, "decode writes fixture JSON; the output must end in .json, not %ls\n", argv[3]);
		return false;
	}
	return true;
}

// A person's unique ID: a nonzero decimal u32, copied to `out` as text.
static bool parsePersonId(const wchar_t *text, char *out, size_t size) {
	wchar_t *end = NULL;
	errno = 0;
	unsigned long long id = wcstoull(text, &end, 10);
	if (errno != 0 || text[0] < L'0' || text[0] > L'9' || *end != 0 || id == 0 || id > UINT32_MAX) {
		return false;
	}
	(void)snprintf(out, size, "%llu", id);
	return true;
}

// "home.player[N]" or "away.player[N]", copied to `out`.
static bool parsePlayerBlock(const wchar_t *text, char *out, size_t size) {
	size_t length = wcslen(text);
	bool side = wcsncmp(text, L"home.player[", 12) == 0 || wcsncmp(text, L"away.player[", 12) == 0;
	if (!side || length < 14 || length >= size || text[length - 1] != L']' ||
			wcsspn(text + 12, L"0123456789") != length - 13) {
		return false;
	}
	for (size_t i = 0; i <= length; ++i) {
		out[i] = (char)text[i];
	}
	return true;
}

// Has the DLL write a player's database values ("fm24-player/1") to `output`; `result` receives
// its result line.
static int fetchPlayer(const char *person, const Path *output, char *result, size_t resultSize) {
	char job[JOB_LENGTH];
	(void)snprintf(
		job, sizeof job, "command=get_player\nperson=%s\noutput=%s\n", person, output->utf8
	);
	(void)snprintf(
		result, resultSize, "Cannot fetch the player; FM24 must be running and idle. See harness.log."
	);
	if (!client_submit(job, CLIENT_SHORT_TIMEOUT_MS, result, resultSize)) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	return client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
}

// Fetches a player for injection into `config` (as fetchPlayer), once the game has the fixture's
// save loaded and idle.
static int fetchForFixture(
	const ByteBuffer *config,
	SaveCheck *check,
	const char *person,
	const Path *player,
	char *result,
	size_t resultSize
) {
	runJob_readFixtureSave(config, check);
	int ready = runJob_ensureSave(check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have the fixture's save loaded and idle.");
		return ready;
	}
	return fetchPlayer(person, player, result, resultSize);
}

// lineup's arguments: the input and side, then nothing (to show the side), two players and the
// output, or a list and the output, with --allow-ai-side anywhere after `lineup` for those two.
// False if they are not, with why when the usage does not say.
static bool parseLineupArguments(int argc, wchar_t **argv, LineupRequest *request) {
	*request = (LineupRequest){0};
	const wchar_t *positional[5];
	unsigned count = 0;
	for (int i = 2; i < argc; ++i) {
		if (wcsncmp(argv[i], L"--", 2) != 0) {
			if (count == sizeof positional / sizeof positional[0]) {
				return false;
			}
			positional[count++] = argv[i];
		} else if (wcscmp(argv[i], L"--allow-ai-side") != 0) {
			fprintf(stderr, "unknown option: %ls\n", argv[i]);
			return false;
		} else if (request->allowAiSide) {
			fprintf(stderr, "--allow-ai-side is given twice\n");
			return false;
		} else {
			request->allowAiSide = true;
		}
	}
	if (count != 2 && count != 4 && count != 5) {
		return false;
	}
	request->input = positional[0];
	if (wcscmp(positional[1], L"home") != 0 && wcscmp(positional[1], L"away") != 0) {
		fprintf(stderr, "the side is home or away, not %ls\n", positional[1]);
		return false;
	}
	request->side = wcscmp(positional[1], L"away") == 0;
	if (!files_hasExtension(request->input, L".json")) {
		fprintf(
			stderr, "lineup reads fixture JSON; the input must end in .json, not %ls\n", request->input
		);
		return false;
	}
	if (count == 2) {
		if (request->allowAiSide) {
			fprintf(stderr, "--allow-ai-side applies only to a new line-up\n");
		}
		return !request->allowAiSide;
	}
	request->output = positional[count - 1];
	if (!files_hasExtension(request->output, L".json")) {
		fprintf(
			stderr, "lineup writes fixture JSON; the output must end in .json, not %ls\n", request->output
		);
		return false;
	}
	return readLineupPlayers(positional + 2, count - 3, request);
}

// The players of lineup's `count` arguments: a list of the XI and any substitutes, cut at its
// commas, or two players to exchange. Each is trimmed, and must not be empty or a malformed place.
static bool readLineupPlayers(const wchar_t *const *texts, unsigned count, LineupRequest *request) {
	if (count < 1 || count > 2) {
		return false;
	}
	size_t size = 0;
	for (unsigned i = 0; i < count; ++i) {
		size += wcslen(texts[i]) * 4 + 1;
	}
	request->text = malloc(size);
	char *at = request->text;
	if (!at) {
		fputs("out of memory\n", stderr);
		return false;
	}
	for (unsigned i = 0; i < count; ++i) {
		if (!platform_toUtf8(texts[i], at, size - (size_t)(at - request->text))) {
			fprintf(stderr, "%ls is not valid text\n", texts[i]);
			return false;
		}
		request->players[request->count++] = at;
		at += strlen(at) + 1;
	}
	request->list = count == 1;
	for (char *comma = request->list ? strchr(request->text, ',') : NULL; comma;
		comma = strchr(comma + 1, ',')) {
		if (request->count == FIXTURE_PLAYERS) {
			fprintf(stderr, "the list names more than %d players\n", FIXTURE_PLAYERS);
			return false;
		}
		*comma = 0;
		request->players[request->count++] = comma + 1;
	}
	if (request->list && request->count == 1) {
		fputs("give two players to exchange, or the XI as a list\n", stderr);
		return false;
	}
	for (unsigned i = 0; i < request->count; ++i) {
		char error[FIXTURE_TEXT + 64];
		unsigned place = 0;
		request->players[i] = trimSpaces(request->players[i]);
		if (!request->players[i][0]) {
			fputs(
				request->list ? "the list has an empty item: name a player between each two commas\n"
											: "a player is empty: give a name, an ID or player[N]\n",
				stderr
			);
			return false;
		}
		if (fixtureLineup_readPlace(request->players[i], request->side, &place, error, sizeof error) <
				0) {
			fprintf(stderr, "%s\n", error);
			return false;
		}
	}
	if (request->list && request->count < FIXTURE_SLOTS) {
		fprintf(
			stderr,
			"the list names %u players: give the XI in slot order, then any substitutes\n",
			request->count
		);
		return false;
	}
	return true;
}

// `text` without the spaces and tabs around it, cut short in place.
static char *trimSpaces(char *text) {
	text += strspn(text, " \t");
	size_t length = strlen(text);
	while (length && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
		text[--length] = 0;
	}
	return text;
}

// The fixture lineup reads, and its players (malloc'd in `data`).
static bool readLineupFixture(const Path *input, ByteBuffer *config, FixtureData **data) {
	if (!files_read(input->wide, config)) {
		fprintf(stderr, "cannot read %s\n", input->utf8);
		return false;
	}
	const char *json = buffer_string(config);
	ConfigHeader header;
	char error[256] = "";
	if (!matchConfig_readHeader(json, config->length, &header) ||
			strcmp(header.format, "fm24-match-config/1") != 0) {
		fprintf(stderr, "%s is not a fixture (fm24-match-config/1)\n", input->utf8);
		return false;
	}
	*data = malloc(sizeof **data);
	if (!*data) {
		fputs("out of memory\n", stderr);
		return false;
	}
	if (!fixtureData_read(json, config->length, *data, error, sizeof error)) {
		fprintf(stderr, "%s: %s\n", input->utf8, error);
		return false;
	}
	return true;
}

// lineup's first form: the side's players in place order, as tab-separated text.
static int showLineup(const FixtureData *data, unsigned side) {
	const FixtureTeam *team = &data->teams[side];
	const char *name = side ? "away" : "home";
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	char error[512];
	if (!fixtureLineup_check(data, side, &lineup, true, error, sizeof error, NULL, 0)) {
		fprintf(stderr, "error: %s\n", error);
		return EXIT_FAILED;
	}
	if (!team->lineupKnown) {
		fprintf(stderr, "note: %s is managed by FM24's AI, which picks its own XI at kick-off\n", name);
	}
	printf("place\tslot\tposition\tplayer\tperson_uid\n");
	for (unsigned place = 0; place < team->playerCount.value; ++place) {
		const FixturePlayer *player = &team->players[place];
		char position[16] = "";
		if (place < FIXTURE_SLOTS && team->slots[place].position.known) {
			fixtureData_position(team->slots[place].position.value, position, sizeof position);
		}
		char shown[FIXTURE_TEXT];
		(void)snprintf(shown, sizeof shown, "%s", player->name);
		for (char *c = shown; *c; ++c) {
			*c = *c == '\t' || *c == '\n' || *c == '\r' ? ' ' : *c;
		}
		if (place < FIXTURE_SLOTS) {
			printf("%u\t%u\t%s\t%s\t", place, place, position, shown);
		} else {
			bool sub = place < FIXTURE_SLOTS + team->benchSize;
			printf("%u\t%s\t\t%s\t", place, sub ? "sub" : "reserve", shown);
		}
		if (player->personUid.known) {
			printf("%llu", (unsigned long long)player->personUid.value);
		}
		printf("\n");
	}
	return EXIT_OK;
}

// lineup's other forms: the side's new order, with two players exchanged or the list's XI and
// substitutes first, written to `output`, and what changed.
static int writeLineup(
	const LineupRequest *request,
	const ByteBuffer *config,
	const FixtureData *data,
	const Path *output
) {
	unsigned side = request->side;
	const FixtureTeam *team = &data->teams[side];
	unsigned count = (unsigned)team->playerCount.value;
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	FixtureLineupResult *result = malloc(sizeof *result);
	unsigned places[FIXTURE_PLAYERS] = {0};
	char error[1024] = "out of memory";
	char warnings[2048] = "";
	// The side's refusals come before its players are looked for.
	bool ok =
		result &&
		fixtureLineup_check(data, side, &lineup, request->allowAiSide, error, sizeof error, NULL, 0);
	for (unsigned i = 0; ok && i < request->count; ++i) {
		ok = fixtureLineup_find(data, side, request->players[i], &places[i], error, sizeof error) &&
				 !givenTwice(team, places, i + 1, error, sizeof error);
	}
	if (ok && request->list) {
		ok = fixtureLineup_promote(&lineup, side, count, places, request->count, error, sizeof error);
	} else if (ok) {
		ok = fixtureLineup_exchange(&lineup, side, count, places[0], places[1]);
	}
	ByteBuffer written = {0};
	ok = ok &&
			 fixtureLineup_check(
				 data, side, &lineup, request->allowAiSide, error, sizeof error, warnings, sizeof warnings
			 ) &&
			 fixtureLineup_apply(
				 buffer_string(config), config->length, &lineup, &written, result, error, sizeof error
			 );
	int status = EXIT_FAILED;
	if (!ok) {
		fprintf(stderr, "error: %s\n", error);
	} else if (!files_write(output->wide, written.data, written.length)) {
		fprintf(stderr, "cannot write %s\n", output->utf8);
	} else {
		for (char *line = warnings; *line;) {
			char *end = strchr(line, '\n');
			int length = (int)(end ? (size_t)(end - line) : strlen(line));
			fprintf(stderr, "warning: %.*s\n", length, line);
			line += length + (end != NULL);
		}
		if (result->opponentRecords[side]) {
			warnOpponentRecords("lineup", side);
		}
		printLineup(team, side, lineup.from[side], result, output, false);
		status = EXIT_OK;
	}
	buffer_free(&written);
	free(result);
	return status;
}

// True, with the message, if the last of `count` places is one of those before it.
static bool givenTwice(
	const FixtureTeam *team,
	const unsigned *places,
	unsigned count,
	char *error,
	size_t size
) {
	unsigned last = places[count - 1];
	for (unsigned i = 0; i + 1 < count; ++i) {
		if (places[i] == last) {
			(void)snprintf(error, size, "player[%u] %s is given twice", last, team->players[last].name);
			return true;
		}
	}
	return false;
}

// What place `place` of the side is: "slot 10, ST", "sub" or "reserve".
static void placeLabel(const FixtureTeam *team, unsigned place, char *out, size_t size) {
	if (place >= FIXTURE_SLOTS) {
		(void)snprintf(out, size, "%s", place < FIXTURE_SLOTS + team->benchSize ? "sub" : "reserve");
		return;
	}
	char position[16] = "";
	if (team->slots[place].position.known) {
		fixtureData_position(team->slots[place].position.value, position, sizeof position);
	}
	(void)snprintf(out, size, "slot %u%s%s", place, position[0] ? ", " : "", position);
}

// What lineup wrote, or replace-home or replace-away when it `kept` the other side's line-up: the
// players it moved, the side's new XI and bench, those it left out of the match-day squad (the XI
// and bench) and those a re-pointed marking of the other side's follows.
static void printLineup(
	const FixtureTeam *team,
	unsigned side,
	const uint8_t *from,
	const FixtureLineupResult *result,
	const Path *output,
	bool kept
) {
	unsigned count = (unsigned)team->playerCount.value;
	unsigned squad = FIXTURE_SLOTS + team->benchSize;
	bool moved[FIXTURE_PLAYERS] = {false};
	bool starters[FIXTURE_PLAYERS] = {false};
	bool bench[FIXTURE_PLAYERS] = {false};
	bool leftOut[FIXTURE_PLAYERS] = {false};
	unsigned movedCount = 0;
	for (unsigned place = 0; place < count; ++place) {
		moved[place] = from[place] != place;
		movedCount += moved[place];
		starters[place] = place < FIXTURE_SLOTS;
		bench[place] = place >= FIXTURE_SLOTS && place < squad;
		leftOut[place] = place >= squad && from[place] < squad;
	}
	printf("ok: %s%s line-up: ", kept ? "kept the input's " : "", side ? "away" : "home");
	if (movedCount == 2) {
		const char *separator = "";
		for (unsigned place = 0; place < count; ++place) {
			char label[32];
			if (moved[place]) {
				placeLabel(team, place, label, sizeof label);
				printf("%s%s to player[%u] (%s)", separator, team->players[from[place]].name, place, label);
				separator = ", ";
			}
		}
	} else if (movedCount) {
		printf("%u players moved", movedCount);
	} else {
		printf("no player moved");
	}
	printf("; wrote %s\n", output->utf8);
	printPlayers("XI: ", team, from, starters, count);
	printPlayers("bench: ", team, from, bench, count);
	printPlayers("left out: ", team, from, leftOut, count);
	char marking[64];
	(void)snprintf(
		marking, sizeof marking, "also re-pointed %s's marking of ", side ? "home" : "away"
	);
	printPlayers(marking, team, from, result->marked[side], count);
	if (movedCount) {
		printf(
			"note: play it with keep or --keep-build; rebuilding each seed puts FM24's own line-up back\n"
		);
	}
}

// `label` and the names of the side's players at the `listed` places, if there are any.
static void printPlayers(
	const char *label,
	const FixtureTeam *team,
	const uint8_t *from,
	const bool *listed,
	unsigned count
) {
	const char *separator = label;
	for (unsigned place = 0; place < count; ++place) {
		if (listed[place]) {
			printf("%s%s", separator, team->players[from[place]].name);
			separator = ", ";
		}
	}
	if (separator != label) {
		printf("\n");
	}
}

// The other side's line-up in replace-home or replace-away's input `json` (malloc'd), or NULL if
// memory runs out. Its problem says why the order cannot be kept.
static KeptLineup *readKeptLineup(const ByteBuffer *json, unsigned side) {
	KeptLineup *kept = malloc(sizeof *kept);
	if (!kept) {
		fputs("out of memory\n", stderr);
		return NULL;
	}
	kept->side = side;
	kept->count = 0;
	kept->problem[0] = 0;
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	char error[256] = "";
	if (!fixtureData_read(buffer_string(json), json->length, &kept->input, error, sizeof error) ||
			!fixtureLineup_check(&kept->input, side, &lineup, true, error, sizeof error, NULL, 0)) {
		(void)snprintf(kept->problem, sizeof kept->problem, "%s", error);
		return kept;
	}
	const FixtureTeam *team = &kept->input.teams[side];
	kept->count = (unsigned)team->playerCount.value;
	for (unsigned place = 0; place < kept->count; ++place) {
		const FixtureNumber *id = &team->players[place].personUid;
		if (!id->known || id->value > UINT32_MAX) {
			(void)snprintf(
				kept->problem,
				sizeof kept->problem,
				"%s.player[%u] has no unique ID (.person_uid)",
				side ? "away" : "home",
				place
			);
			return kept;
		}
		kept->people[place] = (uint32_t)id->value;
	}
	return kept;
}

// replace-home or replace-away's last step: the players of the side `kept` describes are put back
// in their places in the input, by person, in `output`, which gets the side's marker again. A
// line-up that cannot be kept is warned of, and the output left as the game wrote it.
static int keepLineup(const KeptLineup *kept, const Path *output) {
	if (kept->problem[0]) {
		warnLineupNotKept(kept->side, kept->problem);
		return EXIT_OK;
	}
	unsigned side = kept->side;
	ByteBuffer config = {0};
	ByteBuffer written = {0};
	FixtureData *data = malloc(sizeof *data);
	FixtureLineupResult *result = malloc(sizeof *result);
	FixtureLineupPeople *matched = malloc(sizeof *matched);
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	lineup.mark[side] = true;
	char error[1024] = "";
	int status = EXIT_FAILED;
	if (!data || !result || !matched) {
		fputs("out of memory\n", stderr);
	} else if (!files_read(output->wide, &config)) {
		fprintf(stderr, "cannot read %s\n", output->utf8);
	} else if (!fixtureData_read(buffer_string(&config), config.length, data, error, sizeof error) ||
						 !fixtureLineup_check(data, side, &lineup, true, error, sizeof error, NULL, 0) ||
						 !fixtureLineup_byPeople(data, side, kept->people, kept->count, &lineup, matched) ||
						 !fixtureLineup_apply(
							 buffer_string(&config), config.length, &lineup, &written, result, error, sizeof error
						 )) {
		warnLineupNotKept(side, error);
		status = EXIT_OK;
	} else if (!files_write(output->wide, written.data, written.length)) {
		fprintf(stderr, "cannot write %s\n", output->utf8);
	} else {
		warnMissingPlayers(kept, &data->teams[side], lineup.from[side], matched);
		if (result->opponentRecords[side]) {
			warnOpponentRecords(side ? "replace-home" : "replace-away", side);
		}
		printLineup(&data->teams[side], side, lineup.from[side], result, output, true);
		status = EXIT_OK;
	}
	buffer_free(&config);
	buffer_free(&written);
	free(data);
	free(result);
	free(matched);
	return status;
}

static void warnLineupNotKept(unsigned side, const char *problem) {
	const char *name = side ? "away" : "home";
	fprintf(
		stderr,
		"warning: cannot keep the input's %s line-up (%s_lineup): %s; the output lists the players as "
		"the game's build does, without the marker\n",
		name,
		name,
		problem
	);
}

// The players of a kept line-up whom the rebuilt side lacks, and who took their places.
static void warnMissingPlayers(
	const KeptLineup *kept,
	const FixtureTeam *rebuilt,
	const uint8_t *from,
	const FixtureLineupPeople *matched
) {
	const char *name = kept->side ? "away" : "home";
	for (unsigned i = 0; i < kept->count; ++i) {
		if (matched->found[i]) {
			continue;
		}
		fprintf(
			stderr,
			"warning: %s (%" PRIu32 ") of the input's %s line-up is not in the rebuilt side",
			kept->input.teams[kept->side].players[i].name,
			kept->people[i],
			name
		);
		int place = matched->places[i];
		if (place >= 0) {
			const FixturePlayer *player = &rebuilt->players[from[place]];
			char label[32];
			placeLabel(rebuilt, (unsigned)place, label, sizeof label);
			fprintf(stderr, "; player[%d] (%s) has %s", place, label, player->name);
			if (player->personUid.known) {
				fprintf(stderr, " (%llu)", (unsigned long long)player->personUid.value);
			}
			fputs(" instead", stderr);
		}
		fputs("\n", stderr);
	}
}

// The other side's opposition instructions, which `command` leaves as they were after moving the
// players of `side`.
static void warnOpponentRecords(const char *command, unsigned side) {
	fprintf(
		stderr,
		"warning: %s's opposition instructions (part_4240) are not all alike; %s leaves them as "
		"they were, so they may now apply to other %s players\n",
		side ? "home" : "away",
		command,
		side ? "away" : "home"
	);
}

// The option `argument` names, 0 for none. On Windows the save options are none.
static unsigned optionNamed(const wchar_t *argument) {
	static const struct {
		const wchar_t *name;
		unsigned option;
	} options[] = {
		{L"--full", OPTION_FULL},
		{L"--allow-duplicate", OPTION_ALLOW_DUPLICATE},
		{L"--save", OPTION_SAVE & SAVE_OPTIONS},
		{L"--no-launch", OPTION_NO_LAUNCH & SAVE_OPTIONS},
		{L"--any-save", OPTION_ANY_SAVE & SAVE_OPTIONS},
	};
	for (size_t i = 0; i < sizeof options / sizeof options[0]; ++i) {
		if (wcscmp(argument, options[i].name) == 0) {
			return options[i].option;
		}
	}
	return 0;
}

// The options from argv[first] on: each of `allowed` at most once, in any order, with --save's name
// in `check`. False if there is anything else (with a message for a bad name).
static bool parseOptions(
	int argc,
	wchar_t **argv,
	int first,
	unsigned allowed,
	unsigned *given,
	SaveCheck *check
) {
	*given = 0;
	for (int i = first; i < argc; ++i) {
		unsigned option = optionNamed(argv[i]) & allowed;
		if (!option || (*given & option) ||
				(option == OPTION_SAVE &&
					(++i >= argc || !parseSaveName(argv[i], check->save, sizeof check->save)))) {
			return false;
		}
		*given |= option;
	}
	check->noLaunch = (*given & OPTION_NO_LAUNCH) != 0;
	check->anySave = (*given & OPTION_ANY_SAVE) != 0;
	return !(allowed & OPTION_SAVE) || checkSaveOptions(check);
}

// --no-launch and --any-save change the check of --save's save. False (with a message) if they are
// given without it.
static bool checkSaveOptions(const SaveCheck *check) {
	if ((check->noLaunch || check->anySave) && !check->save[0]) {
		fprintf(stderr, "--no-launch and --any-save only apply with --save\n");
		return false;
	}
	return true;
}

// A --save value: a save's name, which is its file name without ".fm", copied to `out` as UTF-8.
// Prints why and returns false if it is not one.
static bool parseSaveName(const wchar_t *text, char *out, size_t size) {
	if (paths_toUtf8(text, out, size) && runJob_isSaveName(out)) {
		return true;
	}
	out[0] = 0;
	fprintf(stderr, "--save takes a save's name, its file name without .fm: %ls\n", text);
	return false;
}

// "fm24ctl <protocol> <commit> <clean|dirty> <slice>"; a build from outside a Git checkout has
// "unknown unknown" for its commit and state.
static void versionLine(char *out, size_t size) {
	const char *state = buildInfo_dirty > 0 ? "dirty" : buildInfo_dirty == 0 ? "clean" : "unknown";
	(void)snprintf(
		out,
		size,
		"fm24ctl %s %s %s %s",
		HARNESS_PROTOCOL_TEXT,
		buildInfo_commit,
		state,
		BUILD_INFO_SLICE
	);
}
