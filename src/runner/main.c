// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// fmNNctl: drives the harness DLL inside the running fm.exe, injecting it when needed.
//
//   fmNNctl
//       exports the next fixture to the OS temp directory and opens the viewer (on macOS,
//       FMNNViewer.app)
//   fmNNctl <fixture.bin|.json>
//       opens the fixture in the viewer
//   fmNNctl run <input.bin|.json> [seeds] [output.csv] [--threads N]
//       [--profile <samples.txt>] [--detailed] [--human-control] [--keep-build]
//       [--no-launch] [--any-save] (macOS)
//       seeds: "keep" (the input's own seed), "42", "1-1000", or "1,5,10-20"
//       (default "keep"); output defaults to the input with a .csv extension
//   fmNNctl get-next-fixture <output.json|.bin> [--seed N] [--full]
//       [--save <name>] [--no-launch] [--any-save] (macOS)
//   fmNNctl encode <input.json> <output.bin>
//   fmNNctl decode <input.bin> <output.json> [--full]
//       the game's decoding of a binary fixture, without building or playing a match
//   fmNNctl replace-away <input.json> <team ID or club name> <output.json> [--full]
//       [--no-launch] [--any-save] (macOS)
//   fmNNctl replace-home <input.json> <team ID or club name> <output.json> [--full]
//       [--no-launch] [--any-save] (macOS)
//       (a <team unique ID> only, where the clubs' names have not been located)
//       the input rebuilt against another away team, keeping its other edits
//   fmNNctl list-teams <output.txt> [--save <name>] [--no-launch] [--any-save] (macOS)
//   fmNNctl list-players <output.txt> [--save <name>] [--no-launch] [--any-save] (macOS)
//   fmNNctl get-player <person unique ID> <output.json>
//       [--save <name>] [--no-launch] [--any-save] (macOS)
//       a player's database values, as a config's player block has them
//   fmNNctl inject-player <input.json> <home|away>.player[N] <person unique ID|player.json>
//       <output.json> [--allow-duplicate] [--no-launch] [--any-save] (macOS)
//       the input with that player block's database values replaced by another player's
//   fmNNctl status
//       what the game is doing; succeeds only if it has a game loaded and idle (macOS; exit code 3
//       if it has not)
//   fmNNctl probe [path to fm.exe]
//       where another build of fm.exe has the code the harness uses (writes probe.txt); on
//       macOS, which builds of the game the harness supports
//   fmNNctl install [--log-only] (macOS)
//       installs the harness for the game from Steam and prints the launch option; with
//       --log-only (where the game loads its engine as a plugin), a harness that only logs how
//       the game loads and unloads the engine and serves no jobs
//   fmNNctl launch [--save <name|path.fm>] [--timeout <s>] [--user-data <dir>] [--rosetta]
//       [path to fm.app]
//       (macOS) starts the game with the harness loaded; with --save, has the game
//       load that save and waits until it is loaded and idle (exit code 3 if the running game has
//       another save loaded); with --rosetta, its x86_64 slice under Rosetta on Apple silicon (exit
//       code 3 if the running game runs natively)
//
// On macOS, run, replace-home/away, inject-player with an ID and, given --save, get-next-fixture,
// list-teams, list-players and get-player first make sure the game has the save loaded and idle
// (the input's "save", or --save's): they wait while it is busy, start the game with the save if
// it is not running (not with --no-launch) and stop with exit code 3 if another save is loaded
// (unless --any-save). Without a save, with a .bin input and on Windows, jobs go ahead as before;
// but where the edition's exports record no save, an input without one is for the save the running
// game has loaded: they wait until it is idle, and stop if no game is running.
//
// fmNNctl.exe, the DLL, the job file and the result file all live in the same directory. On macOS
// the harness lives in ~/Library/Application Support/fmNN-harness (see embedded.h) and job files
// in the user's temporary directory.

#include "commands.h"
#include "edition.h"
#include "files.h"
#include "protocol.h"

#include <stdio.h>
#include <wchar.h>

#ifdef _WIN32
#include "fixture_window.h"

#include <windows.h>
#else
#include "macos_game.h"
#include "macos_viewer.h"
#include "paths.h"
#include "platform.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#endif


// The usage lines of the options that check the game's save before a job (macOS; see commands.c).
#ifdef _WIN32
#define SAVE_FLAGS_LINE ""
#define SAVE_OPTIONS_LINE ""
#else
#define SAVE_FLAGS_LINE "      [--no-launch] [--any-save]\n"
#define SAVE_OPTIONS_LINE "      [--save <name>] [--no-launch] [--any-save]\n"
#endif

// clang-format off: one usage line per source line, with the platform's option lines after them
static const char usage[] =
	"usage:\n"
	"  " EDITION_CLI "  (export and view the next fixture)\n"
	"  " EDITION_CLI " <fixture.bin|.json>  (open the native fixture viewer)\n"
	"  " EDITION_CLI " run <input.bin|.json> [seeds] [output.csv] [--threads N]\n"
	"      [--profile <samples.txt>] [--detailed] [--human-control] [--keep-build]\n"
	SAVE_FLAGS_LINE
	"  " EDITION_CLI " get-next-fixture <output.json|.bin> [--seed N] [--full]\n"
	SAVE_OPTIONS_LINE
	"  " EDITION_CLI " encode <input.json> <output.bin>\n"
	"  " EDITION_CLI " decode <input.bin> <output.json> [--full]\n"
	"  " EDITION_CLI " replace-away <input.json> " COMMANDS_TEAM_ARGUMENT " <output.json> [--full]\n"
	SAVE_FLAGS_LINE
	"  " EDITION_CLI " replace-home <input.json> " COMMANDS_TEAM_ARGUMENT " <output.json> [--full]\n"
	SAVE_FLAGS_LINE
	"  " EDITION_CLI " list-teams <output.txt>\n"
	SAVE_OPTIONS_LINE
	"  " EDITION_CLI " list-players <output.txt>\n"
	SAVE_OPTIONS_LINE
	"  " EDITION_CLI " get-player <person unique ID> <output.json>\n"
	SAVE_OPTIONS_LINE
	"  " EDITION_CLI " inject-player <input.json> <home|away>.player[N]\n"
	"      <person unique ID|player.json> <output.json> [--allow-duplicate]\n"
	SAVE_FLAGS_LINE
#ifdef _WIN32
	"  " EDITION_CLI " probe [path to fm.exe]\n";
#else
	"  " EDITION_CLI " status  (whether " EDITION_PRODUCT " has a game loaded and idle)\n"
	"  " EDITION_CLI " probe [path to fm.app]\n"
#if EDITION_ENGINE_PLUGIN
	"  " EDITION_CLI " install [--log-only]  (set up the harness for " EDITION_GAME " from Steam;\n"
	"      --log-only has it only log how the game loads and unloads its engine)\n"
#else
	"  " EDITION_CLI " install  (set up the harness for " EDITION_GAME " from Steam)\n"
#endif
	"  " EDITION_CLI " launch [--save <name|path.fm>] [--timeout <s>] [--user-data <dir>]\n"
	"      [--rosetta] [path to fm.app]  (start the game with the harness; --save loads\n"
	"      a save, --rosetta runs its x86_64 slice under Rosetta on Apple silicon)\n"
	"Commands with these options first make sure " EDITION_PRODUCT " has the input's save (or --save's)\n"
	"loaded and idle, starting the game with it if needed (not with --no-launch);\n"
	"--any-save lets them use another loaded save.\n"
#if EDITION_ENGINE_PLUGIN
	"An input without a save, as " EDITION_PRODUCT "'s exports are, is for the save " EDITION_PRODUCT " has loaded:\n"
	"they wait until it is idle, and never start the game.\n"
#endif
	;
#endif
// clang-format on

static int runCommand(int argc, wchar_t **argv);
#ifndef _WIN32
static int exportAndOpen(wchar_t *program);
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t **argv);

int wmain(int argc, wchar_t **argv) {
	if (argc == 1) {
		wchar_t path[HARNESS_PATH_LENGTH];
		static const wchar_t filename[] = L"" EDITION_TEMPORARY_FIXTURE;
		size_t filenameLength = sizeof filename / sizeof filename[0];
		DWORD length = GetTempPathW(HARNESS_PATH_LENGTH, path);
		if (!length) {
			fprintf(stderr, "cannot resolve temporary directory (Windows error %lu)\n", GetLastError());
			return 1;
		}
		if (length > HARNESS_PATH_LENGTH - filenameLength) {
			fputs("temporary fixture path is too long\n", stderr);
			return 1;
		}
		wmemcpy(path + length, filename, filenameLength);
		wchar_t *exportArgs[] = {argv[0], L"get-next-fixture", path, NULL};
		int result = commands_getNextFixture(3, exportArgs);
		return result ? result : fixtureWindow_open(path, true);
	}
	const wchar_t *command = argc >= 2 ? argv[1] : L"";
	if (argc == 2 &&
			(files_hasExtension(command, L".bin") || files_hasExtension(command, L".json"))) {
		return fixtureWindow_open(command, false);
	}
	return runCommand(argc, argv);
}
#else
// macOS hands over UTF-8 arguments; the commands take them as wide (UTF-32) strings, like Windows.
int main(int argc, char **argv) {
	// A line at a time, even into a pipe: the viewer shows fmNNctl's progress as it is printed, and
	// a full buffer would hold it back until fmNNctl exits (or lose it if fmNNctl is ended).
	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	// Wide strings print through the C library's locale; make sure it can encode any path. There
	// are no other threads yet.
	if (!setlocale(LC_CTYPE, "") || MB_CUR_MAX == 1) { // NOLINT(concurrency-mt-unsafe)
		(void)setlocale(LC_CTYPE, "UTF-8");              // NOLINT(concurrency-mt-unsafe)
	}
	wchar_t **wide = calloc((size_t)argc + 1, sizeof *wide);
	if (!wide) {
		fputs("out of memory\n", stderr);
		return 1;
	}
	int status = 1;
	int converted = 0;
	for (; converted < argc; ++converted) {
		size_t length = strlen(argv[converted]) + 1;
		wide[converted] = malloc(length * sizeof **wide);
		if (!wide[converted] || !platform_fromUtf8(argv[converted], wide[converted], length)) {
			fprintf(stderr, "argument %d is not valid UTF-8 text\n", converted);
			break;
		}
	}
	// As wmain does, with the viewer in FMNNViewer.app: fmNNctl returns once it has handed the
	// fixture over.
	if (converted == argc && argc == 1) {
		status = exportAndOpen(wide[0]);
	} else if (converted == argc && argc == 2 &&
						 (files_hasExtension(wide[1], L".bin") || files_hasExtension(wide[1], L".json"))) {
		status = macosViewer_open(wide[1]);
	} else if (converted == argc) {
		status = runCommand(argc, wide);
	}
	for (int i = 0; i < argc; ++i) {
		free(wide[i]);
	}
	free(wide);
	return status;
}

// Exports the next fixture to the edition's EDITION_TEMPORARY_FIXTURE in the user's temporary
// directory, which the game can always write to, and opens it in the viewer if that worked.
static int exportAndOpen(wchar_t *program) {
	wchar_t directory[HARNESS_PATH_LENGTH];
	wchar_t path[HARNESS_PATH_LENGTH];
	if (!paths_jobDirectory(directory, HARNESS_PATH_LENGTH) ||
			!paths_join(path, HARNESS_PATH_LENGTH, directory, MACOS_VIEWER_TEMPORARY_FIXTURE)) {
		fputs("cannot resolve the temporary fixture path\n", stderr);
		return 1;
	}
	wchar_t *exportArgs[] = {program, L"get-next-fixture", path, NULL};
	int result = commands_getNextFixture(3, exportArgs);
	return result ? result : macosViewer_open(path);
}
#endif

static int runCommand(int argc, wchar_t **argv) {
	const wchar_t *command = argc >= 2 ? argv[1] : L"";
	static const struct {
		const wchar_t *name;
		int (*run)(int argc, wchar_t **argv);
	} commands[] = {
		{L"run", commands_run},
		{L"get-next-fixture", commands_getNextFixture},
		{L"encode", commands_encode},
		{L"decode", commands_decode},
		{L"replace-away", commands_replaceTeam},
		{L"replace-home", commands_replaceTeam},
		{L"list-teams", commands_listTeams},
		{L"list-players", commands_listPlayers},
		{L"get-player", commands_getPlayer},
		{L"inject-player", commands_injectPlayer},
		{L"status", commands_status},
		{L"probe", commands_probe},
#ifndef _WIN32
		{L"install", macosGame_install},
		{L"launch", macosGame_launch},
#endif
	};
	for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
		if (wcscmp(command, commands[i].name) == 0) {
			return commands[i].run(argc, argv);
		}
	}
	fputs(usage, stderr);
	return 2;
}
