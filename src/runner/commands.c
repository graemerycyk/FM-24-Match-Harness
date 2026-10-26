// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "commands.h"

#include "client.h"
#include "files.h"
#include "match_config.h"
#include "paths.h"
#include "platform.h"
#include "player_inject.h"
#include "protocol.h"
#include "saves.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
	SUMMARY_SHOWN = 10, // how many individual results `run` prints
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

// The options that have the game's save checked before a job (see ensureSave), and their usage
// text. Windows, whose harness has no status job yet, has none of them.
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

// The save a job is for, which the game must have loaded and idle first (see ensureSave).
typedef struct SaveCheck {
	char save[SAVES_NAME_LENGTH]; // "" for none: the job goes ahead without a check
	char date[SAVES_DATE_LENGTH]; // the in-game date the fixture was exported at, "" if unknown
	bool badSave;                 // the fixture's "save" is not a save's name
	bool noLaunch;                // --no-launch
	bool anySave;                 // --any-save
} SaveCheck;

typedef struct RunOptions {
	const wchar_t *input;
	const wchar_t *seeds;  // default "keep"
	const wchar_t *output; // default: the input with a .csv extension
	unsigned long threads;
	const wchar_t *profile;
	bool detailed;
	bool humanControl;
	bool keepBuild;
	SaveCheck check; // the fixture's save, and --no-launch and --any-save
} RunOptions;

static bool absolutePath(const wchar_t *argument, Path *out);
static bool outputPath(const wchar_t *argument, Path *out, bool opened);
static bool jobInputPath(Path *out, const wchar_t *extension);
static void removeJobInput(const Path *path);
static int submitAndReport(const char *job);
static bool encodeConfig(const Path *json, ByteBuffer *bytes, uint32_t teams[2], SaveCheck *check);
static bool encodeToFile(
	const Path *json,
	const Path *output,
	size_t *size,
	uint32_t teams[2],
	SaveCheck *check
);
static void addResult(Totals *totals, unsigned seed, unsigned home, unsigned away);
static bool parseCsvNumbers(const char *line, unsigned long *values, size_t count);
static int printSummary(const Path *csvPath);
static bool prepareInput(Path *input, uint32_t teams[2], bool *encoded, SaveCheck *check);
static bool binaryFound(const Path *input);
static bool isSeedList(const wchar_t *text);
static bool seedNumber(const char *text, const char **end, unsigned long long *value);
static bool checkSeedPattern(const wchar_t *pattern);
static bool parseRunOption(int argc, wchar_t **argv, int *i, RunOptions *options);
static bool placeRunArgument(const wchar_t *argument, RunOptions *options);
static bool parseRunArguments(int argc, wchar_t **argv, RunOptions *options);
static bool defaultOutput(const Path *input, Path *output);
static bool checkDetailedPaths(const Path *output);
static void printDetailedPaths(const Path *output);
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
static bool isSaveName(const char *name);
static void readFixtureSave(const ByteBuffer *config, SaveCheck *check);
static int ensureSave(const SaveCheck *check, unsigned timeoutMs, char *date, size_t size);
static void warnIfDated(const SaveCheck *check, const char *date);

int commands_run(int argc, wchar_t **argv) {
	return commands_runWithProgress(argc, argv, 0);
}

int commands_runWithProgress(int argc, wchar_t **argv, uintptr_t progressWindow) {
	RunOptions options;
	if (!parseRunArguments(argc, argv, &options)) {
		fprintf(
			stderr,
			"usage: fm24ctl run <input.bin|.json> [seeds] [output.csv] [--threads N]"
			" [--profile <samples.txt>] [--detailed] [--human-control] [--keep-build]" SAVE_FLAGS_USAGE
			"\n"
		);
		return EXIT_USAGE;
	}
	Path input;
	Path output;
	Path profile = {0};
	char seeds[SEED_PATTERN_LENGTH + 1];
	char date[SAVES_DATE_LENGTH];
	uint32_t teams[2] = {0};
	bool encoded = false;
	if (!absolutePath(options.input, &input) ||
			(options.output && !outputPath(options.output, &output, true)) ||
			(options.profile && !outputPath(options.profile, &profile, false)) ||
			!paths_toUtf8(options.seeds, seeds, sizeof seeds)) {
		return EXIT_FAILED;
	}
	// The default results go beside the input, once it has been read: a missing input says so first.
	Path given = input;
	if (!prepareInput(&input, teams, &encoded, &options.check)) {
		return EXIT_FAILED;
	}
	if ((!options.output && !defaultOutput(&given, &output)) ||
			(options.detailed && !checkDetailedPaths(&output))) {
		if (encoded) {
			removeJobInput(&input);
		}
		return EXIT_FAILED;
	}
	int ready = ensureSave(&options.check, CLIENT_LONG_TIMEOUT_MS, date, sizeof date);
	if (ready != EXIT_OK) {
		if (encoded) {
			removeJobInput(&input);
		}
		return ready;
	}
	warnIfDated(&options.check, date);
	printf("results: %s\n", output.utf8);
	char job[JOB_LENGTH];
	(void)snprintf(
		job,
		sizeof job,
		"command=run\ninput=%s\nseeds=%s\noutput=%s\nthreads=%lu\nprofile=%s\ndetailed=%d\n"
		"human_control=%d\nkeep_build=%d\nhome_team=%.0u\naway_team=%.0u\nprogress_window=%llu\n",
		input.utf8,
		seeds,
		output.utf8,
		options.threads,
		profile.utf8,
		options.detailed,
		options.humanControl,
		options.keepBuild,
		teams[0],
		teams[1],
		(unsigned long long)progressWindow
	);
	printf("job submitted; waiting...\n");
	char result[CLIENT_RESULT_LENGTH];
	bool submitted = client_submit(job, CLIENT_LONG_TIMEOUT_MS, result, sizeof result);
	if (encoded) {
		removeJobInput(&input);
	}
	if (!submitted) {
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	if (!client_succeeded(result)) {
		return EXIT_FAILED;
	}
	if (options.detailed) {
		printDetailedPaths(&output);
	}
	return printSummary(&output);
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
	int ready = ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
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
	if (!encodeToFile(&input, &bin, &size, teams, &check)) {
		removeJobInput(&bin);
		return EXIT_FAILED;
	}
	int ready = ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have the fixture's save loaded and idle.");
		removeJobInput(&bin);
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
		return EXIT_FAILED;
	}
	printf("%s\n", result);
	return client_succeeded(result) ? EXIT_OK : EXIT_FAILED;
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
	int ready = ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
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
	int ready = ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
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
	int ready = ensureSave(&check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
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

int commands_encode(int argc, wchar_t **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: fm24ctl encode <input.json> <output.bin>\n");
		return EXIT_USAGE;
	}
	Path input;
	Path output;
	size_t size = 0;
	if (!absolutePath(argv[2], &input) || !absolutePath(argv[3], &output) ||
			!encodeToFile(&input, &output, &size, NULL, NULL)) {
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
// which it reads back, and inject-player's output, which it writes). Otherwise, as a folder that
// could not be checked, it is left to the game.
static bool outputPath(const wchar_t *argument, Path *out, bool opened) {
	if (!absolutePath(argument, out)) {
		return false;
	}
	const char *problem = NULL;
	switch (paths_parentFolder(out->wide)) {
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

// Encodes a JSON config into match bytes, and reads the save check its header asks for into
// `check`, if given.
static bool encodeConfig(const Path *json, ByteBuffer *bytes, uint32_t teams[2], SaveCheck *check) {
	ByteBuffer text = {0};
	char error[256] = "";
	bool ok = files_read(json->wide, &text);
	if (!ok) {
		fprintf(stderr, "cannot read %s\n", json->utf8);
	} else if (!(ok = matchConfig_encode(
								 buffer_string(&text), text.length, bytes, teams, error, sizeof error
							 ))) {
		fprintf(stderr, "%s: %s\n", json->utf8, error);
	} else if (check) {
		readFixtureSave(&text, check);
	}
	buffer_free(&text);
	return ok;
}

static bool encodeToFile(
	const Path *json,
	const Path *output,
	size_t *size,
	uint32_t teams[2],
	SaveCheck *check
) {
	ByteBuffer bytes = {0};
	bool ok = encodeConfig(json, &bytes, teams, check);
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

// Turns a .json config into the .bin the DLL plays, and reads its team overrides and the save it
// asks for; a .bin is played as it is, once fm24ctl has seen that it is there.
static bool prepareInput(Path *input, uint32_t teams[2], bool *encoded, SaveCheck *check) {
	teams[0] = 0;
	teams[1] = 0;
	*encoded = false;
	if (!files_hasExtension(input->wide, L".json")) {
		return binaryFound(input);
	}
	Path bin;
	size_t size = 0;
	if (!jobInputPath(&bin, L".bin")) {
		return false;
	}
	if (!encodeToFile(input, &bin, &size, teams, check)) {
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

// The input path with its extension replaced by .csv ("next.json" -> "next.csv"), checked as an
// explicit output is.
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
	return outputPath(path, output, true);
}

// The files the DLL writes for --detailed, checked as outputs the game writes: beside an output
// that already exists, they may be new files (a path too long for them is left to the DLL).
static bool checkDetailedPaths(const Path *output) {
	static const wchar_t *const suffixes[] = {L"-players.csv", L"-teams.csv"};
	size_t length = wcslen(output->wide);
	size_t stem =
		length > 4 && platform_compareWideIgnoringCase(output->wide + length - 4, L".csv") == 0
			? length - 4
			: length;
	for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; ++i) {
		wchar_t path[HARNESS_PATH_LENGTH];
		Path report;
		int written =
			swprintf(path, HARNESS_PATH_LENGTH, L"%.*ls%ls", (int)stem, output->wide, suffixes[i]);
		if (written > 0 && (size_t)written < HARNESS_PATH_LENGTH && !outputPath(path, &report, false)) {
			return false;
		}
	}
	return true;
}

// The files the DLL writes for --detailed: the output path without ".csv", plus "-players.csv"
// and "-teams.csv" (see match_stats.h).
static void printDetailedPaths(const Path *output) {
	size_t length = strlen(output->utf8);
	size_t stem = length > 4 && platform_compareIgnoringCase(output->utf8 + length - 4, ".csv") == 0
									? length - 4
									: length;
	printf("player statistics: %.*s-players.csv\n", (int)stem, output->utf8);
	printf("team statistics: %.*s-teams.csv\n", (int)stem, output->utf8);
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
	readFixtureSave(config, check);
	int ready = ensureSave(check, CLIENT_SHORT_TIMEOUT_MS, NULL, 0);
	if (ready != EXIT_OK) {
		(void)snprintf(result, resultSize, "FM24 does not have the fixture's save loaded and idle.");
		return ready;
	}
	return fetchPlayer(person, player, result, resultSize);
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
	if (paths_toUtf8(text, out, size) && isSaveName(out)) {
		return true;
	}
	out[0] = 0;
	fprintf(stderr, "--save takes a save's name, its file name without .fm: %ls\n", text);
	return false;
}

// True if `name` can be a save's name (saves_isName) and has no control characters.
static bool isSaveName(const char *name) {
	for (const char *c = name; *c; ++c) {
		if ((unsigned char)*c < 0x20 || *c == 0x7F) {
			return false;
		}
	}
	return saves_isName(name);
}

// The save check a fixture's "save" and "game_date" ask for. Only the save's last path part counts,
// as in the harness's status (saves_isSave).
static void readFixtureSave(const ByteBuffer *config, SaveCheck *check) {
	ConfigOrigin origin;
	(void)matchConfig_readOrigin(buffer_string(config), config->length, &origin);
	(void)snprintf(check->date, sizeof check->date, "%s", origin.gameDate);
	check->badSave =
		origin.hasSave &&
		!(saves_nameOfPath(origin.save, check->save, sizeof check->save) && isSaveName(check->save));
	if (!origin.hasSave || check->badSave) {
		check->save[0] = 0;
	}
}

// Before a job for `check`'s save: makes sure the game has it loaded and idle (see
// macosGame_ensureSave), letting each status query wait for another runner's job as long as the job
// itself may (`timeoutMs`), and writes the save's in-game date to `date` if given ("" if unknown).
// 0 to submit the job, else the exit code. Without a save, and on Windows, whose harness has no
// status job yet, the job goes ahead as before.
static int ensureSave(const SaveCheck *check, unsigned timeoutMs, char *date, size_t size) {
	if (date && size) {
		date[0] = 0;
	}
#ifdef _WIN32
	(void)check;
	(void)timeoutMs;
	return EXIT_OK;
#else
	if (check->badSave) {
		fprintf(stderr, "the input's \"save\" is not a save's name; remove it to use any save\n");
		return EXIT_FAILED;
	}
	if (!check->save[0]) {
		return EXIT_OK;
	}
	SessionNeed need = {check->save, !check->noLaunch, check->anySave, timeoutMs};
	return macosGame_ensureSave(&need, date, size);
#endif
}

// Warns if the loaded save is at another in-game date (`date`) than the one the fixture was
// exported at.
static void warnIfDated(const SaveCheck *check, const char *date) {
	if (check->date[0] && date[0] && strcmp(check->date, date) != 0) {
		fprintf(
			stderr,
			"warning: the save is at %s, the fixture was exported at %s; the results reflect the save\n",
			date,
			check->date
		);
	}
}
