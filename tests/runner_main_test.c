// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "test.h"
#include "tests.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#endif


#ifdef _WIN32
static DWORD tempPath(DWORD size, wchar_t *out);

#define GetTempPathW tempPath
#define wmain runnerMain
#define fixtureWindow_open runnerTest_open
#define files_hasExtension runnerTest_hasExtension
#else
int runnerMain(int argc, char **argv);
bool runnerTest_executableDirectory(wchar_t *out, size_t size);
bool runnerTest_jobDirectory(wchar_t *out, size_t size);
int runnerTest_spawn(
	pid_t *pid,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attributes,
	char *const argv[],
	char *const environment[]
);
pid_t runnerTest_wait(pid_t pid, int *status, int options);

// macos_test.c has the real install and launch. The viewer is "opened" by stubs of the process
// calls, in place of /usr/bin/open, from directories the tests choose.
#define main runnerMain
#define macosGame_install runnerTest_install
#define macosGame_launch runnerTest_launch
#define paths_executableDirectory runnerTest_executableDirectory
#define paths_jobDirectory runnerTest_jobDirectory
#define posix_spawn runnerTest_spawn
#define waitpid runnerTest_wait
#endif
#include "../src/runner/main.c" // NOLINT(bugprone-suspicious-include): test startup with stubs.
#ifndef _WIN32
#include "../src/runner/macos_viewer.c" // NOLINT(bugprone-suspicious-include): stubbed processes.
#endif


#undef main
#undef wmain
#undef GetTempPathW

#ifdef _WIN32
static wchar_t directory[HARNESS_PATH_LENGTH + 1] = L"C:\\Temp\\";
static DWORD directoryLength = 8;
static int exportResult;
static unsigned exports;
static unsigned opens;
static bool expectTemporary = true;
static wchar_t exportedPath[HARNESS_PATH_LENGTH];

static DWORD tempPath(DWORD size, wchar_t *out) {
	ASSERT_BOOL_EQ(size == HARNESS_PATH_LENGTH, true);
	if (directoryLength && directoryLength < size) {
		wmemcpy(out, directory, directoryLength + 1);
	}
	return directoryLength;
}

int commands_getNextFixture(int argc, wchar_t **argv) {
	++exports;
	ASSERT_INT_EQ(argc, 3);
	ASSERT_INT_EQ(wcscmp(argv[0], L"fm24ctl.exe"), 0);
	ASSERT_INT_EQ(wcscmp(argv[1], L"get-next-fixture"), 0);
	ASSERT_BOOL_EQ(argv[3] == NULL, true);
	ASSERT_BOOL_EQ(wcslen(argv[2]) == directoryLength + wcslen(L"temp-fixture.json"), true);
	ASSERT_INT_EQ(wcsncmp(argv[2], directory, directoryLength), 0);
	ASSERT_INT_EQ(wcscmp(argv[2] + directoryLength, L"temp-fixture.json"), 0);
	wcscpy(exportedPath, argv[2]);
	return exportResult;
}

int fixtureWindow_open(const wchar_t *path, bool temporary) {
	++opens;
	ASSERT_INT_EQ(wcscmp(path, exportedPath), 0);
	ASSERT_BOOL_EQ(temporary, expectTemporary);
	return 9;
}

bool files_hasExtension(const wchar_t *path, const wchar_t *suffix) {
	size_t length = wcslen(path);
	size_t suffixLength = wcslen(suffix);
	return length >= suffixLength && wcscmp(path + length - suffixLength, suffix) == 0;
}
#else
static wchar_t launched[3][128]; // the arguments `launch` was given
static int launchedCount;
static char viewerDirectory[PATH_MAX];    // where fm24ctl seems to be
static char temporaryDirectory[PATH_MAX]; // the user's temporary directory, as it seems
static int exportStatus = 8;              // get-next-fixture's exit status
static unsigned exports;
static char exported[PATH_MAX]; // the file get-next-fixture was given
static unsigned opens;
static int openStatus;           // /usr/bin/open's exit status
static char opened[4][PATH_MAX]; // and its arguments
static int openedCount;

int commands_getNextFixture(int argc, wchar_t **argv) {
	++exports;
	exported[0] = 0;
	if (argc == 3) {
		ASSERT_INT_EQ(wcscmp(argv[1], L"get-next-fixture"), 0);
		ASSERT_BOOL_EQ(argv[3] == NULL, true);
		ASSERT_BOOL_EQ(platform_toUtf8(argv[2], exported, sizeof exported), true);
		FILE *file = exportStatus == 0 ? fopen(exported, "wb") : NULL;
		if (file) {
			fclose(file);
		}
	}
	return exportStatus;
}

bool runnerTest_executableDirectory(wchar_t *out, size_t size) {
	return platform_fromUtf8(viewerDirectory, out, size);
}

bool runnerTest_jobDirectory(wchar_t *out, size_t size) {
	return platform_fromUtf8(temporaryDirectory, out, size);
}

int runnerTest_spawn(
	pid_t *pid,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attributes,
	char *const argv[],
	char *const environment[]
) {
	++opens;
	ASSERT_INT_EQ(strcmp(path, "/usr/bin/open"), 0);
	ASSERT_BOOL_EQ(actions == NULL && attributes == NULL && environment == environ, true);
	openedCount = 0;
	while (argv[openedCount] && openedCount < 4) {
		(void)snprintf(opened[openedCount], sizeof opened[0], "%s", argv[openedCount]);
		++openedCount;
	}
	ASSERT_BOOL_EQ(argv[openedCount] == NULL, true);
	*pid = 4242;
	return 0;
}

pid_t runnerTest_wait(pid_t pid, int *status, int options) {
	ASSERT_INT_EQ(pid, 4242);
	ASSERT_INT_EQ(options, 0);
	*status = (openStatus & 0xff) << 8;
	return pid;
}

int runnerTest_install(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 18;
}

static int printedPipe = -1; // while set, the reading end of the pipe stdout goes into
static bool lineArrived;     // the line launch printed was in it before launch returned

int runnerTest_launch(int argc, wchar_t **argv) {
	launchedCount = argc;
	for (int i = 0; i < argc && i < 3; ++i) {
		wcsncpy(launched[i], argv[i], 127);
	}
	ASSERT_BOOL_EQ(argv[argc] == NULL, true);
	if (printedPipe >= 0) {
		static const char line[] = "waiting for the game to start the harness...\n";
		char got[sizeof line] = "";
		printf("%s", line);
		// The pipe does not block: what has not been written yet is not there.
		lineArrived =
			read(printedPipe, got, sizeof got - 1) == (ssize_t)sizeof line - 1 && strcmp(got, line) == 0;
	}
	return 19;
}
#endif

int commands_run(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 10;
}

int commands_encode(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 11;
}

int commands_decode(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 17;
}

int commands_replaceTeam(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 0;
}

int commands_listTeams(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 13;
}

int commands_listPlayers(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 16;
}

int commands_getPlayer(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 14;
}

int commands_injectPlayer(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 15;
}

int commands_probe(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 12;
}

int commands_status(int argc, wchar_t **argv) {
	(void)argc;
	(void)argv;
	return 20;
}

#ifdef _WIN32
TEST(exports_next_fixture_and_dispatches_commands) {
	wchar_t *arguments[] = {L"fm24ctl.exe", NULL, NULL};
	ASSERT_INT_EQ(runnerMain(1, arguments), 9);
	ASSERT_BOOL_EQ(exports == 1 && opens == 1, true);
	ASSERT_INT_EQ(wcscmp(exportedPath, L"C:\\Temp\\temp-fixture.json"), 0);

	exportResult = 7;
	ASSERT_INT_EQ(runnerMain(1, arguments), 7);
	ASSERT_BOOL_EQ(exports == 2 && opens == 1, true);

	directoryLength = 0;
	ASSERT_INT_EQ(runnerMain(1, arguments), 1);
	directoryLength = HARNESS_PATH_LENGTH;
	ASSERT_INT_EQ(runnerMain(1, arguments), 1);
	directoryLength = HARNESS_PATH_LENGTH - (DWORD)wcslen(L"temp-fixture.json");
	ASSERT_INT_EQ(runnerMain(1, arguments), 1);
	ASSERT_BOOL_EQ(exports == 2 && opens == 1, true);

	--directoryLength;
	wmemset(directory, L'x', directoryLength);
	directory[directoryLength - 1] = L'\\';
	directory[directoryLength] = 0;
	exportResult = 0;
	ASSERT_INT_EQ(runnerMain(1, arguments), 9);
	ASSERT_BOOL_EQ(exports == 3 && opens == 2, true);

	arguments[1] = L"run";
	ASSERT_INT_EQ(runnerMain(2, arguments), 10);
	arguments[1] = L"encode";
	ASSERT_INT_EQ(runnerMain(2, arguments), 11);
	arguments[1] = L"decode";
	ASSERT_INT_EQ(runnerMain(2, arguments), 17);
	arguments[1] = L"probe";
	ASSERT_INT_EQ(runnerMain(2, arguments), 12);
	arguments[1] = L"replace-home";
	ASSERT_INT_EQ(runnerMain(2, arguments), 0);
	arguments[1] = L"replace-away";
	ASSERT_INT_EQ(runnerMain(2, arguments), 0);
	arguments[1] = L"list-teams";
	ASSERT_INT_EQ(runnerMain(2, arguments), 13);
	arguments[1] = L"list-players";
	ASSERT_INT_EQ(runnerMain(2, arguments), 16);
	arguments[1] = L"get-player";
	ASSERT_INT_EQ(runnerMain(2, arguments), 14);
	arguments[1] = L"inject-player";
	ASSERT_INT_EQ(runnerMain(2, arguments), 15);
	arguments[1] = L"status";
	ASSERT_INT_EQ(runnerMain(2, arguments), 20);
	arguments[1] = exportedPath;
	expectTemporary = false;
	ASSERT_INT_EQ(runnerMain(2, arguments), 9);
	ASSERT_BOOL_EQ(exports == 3 && opens == 3, true);
	arguments[1] = L"unknown";
	ASSERT_INT_EQ(runnerMain(2, arguments), 2);
}
#else
// runnerMain's exit code, with what it wrote to stderr in `out`.
static int runCapturingErrors(int argc, char **argv, char *out, size_t size) {
	char printed[] = "/tmp/fm24-main-out-XXXXXX";
	int file = mkstemp(printed);
	int saved = dup(STDERR_FILENO);
	int status = -1;
	out[0] = 0;
	if (file >= 0 && saved >= 0 && dup2(file, STDERR_FILENO) >= 0) {
		status = runnerMain(argc, argv);
		(void)dup2(saved, STDERR_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	if (file >= 0) {
		ssize_t length = pread(file, out, size - 1, 0);
		out[length > 0 ? length : 0] = 0;
		close(file);
		(void)unlink(printed);
	}
	return status;
}

TEST(converts_arguments_and_dispatches_commands) {
	char program[] = "fm24ctl";
	char launch[] = "launch";
	char app[] = "/Applications/\xC3\x98"
							 "degaard \xE2\x82\xAC/fm.app";
	char *arguments[] = {program, launch, app, NULL, NULL};
	char errors[4096];
	char *locale = strdup(setlocale(LC_CTYPE, NULL));
	// Wide text prints through the locale, which must encode any path even where the
	// environment's cannot.
	char *language = getenv("LC_ALL") ? strdup(getenv("LC_ALL")) : NULL;
	setenv("LC_ALL", "C", 1);
	(void)setlocale(LC_CTYPE, "C");
	ASSERT_INT_EQ(runnerMain(3, arguments), 19);
	ASSERT_BOOL_EQ(MB_CUR_MAX > 1, true);
	ASSERT_INT_EQ(launchedCount, 3);
	ASSERT_INT_EQ(wcscmp(launched[0], L"fm24ctl"), 0);
	ASSERT_INT_EQ(wcscmp(launched[2], L"/Applications/\u00D8degaard \u20AC/fm.app"), 0);
	if (language) {
		setenv("LC_ALL", language, 1);
		free(language);
	} else {
		unsetenv("LC_ALL");
	}
	// An argument that is not UTF-8 runs nothing.
	char invalid[] = "\xC0\xAF.app";
	arguments[2] = invalid;
	launchedCount = 0;
	ASSERT_INT_EQ(runCapturingErrors(3, arguments, errors, sizeof errors), 1);
	ASSERT_BOOL_EQ(strstr(errors, "argument 2 is not valid UTF-8 text") != NULL, true);
	ASSERT_INT_EQ(launchedCount, 0);
	static const struct {
		const char *name;
		int status;
	} commands[] = {
		{"run", 10},
		{"get-next-fixture", 8},
		{"encode", 11},
		{"decode", 17},
		{"replace-home", 0},
		{"replace-away", 0},
		{"list-teams", 13},
		{"list-players", 16},
		{"get-player", 14},
		{"inject-player", 15},
		{"probe", 12},
		{"status", 20},
		{"install", 18},
		{"launch", 19},
		{"unknown", 2},
	};
	for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
		char name[32];
		(void)snprintf(name, sizeof name, "%s", commands[i].name);
		arguments[1] = name;
		ASSERT_INT_EQ(runCapturingErrors(2, arguments, errors, sizeof errors), commands[i].status);
	}
	ASSERT_BOOL_EQ(strstr(errors, "usage:\n") == errors, true);
	ASSERT_BOOL_EQ(strstr(errors, "fm24ctl launch [--save <name|path.fm>]") != NULL, true);
	ASSERT_BOOL_EQ(strstr(errors, "fm24ctl status") != NULL, true);
	ASSERT_BOOL_EQ(strstr(errors, "  fm24ctl  (export and view the next fixture)\n") != NULL, true);
	// The save options under the commands that take them.
	ASSERT_BOOL_EQ(
		strstr(errors, "  fm24ctl list-teams <output.txt>\n      [--save <name>] [--no-launch] [--any-save]\n") !=
			NULL,
		true
	);
	ASSERT_BOOL_EQ(
		strstr(errors, "[--keep-build]\n      [--no-launch] [--any-save]\n  fm24ctl get-next-fixture") !=
			NULL,
		true
	);
	if (locale) {
		(void)setlocale(LC_CTYPE, locale);
		free(locale);
	}
}

// The viewer reads fm24ctl's standard output through a pipe, as it is printed: each line must
// reach the pipe as fm24ctl prints it, not when it exits.
TEST(prints_each_line_as_it_comes) {
	char program[] = "fm24ctl";
	char launch[] = "launch";
	char *arguments[] = {program, launch, NULL};
	int ends[2];
	ASSERT_INT_EQ(pipe(ends), 0);
	ASSERT_INT_EQ(fcntl(ends[0], F_SETFL, O_NONBLOCK), 0);
	(void)fflush(stdout);
	int saved = dup(STDOUT_FILENO);
	ASSERT_BOOL_EQ(saved >= 0, true);
	// Buffered as a pipe would have it, whatever the tests' own output goes to.
	ASSERT_INT_EQ(setvbuf(stdout, NULL, _IOFBF, BUFSIZ), 0);
	if (saved >= 0 && dup2(ends[1], STDOUT_FILENO) >= 0) {
		printedPipe = ends[0];
		lineArrived = false;
		ASSERT_INT_EQ(runnerMain(2, arguments), 19);
		printedPipe = -1;
		(void)fflush(stdout);
		(void)dup2(saved, STDOUT_FILENO);
	}
	ASSERT_BOOL_EQ(lineArrived, true);
	if (saved >= 0) {
		close(saved);
	}
	close(ends[0]);
	close(ends[1]);
}

// Makes an empty file, or (`executable`) one that can be run, with its folders.
static bool makeFile(const char *path, bool executable) {
	char folder[PATH_MAX];
	(void)snprintf(folder, sizeof folder, "%s", path);
	for (char *slash = strchr(folder + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
		*slash = 0;
		if (mkdir(folder, 0700) != 0 && errno != EEXIST) {
			return false;
		}
		*slash = '/';
	}
	FILE *file = fopen(path, "wb");
	return file && fclose(file) == 0 && (!executable || chmod(path, 0700) == 0);
}

// runnerMain with one argument (or none), as fm24ctl in `directory`.
static int runViewer(const char *directory, const char *argument, char *errors, size_t size) {
	char program[] = "fm24ctl";
	char given[PATH_MAX];
	(void)snprintf(given, sizeof given, "%s", argument ? argument : "");
	char *arguments[] = {program, argument ? given : NULL, NULL};
	(void)snprintf(viewerDirectory, sizeof viewerDirectory, "%s", directory);
	return runCapturingErrors(argument ? 2 : 1, arguments, errors, size);
}

// The arguments of the last /usr/bin/open: whether it opened `fixture` with -a `app` (or -b and
// the viewer's bundle identifier if `app` is NULL).
static bool openedWith(const char *app, const char *fixture) {
	return openedCount == 4 && strcmp(opened[0], "/usr/bin/open") == 0 &&
				 strcmp(opened[1], app ? "-a" : "-b") == 0 &&
				 strcmp(opened[2], app ? app : MACOS_VIEWER_IDENTIFIER) == 0 &&
				 strcmp(opened[3], fixture) == 0;
}

TEST(opens_fixtures_in_the_viewer) {
	char made[] = "/tmp/fm24-main-viewer-XXXXXX";
	char root[PATH_MAX];
	char errors[4096];
	char path[PATH_MAX * 2];
	if (!mkdtemp(made) || !realpath(made, root)) {
		ASSERT_BOOL_EQ(false, true);
		return;
	}
	char bin[PATH_MAX];
	char app[PATH_MAX];
	char fixture[PATH_MAX];
	(void)snprintf(bin, sizeof bin, "%s/bin", root);
	(void)snprintf(app, sizeof app, "%s/FM24Viewer.app", bin);
	(void)snprintf(fixture, sizeof fixture, "%s/next.json", root);
	(void)snprintf(temporaryDirectory, sizeof temporaryDirectory, "%s/T", root);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/fm24viewer", app);
	bool ready = makeFile(path, true) && makeFile(fixture, false);
	(void)snprintf(path, sizeof path, "%s/Next.BIN", root);
	ready = ready && makeFile(path, false) && mkdir(temporaryDirectory, 0700) == 0;
	ASSERT_BOOL_EQ(ready, true);

	// No arguments: export the next fixture to the temporary directory, then open it.
	exports = 0;
	opens = 0;
	exportStatus = 0;
	ASSERT_INT_EQ(runViewer(bin, NULL, errors, sizeof errors), 0);
	(void)snprintf(path, sizeof path, "%s/temp-fixture.json", temporaryDirectory);
	ASSERT_BOOL_EQ(exports == 1 && strcmp(exported, path) == 0 && opens == 1, true);
	ASSERT_BOOL_EQ(openedWith(app, path), true);
	// A failed export opens nothing and keeps its exit status.
	exportStatus = 7;
	ASSERT_INT_EQ(runViewer(bin, NULL, errors, sizeof errors), 7);
	ASSERT_BOOL_EQ(exports == 2 && opens == 1, true);
	exportStatus = 8;

	// A fixture, with any case of extension, by absolute or relative path.
	ASSERT_INT_EQ(runViewer(bin, fixture, errors, sizeof errors), 0);
	ASSERT_BOOL_EQ(opens == 2 && openedWith(app, fixture), true);
	int cwd = open(".", O_RDONLY);
	if (cwd >= 0 && chdir(root) == 0) {
		ASSERT_INT_EQ(runViewer(bin, "./bin/../Next.BIN", errors, sizeof errors), 0);
		(void)snprintf(path, sizeof path, "%s/Next.BIN", root);
		ASSERT_BOOL_EQ(opens == 3 && openedWith(app, path), true);
		ASSERT_INT_EQ(fchdir(cwd), 0);
	} else {
		ASSERT_BOOL_EQ(false, true);
	}
	if (cwd >= 0) {
		close(cwd);
	}
	// A fixture that is not there opens nothing.
	(void)snprintf(path, sizeof path, "%s/missing.json", root);
	ASSERT_INT_EQ(runViewer(bin, path, errors, sizeof errors), 1);
	ASSERT_BOOL_EQ(opens == 3 && strstr(errors, "cannot open ") == errors, true);

	// The fm24ctl inside an app bundle opens that app, whatever its name.
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS", app);
	ASSERT_INT_EQ(runViewer(path, fixture, errors, sizeof errors), 0);
	ASSERT_BOOL_EQ(opens == 4 && openedWith(app, fixture), true);
	char renamed[PATH_MAX];
	ASSERT_BOOL_EQ(
		macosViewer_find("/Applications/FM24 Viewer.app/Contents/MacOS", renamed, sizeof renamed), true
	);
	ASSERT_INT_EQ(strcmp(renamed, "/Applications/FM24 Viewer.app"), 0);
	ASSERT_BOOL_EQ(
		macosViewer_find("/Applications/FM24 Viewer.app/Contents/MacOS", renamed, 8), false
	);
	ASSERT_BOOL_EQ(macosViewer_find(root, renamed, sizeof renamed), false);

	// Without an app beside it, the one macOS knows by its identifier; failing that, how to build it.
	ASSERT_INT_EQ(runViewer(root, fixture, errors, sizeof errors), 0);
	ASSERT_BOOL_EQ(opens == 5 && openedWith(NULL, fixture), true);
	openStatus = 1;
	ASSERT_INT_EQ(runViewer(root, fixture, errors, sizeof errors), 2);
	ASSERT_BOOL_EQ(opens == 6 && strstr(errors, "build it with scripts/build.sh") != NULL, true);
	// An app that cannot open the fixture.
	ASSERT_INT_EQ(runViewer(bin, fixture, errors, sizeof errors), 1);
	(void)snprintf(path, sizeof path, "cannot open %s in %s\n", fixture, app);
	ASSERT_BOOL_EQ(opens == 7 && strcmp(errors, path) == 0, true);
	openStatus = 0;

	(void)snprintf(path, sizeof path, "/bin/rm -rf '%s'", root);
	ASSERT_INT_EQ(system(path), 0);
}
#endif

void registerRunnerMainTests(void) {
#ifdef _WIN32
	test_run(
		"runner exports the next fixture and dispatches commands",
		test_exports_next_fixture_and_dispatches_commands
	);
#else
	test_run(
		"runner converts arguments and dispatches commands",
		test_converts_arguments_and_dispatches_commands
	);
	test_run("runner prints each line as it comes", test_prints_each_line_as_it_comes);
	test_run("runner opens fixtures in the macOS viewer", test_opens_fixtures_in_the_viewer);
#endif
}
