// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// A stand-in for fm24ctl in the macOS viewer's process tests (tests/viewer): it never contacts the
// game. Its standard output is line-buffered, as fm24ctl's main makes its own (the
// viewer_stand_in_texts test checks both), so the viewer gets each line as it is printed. It
// appends its arguments to the file FM24_STAND_IN_LOG names (tab-separated, one line per run), and
// the environment chooses what it does:
//   FM24_STAND_IN_STATUS   the exit status (0 by default); a failing run writes no file
//   FM24_STAND_IN_STDERR   text written to stderr first, then FM24_STAND_IN_DELAY_MS of sleep
//   FM24_STAND_IN_STDOUT   text written to stdout at the end, in place of "ok: stand-in"
//   FM24_STAND_IN_FIXTURE  a file whose bytes get-next-fixture and decode write, in place of the
//                          sample FM24_STAND_IN_SAMPLE (a FixtureSample, 0 by default)
//   FM24_STAND_IN_FLOOD    that many numbered lines on both stdout and stderr
//   FM24_STAND_IN_LINGER   a child that keeps stdout and stderr open this many ms after the exit
//   FM24_STAND_IN_STDIN    if set, reads stdin to its end and reports its length on stdout
// `run <input> <seeds> <output.csv> ...` writes a results CSV as the harness does: a row per match
// of the pattern (row i is seed i + 1, ending i % 3 to 1), each flushed after FM24_STAND_IN_ROW_MS,
// with the header coming with the first. With --detailed it then writes the two reports beside it.
// A pattern the harness would not take stops it first, with status 2, as fm24ctl refuses one: its
// reason for the pattern (commands.c's checkSeedPattern), then its usage, on stderr.
//   FM24_STAND_IN_INPUT    a file the input's bytes are copied to
//   FM24_STAND_IN_FAIL_AT  the row at which the run fails, with an "error:" line and status 1
//   FM24_STAND_IN_LOST_AT  the row at which the game stops answering (it has crashed): fm24ctl's
//                          line of errors, with status 1
//   FM24_STAND_IN_BAD_ROW  a row written as a line that is not a result
// `replace-home|replace-away <input> <team> <output>` and `inject-player <input> <block> <person>
// <output> [--allow-duplicate]` copy the input to FM24_STAND_IN_INPUT, if set, and write the
// fixture get-next-fixture writes. With FM24_STAND_IN_DUPLICATE set, inject-player refuses, as
// fm24ctl does a player already in the fixture, unless given --allow-duplicate. `list-teams` and
// `list-players <output>` write the Windows viewer tests' lists (with lines to skip), or the file
// FM24_STAND_IN_LIST names.
// As fm24ctl does on macOS, `run`, `replace-*` and `inject-player` with a person ID first check the
// input's "save" against the game the environment describes, in fm24ctl's own words (the
// viewer_stand_in_texts test finds each text in fm24ctl's sources):
//   FM24_STAND_IN_LOADED   the save FM24 has loaded: an input for another stops with status 3, or
//                          with --any-save goes ahead
//   FM24_STAND_IN_CLOSED   if set, FM24 is not running: what fm24ctl prints as it starts the game
//                          through Steam to load the input's save comes first, the load taking
//                          FM24_STAND_IN_LOAD_MS
//   FM24_STAND_IN_DATE     the loaded save's in-game date, which run warns of if the input's
//                          "game_date" is another
//   FM24_STAND_IN_UNNAMED  if set, the harness cannot read the loaded save's name, which fm24ctl
//                          takes to be the input's, saying so
// and the environment can set what the harness does:
//   FM24_STAND_IN_BUSY     a reason such as "loading": a job is refused as the harness refuses one
//                          while the game loads or processes, with status 1. Before a job whose
//                          input's save fm24ctl checks, fm24ctl waits instead, printing the game's
//                          state: until FM24_STAND_IN_BUSY_MS have passed (with a "still waiting"
//                          line halfway), after which the game is idle and the job goes ahead, or,
//                          without it, past fm24ctl's timeout, which stops it with status 3
//   FM24_STAND_IN_SAVED    if set, the fixtures written have the "save" and "game_date" of
//                          fixtureSamples_savedJson, as the macOS harness's exports do
// `launch [--save <name|path.fm>] [--rosetta] [--timeout <s>]` prints what fm24ctl prints as it
// starts the game through Steam's launch option and waits for the harness (FM24_STAND_IN_LAUNCH_MS,
// with a "still waiting" line halfway) and the save (FM24_STAND_IN_LOAD_MS, as for a job), which is
// then at FM24_STAND_IN_DATE. A path must name a .fm file. With FM24_STAND_IN_RUNNING set, FM24 is
// running with the harness and FM24_STAND_IN_LOADED loaded: another save stops it with status 3,
// as does --rosetta if FM24_STAND_IN_NATIVE is set too. With FM24_STAND_IN_UNCLEAR set, Steam's
// launch option sets the game's data folder in a way fm24ctl does not follow: a save's .fm file,
// or its name while FM24 is not running, stops it with status 1. `install` prints what fm24ctl
// prints as it installs the harness under HOME (it writes nothing): FM24_STAND_IN_OPTION_SET says
// Steam's launch option uses the launcher (with FM24_STAND_IN_UNCLEAR set too, setting the data
// folder in a way fm24ctl does not follow), FM24_STAND_IN_CURRENT_OPTION gives the account's
// other option, FM24_STAND_IN_NO_GAME says that Steam has no FM24, and FM24_STAND_IN_NO_STEAM that
// there is no Steam. FM24_STAND_IN_STDOUT replaces what either prints.

#include "fixture_samples.h"
#include "fixture_view.h"
#include "match_config.h"
#include "saves.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>


// fm24ctl's texts, as src/dll/job_execute.c and src/runner/client_posix.c, commands.c,
// macos_game.c and session.c print them.
static const char busyText[] = "error: FM24 is busy (%s); wait until it is idle";
static const char badSaveText[] =
	"the input's \"save\" is not a save's name; remove it to use any save\n";
static const char otherSaveText[] =
	"FM24 has '%s' loaded, not '%s'; fm24ctl will not switch saves in a running game (unsaved "
	"progress). Quit FM24 (saving first only if you loaded that save yourself and have done no "
	"harness work since), then run this again%s.\n";
static const char anySaveHintText[] = ", or pass --any-save to use the loaded save";
static const char anySaveText[] = "FM24 has '%s' loaded, not '%s'; using it, as --any-save asks\n";
static const char notRunningText[] =
	"Football Manager 2024 is not running; starting it to load %s (--no-launch turns this off)\n";
static const char loadingText[] = "loading %s: %s\n";
static const char throughSteamText[] =
	"starting Football Manager 2024 through Steam (its launch option loads the harness%s)\n";
static const char harnessWaitText[] = "waiting for the game to start the harness%s...\n";
static const char harnessLoadedText[] =
	"the harness is loaded; checking that the game keeps running...\n";
static const char harnessReadyText[] =
	"the harness is ready in Football Manager 2024 (pid %d%s)%s\n";
static const char saveWaitText[] = "waiting for it to load %s (at most %u s)...\n";
static const char stateText[] = "FM24 state: %s\n";
static const char readyText[] = "ready: %s %sloaded (pid %d%s%s)\n";
static const char unnamedText[] =
	"the harness cannot read which save FM24 has loaded; taking it to be %s\n";
static const char lostText[] = "cannot read a complete result; see harness.log\n";
static const char datedText[] =
	"warning: the save is at %s, the fixture was exported at %s; the results reflect the save\n";
static const char rosettaSteamText[] = " and starts the x86_64 slice under Rosetta";
static const char rosettaWaitText[] =
	" (a first start under Rosetta after an update translates the game first, "
	"which can take minutes)";
static const char stillWaitingText[] = "still waiting (%lld s)...\n";
static const char underRosettaText[] = ", under Rosetta";
static const char loadYourSaveText[] =
	". Load your save and leave the game idle before running fm24ctl commands.";
static const char runningText[] =
	"Football Manager 2024 (pid %d) is already running with the harness%s\n";
static const char nativeText[] =
	"Football Manager 2024 (pid %d) is already running natively, not under Rosetta; fm24ctl "
	"will not restart a running game. Quit it, then run this again.\n";
static const char stillText[] = "FM24 was still ";
static const char timedOutText[] = "%s%s after %u s, without %s ready. Check the game window%s.\n";
static const char notSaveText[] = "%s is not a save: --save takes a save's name or its .fm file\n";
static const char missingSaveText[] = "cannot find the save %s\n";
static const char unclearText[] =
	"Steam's launch option for FM24 sets its data folder (user_data_location) in a way fm24ctl "
	"does not follow; %s\n";
static const char userDataHintText[] = "pass the folder with --user-data";
static const char installedText[] =
	"installed harness %s%s: %s/%s\nlauncher: %s\nlog: %s/harness.log\n";
static const char gameFoundText[] = "Football Manager 2024: %s\n";
static const char gameMissingText[] =
	"Football Manager 2024 was not found in your Steam libraries\n";
static const char optionSetText[] =
	"\nSteam's launch option for Football Manager 2024 already uses this launcher: start the game "
	"from Steam as usual.\n";
static const char optionText[] =
	"\nTo start Football Manager 2024 from Steam with the harness, set its launch option once:\n"
	"  Steam > Library > Football Manager 2024 > Properties > General > Launch Options:\n"
	"  /bin/sh \"%s\" %%command%%\n%s";
static const char checkOptionText[] =
	"Keep any arguments of your own after %command%. Steam may save the option only when it "
	"quits: run `fm24ctl install` again until it says that Steam's launch option uses this "
	"launcher, quitting Steam if it does not. Until then, `fm24ctl launch` starts the game "
	"without the launch option, which has not been verified: the game crashed in its one "
	"attempt.\n";
static const char reinstallText[] =
	"\nRun `fm24ctl install` again after updating fm24ctl: it updates the harness and the "
	"launcher.\n";
static const char noSteamText[] =
	"Steam is not in %s, where fm24ctl looks for it: install Steam and Football Manager 2024 "
	"(24.4.2), then run `fm24ctl install` again.\n";
static const char noGameText[] =
	"Install Football Manager 2024 (24.4.2) in Steam, then run `fm24ctl install` again; fm24ctl "
	"looks in the Steam libraries that %s/steamapps/libraryfolders.vdf lists.\n";
static const char currentOptionText[] =
	"\nSteam's launch option for Football Manager 2024 is now: %s\n";
static const char dataFolderText[] =
	"It sets FM24's data folder (user_data_location) in a way fm24ctl does not follow, which "
	"stops `fm24ctl launch --save` and the commands that start the game for a fixture's save: "
	"write it as --user_data_location=<absolute folder>.\n";
static const char seedsLengthText[] =
	"the seed pattern must fit in %d characters (this one has %zu)\n";
static const char missingSeedText[] = "a seed is missing next to a comma in %s\n";
static const char notSeedsText[] = "%.*s is not a seed or a range of seeds (first-last)\n";
static const char seedBoundsText[] = "seeds must be numbers from 0 to 4294967295, not %.*s\n";
static const char backwardsText[] = "the seed range %.*s runs backwards: write %llu-%llu\n";
static const char rangeSizeText[] = "the seed range %.*s has more than %d seeds: split it\n";
static const char runUsageText[] =
	"usage: fm24ctl run <input.bin|.json> [seeds] [output.csv] [--threads N]"
	" [--profile <samples.txt>] [--detailed] [--human-control] [--keep-build]";

static long number(const char *name, long fallback);
static void sleepFor(long milliseconds);
static bool logArguments(int argc, char **argv);
static bool checksSave(int argc, char **argv);
static int checkSave(int argc, char **argv);
static void startForSave(const char *save, const char *date);
static int waitWhileBusy(const char *save);
static bool hasArgument(int argc, char **argv, const char *argument);
static bool refused(void);
static bool refusesSeeds(const char *pattern);
static bool refusesSeedPart(const char *pattern);
static char *readFile(const char *path, size_t *length);
static bool writeFixture(const char *path);
static int run(int argc, char **argv);
static int replace(int argc, char **argv);
static int list(int argc, char **argv);
static int launch(int argc, char **argv);
static int install(void);
static bool copyFile(const char *from, const char *to);

// The game was busy before this run's job, and has become idle since.
static bool idle;

int main(int argc, char **argv) {
	// As fm24ctl's main has it.
	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	if (!logArguments(argc, argv)) {
		return 70;
	}
	const char *errors = getenv("FM24_STAND_IN_STDERR");
	if (errors) {
		fputs(errors, stderr);
		fflush(stderr);
	}
	sleepFor(number("FM24_STAND_IN_DELAY_MS", 0));
	if (getenv("FM24_STAND_IN_STDIN")) {
		size_t length = 0;
		char buffer[4096];
		for (size_t got = 1; got > 0; length += got) {
			got = fread(buffer, 1, sizeof buffer, stdin);
		}
		printf("stdin: %zu bytes\n", length);
	}
	long flood = number("FM24_STAND_IN_FLOOD", 0);
	for (long line = 1; line <= flood; ++line) {
		printf("out %ld: the harness wrote a line of output to fill the pipe\n", line);
		fprintf(stderr, "err %ld: the harness wrote a line of errors to fill the pipe\n", line);
	}
	int status = (int)number("FM24_STAND_IN_STATUS", 0);
	const char *command = argc > 1 ? argv[1] : "";
	if (status == 0 && strcmp(command, "run") == 0 && argc > 3 && refusesSeeds(argv[3])) {
		return 2;
	}
	int checked = status == 0 && checksSave(argc, argv) ? checkSave(argc, argv) : 0;
	if (checked != 0) {
		return checked;
	}
	bool writes = strcmp(command, "get-next-fixture") == 0 || strcmp(command, "decode") == 0;
	if (status == 0 && writes) {
		FILE *input = strcmp(command, "decode") == 0 && argc > 2 ? fopen(argv[2], "rb") : NULL;
		if (strcmp(command, "decode") == 0 && !input) {
			printf("error: cannot read %s\n", argc > 2 ? argv[2] : "the input");
			return 1;
		}
		if (input) {
			fclose(input);
		}
		if (refused()) {
			return 1;
		}
		const char *output = argv[strcmp(command, "decode") == 0 ? 3 : 2];
		if (argc < (strcmp(command, "decode") == 0 ? 4 : 3) || !writeFixture(output)) {
			printf("error: cannot write the fixture\n");
			return 1;
		}
	} else if (status == 0 && strcmp(command, "run") == 0) {
		int ran = run(argc, argv);
		if (ran != 0) {
			return ran;
		}
	} else if (status == 0 &&
						 (strncmp(command, "replace-", 8) == 0 || strcmp(command, "inject-player") == 0)) {
		int replaced = replace(argc, argv);
		if (replaced != 0) {
			return replaced;
		}
	} else if (status == 0 && strncmp(command, "list-", 5) == 0) {
		int listed = list(argc, argv);
		if (listed != 0) {
			return listed;
		}
	} else if (status == 0 && strcmp(command, "launch") != 0 && strcmp(command, "install") != 0) {
		printf("error: the stand-in does not do %s\n", command);
		return 1;
	}
	const char *output = getenv("FM24_STAND_IN_STDOUT");
	bool own = strcmp(command, "launch") == 0 || strcmp(command, "install") == 0;
	if (status == 0 && own && !output) {
		int done = strcmp(command, "launch") == 0 ? launch(argc, argv) : install();
		if (done != 0) {
			return done;
		}
	} else {
		fputs(output ? output : "ok: stand-in\n", stdout);
	}
	long linger = number("FM24_STAND_IN_LINGER", 0);
	if (linger > 0 && fork() == 0) {
		sleepFor(linger);
		_exit(0);
	}
	return status;
}

static long number(const char *name, long fallback) {
	const char *text = getenv(name);
	return text ? strtol(text, NULL, 10) : fallback;
}

static void sleepFor(long milliseconds) {
	struct timespec wait = {
		.tv_sec = milliseconds / 1000, .tv_nsec = (milliseconds % 1000) * 1000000L};
	while (milliseconds > 0 && nanosleep(&wait, &wait) != 0 && errno == EINTR) {
	}
}

static bool logArguments(int argc, char **argv) {
	const char *path = getenv("FM24_STAND_IN_LOG");
	if (!path) {
		return true;
	}
	FILE *log = fopen(path, "ab");
	if (!log) {
		return false;
	}
	for (int i = 1; i < argc; ++i) {
		fprintf(log, "%s%s", i > 1 ? "\t" : "", argv[i]);
	}
	fputc('\n', log);
	return fclose(log) == 0;
}

// Whether fm24ctl checks the save of this command's input first: run, replace-home, replace-away
// and inject-player with a person ID.
static bool checksSave(int argc, char **argv) {
	const char *command = argc > 2 ? argv[1] : "";
	bool inject = strcmp(command, "inject-player") == 0;
	size_t length = argc > 4 ? strlen(argv[4]) : 0;
	bool playerFile = inject && length >= 5 && strcmp(argv[4] + length - 5, ".json") == 0;
	return strcmp(command, "run") == 0 || strcmp(command, "replace-home") == 0 ||
				 strcmp(command, "replace-away") == 0 || (inject && argc > 4 && !playerFile);
}

// fm24ctl's check of the input's "save" (macosGame_ensureSave) against the game the environment
// describes: 0 to go on, else the exit status.
static int checkSave(int argc, char **argv) {
	size_t length = 0;
	char *text = readFile(argv[2], &length);
	ConfigOrigin origin;
	bool read = text && matchConfig_readOrigin(text, length, &origin);
	free(text);
	if (!read || !origin.hasSave) {
		return 0;
	}
	char save[SAVES_NAME_LENGTH];
	if (!saves_nameOfPath(origin.save, save, sizeof save) || !saves_isName(save)) {
		fputs(badSaveText, stderr);
		return 1;
	}
	const char *loaded = getenv("FM24_STAND_IN_LOADED");
	const char *date = getenv("FM24_STAND_IN_DATE");
	const char *loadedDate = date ? date : origin.gameDate;
	bool closed = getenv("FM24_STAND_IN_CLOSED") != NULL;
	bool unnamed = getenv("FM24_STAND_IN_UNNAMED") != NULL;
	int waited = closed || !getenv("FM24_STAND_IN_BUSY") ? -1 : waitWhileBusy(save);
	if (waited > 0) {
		return waited;
	}
	if (closed) {
		startForSave(save, loadedDate);
	} else if (!unnamed && loaded && !saves_isSave(loaded, save)) {
		if (!hasArgument(argc, argv, "--any-save")) {
			fprintf(stderr, otherSaveText, loaded, save, anySaveHintText);
			return 3;
		}
		printf(anySaveText, loaded, save);
		return 0;
	} else {
		if (unnamed) {
			fprintf(stderr, unnamedText, save);
		}
		// A job that goes ahead at once follows without a word.
		if (waited == 0) {
			printf(readyText, save, "is already ", (int)getpid(), loadedDate[0] ? ", " : "", loadedDate);
		}
	}
	if (strcmp(argv[1], "run") == 0 && date && origin.gameDate[0] &&
			strcmp(date, origin.gameDate) != 0) {
		fprintf(stderr, datedText, date, origin.gameDate);
	}
	return 0;
}

// What fm24ctl prints as it starts the game through Steam's launch option to load `save`, which is
// at `date` once loaded, and waits for it (macosGame_ensureSave, launchForJob, session_waitReady).
static void startForSave(const char *save, const char *date) {
	const char *home = getenv("HOME");
	int pid = (int)getpid();
	printf(notRunningText, save);
	char path[1100];
	(void)snprintf(
		path,
		sizeof path,
		"%s/Library/Application Support/Sports Interactive/Football Manager 2024/cloud/games/%s.fm",
		home ? home : "",
		save
	);
	printf(loadingText, save, path);
	printf(throughSteamText, "");
	printf(harnessWaitText, "");
	fputs(harnessLoadedText, stdout);
	printf(harnessReadyText, pid, "", "");
	printf(saveWaitText, save, 900U);
	printf(stateText, "loading");
	sleepFor(number("FM24_STAND_IN_LOAD_MS", 0));
	printf(stateText, "ready");
	printf(readyText, save, "", pid, date[0] ? ", " : "", date);
}

// What fm24ctl prints as it waits for the game, running with the harness but busy, to be idle
// with `save` loaded before a job (useRunningGame, waitForSave, session_waitReady): 0 once it is,
// else the exit status.
static int waitWhileBusy(const char *save) {
	const char *phase = strcmp(getenv("FM24_STAND_IN_BUSY"), "loading") == 0 ? "loading" : "busy";
	printf(stateText, phase);
	const char *busyFor = getenv("FM24_STAND_IN_BUSY_MS");
	if (!busyFor) {
		fprintf(stderr, timedOutText, stillText, phase, 900U, save, "");
		return 3;
	}
	long milliseconds = strtol(busyFor, NULL, 10);
	sleepFor(milliseconds / 2);
	if (milliseconds > 0) {
		printf(stillWaitingText, 60LL);
	}
	sleepFor(milliseconds - milliseconds / 2);
	printf(stateText, "ready");
	idle = true;
	return 0;
}

static bool hasArgument(int argc, char **argv, const char *argument) {
	for (int i = 2; i < argc; ++i) {
		if (strcmp(argv[i], argument) == 0) {
			return true;
		}
	}
	return false;
}

// The harness's answer to a job while the game loads or processes (FM24_STAND_IN_BUSY), which
// fm24ctl prints: true if it refused.
static bool refused(void) {
	const char *reason = getenv("FM24_STAND_IN_BUSY");
	if (!reason || idle) {
		return false;
	}
	printf(busyText, reason);
	putchar('\n');
	return true;
}

// A file's bytes, malloc'd and NUL-terminated (`length` excludes the NUL), or NULL.
static char *readFile(const char *path, size_t *length) {
	char *bytes = NULL;
	FILE *file = fopen(path, "rb");
	bool ok = file && fseek(file, 0, SEEK_END) == 0;
	long size = ok ? ftell(file) : -1;
	ok = ok && size >= 0 && fseek(file, 0, SEEK_SET) == 0 &&
			 (bytes = malloc((size_t)size + 1)) != NULL &&
			 fread(bytes, 1, (size_t)size, file) == (size_t)size;
	if (file) {
		fclose(file);
	}
	if (!ok) {
		free(bytes);
		*length = 0;
		return NULL;
	}
	bytes[size] = 0;
	*length = (size_t)size;
	return bytes;
}

static bool writeFixture(const char *path) {
	const char *source = getenv("FM24_STAND_IN_FIXTURE");
	char *bytes = NULL;
	size_t length = 0;
	if (source) {
		bytes = readFile(source, &length);
		if (!bytes) {
			return false;
		}
	} else {
		int sample = (int)number("FM24_STAND_IN_SAMPLE", 0);
		bytes = getenv("FM24_STAND_IN_SAVED") ? fixtureSamples_savedJson(sample, &length)
																					: fixtureSamples_json(sample, &length);
	}
	FILE *output = bytes ? fopen(path, "wb") : NULL;
	bool ok = output && fwrite(bytes, 1, length, output) == length;
	ok = output && fclose(output) == 0 && ok;
	free(bytes);
	return ok;
}

// A results row, as job_run.c writes them; the goals follow the row's index.
static void writeRow(FILE *csv, unsigned long index) {
	fprintf(
		csv,
		"%lu,%lu,1,%lu,1,%016llx,%.1f\n",
		index + 1,
		index % 3,
		23000 + index,
		(unsigned long long)index * 2654435761ULL,
		4000.0 + (double)index
	);
}

static int run(int argc, char **argv) {
	if (argc < 5) {
		printf("error: run needs an input, seeds and an output\n");
		return 1;
	}
	const char *input = argv[2];
	const char *output = argv[4];
	const char *copy = getenv("FM24_STAND_IN_INPUT");
	FILE *readable = fopen(input, "rb");
	if (!readable) {
		printf("error: cannot read %s\n", input);
		return 1;
	}
	fclose(readable);
	if (copy && !copyFile(input, copy)) {
		printf("error: cannot copy the input\n");
		return 1;
	}
	size_t count = 0;
	if (!fixtureView_seedCount(argv[3], &count)) {
		return 2; // refusesSeeds has said why
	}
	bool detailed = false;
	for (int i = 5; i < argc; ++i) {
		detailed = detailed || strcmp(argv[i], "--detailed") == 0;
	}
	printf("results: %s\njob submitted; waiting...\n", output);
	if (refused()) {
		return 1;
	}
	FILE *csv = fopen(output, "wb");
	if (!csv) {
		printf("error: cannot open %s\n", output);
		return 1;
	}
	// Unflushed, as the harness writes it: it reaches the file with the first row.
	fputs("seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n", csv);
	long delay = number("FM24_STAND_IN_ROW_MS", 0);
	long failAt = number("FM24_STAND_IN_FAIL_AT", -1);
	long lostAt = number("FM24_STAND_IN_LOST_AT", -1);
	long badRow = number("FM24_STAND_IN_BAD_ROW", -1);
	for (size_t row = 0; row < count; ++row) {
		sleepFor(delay);
		if ((long)row == failAt) {
			fclose(csv);
			printf("error: the stand-in stopped after %zu matches\n", row);
			return 1;
		}
		if ((long)row == lostAt) {
			fclose(csv);
			fputs(lostText, stderr);
			return 1;
		}
		if ((long)row == badRow) {
			fputs("not,a,result\n", csv);
		} else {
			writeRow(csv, (unsigned long)row);
		}
		fflush(csv);
	}
	if (fclose(csv) != 0) {
		printf("error: cannot write %s\n", output);
		return 1;
	}
	size_t stem = strlen(output) > 4 ? strlen(output) - 4 : strlen(output);
	for (int report = 0; detailed && report < 2; ++report) {
		char path[1100];
		const char *suffix = report ? "-teams.csv" : "-players.csv";
		snprintf(path, sizeof path, "%.*s%s", (int)stem, output, suffix);
		FILE *file = fopen(path, "wb");
		if (!file || fprintf(file, "stand-in%s\n", suffix) < 0 || fclose(file) != 0) {
			printf("error: cannot write %s\n", path);
			return 1;
		}
	}
	return 0;
}

// replace-home, replace-away and inject-player.
static int replace(int argc, char **argv) {
	bool inject = strcmp(argv[1], "inject-player") == 0;
	int fixed = inject ? 6 : 5;
	bool allowDuplicate = false;
	bool unknown = false;
	for (int i = fixed; i < argc; ++i) {
		bool duplicate = inject && strcmp(argv[i], "--allow-duplicate") == 0;
		allowDuplicate = allowDuplicate || duplicate;
		unknown = unknown || !(duplicate || strcmp(argv[i], "--any-save") == 0 ||
													 strcmp(argv[i], "--no-launch") == 0);
	}
	if (argc < fixed || unknown) {
		printf(
			"error: %s needs an input, %s and an output\n",
			argv[1],
			inject ? "a block, a player" : "a team"
		);
		return 1;
	}
	const char *input = argv[2];
	const char *output = argv[inject ? 5 : 4];
	FILE *readable = fopen(input, "rb");
	if (!readable) {
		fprintf(stderr, "cannot read %s\n", input);
		return 1;
	}
	fclose(readable);
	const char *copy = getenv("FM24_STAND_IN_INPUT");
	if (copy && !copyFile(input, copy)) {
		printf("error: cannot copy the input\n");
		return 1;
	}
	// inject-player's first job fetches the player.
	if (refused()) {
		return 1;
	}
	if (inject) {
		printf("ok: fetched player %s\n", argv[4]);
	}
	if (inject && getenv("FM24_STAND_IN_DUPLICATE") && !allowDuplicate) {
		fprintf(
			stderr,
			"error: %s.person_uid (Stand-in) is already this player; use --allow-duplicate to inject "
			"anyway\n",
			argv[3]
		);
		return 1;
	}
	if (!writeFixture(output)) {
		printf("error: cannot write the fixture\n");
		return 1;
	}
	return 0;
}

// list-teams and list-players.
static int list(int argc, char **argv) {
	static const char teams[] =
		"677\tLuton Town\n12\tAston Villa\r\n99\tUnited\n691\tNorwich City\n98\tUnited\nbad\n";
	// Five players: one with a common name, one without a club; then a line with too few fields,
	// a player without names and a malformed line, which are skipped.
	static const char players[] = "40347\tMartin\t\xC3\x98"
																"degaard\t\tArsenal\n"
																"1001\tEdson\tArantes do Nascimento\tPel\xC3\xA9\t\n"
																"2002\tThomas\tM\xC3\xBC"
																"ller\t\tFC Bayern\n"
																"3003\t\xC5\x81ukasz\tFabia\xC5\x84"
																"ski\t\tWest Ham\n"
																"5005\tJan\tBodegaard\t\t\r\n"
																"6006\tToo\tFew\n"
																"7007\t\t\t\t\n"
																"bad\n";
	bool listsPlayers = strcmp(argv[1], "list-players") == 0;
	if (argc != 3 || (!listsPlayers && strcmp(argv[1], "list-teams") != 0)) {
		printf("error: the stand-in does not do %s\n", argv[1]);
		return 1;
	}
	if (refused()) {
		return 1;
	}
	const char *source = getenv("FM24_STAND_IN_LIST");
	if (source) {
		if (!copyFile(source, argv[2])) {
			printf("error: cannot write %s\n", argv[2]);
			return 1;
		}
		return 0;
	}
	const char *text = listsPlayers ? players : teams;
	size_t length = listsPlayers ? sizeof players - 1 : sizeof teams - 1;
	FILE *output = fopen(argv[2], "wb");
	bool ok = output && fwrite(text, 1, length, output) == length;
	ok = output && fclose(output) == 0 && ok;
	if (!ok) {
		printf("error: cannot write %s\n", argv[2]);
		return 1;
	}
	return 0;
}

// What fm24ctl's launch prints (macosGame_launch: useRunningGame, launchWithSave, startGame,
// waitForHarness, launchExitCode, waitForSave and reportSave).
static int launch(int argc, char **argv) {
	const char *given = NULL;
	unsigned timeout = 900;
	bool rosetta = false;
	for (int i = 2; i < argc; ++i) {
		bool hasValue = i + 1 < argc;
		if (strcmp(argv[i], "--save") == 0 && hasValue) {
			given = argv[++i];
		} else if (strcmp(argv[i], "--timeout") == 0 && hasValue) {
			timeout = (unsigned)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--rosetta") == 0) {
			rosetta = true;
		} else {
			fprintf(stderr, "unknown option, missing value or extra argument: %s\n", argv[i]);
			return 2;
		}
	}
	char name[SAVES_NAME_LENGTH] = "";
	char path[1100] = "";
	const char *home = getenv("HOME");
	if (given && saves_isName(given)) {
		(void)snprintf(name, sizeof name, "%s", given);
		(void)snprintf(
			path,
			sizeof path,
			"%s/Library/Application Support/Sports Interactive/Football Manager 2024/cloud/games/%s.fm",
			home ? home : "",
			given
		);
	} else if (given) {
		size_t length = strlen(given);
		FILE *save = NULL;
		if (length < 4 || strcasecmp(given + length - 3, ".fm") != 0) {
			fprintf(stderr, notSaveText, given);
			return 1;
		}
		if (!(save = fopen(given, "rb"))) {
			fprintf(stderr, missingSaveText, given);
			return 1;
		}
		fclose(save);
		(void)saves_nameOfPath(given, name, sizeof name);
		(void)snprintf(path, sizeof path, "%s", given);
	}
	bool running = getenv("FM24_STAND_IN_RUNNING") != NULL;
	// A .fm file, or a name looked up while the game is not running, needs the data folder.
	if (given && getenv("FM24_STAND_IN_UNCLEAR") && (!saves_isName(given) || !running)) {
		fprintf(stderr, unclearText, userDataHintText);
		return 1;
	}
	int pid = (int)getpid();
	const char *date = getenv("FM24_STAND_IN_DATE");
	date = date ? date : "";
	if (running) {
		const char *loaded = getenv("FM24_STAND_IN_LOADED");
		if (rosetta && getenv("FM24_STAND_IN_NATIVE")) {
			fprintf(stderr, nativeText, pid);
			return 3;
		}
		if (!given) {
			printf(runningText, pid, rosetta ? underRosettaText : "");
			return 0;
		}
		if (loaded && !saves_isSave(loaded, name)) {
			fprintf(stderr, otherSaveText, loaded, name, "");
			return 3;
		}
		printf(readyText, name, "is already ", pid, date[0] ? ", " : "", date);
		return 0;
	}
	if (given) {
		printf(loadingText, name, path);
	}
	printf(throughSteamText, rosetta ? rosettaSteamText : "");
	printf(harnessWaitText, rosetta ? rosettaWaitText : "");
	long starting = number("FM24_STAND_IN_LAUNCH_MS", 0);
	sleepFor(starting / 2);
	if (starting > 0) {
		printf(stillWaitingText, 60LL);
	}
	sleepFor(starting - starting / 2);
	fputs(harnessLoadedText, stdout);
	printf(harnessReadyText, pid, rosetta ? underRosettaText : "", given ? "" : loadYourSaveText);
	if (given) {
		printf(saveWaitText, name, timeout);
		printf(stateText, "loading");
		sleepFor(number("FM24_STAND_IN_LOAD_MS", 0));
		printf(stateText, "ready");
		printf(readyText, name, "", pid, date[0] ? ", " : "", date);
	}
	return 0;
}

// What fm24ctl's install prints (macosGame_install, macosGame_installHarness and
// describeLaunchOption), without writing anything.
static int install(void) {
	const char *home = getenv("HOME");
	char folder[1100];
	char launcher[1200];
	char dylib[64];
	char app[1200];
	char steam[1100];
	(void)snprintf(
		folder, sizeof folder, "%s/Library/Application Support/fm24-harness", home ? home : ""
	);
	(void)snprintf(steam, sizeof steam, "%s/Library/Application Support/Steam", home ? home : "");
	(void)snprintf(launcher, sizeof launcher, "%s/launch-fm24.sh", folder);
	(void)snprintf(dylib, sizeof dylib, "fm24-harness-%s.dylib", fixtureView_protocol());
	(void)snprintf(
		app,
		sizeof app,
		"%s/Library/Application Support/Steam/steamapps/common/Football Manager 2024/fm.app",
		home ? home : ""
	);
	printf(installedText, fixtureView_protocol(), "", folder, dylib, launcher, folder);
	bool noSteam = getenv("FM24_STAND_IN_NO_STEAM") != NULL;
	if (noSteam || getenv("FM24_STAND_IN_NO_GAME")) {
		fputs(gameMissingText, stdout);
		printf(noSteam ? noSteamText : noGameText, steam);
	} else {
		printf(gameFoundText, app);
	}
	const char *current = getenv("FM24_STAND_IN_CURRENT_OPTION");
	if (getenv("FM24_STAND_IN_OPTION_SET")) {
		fputs(optionSetText, stdout);
		if (getenv("FM24_STAND_IN_UNCLEAR")) {
			fputs(dataFolderText, stdout);
		}
	} else {
		if (current && current[0]) {
			printf(currentOptionText, current);
		}
		printf(optionText, launcher, checkOptionText);
	}
	fputs(reinstallText, stdout);
	return 0;
}

// fm24ctl's refusal of a seed pattern the harness would not take (commands.c's checkSeedPattern,
// whose checks and texts these are), before it checks the save or submits the job: true, having
// given its reason and usage, if it refuses `pattern`.
static bool refusesSeeds(const char *pattern) {
	enum { SEED_PATTERN_LENGTH = 255 }; // as commands.c's
	size_t length = strlen(pattern);
	if (length > SEED_PATTERN_LENGTH) {
		fprintf(stderr, seedsLengthText, SEED_PATTERN_LENGTH, length);
	} else if (strcmp(pattern, "keep") == 0 || !refusesSeedPart(pattern)) {
		return false;
	}
	fprintf(stderr, "%s\n", runUsageText);
	return true;
}

// True, having given fm24ctl's reason, if a part of `pattern` between commas is not a seed or a
// range of seeds that the harness takes.
static bool refusesSeedPart(const char *pattern) {
	enum { MAX_SEED_RANGE = 10000000 }; // as commands.c's
	for (const char *part = pattern;;) {
		const char *comma = strchr(part, ',');
		int length = (int)(comma ? (size_t)(comma - part) : strlen(part));
		char *end = (char *)part;
		unsigned long long first = 0;
		bool read = length > 0 && part[0] >= '0' && part[0] <= '9';
		if (read) {
			first = strtoull(part, &end, 10);
		}
		unsigned long long last = first;
		if (read && *end == '-') {
			read = end[1] >= '0' && end[1] <= '9';
			if (read) {
				last = strtoull(end + 1, &end, 10);
			}
		}
		if (length == 0) {
			fprintf(stderr, missingSeedText, pattern);
		} else if (!read || end != part + length) {
			fprintf(stderr, notSeedsText, length, part);
		} else if (first > UINT32_MAX || last > UINT32_MAX) {
			fprintf(stderr, seedBoundsText, length, part);
		} else if (last < first) {
			fprintf(stderr, backwardsText, length, part, last, first);
		} else if (last - first > MAX_SEED_RANGE) {
			fprintf(stderr, rangeSizeText, length, part, MAX_SEED_RANGE + 1);
		} else if (comma) {
			part = comma + 1;
			continue;
		} else {
			return false;
		}
		return true;
	}
}

static bool copyFile(const char *from, const char *to) {
	FILE *source = fopen(from, "rb");
	FILE *target = source ? fopen(to, "wb") : NULL;
	bool ok = source && target;
	char buffer[4096];
	for (size_t got = 1; ok && got > 0;) {
		got = fread(buffer, 1, sizeof buffer, source);
		ok = fwrite(buffer, 1, got, target) == got;
	}
	if (source) {
		fclose(source);
	}
	if (target) {
		ok = fclose(target) == 0 && ok;
	}
	return ok;
}
