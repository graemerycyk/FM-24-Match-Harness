// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "macos_game.h"

#include "buffer.h"
#include "channel.h"
#include "client.h"
#include "embedded.h"
#include "files.h"
#include "paths.h"
#include "platform.h"
#include "protocol.h"
#include "saves.h"
#include "session.h"

#include <dirent.h>
#include <errno.h>
#include <libproc.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


extern char **environ;

enum {
	EXIT_WRONG_STATE = 3, // the game is not in the state the command needs (see commands.h)
	PATH_LENGTH = 4096,
	START_TIMEOUT_S = 600,    // from launching until the harness is listening (the game may update)
	NO_HARNESS_GRACE_S = 120, // a running game that still has no harness socket after this long
	ROSETTA_GRACE_S = 600,    // the same under Rosetta, which translates the game on a first start
	EXIT_GRACE_S = 30,        // no game process for this long after one ran: it exited
	SETTLE_S = 10,            // the harness serves this long in one game process: it is ready
	PROGRESS_INTERVAL_S = 60, // between "still waiting" lines
	SAVE_TIMEOUT_S = 900,     // then until the save is loaded (--timeout; Rosetta is slower)
	MAX_TIMEOUT_S = 86400,    // the longest --timeout, a day
	VDF_DEPTH = 8,            // KeyValues blocks deeper than this are skipped
	VDF_KEY = 64,
};

static const char steamAppId[] = "2252570";
static const char bundleIdentifier[] = "com.sigames.eur.FootballManager2024";
static const char executableSuffix[] = "/Contents/MacOS/fm";
static const unsigned long long steamIdBase = 76561197960265728ULL; // SteamID64 of account ID 0
static const char dataFolderPath[] =
	"Library/Application Support/Sports Interactive/Football Manager 2024"; // under the home folder
// Auto-play files, next to the launcher: a request for it, named after fm24ctl's process, and the
// copy it hands the game, named after the game's (or a direct start's, named after fm24ctl's).
static const char autoplayRequestPrefix[] = "autoplay-request-";
static const char autoplayPrefix[] = "autoplay-";
// A request for the x86_64 slice, named after fm24ctl's process, and its name once the launcher has
// taken it, after its own (which it then deletes).
static const char rosettaRequestPrefix[] = "rosetta-request-";
static const char rosettaPrefix[] = "rosetta-";

// What `launch` was asked to do, or what a command's job needs (see macosGame_ensureSave).
typedef struct LaunchOptions {
	const wchar_t *app;      // the fm.app given, or NULL
	const wchar_t *save;     // --save: a save's name or its .fm file, or NULL
	const wchar_t *userData; // --user-data: FM24's data folder, or NULL for the usual one
	unsigned timeoutS;       // --timeout
	bool rosetta;            // --rosetta: the x86_64 slice, under Rosetta on Apple silicon
	const SessionNeed *job;  // the command's job that follows, or NULL for `launch`
} LaunchOptions;

// The save `--save` stands for.
typedef struct SaveTarget {
	char name[SAVES_NAME_LENGTH]; // as the harness reports it
	char path[PATH_LENGTH];       // its .fm file, absolute
	char date[SAVES_DATE_LENGTH]; // its in-game date once it is ready, "" if unknown
} SaveTarget;

// What pollGame samples: the game's process, and how long a status query may wait for another
// runner's job.
typedef struct GamePoll {
	pid_t pid;
	unsigned timeoutMs;
} GamePoll;

// A `"key" "value"` pair of a Valve KeyValues text, with the keys of the blocks it is in.
typedef struct VdfPair {
	const char (*blocks)[VDF_KEY]; // outermost first
	size_t depth;
	const char *key;
	const char *value;
} VdfPair;
typedef bool VdfVisit(void *context, const VdfPair *pair); // false to stop

// What Steam's launch option says about the game's data folder (`user_data_location`).
typedef enum OptionData {
	OPTION_DATA_NONE,    // nothing: the game uses its usual folder
	OPTION_DATA_FOUND,   // an absolute folder
	OPTION_DATA_UNCLEAR, // something fm24ctl does not follow
} OptionData;

static bool readFile(const char *path, ByteBuffer *out);
static bool contains(const ByteBuffer *text, const char *needle);
static bool steamRoot(char *out, size_t size);
static const char *vdfSkip(const char *c);
static char vdfUnescape(char c);
static const char *vdfString(const char *c, char *out, size_t size);
static int vdfToken(const char **cursor, char *out, size_t size);
static void vdfVisit(const char *text, VdfVisit *visit, void *context);
static bool vdfVisitFile(const char *path, VdfVisit *visit, void *context);
static bool libraryApp(const char *library, char *app, size_t size);
static bool currentAccount(const char *root, char *out, size_t size);
static bool readLaunchOption(char *option, size_t size);
static bool launchOptionSet(const char *launcher);
static bool optionWord(const char **cursor, char *out, size_t size);
static OptionData optionDataFolder(const char *option, char *out, size_t size);
static bool writeLauncher(const char *path, const char *dylibName);
static bool harnessListening(void);
static bool onAppleSilicon(void);
static bool
parseLaunchArguments(int argc, wchar_t **argv, bool appleSilicon, LaunchOptions *options);
static bool parseTimeout(const wchar_t *text, unsigned *seconds);
static int launchGame(const LaunchOptions *options, const char *launcher, pid_t pid);
static int ensureSave(const SessionNeed *need, pid_t pid, char *date, size_t size);
static int launchForJob(const LaunchOptions *options, SaveTarget *target);
static int useRunningGame(pid_t pid, const LaunchOptions *options, SaveTarget *target);
static bool underRosetta(pid_t pid);
static bool
runsAskedSlice(pid_t pid, const LaunchOptions *options, bool started, bool throughSteam);
static int launchWithSave(
	const char *launcher,
	bool throughSteam,
	const LaunchOptions *options,
	SaveTarget *target
);
static int startGame(
	const char *launcher,
	bool throughSteam,
	const LaunchOptions *options,
	const char *autoplay,
	pid_t *pid
);
static bool writeRosettaRequest(const char *launcher, char *path, size_t size);
static bool startThroughSteam(bool rosetta);
static bool
startDirectly(const wchar_t *given, const char *launcher, const char *autoplay, bool rosetta);
static int
waitForHarness(const char *launcher, bool throughSteam, const LaunchOptions *options, pid_t *pid);
static void reportNoHarness(const char *launcher, bool throughSteam);
static bool
findSave(const LaunchOptions *options, bool throughSteam, bool lookUp, SaveTarget *target);
static void reportMissingSave(const SaveSearch *search, const char *data, bool listed, bool forJob);
static bool findSaveFile(const LaunchOptions *options, bool throughSteam, SaveTarget *target);
static bool uniquelyNamed(const char *data, const SaveTarget *target, const struct stat *file);
static bool dataFolder(const LaunchOptions *options, bool throughSteam, char *out, size_t size);
static bool listSaves(const char *data, SavesFolder folder, SaveSearch *search);
static bool launcherFolder(const char *launcher, char *out, size_t size);
static bool autoplayPath(const char *folder, bool throughSteam, char *out, size_t size);
static bool writeAutoplay(const char *path, const char *save);
static void removeAutoplay(
	const char *folder,
	const char *file,
	bool throughSteam,
	pid_t pid,
	SessionOutcome outcome
);
static void removeStaleAutoplay(const char *directory, bool gameRunning);
static bool processRunning(pid_t pid);
static void pollGame(void *context, SessionSample *sample);
static SessionOutcome waitForSave(
	pid_t pid,
	SaveTarget *target,
	const LaunchOptions *options,
	bool launched,
	bool throughSteam
);
static void reportSave(
	SessionOutcome outcome,
	const SessionWait *wait,
	pid_t pid,
	const SaveTarget *target,
	const SessionNeed *job,
	bool throughSteam
);
static void
reportOtherSave(const GameStatus *last, const SaveTarget *target, const SessionNeed *job);
static int saveExitCode(SessionOutcome outcome, const SessionNeed *job);
static bool runProgram(char *const *arguments);
static void reportNotLaunched(const char *save);
static bool shellWord(const char *text, char *out, size_t size);
static bool launcherPath(char *out, size_t size);
static void describeMissingGame(void);
static void describeLaunchOption(const char *launcher, bool forInstall);

int macosGame_install(int argc, wchar_t **argv) {
	(void)argv;
	if (argc != 2) {
		fprintf(stderr, "usage: fm24ctl install\n");
		return 2;
	}
	char launcher[PATH_LENGTH];
	if (!macosGame_installHarness(true, launcher, sizeof launcher)) {
		return 1;
	}
	char app[PATH_LENGTH];
	if (macosGame_findInstalled(app, sizeof app)) {
		printf("Football Manager 2024: %s\n", app);
	} else {
		printf("Football Manager 2024 was not found in your Steam libraries\n");
		describeMissingGame();
	}
	char option[PATH_LENGTH];
	char data[PATH_LENGTH];
	if (launchOptionSet(launcher)) {
		printf(
			"\nSteam's launch option for Football Manager 2024 already uses this launcher: start the "
			"game "
			"from Steam as usual.\n"
		);
		if (readLaunchOption(option, sizeof option) &&
				optionDataFolder(option, data, sizeof data) == OPTION_DATA_UNCLEAR) {
			printf(
				"It sets FM24's data folder (user_data_location) in a way fm24ctl does not follow, which "
				"stops `fm24ctl launch --save` and the commands that start the game for a fixture's save: "
				"write it as --user_data_location=<absolute folder>.\n"
			);
		}
	} else {
		describeLaunchOption(launcher, true);
	}
	printf(
		"\nRun `fm24ctl install` again after updating fm24ctl: it updates the harness and the "
		"launcher.\n"
	);
	return 0;
}

int macosGame_launch(int argc, wchar_t **argv) {
	LaunchOptions options;
	if (!parseLaunchArguments(argc, argv, onAppleSilicon(), &options)) {
		fprintf(
			stderr,
			"usage: fm24ctl launch [--save <name|path.fm>] [--timeout <s>] [--user-data <dir>] "
			"[--rosetta] [path to fm.app]\n"
		);
		return 2;
	}
	char launcher[PATH_LENGTH];
	if (!macosGame_installHarness(false, launcher, sizeof launcher)) {
		return 1;
	}
	char running[PATH_LENGTH];
	return launchGame(&options, launcher, macosGame_findRunning(running, sizeof running));
}

// `launch` once the harness and `launcher` are installed, with game process `pid` running (0 if
// none). A running game's save is checked by its name, as the commands' checks do, without looking
// a name up in the save folders.
static int launchGame(const LaunchOptions *options, const char *launcher, pid_t pid) {
	bool throughSteam = !options->app && launchOptionSet(launcher);
	SaveTarget target = {0};
	if (options->save && !findSave(options, throughSteam, !pid, &target)) {
		return 1;
	}
	if (pid) {
		return useRunningGame(pid, options, &target);
	}
	if (options->save) {
		return launchWithSave(launcher, throughSteam, options, &target);
	}
	return startGame(launcher, throughSteam, options, NULL, &pid);
}

// True on Apple silicon, which sysctl also reports to a process under Rosetta.
static bool onAppleSilicon(void) {
	int arm64 = 0;
	size_t size = sizeof arm64;
	return sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64 == 1;
}

// launch's arguments: the options in any order, and at most one fm.app; --rosetta only on
// `appleSilicon`. False (with a message) if they are not.
static bool
parseLaunchArguments(int argc, wchar_t **argv, bool appleSilicon, LaunchOptions *options) {
	*options = (LaunchOptions){.timeoutS = SAVE_TIMEOUT_S};
	bool timed = false;
	for (int i = 2; i < argc; ++i) {
		const wchar_t *argument = argv[i];
		bool hasValue = i + 1 < argc;
		if (wcscmp(argument, L"--save") == 0 && hasValue) {
			options->save = argv[++i];
		} else if (wcscmp(argument, L"--user-data") == 0 && hasValue) {
			options->userData = argv[++i];
		} else if (wcscmp(argument, L"--timeout") == 0 && hasValue) {
			timed = true;
			if (!parseTimeout(argv[++i], &options->timeoutS)) {
				return false;
			}
		} else if (wcscmp(argument, L"--rosetta") == 0) {
			options->rosetta = true;
		} else if (wcsncmp(argument, L"--", 2) != 0 && !options->app) {
			options->app = argument;
		} else {
			fprintf(stderr, "unknown option, missing value or extra argument: %ls\n", argument);
			return false;
		}
	}
	if ((timed || options->userData) && !options->save) {
		fprintf(stderr, "--timeout and --user-data only apply with --save\n");
		return false;
	}
	if (options->rosetta && !appleSilicon) {
		fprintf(
			stderr, "--rosetta is for Apple silicon: this Mac runs the game's x86_64 slice natively\n"
		);
		return false;
	}
	return true;
}

// A --timeout value: whole seconds, from 1 to MAX_TIMEOUT_S.
static bool parseTimeout(const wchar_t *text, unsigned *seconds) {
	wchar_t *end = NULL;
	errno = 0;
	unsigned long value = wcstoul(text, &end, 10);
	if (errno != 0 || text[0] < L'0' || text[0] > L'9' || *end != 0 || value < 1 ||
			value > MAX_TIMEOUT_S) {
		fprintf(stderr, "--timeout must be 1-%d seconds\n", MAX_TIMEOUT_S);
		return false;
	}
	*seconds = (unsigned)value;
	return true;
}

// `launch` with the game already running as `pid`: it must have this harness, and with --save it
// must have that save loaded, which fm24ctl waits for while the game is busy.
static int useRunningGame(pid_t pid, const LaunchOptions *options, SaveTarget *target) {
	if (!harnessListening()) {
		fprintf(
			stderr,
			"Football Manager 2024 (pid %d) is already running without harness %s; quit it first\n",
			(int)pid,
			HARNESS_PROTOCOL_TEXT
		);
		return 1;
	}
	if (!runsAskedSlice(pid, options, false, false)) {
		return EXIT_WRONG_STATE;
	}
	if (options->save) {
		return saveExitCode(waitForSave(pid, target, options, false, false), options->job);
	}
	printf(
		"Football Manager 2024 (pid %d) is already running with the harness%s\n",
		(int)pid,
		options->rosetta ? ", under Rosetta" : ""
	);
	return 0;
}

// True if process `pid` runs under Rosetta; false if it does not, or has exited.
static bool underRosetta(pid_t pid) {
	struct kinfo_proc info = {0};
	size_t size = sizeof info;
	int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
	return sysctl(name, (unsigned)(sizeof name / sizeof name[0]), &info, &size, NULL, 0) == 0 &&
				 size == sizeof info && (info.kp_proc.p_flag & P_TRANSLATED) != 0;
}

// False (with a message) if --rosetta asks for the x86_64 slice and game `pid` does not run under
// Rosetta: a game that was already running, which fm24ctl never restarts, or one it has just
// `started`, `throughSteam` or directly.
static bool
runsAskedSlice(pid_t pid, const LaunchOptions *options, bool started, bool throughSteam) {
	if (!options->rosetta || underRosetta(pid)) {
		return true;
	}
	if (!started) {
		fprintf(
			stderr,
			"Football Manager 2024 (pid %d) is already running natively, not under Rosetta; fm24ctl "
			"will not restart a running game. Quit it, then run this again.\n",
			(int)pid
		);
		return false;
	}
	fprintf(
		stderr,
		"Football Manager 2024 (pid %d) started natively, not under Rosetta: %s. Quit it, then run "
		"this again.\n",
		(int)pid,
		throughSteam ? "the launcher did not take the request; Steam must start the game within "
									 "five minutes, through the launcher in its launch option"
								 : "the game may have restarted through Steam"
	);
	return false;
}

// Starts the game to load `target`, waits until it has, and removes the auto-play files it leaves.
// Through Steam the launcher takes the request while this fm24ctl waits for it (see
// writeLauncher); a direct start passes the file itself. Either is named after fm24ctl's process,
// so removeStaleAutoplay also clears it once fm24ctl has exited (a direct start's once no game
// runs either). startGame handles --rosetta's.
static int launchWithSave(
	const char *launcher,
	bool throughSteam,
	const LaunchOptions *options,
	SaveTarget *target
) {
	char folder[PATH_LENGTH];
	char file[PATH_LENGTH];
	if (!launcherFolder(launcher, folder, sizeof folder) ||
			!autoplayPath(folder, throughSteam, file, sizeof file)) {
		fprintf(stderr, "the auto-play file's path is too long\n");
		return 1;
	}
	if (!writeAutoplay(file, target->path)) {
		return 1;
	}
	printf("loading %s: %s\n", target->name, target->path);
	pid_t pid = 0;
	int status = startGame(launcher, throughSteam, options, file, &pid);
	SessionOutcome outcome =
		status == 0 ? waitForSave(pid, target, options, true, throughSteam) : SESSION_EXITED;
	removeAutoplay(folder, file, throughSteam, pid, outcome);
	return status != 0 ? status : saveExitCode(outcome, options->job);
}

int macosGame_ensureSave(const SessionNeed *need, char *date, size_t size) {
	char running[PATH_LENGTH];
	return ensureSave(need, macosGame_findRunning(running, sizeof running), date, size);
}

// macosGame_ensureSave with the running game's process ID, 0 if there is none.
static int ensureSave(const SessionNeed *need, pid_t pid, char *date, size_t size) {
	if (date && size) {
		date[0] = 0;
	}
	SessionPlan plan = session_plan(need, pid != 0, pid && harnessListening());
	if (plan == SESSION_PLAN_SUBMIT) {
		if (!pid && !need->launch && need->save && need->save[0]) {
			reportNotLaunched(need->save);
		}
		return 0;
	}
	wchar_t save[SAVES_NAME_LENGTH];
	LaunchOptions options = {.save = save, .timeoutS = SAVE_TIMEOUT_S, .job = need};
	SaveTarget target = {0};
	if (!platform_fromUtf8(need->save, save, SAVES_NAME_LENGTH)) {
		fprintf(stderr, "bad save name: %s\n", need->save);
		return 1;
	}
	(void)snprintf(target.name, sizeof target.name, "%s", need->save);
	int status = plan == SESSION_PLAN_CHECK ? useRunningGame(pid, &options, &target)
																					: launchForJob(&options, &target);
	if (status == 0 && date) {
		(void)snprintf(date, size, "%s", target.date);
	}
	return status;
}

// Starts the game, which is not running, with the save a command's job is for.
static int launchForJob(const LaunchOptions *options, SaveTarget *target) {
	char launcher[PATH_LENGTH];
	if (!macosGame_installHarness(false, launcher, sizeof launcher)) {
		return 1;
	}
	bool throughSteam = launchOptionSet(launcher);
	if (!findSave(options, throughSteam, true, target)) {
		return 1;
	}
	printf(
		"Football Manager 2024 is not running; starting it to load %s (--no-launch turns this off)\n",
		target->name
	);
	return launchWithSave(launcher, throughSteam, options, target);
}

// Starts the game through Steam, or directly with `autoplay` (if given) as its auto-play file, and
// waits for the harness (see waitForHarness). With --rosetta a direct start opens the x86_64 slice;
// through Steam fm24ctl leaves a request for it next to the launcher, named after its own process,
// which the launcher takes as it takes a save's (see writeLauncher) and which goes once the wait is
// over (or, if fm24ctl is interrupted, through removeStaleAutoplay).
static int startGame(
	const char *launcher,
	bool throughSteam,
	const LaunchOptions *options,
	const char *autoplay,
	pid_t *pid
) {
	char request[PATH_LENGTH] = "";
	if (throughSteam && options->rosetta && !writeRosettaRequest(launcher, request, sizeof request)) {
		return 1;
	}
	bool started = throughSteam ? startThroughSteam(options->rosetta)
															: startDirectly(options->app, launcher, autoplay, options->rosetta);
	int status = started ? waitForHarness(launcher, throughSteam, options, pid) : 1;
	if (request[0]) {
		(void)unlink(request);
	}
	return status;
}

// Writes the request for the x86_64 slice next to `launcher`, rosetta-request-<pid>, and its path
// to `path`. Prints why and leaves none on failure.
static bool writeRosettaRequest(const char *launcher, char *path, size_t size) {
	static const char text[] = "x86_64\n";
	char folder[PATH_LENGTH];
	wchar_t wide[HARNESS_PATH_LENGTH];
	int written = launcherFolder(launcher, folder, sizeof folder)
									? snprintf(path, size, "%s/%s%d", folder, rosettaRequestPrefix, (int)getpid())
									: -1;
	if (written <= 0 || (size_t)written >= size) {
		fprintf(stderr, "the Rosetta request's path is too long\n");
		path[0] = 0;
		return false;
	}
	if (!platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) ||
			!files_write(wide, text, sizeof text - 1)) {
		fprintf(stderr, "cannot write %s\n", path);
		(void)unlink(path);
		path[0] = 0;
		return false;
	}
	return true;
}

// Steam's launch option runs the launcher, which loads the harness.
static bool startThroughSteam(bool rosetta) {
	printf(
		"starting Football Manager 2024 through Steam (its launch option loads the harness%s)\n",
		rosetta ? " and starts the x86_64 slice under Rosetta" : ""
	);
	char *const arguments[] = {"/usr/bin/open", "steam://rungameid/2252570", NULL};
	return runProgram(arguments);
}

// Opens fm.app (`given`, or the Steam install's) with the dylib next to `launcher` inserted, with
// `autoplay`, if given, as the game's auto-play file, and its x86_64 slice if `rosetta`.
static bool
startDirectly(const wchar_t *given, const char *launcher, const char *autoplay, bool rosetta) {
	char app[PATH_LENGTH];
	char path[PATH_LENGTH];
	if (given) {
		if (!paths_toUtf8(given, path, sizeof path) || !macosGame_bundle(path, app, sizeof app)) {
			fprintf(stderr, "%ls is not Football Manager 2024's fm.app\n", given);
			return false;
		}
	} else if (!macosGame_findInstalled(app, sizeof app)) {
		fprintf(stderr, "cannot find Football Manager 2024; pass the path to its fm.app\n");
		return false;
	}
	char inserted[PATH_LENGTH + 32];
	const char *slash = strrchr(launcher, '/');
	int directory = slash ? (int)(slash - launcher) : 0;
	int written = snprintf(
		inserted,
		sizeof inserted,
		"DYLD_INSERT_LIBRARIES=%.*s/fm24-harness-%s.dylib",
		directory,
		launcher,
		HARNESS_PROTOCOL_TEXT
	);
	char autoplayArgument[PATH_LENGTH + 32] = "";
	int autoplayWritten =
		autoplay
			? snprintf(autoplayArgument, sizeof autoplayArgument, "--auto_play_game_file=%s", autoplay)
			: 0;
	if (written <= 0 || (size_t)written >= sizeof inserted || autoplayWritten < 0 ||
			(size_t)autoplayWritten >= sizeof autoplayArgument) {
		fprintf(stderr, "the harness path is too long\n");
		return false;
	}
	printf(
		"%s, which has not been verified: the game crashed in its one attempt (`fm24ctl install` says "
		"how to set Steam's launch option)\n",
		given ? "Opening the given fm.app directly, without Steam's launch option"
					: "Steam's launch option for Football Manager 2024 is not set, or Steam has not saved it "
						"yet: opening fm.app directly"
	);
	printf("starting %s with the harness%s\n", app, rosetta ? ", x86_64 under Rosetta" : "");
	// SteamAppId tells the game's Steam client library it was started for its own app, so it
	// connects to the running Steam client instead of asking Steam to restart it. open's --arch
	// picks the slice LaunchServices starts, as "Open using Rosetta" does.
	char *arguments[14] = {
		"/usr/bin/open",
		"-n",
		"--env",
		inserted,
		"--env",
		"SteamAppId=2252570",
		"--env",
		"SteamGameId=2252570",
	};
	size_t count = 8;
	if (rosetta) {
		arguments[count++] = "--arch";
		arguments[count++] = "x86_64";
	}
	arguments[count++] = app;
	if (autoplay) {
		arguments[count++] = "--args";
		arguments[count] = autoplayArgument;
	}
	return runProgram(arguments);
}

// What waitForHarness has seen of the starting game.
typedef struct LaunchWatch {
	time_t started;
	time_t reported; // the last "still waiting" line
	time_t gameSeen; // when a game process first appeared
	time_t lastSeen; // when one was last running
	time_t readyAt;  // since when readyPid has served the harness socket
	pid_t readyPid;
	bool wasReady;
	bool rosetta; // the game starts under Rosetta, which may first translate it
} LaunchWatch;

typedef enum LaunchState {
	LAUNCH_WAITING,
	LAUNCH_READY,
	LAUNCH_NO_HARNESS,
	LAUNCH_EXITED,
	LAUNCH_NEVER_STARTED,
} LaunchState;

// The launch's state after a poll at `now` found game process `pid` (0 if none), serving the
// harness socket if `serving`. The harness serves as soon as the game loads it, before the game has
// started Steam, so a game that keeps serving for SETTLE_S is ready; one that quits meanwhile is
// not. Rosetta translates the game before it runs on its first start after an update, so a game
// under it has longer to load the harness.
static LaunchState launchState(LaunchWatch *watch, pid_t pid, bool serving, time_t now) {
	time_t grace = watch->rosetta ? ROSETTA_GRACE_S : NO_HARNESS_GRACE_S;
	if (serving) {
		if (pid != watch->readyPid) {
			printf("the harness is loaded; checking that the game keeps running...\n");
			watch->readyPid = pid;
			watch->readyAt = now;
		}
		watch->wasReady = true;
		if (now - watch->readyAt >= SETTLE_S) {
			return LAUNCH_READY;
		}
	} else {
		watch->readyPid = 0;
	}
	if (pid) {
		watch->gameSeen = watch->gameSeen ? watch->gameSeen : now;
		watch->lastSeen = now;
		return !watch->readyPid && now - watch->gameSeen > grace ? LAUNCH_NO_HARNESS : LAUNCH_WAITING;
	}
	if (watch->lastSeen) {
		return now - watch->lastSeen > EXIT_GRACE_S ? LAUNCH_EXITED : LAUNCH_WAITING;
	}
	return now - watch->started >= START_TIMEOUT_S ? LAUNCH_NEVER_STARTED : LAUNCH_WAITING;
}

// Polls the game once.
static LaunchState watchLaunch(LaunchWatch *watch, time_t now) {
	char running[PATH_LENGTH];
	pid_t pid = macosGame_findRunning(running, sizeof running);
	return launchState(watch, pid, pid && harnessListening(), now);
}

// Reports how the launch that waitForHarness watched ended, in `state` with what `watch` saw, and
// returns its exit code: failure also if the ready game does not run the slice --rosetta asks for.
// Writes the ready game's process ID to `pid`.
static int launchExitCode(
	LaunchState state,
	const LaunchWatch *watch,
	const char *launcher,
	bool throughSteam,
	const LaunchOptions *options,
	pid_t *pid
) {
	switch (state) {
		case LAUNCH_READY:
			*pid = watch->readyPid;
			if (!runsAskedSlice(watch->readyPid, options, true, throughSteam)) {
				return 1;
			}
			printf(
				"the harness is ready in Football Manager 2024 (pid %d%s)%s\n",
				(int)watch->readyPid,
				options->rosetta ? ", under Rosetta" : "",
				options->save ? ""
											: ". Load your save and leave the game idle before running fm24ctl commands."
			);
			return 0;
		case LAUNCH_EXITED:
			fprintf(
				stderr,
				"Football Manager 2024 exited %s.%s See harness.log next to the dylib, and any crash "
				"report in ~/Library/Logs/DiagnosticReports.\n",
				watch->wasReady ? "soon after loading the harness" : "before the harness started",
				throughSteam ? "" : " If Steam was not running, start it first."
			);
			return 1;
		case LAUNCH_NEVER_STARTED:
			fprintf(
				stderr,
				"Football Manager 2024 did not start within %d minutes%s\n",
				START_TIMEOUT_S / 60,
				throughSteam ? "; check Steam for an update or an error" : ""
			);
			return 1;
		default: reportNoHarness(launcher, throughSteam); return 1;
	}
}

// Waits until the started game serves this protocol's socket, and writes its process ID to `pid`.
// Returns the exit code (see launchExitCode). fm24ctl waits for --save's save next.
static int
waitForHarness(const char *launcher, bool throughSteam, const LaunchOptions *options, pid_t *pid) {
	printf(
		"waiting for the game to start the harness%s...\n",
		options->rosetta ? " (a first start under Rosetta after an update translates the game first, "
											 "which can take minutes)"
										 : ""
	);
	time_t now = time(NULL);
	LaunchWatch watch = {.started = now, .reported = now, .rosetta = options->rosetta};
	LaunchState state = LAUNCH_WAITING;
	while ((state = watchLaunch(&watch, now)) == LAUNCH_WAITING) {
		if (now - watch.reported >= PROGRESS_INTERVAL_S) {
			printf("still waiting (%lld s)...\n", (long long)(now - watch.started));
			watch.reported = now;
		}
		platform_sleep(500);
		now = time(NULL);
	}
	return launchExitCode(state, &watch, launcher, throughSteam, options, pid);
}

// After the game ran for a while without the harness: why, and how to start it with the harness.
static void reportNoHarness(const char *launcher, bool throughSteam) {
	fprintf(
		stderr,
		"Football Manager 2024 is running without the harness%s. Quit it first: the harness loads only "
		"when the game starts. See harness.log next to the dylib if the harness loaded.\n",
		throughSteam ? ": Steam did not start it through the launch option"
								 : ": Steam may have restarted it"
	);
	if (throughSteam) {
		// Starting through Steam again would fail the same way; opening fm.app does not use Steam's
		// option.
		char app[PATH_LENGTH];
		printf(
			"\nSteam's launch option for Football Manager 2024 should be:\n  /bin/sh \"%s\" %%command%%\n"
			"Or start the game with the harness without it, which has not been verified: fm24ctl launch "
			"\"%s\"\n",
			launcher,
			macosGame_findInstalled(app, sizeof app) ? app : "<path to fm.app>"
		);
	} else {
		describeLaunchOption(launcher, false);
	}
}

// The save --save names: its .fm file, or a save's name, looked up in FM24's save folders if
// `lookUp` (a running game's save is only compared by name). `throughSteam` if Steam starts the
// game, whose launch option may give it another data folder.
static bool
findSave(const LaunchOptions *options, bool throughSteam, bool lookUp, SaveTarget *target) {
	char given[PATH_LENGTH];
	if (!paths_toUtf8(options->save, given, sizeof given) || !given[0]) {
		fprintf(stderr, "bad save name: %ls\n", options->save);
		return false;
	}
	if (!saves_isName(given)) {
		return findSaveFile(options, throughSteam, target);
	}
	if (!lookUp) {
		int written = snprintf(target->name, sizeof target->name, "%s", given);
		if (written <= 0 || (size_t)written >= sizeof target->name) {
			fprintf(stderr, "the save's name is too long: %s\n", given);
			return false;
		}
		return true;
	}
	char data[PATH_LENGTH];
	if (!dataFolder(options, throughSteam, data, sizeof data)) {
		return false;
	}
	SaveSearch search;
	saves_startSearch(&search, given);
	bool listed = false;
	for (int folder = 0; folder < SAVES_FOLDER_COUNT; ++folder) {
		listed = listSaves(data, (SavesFolder)folder, &search) || listed;
	}
	SavesFolder folder = SAVES_CLOUD;
	switch (saves_result(&search, &folder)) {
		case SAVES_FOUND:
			if (!saves_path(data, folder, given, target->path, sizeof target->path) ||
					!saves_nameOfPath(target->path, target->name, sizeof target->name)) {
				fprintf(stderr, "the save's path is too long\n");
				return false;
			}
			return true;
		case SAVES_AMBIGUOUS:
			fprintf(
				stderr,
				"%s/%s and %s/%s both have a save named %s; %s\n",
				data,
				saves_folderName(SAVES_CLOUD),
				data,
				saves_folderName(SAVES_LOCAL),
				given,
				options->job
					? "load the one you mean with fm24ctl launch --save <path>, then run this again"
					: "pass the path of the one to load"
			);
			return false;
		default: break;
	}
	reportMissingSave(&search, data, listed, options->job != NULL);
	return false;
}

// Why `search` found no save in `data`, and the saves whose names start with the name. `listed` if
// a save folder could be read; `forJob` for a command's job, which has no --user-data.
static void
reportMissingSave(const SaveSearch *search, const char *data, bool listed, bool forJob) {
	const char *unread = forJob
												 ? " (neither folder exists or can be read)"
												 : " (neither folder exists or can be read: pass FM24's data folder with "
													 "--user-data)";
	fprintf(
		stderr,
		"no save named %s in %s/%s or %s/%s%s\n",
		search->name,
		data,
		saves_folderName(SAVES_CLOUD),
		data,
		saves_folderName(SAVES_LOCAL),
		listed ? "" : unread
	);
	if (search->similarCount) {
		fprintf(stderr, "saves whose names start with it: %s", search->similar);
		if (search->similarListed < search->similarCount) {
			fprintf(stderr, " and %zu more", search->similarCount - search->similarListed);
		}
		fputc('\n', stderr);
	}
}

// --save's .fm file, which must exist, and must not share its name with another save in FM24's
// save folders: fm24ctl tells saves apart by name.
static bool findSaveFile(const LaunchOptions *options, bool throughSteam, SaveTarget *target) {
	wchar_t absolute[HARNESS_PATH_LENGTH];
	if (!paths_absolute(options->save, absolute, HARNESS_PATH_LENGTH) ||
			!paths_toUtf8(absolute, target->path, sizeof target->path)) {
		fprintf(stderr, "bad path: %ls\n", options->save);
		return false;
	}
	size_t length = strlen(target->path);
	struct stat status;
	if (length < 4 || platform_compareIgnoringCase(target->path + length - 3, ".fm") != 0) {
		fprintf(stderr, "%s is not a save: --save takes a save's name or its .fm file\n", target->path);
		return false;
	}
	if (stat(target->path, &status) != 0 || !S_ISREG(status.st_mode)) {
		fprintf(stderr, "cannot find the save %s\n", target->path);
		return false;
	}
	if (!saves_nameOfPath(target->path, target->name, sizeof target->name)) {
		fprintf(stderr, "the save's name is too long: %s\n", target->path);
		return false;
	}
	char data[PATH_LENGTH];
	return dataFolder(options, throughSteam, data, sizeof data) &&
				 uniquelyNamed(data, target, &status);
}

// False (with a message) if a save in `data`'s save folders has `target`'s name but is not its
// file, `file`. One of FM24's own saves passes.
static bool uniquelyNamed(const char *data, const SaveTarget *target, const struct stat *file) {
	SaveSearch search;
	saves_startSearch(&search, target->name);
	char other[PATH_LENGTH] = "";
	for (int folder = 0; folder < SAVES_FOLDER_COUNT; ++folder) {
		char path[PATH_LENGTH];
		struct stat status;
		if (!listSaves(data, (SavesFolder)folder, &search) || !search.found[folder] ||
				!saves_path(data, (SavesFolder)folder, target->name, path, sizeof path) ||
				stat(path, &status) != 0) {
			continue;
		}
		if (status.st_dev == file->st_dev && status.st_ino == file->st_ino) {
			return true;
		}
		if (!other[0]) {
			(void)snprintf(other, sizeof other, "%s", path);
		}
	}
	if (other[0]) {
		fprintf(
			stderr,
			"%s has the name of the save %s, and fm24ctl tells saves apart by name: give the copy a "
			"name of its own\n",
			target->path,
			other
		);
		return false;
	}
	return true;
}

// FM24's data folder: --user-data, else the one Steam's launch option gives the game if Steam
// starts it (`throughSteam`), else the usual one in the home folder.
static bool dataFolder(const LaunchOptions *options, bool throughSteam, char *out, size_t size) {
	wchar_t absolute[HARNESS_PATH_LENGTH];
	if (options->userData) {
		bool ok = paths_absolute(options->userData, absolute, HARNESS_PATH_LENGTH) &&
							paths_toUtf8(absolute, out, size);
		if (!ok) {
			fprintf(stderr, "bad path: %ls\n", options->userData);
		}
		return ok;
	}
	char option[PATH_LENGTH];
	OptionData given = throughSteam && readLaunchOption(option, sizeof option)
											 ? optionDataFolder(option, out, size)
											 : OPTION_DATA_NONE;
	if (given == OPTION_DATA_FOUND) {
		return true;
	}
	if (given == OPTION_DATA_UNCLEAR) {
		fprintf(
			stderr,
			"Steam's launch option for FM24 sets its data folder (user_data_location) in a way fm24ctl "
			"does not follow; %s\n",
			options->job ? "load the save with fm24ctl launch --save <name> --user-data <dir>, then run "
										 "this again"
									 : "pass the folder with --user-data"
		);
		return false;
	}
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): fm24ctl has one thread
	int written = home ? snprintf(out, size, "%s/%s", home, dataFolderPath) : -1;
	if (written <= 0 || (size_t)written >= size) {
		fprintf(stderr, "cannot find FM24's data folder; pass it with --user-data\n");
		return false;
	}
	return true;
}

// Adds each regular file in one of `data`'s save folders to the search. False if the folder
// cannot be read.
static bool listSaves(const char *data, SavesFolder folder, SaveSearch *search) {
	char path[PATH_LENGTH];
	int written = snprintf(path, sizeof path, "%s/%s", data, saves_folderName(folder));
	DIR *directory = written > 0 && (size_t)written < sizeof path ? opendir(path) : NULL;
	if (!directory) {
		return false;
	}
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
		char file[PATH_LENGTH];
		struct stat status;
		bool regular = entry->d_type == DT_REG;
		if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
			written = snprintf(file, sizeof file, "%s/%s", path, entry->d_name);
			regular = written > 0 && (size_t)written < sizeof file && stat(file, &status) == 0 &&
								S_ISREG(status.st_mode);
		}
		if (regular) {
			saves_note(search, folder, entry->d_name);
		}
	}
	closedir(directory);
	return true;
}

// The folder `launcher` is in.
static bool launcherFolder(const char *launcher, char *out, size_t size) {
	const char *slash = strrchr(launcher, '/');
	int written = snprintf(out, size, "%.*s", slash ? (int)(slash - launcher) : 0, launcher);
	return written > 0 && (size_t)written < size;
}

// The auto-play file fm24ctl writes in `folder` to load a save, named after its own process: the
// request the launcher takes through Steam, or the file a direct start passes.
static bool autoplayPath(const char *folder, bool throughSteam, char *out, size_t size) {
	const char *prefix = throughSteam ? autoplayRequestPrefix : autoplayPrefix;
	int written = snprintf(out, size, "%s/%s%d.json", folder, prefix, (int)getpid());
	return written > 0 && (size_t)written < size;
}

// Writes the auto-play file that loads the save at `save` to `path`, or leaves none.
static bool writeAutoplay(const char *path, const char *save) {
	char json[2 * PATH_LENGTH + 32];
	wchar_t wide[HARNESS_PATH_LENGTH];
	if (!saves_autoplayJson(save, json, sizeof json)) {
		fprintf(stderr, "the save's path cannot go into an auto-play file: %s\n", save);
		return false;
	}
	if (!platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) ||
			!files_write(wide, json, strlen(json))) {
		fprintf(stderr, "cannot write %s\n", path);
		(void)unlink(path);
		return false;
	}
	return true;
}

// After launchWithSave wrote `file` in `folder` and the wait for game `pid` (0 if none started)
// ended in `outcome`: deletes the request the launcher did not take, so that no later start loads
// the save, and the game's copy, unless the game may still be loading it.
static void removeAutoplay(
	const char *folder,
	const char *file,
	bool throughSteam,
	pid_t pid,
	SessionOutcome outcome
) {
	char copy[PATH_LENGTH];
	int written = -1;
	if (throughSteam) {
		(void)unlink(file);
		if (pid) {
			written = snprintf(copy, sizeof copy, "%s/%s%d.json", folder, autoplayPrefix, (int)pid);
		}
	} else {
		written = snprintf(copy, sizeof copy, "%s", file);
	}
	if (outcome != SESSION_TIMED_OUT && written > 0 && (size_t)written < sizeof copy) {
		(void)unlink(copy);
	}
}

// Deletes the auto-play and Rosetta files in `directory` that no running process needs: requests
// (autoplay-request-<pid>.json, rosetta-request-<pid>) of an fm24ctl that has exited, and other
// files (autoplay-<pid>.json, rosetta-<pid>) of a game, launcher or fm24ctl that has. While a game
// runs (`gameRunning`), auto-play files stay: an fm24ctl that started the game directly may have
// been ended (the viewer's Cancel) before the game read its file at the start screen.
static void removeStaleAutoplay(const char *directory, bool gameRunning) {
	// Each prefix comes before the shorter one it starts with.
	static const char *const kinds[][2] = {
		{autoplayRequestPrefix, ".json"},
		{autoplayPrefix, ".json"},
		{rosettaRequestPrefix, ""},
		{rosettaPrefix, ""},
	};
	size_t kindCount = sizeof kinds / sizeof kinds[0];
	DIR *folder = opendir(directory);
	if (!folder) {
		return;
	}
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(folder); entry; entry = readdir(folder)) {
		const char *name = entry->d_name;
		size_t kind = 0;
		while (kind < kindCount && strncmp(name, kinds[kind][0], strlen(kinds[kind][0])) != 0) {
			++kind;
		}
		if (kind == kindCount || (gameRunning && kinds[kind][0] == autoplayPrefix)) {
			continue;
		}
		const char *number = name + strlen(kinds[kind][0]);
		size_t digits = strspn(number, "0123456789");
		if (!digits || digits > 9 || strcmp(number + digits, kinds[kind][1]) != 0 ||
				processRunning((pid_t)strtol(number, NULL, 10))) {
			continue;
		}
		char path[PATH_LENGTH];
		int written = snprintf(path, sizeof path, "%s/%s", directory, name);
		if (written > 0 && (size_t)written < sizeof path) {
			(void)unlink(path);
		}
	}
	closedir(folder);
}

static bool processRunning(pid_t pid) {
	return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

// Samples the game for waitForSave (`context` is a GamePoll).
static void pollGame(void *context, SessionSample *sample) {
	const GamePoll *game = context;
	sample->running = processRunning(game->pid);
	sample->answer = sample->running && harnessListening()
										 ? session_query(&sample->status, game->timeoutMs)
										 : SESSION_ANSWER_NONE;
}

// Waits until game `pid` has `target` loaded and idle, for at most `options->timeoutS`, and says
// how it went; once it has, `target->date` is the save's in-game date. `launched` if fm24ctl has
// just started it to load the save, `throughSteam` or directly.
static SessionOutcome waitForSave(
	pid_t pid,
	SaveTarget *target,
	const LaunchOptions *options,
	bool launched,
	bool throughSteam
) {
	SessionWait wait;
	// A command's status queries wait for another runner's job as long as its own job would.
	GamePoll game = {pid, options->job ? options->job->timeoutMs : CLIENT_STATUS_TIMEOUT_MS};
	session_start(&wait, target->name, launched, options->timeoutS, time(NULL));
	if (launched) {
		printf("waiting for it to load %s (at most %u s)...\n", target->name, options->timeoutS);
	}
	SessionOutcome outcome = session_waitReady(&wait, pollGame, &game);
	if (outcome == SESSION_READY) {
		(void)snprintf(target->date, sizeof target->date, "%s", wait.last.date);
	}
	reportSave(outcome, &wait, pid, target, options->job, throughSteam);
	return outcome;
}

// What the wait came to, for `launch` or before a command's `job`.
static void reportSave(
	SessionOutcome outcome,
	const SessionWait *wait,
	pid_t pid,
	const SaveTarget *target,
	const SessionNeed *job,
	bool throughSteam
) {
	const GameStatus *last = &wait->last;
	switch (outcome) {
		case SESSION_READY:
			if (!last->save[0]) {
				fprintf(
					stderr,
					"the harness cannot read which save FM24 has loaded; taking it to be %s\n",
					target->name
				);
			}
			// A job that can go ahead at once follows without a word.
			if (!job || wait->launched || wait->waited) {
				printf(
					"ready: %s %sloaded (pid %d%s%s)\n",
					target->name,
					wait->launched ? "" : "is already ",
					(int)pid,
					last->date[0] ? ", " : "",
					last->date
				);
			}
			break;
		case SESSION_OTHER_SAVE: reportOtherSave(last, target, job); break;
		case SESSION_AT_MENU:
			fprintf(
				stderr,
				"FM24 (pid %d) is at its start screen with no save loaded: load %s in FM24, or quit FM24 "
				"and run this again.\n",
				(int)pid,
				target->name
			);
			break;
		case SESSION_NO_AUTOPLAY:
			fprintf(
				stderr,
				"FM24 reached its start screen without the auto-play file, so %s was not loaded. %s\n",
				target->name,
				throughSteam ? "Steam must start the game within five minutes, through the launcher in its "
											 "launch option (see `fm24ctl install`)."
										 : "The game may have restarted through Steam, which drops its arguments; set "
											 "Steam's launch option (see `fm24ctl install`) and run this again."
			);
			break;
		case SESSION_NOT_LOADED:
			fprintf(
				stderr,
				"FM24 read the auto-play file but did not load %s; check the game window and "
				"harness.log.\n",
				target->name
			);
			break;
		case SESSION_NO_TEAM:
			fprintf(
				stderr,
				"FM24 has %s loaded, but the harness finds no human manager's team.\n",
				last->save[0] ? last->save : target->name
			);
			break;
		case SESSION_EXITED:
			fprintf(
				stderr,
				"Football Manager 2024 exited before %s was ready. See harness.log next to the dylib, and "
				"any crash report in ~/Library/Logs/DiagnosticReports.\n",
				target->name
			);
			break;
		case SESSION_TIMED_OUT:
			fprintf(
				stderr,
				"%s%s after %u s, without %s ready. Check the game window%s.\n",
				wait->answered ? "FM24 was still " : "the harness had not answered",
				wait->answered ? saves_phaseName(last->phase) : "",
				wait->timeoutS,
				target->name,
				job ? "" : ", or run this again with a longer --timeout"
			);
			break;
		case SESSION_REFUSED:
			// session_query printed the harness's answer.
			if (job) {
				fprintf(stderr, "cannot check which save FM24 has loaded; going ahead\n");
			}
			break;
		default: break;
	}
}

// Another save is loaded and idle: `launch` and a command stop, unless --any-save lets the
// command's job use it.
static void
reportOtherSave(const GameStatus *last, const SaveTarget *target, const SessionNeed *job) {
	if (job && job->anySave) {
		printf(
			"FM24 has '%s' loaded, not '%s'; using it, as --any-save asks\n", last->save, target->name
		);
		return;
	}
	fprintf(
		stderr,
		"FM24 has '%s' loaded, not '%s'; fm24ctl will not switch saves in a running game (unsaved "
		"progress). Quit FM24 (saving first only if you loaded that save yourself and have done no "
		"harness work since), then run this again%s.\n",
		last->save,
		target->name,
		job ? ", or pass --any-save to use the loaded save" : ""
	);
}

// The exit code once the wait has ended in `outcome`: 0 if `job`, if any, can be submitted.
static int saveExitCode(SessionOutcome outcome, const SessionNeed *job) {
	if (job ? session_submits(job, outcome) : outcome == SESSION_READY) {
		return 0;
	}
	switch (outcome) {
		case SESSION_OTHER_SAVE:
		case SESSION_AT_MENU:
		case SESSION_NO_TEAM:
		case SESSION_TIMED_OUT: return EXIT_WRONG_STATE;
		default: return 1;
	}
}

bool macosGame_installHarness(bool verbose, char *launcher, size_t size) {
	wchar_t dylib[HARNESS_PATH_LENGTH];
	char directory[PATH_LENGTH];
	if (!embedded_directory(directory, sizeof directory) ||
			!embedded_dllPath(dylib, HARNESS_PATH_LENGTH)) {
		return false;
	}
	// A development build's dylib next to fm24ctl takes precedence over the built-in one.
	wchar_t here[HARNESS_PATH_LENGTH];
	wchar_t loose[HARNESS_PATH_LENGTH];
	ByteBuffer bytes = {0};
	bool fromBuild = paths_executableDirectory(here, HARNESS_PATH_LENGTH) &&
									 paths_join(loose, HARNESS_PATH_LENGTH, here, HARNESS_DLL_NAME) &&
									 files_read(loose, &bytes) && bytes.length > 0;
	bool ok = fromBuild ? embedded_write(dylib, bytes.data, bytes.length) : embedded_extract(dylib);
	buffer_free(&bytes);
	int written = snprintf(launcher, size, "%s/%s", directory, MACOS_GAME_LAUNCHER);
	if (!ok || written <= 0 || (size_t)written >= size) {
		return false;
	}
	char name[64];
	(void)snprintf(name, sizeof name, "fm24-harness-%s.dylib", HARNESS_PROTOCOL_TEXT);
	if (!writeLauncher(launcher, name)) {
		return false;
	}
	char running[PATH_LENGTH];
	removeStaleAutoplay(directory, macosGame_findRunning(running, sizeof running) != 0);
	if (verbose) {
		printf(
			"installed harness %s%s: %s/%s\nlauncher: %s\nlog: %s/harness.log\n",
			HARNESS_PROTOCOL_TEXT,
			fromBuild ? " (the development build next to fm24ctl)" : "",
			directory,
			name,
			launcher,
			directory
		);
	}
	return true;
}

pid_t macosGame_findRunning(char *path, size_t size) {
	int capacity = proc_listallpids(NULL, 0);
	if (capacity <= 0) {
		return 0;
	}
	capacity += 64;
	pid_t *pids = calloc((size_t)capacity, sizeof *pids);
	if (!pids) {
		return 0;
	}
	int count = proc_listallpids(pids, capacity * (int)sizeof *pids);
	pid_t found = 0;
	char candidate[PROC_PIDPATHINFO_MAXSIZE];
	for (int i = 0; i < count && !found; ++i) {
		if (pids[i] > 0 && proc_pidpath(pids[i], candidate, sizeof candidate) > 0 &&
				macosGame_isGameExecutable(candidate) && strlen(candidate) < size) {
			strcpy(path, candidate);
			found = pids[i];
		}
	}
	free(pids);
	return found;
}

// Looks for the game in each library folder libraryfolders.vdf lists ("libraryfolders" > N >
// "path").
typedef struct LibrarySearch {
	char *app;
	size_t size;
	bool found;
} LibrarySearch;

static bool visitLibrary(void *context, const VdfPair *pair) {
	LibrarySearch *search = context;
	if (pair->depth == 2 && strcasecmp(pair->key, "path") == 0) {
		search->found = libraryApp(pair->value, search->app, search->size);
	}
	return !search->found;
}

bool macosGame_findInstalled(char *app, size_t size) {
	char root[PATH_LENGTH];
	char path[PATH_LENGTH];
	if (!steamRoot(root, sizeof root)) {
		return false;
	}
	int written = snprintf(path, sizeof path, "%s/steamapps/libraryfolders.vdf", root);
	LibrarySearch search = {app, size, false};
	if (written > 0 && (size_t)written < sizeof path) {
		(void)vdfVisitFile(path, visitLibrary, &search);
	}
	return search.found || libraryApp(root, app, size);
}

bool macosGame_bundle(const char *path, char *app, size_t size) {
	size_t length = strlen(path);
	while (length > 1 && path[length - 1] == '/') {
		--length;
	}
	size_t suffix = sizeof executableSuffix - 1;
	if (length > suffix && strncmp(path + length - suffix, executableSuffix, suffix) == 0) {
		length -= suffix;
	}
	char executable[PATH_LENGTH];
	int written = snprintf(app, size, "%.*s", (int)length, path);
	int executableWritten = snprintf(executable, sizeof executable, "%s%s", app, executableSuffix);
	return written > 0 && (size_t)written < size && executableWritten > 0 &&
				 (size_t)executableWritten < sizeof executable && macosGame_isGameExecutable(executable);
}

bool macosGame_isGameExecutable(const char *path) {
	size_t length = strlen(path);
	size_t suffix = sizeof executableSuffix - 1;
	if (length <= suffix || strcmp(path + length - suffix, executableSuffix) != 0) {
		return false;
	}
	char plist[PATH_LENGTH];
	int written =
		snprintf(plist, sizeof plist, "%.*s/Contents/Info.plist", (int)(length - suffix), path);
	ByteBuffer info = {0};
	bool ok = written > 0 && (size_t)written < sizeof plist && readFile(plist, &info) &&
						contains(&info, bundleIdentifier);
	buffer_free(&info);
	return ok;
}

static bool readFile(const char *path, ByteBuffer *out) {
	wchar_t wide[HARNESS_PATH_LENGTH];
	return platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) && files_read(wide, out);
}

static bool contains(const ByteBuffer *text, const char *needle) {
	size_t length = strlen(needle);
	for (size_t i = 0; i + length <= text->length; ++i) {
		if (memcmp(text->data + i, needle, length) == 0) {
			return true;
		}
	}
	return false;
}

static bool steamRoot(char *out, size_t size) {
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): fm24ctl has one thread
	int written = home ? snprintf(out, size, "%s/Library/Application Support/Steam", home) : -1;
	return written > 0 && (size_t)written < size;
}

// Skips white space and // comments.
static const char *vdfSkip(const char *c) {
	for (;;) {
		while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') {
			++c;
		}
		if (c[0] != '/' || c[1] != '/') {
			return c;
		}
		while (*c && *c != '\n') {
			++c;
		}
	}
}

static char vdfUnescape(char c) {
	switch (c) {
		case 'n': return '\n';
		case 't': return '\t';
		default: return c;
	}
}

// Copies a string token at `c` (quoted, or bare up to white space, a brace or a quote) into `out`,
// unescaped and cut to fit. Returns the position after it.
static const char *vdfString(const char *c, char *out, size_t size) {
	size_t length = 0;
	bool quoted = *c == '"';
	for (c += quoted; *c && (quoted ? *c != '"' : !strchr(" \t\r\n{}\"", *c)); ++c) {
		char next = *c;
		if (quoted && next == '\\' && c[1]) {
			next = vdfUnescape(*++c);
		}
		if (length + 1 < size) {
			out[length++] = next;
		}
	}
	out[length] = 0;
	return c + (quoted && *c == '"');
}

// Reads the next token of a Valve KeyValues text at `*cursor`: 's' for a string (unescaped into
// `out`, which must have room, cut to fit), '{', '}', '[' for a platform condition, or 0 at the
// end.
static int vdfToken(const char **cursor, char *out, size_t size) {
	const char *c = vdfSkip(*cursor);
	int kind = *c == '{' || *c == '}' || *c == '[' ? *c : *c ? 's' : 0;
	out[0] = 0;
	*cursor = kind == '{' || kind == '}' ? c + 1 : kind ? vdfString(c, out, size) : c;
	return kind;
}

// Calls `visit` for each `"key" "value"` pair of a Valve KeyValues text, in order, until it returns
// false.
static void vdfVisit(const char *text, VdfVisit *visit, void *context) {
	char blocks[VDF_DEPTH][VDF_KEY];
	char key[VDF_KEY];
	char value[PATH_LENGTH];
	size_t depth = 0;
	size_t skipped = 0; // blocks deeper than VDF_DEPTH
	bool keyed = false;
	const char *cursor = text;
	for (int kind = vdfToken(&cursor, value, sizeof value); kind;
		kind = vdfToken(&cursor, value, sizeof value)) {
		if (kind == 's' && !keyed) {
			(void)snprintf(key, sizeof key, "%s", value);
			keyed = true;
		} else if (kind == 's') {
			VdfPair pair = {blocks, depth, key, value};
			if (!skipped && !visit(context, &pair)) {
				return;
			}
			keyed = false;
		} else if (kind == '{') {
			if (skipped || depth == VDF_DEPTH) {
				++skipped;
			} else {
				(void)snprintf(blocks[depth++], VDF_KEY, "%s", keyed ? key : "");
			}
			keyed = false;
		} else if (kind == '}') {
			if (skipped) {
				--skipped;
			} else if (depth) {
				--depth;
			}
			keyed = false;
		}
	}
}

static bool vdfVisitFile(const char *path, VdfVisit *visit, void *context) {
	ByteBuffer text = {0};
	bool read = readFile(path, &text);
	if (read) {
		vdfVisit(buffer_string(&text), visit, context);
	}
	buffer_free(&text);
	return read;
}

// The value of a top-level block's `key` ("AppState" > "installdir").
typedef struct VdfLookup {
	const char *key;
	char *out;
	size_t size;
	bool found;
} VdfLookup;

static bool visitTopLevel(void *context, const VdfPair *pair) {
	VdfLookup *lookup = context;
	if (pair->depth == 1 && strcasecmp(pair->key, lookup->key) == 0) {
		int written = snprintf(lookup->out, lookup->size, "%s", pair->value);
		lookup->found = written > 0 && (size_t)written < lookup->size;
	}
	return !lookup->found;
}

// The fm.app of app 2252570 in one Steam library folder, if it is installed there.
static bool libraryApp(const char *library, char *app, size_t size) {
	char manifest[PATH_LENGTH];
	int written =
		snprintf(manifest, sizeof manifest, "%s/steamapps/appmanifest_%s.acf", library, steamAppId);
	char installDirectory[512];
	VdfLookup lookup = {"installdir", installDirectory, sizeof installDirectory, false};
	if (written <= 0 || (size_t)written >= sizeof manifest ||
			!vdfVisitFile(manifest, visitTopLevel, &lookup) || !lookup.found) {
		return false;
	}
	char executable[PATH_LENGTH];
	written = snprintf(app, size, "%s/steamapps/common/%s/fm.app", library, installDirectory);
	int executableWritten = snprintf(executable, sizeof executable, "%s%s", app, executableSuffix);
	return written > 0 && (size_t)written < size && executableWritten > 0 &&
				 (size_t)executableWritten < sizeof executable && macosGame_isGameExecutable(executable);
}

// The Steam account that signed in last, from loginusers.vdf ("users" > SteamID64 > "MostRecent"
// or the latest "Timestamp").
typedef struct AccountSearch {
	unsigned long long best; // a SteamID64
	unsigned long long bestTimestamp;
	bool mostRecent;
} AccountSearch;

static bool visitLoginUser(void *context, const VdfPair *pair) {
	AccountSearch *search = context;
	if (pair->depth != 2 || strcasecmp(pair->blocks[0], "users") != 0) {
		return true;
	}
	unsigned long long id = strtoull(pair->blocks[1], NULL, 10);
	if (strcasecmp(pair->key, "MostRecent") == 0 && strcmp(pair->value, "1") == 0) {
		search->best = id;
		search->mostRecent = true;
	} else if (strcasecmp(pair->key, "Timestamp") == 0 && !search->mostRecent) {
		unsigned long long timestamp = strtoull(pair->value, NULL, 10);
		if (timestamp >= search->bestTimestamp) {
			search->best = id;
			search->bestTimestamp = timestamp;
		}
	}
	return true;
}

// The userdata folder name (account ID) of the Steam account whose settings apply: the only one,
// else the one that signed in last.
static bool currentAccount(const char *root, char *out, size_t size) {
	char users[PATH_LENGTH];
	int written = snprintf(users, sizeof users, "%s/userdata", root);
	DIR *directory = written > 0 && (size_t)written < sizeof users ? opendir(users) : NULL;
	if (!directory) {
		return false;
	}
	int accounts = 0;
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
		if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9' &&
				strspn(entry->d_name, "0123456789") == strlen(entry->d_name) && accounts++ == 0) {
			(void)snprintf(out, size, "%s", entry->d_name);
		}
	}
	closedir(directory);
	if (accounts <= 1) {
		return accounts == 1;
	}
	char logins[PATH_LENGTH];
	written = snprintf(logins, sizeof logins, "%s/config/loginusers.vdf", root);
	AccountSearch search = {0};
	if (written <= 0 || (size_t)written >= sizeof logins ||
			!vdfVisitFile(logins, visitLoginUser, &search) || search.best <= steamIdBase) {
		return false;
	}
	written = snprintf(out, size, "%llu", search.best - steamIdBase);
	return written > 0 && (size_t)written < size;
}

// The launch option of app 2252570 ("apps" > "2252570" > "LaunchOptions").
static bool visitLaunchOption(void *context, const VdfPair *pair) {
	VdfLookup *lookup = context;
	if (pair->depth >= 2 && strcasecmp(pair->key, "LaunchOptions") == 0 &&
			strcmp(pair->blocks[pair->depth - 1], steamAppId) == 0 &&
			strcasecmp(pair->blocks[pair->depth - 2], "apps") == 0) {
		int written = snprintf(lookup->out, lookup->size, "%s", pair->value);
		lookup->found = written > 0 && (size_t)written < lookup->size;
		return false;
	}
	return true;
}

// The current Steam account's launch option for Football Manager 2024, if it has one.
static bool readLaunchOption(char *option, size_t size) {
	char root[PATH_LENGTH];
	char account[64];
	char config[PATH_LENGTH];
	if (!steamRoot(root, sizeof root) || !currentAccount(root, account, sizeof account)) {
		return false;
	}
	int written =
		snprintf(config, sizeof config, "%s/userdata/%s/config/localconfig.vdf", root, account);
	VdfLookup lookup = {"LaunchOptions", option, size, false};
	return written > 0 && (size_t)written < sizeof config &&
				 vdfVisitFile(config, visitLaunchOption, &lookup) && lookup.found;
}

// True if the current Steam account gives Football Manager 2024 a launch option that runs
// `launcher` with the game's command.
static bool launchOptionSet(const char *launcher) {
	char option[PATH_LENGTH];
	return readLaunchOption(option, sizeof option) && strstr(option, launcher) &&
				 strstr(option, "%command%");
}

bool macosGame_launchOptionSet(const char *launcher) {
	return launchOptionSet(launcher);
}

// The next word of a launch option at `*cursor`, split and unquoted as a shell does: white space
// separates words, '...' and "..." quote, and a backslash escapes the next character (in "...",
// only ", \, $ and `). `out` must have room for the whole option. False at the end.
static bool optionWord(const char **cursor, char *out, size_t size) {
	const char *c = *cursor + strspn(*cursor, " \t\r\n");
	if (!*c) {
		*cursor = c;
		return false;
	}
	size_t length = 0;
	char quote = 0;
	for (; *c && (quote || !strchr(" \t\r\n", *c)); ++c) {
		char next = *c;
		if (quote != '\'' && next == '\\' && c[1] && (!quote || strchr("\"\\$`", c[1]))) {
			next = *++c;
		} else if (quote ? next == quote : next == '"' || next == '\'') {
			quote = quote ? 0 : next;
			continue;
		}
		if (length + 1 < size) {
			out[length++] = next;
		}
	}
	out[length] = 0;
	*cursor = c;
	return true;
}

// The data folder that Steam's launch option `option` gives the game, as the game reads its
// arguments: `-user_data_location=<dir>` or `--user_data_location=<dir>`. Any other mention of the
// setting, a folder that is not absolute or two different folders are OPTION_DATA_UNCLEAR.
static OptionData optionDataFolder(const char *option, char *out, size_t size) {
	static const char setting[] = "user_data_location=";
	char word[PATH_LENGTH];
	OptionData found = OPTION_DATA_NONE;
	for (const char *cursor = option; optionWord(&cursor, word, sizeof word);) {
		if (!strstr(word, "user_data_location")) {
			continue;
		}
		const char *name = word + strspn(word, "-");
		const char *value = name + sizeof setting - 1;
		bool named = name > word && name <= word + 2 && strncmp(name, setting, sizeof setting - 1) == 0;
		if (!named || value[0] != '/' || (found == OPTION_DATA_FOUND && strcmp(out, value) != 0)) {
			return OPTION_DATA_UNCLEAR;
		}
		int written = snprintf(out, size, "%s", value);
		if (written <= 0 || (size_t)written >= size) {
			return OPTION_DATA_UNCLEAR;
		}
		found = OPTION_DATA_FOUND;
	}
	return found;
}

// The script Steam's launch option runs: it starts the game (passed as fm.app or its executable,
// with any arguments) with the harness, after any libraries Steam inserts itself (its overlay).
// `launch` leaves requests for the game it starts, named after its own process: a save to load
// (autoplay-request-<pid>.json, --save) and the x86_64 slice (rosetta-request-<pid>, --rosetta).
// The script takes each one under five minutes old whose fm24ctl is still running, and so still
// waiting for it, once: it renames it after its own process ID, which the game keeps. It adds the
// save's to the game's arguments as its auto-play file, and deletes the other. Any other request
// changes nothing.
// On Apple silicon, Steam starts the script as an Intel process, and a plain exec would run the
// game's x86_64 slice under Rosetta too. arch runs the arm64 slice instead (or x86_64 if the game
// had none), in the same process, so Steam still tracks the game; a Rosetta request runs x86_64.
// The script asks for arm64 only where sysctl reports it, which it also does under Rosetta: arch
// on an Intel Mac rejects the name. As a system binary, arch loses DYLD_* from its own environment
// (as /bin/sh does), so -e hands the libraries on.
static bool writeLauncher(const char *path, const char *dylibName) {
	char script[2 * PATH_LENGTH];
	int written = snprintf(
		script,
		sizeof script,
		"#!/bin/sh\n"
		"# Written by fm24ctl install: starts Football Manager 2024 with the FM24 match-engine "
		"harness.\n"
		"# Steam runs it through the game's launch option:  /bin/sh \"%s\" %%command%%\n"
		"here=$(cd \"$(dirname \"$0\")\" && pwd)\n"
		"game=$1\n"
		"[ $# -gt 0 ] && shift\n"
		"case $game in\n"
		"\t*.app | *.app/) game=\"${game%%/}/Contents/MacOS/fm\" ;;\n"
		"esac\n"
		"# Takes a request ($1<pid>$2) left by an `fm24ctl launch` that is still waiting for it, if\n"
		"# under five minutes old: renamed $3<this pid>$2, it is used once.\n"
		"take() {\n"
		"\tfor request in \"$here/$1\"*\"$2\"; do\n"
		"\t\trequester=${request##*/\"$1\"}\n"
		"\t\trequester=${requester%%\"$2\"}\n"
		"\t\tcase $requester in\n"
		"\t\t\t'' | *[!0-9]*) continue ;;\n"
		"\t\tesac\n"
		"\t\tif kill -0 \"$requester\" 2>/dev/null &&\n"
		"\t\t\t[ -n \"$(/usr/bin/find \"$request\" -type f -mmin -5 2>/dev/null)\" ] &&\n"
		"\t\t\t/bin/mv -f \"$request\" \"$here/$3$$$2\" 2>/dev/null; then\n"
		"\t\t\treturn 0\n"
		"\t\tfi\n"
		"\tdone\n"
		"\treturn 1\n"
		"}\n"
		"# A save to load (launch --save), as the game's auto-play file.\n"
		"if take %s .json %s; then\n"
		"\tset -- \"$@\" \"--auto_play_game_file=$here/%s$$.json\"\n"
		"fi\n"
		"libraries=\"${STEAM_DYLD_INSERT_LIBRARIES:+$STEAM_DYLD_INSERT_LIBRARIES:}$here/%s\"\n"
		"# The x86_64 slice under Rosetta (launch --rosetta), else arm64 where there is Apple "
		"silicon.\n"
		"if take %s '' %s; then\n"
		"\t/bin/rm -f \"$here/%s$$\"\n"
		"elif [ \"$(/usr/sbin/sysctl -n hw.optional.arm64 2>/dev/null)\" = 1 ]; then\n"
		"\texec /usr/bin/arch -arch arm64 -arch x86_64 -e \"DYLD_INSERT_LIBRARIES=$libraries\" "
		"\"$game\" \"$@\"\n"
		"fi\n"
		"exec /usr/bin/arch -arch x86_64 -e \"DYLD_INSERT_LIBRARIES=$libraries\" \"$game\" \"$@\"\n",
		path,
		autoplayRequestPrefix,
		autoplayPrefix,
		autoplayPrefix,
		dylibName,
		rosettaRequestPrefix,
		rosettaPrefix,
		rosettaPrefix
	);
	wchar_t wide[HARNESS_PATH_LENGTH];
	return written > 0 && (size_t)written < sizeof script &&
				 platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) &&
				 embedded_write(wide, script, (size_t)written);
}

// True if a running game serves this protocol's harness socket: its harness holds the socket's
// lock. A socket file left by a game that crashed or was force-quit does not count.
static bool harnessListening(void) {
	char path[PATH_LENGTH];
	struct stat status;
	return channel_socketPath(path, sizeof path) && channel_served(path) &&
				 stat(path, &status) == 0 && S_ISSOCK(status.st_mode);
}

static bool runProgram(char *const *arguments) {
	pid_t child = 0;
	int error = posix_spawn(&child, arguments[0], NULL, NULL, arguments, environ);
	int status = 0;
	if (error != 0 || waitpid(child, &status, 0) < 0 || !WIFEXITED(status) ||
			WEXITSTATUS(status) != 0) {
		fprintf(stderr, "cannot start the game (%s failed)\n", arguments[0]);
		return false;
	}
	return true;
}

// For a job for `save` that --no-launch keeps from starting the game: how to have the game load
// the save first. `launch --save` is suggested only once Steam's launch option is set, as `launch`
// would otherwise start the game directly, which has not been verified.
static void reportNotLaunched(const char *save) {
	char word[4 * SAVES_NAME_LENGTH + 3];
	char launcher[PATH_LENGTH];
	if (launcherPath(launcher, sizeof launcher) && launchOptionSet(launcher) &&
			shellWord(save, word, sizeof word)) {
		fprintf(
			stderr,
			"The job is for the save %s, and --no-launch keeps fm24ctl from starting FM24 with it: start "
			"FM24 from Steam and load that save, or run `fm24ctl launch --save %s`, then run this "
			"again; or leave out --no-launch.\n",
			save,
			word
		);
	} else {
		fprintf(
			stderr,
			"The job is for the save %s, and --no-launch keeps fm24ctl from starting FM24 with it: set "
			"Steam's launch option for the harness (`fm24ctl install` says how), start FM24 from Steam "
			"and load that save, then run this again.\n",
			save
		);
	}
}

// `text` as one word for a shell command to paste: as it is if no character in it needs quoting,
// else in single quotes, with each ' in it written as '\''. False if `out` is too small.
static bool shellWord(const char *text, char *out, size_t size) {
	static const char plain[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.,+:@%/";
	bool quoted = !text[0] || strspn(text, plain) != strlen(text);
	size_t length = 0;
	if (quoted && length < size) {
		out[length++] = '\'';
	}
	for (const char *c = text; *c && length < size; ++c) {
		if (*c == '\'' && size - length > 4) {
			memcpy(out + length, "'\\''", 4);
			length += 4;
		} else if (*c == '\'') {
			return false;
		} else {
			out[length++] = *c;
		}
	}
	if (quoted && length < size) {
		out[length++] = '\'';
	}
	if (length >= size) {
		return false;
	}
	out[length] = 0;
	return true;
}

// Where macosGame_installHarness writes the launcher, without installing anything.
static bool launcherPath(char *out, size_t size) {
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): fm24ctl has one thread
	int written =
		home && home[0] == '/'
			? snprintf(
					out, size, "%s/Library/Application Support/fm24-harness/%s", home, MACOS_GAME_LAUNCHER
				)
			: -1;
	return written > 0 && (size_t)written < size;
}

// Where `install` looked for the game it did not find, and what to do.
static void describeMissingGame(void) {
	char root[PATH_LENGTH];
	struct stat status;
	if (!steamRoot(root, sizeof root)) {
		return;
	}
	if (stat(root, &status) != 0 || !S_ISDIR(status.st_mode)) {
		printf(
			"Steam is not in %s, where fm24ctl looks for it: install Steam and Football Manager 2024 "
			"(24.4.2), then run `fm24ctl install` again.\n",
			root
		);
	} else {
		printf(
			"Install Football Manager 2024 (24.4.2) in Steam, then run `fm24ctl install` again; fm24ctl "
			"looks in the Steam libraries that %s/steamapps/libraryfolders.vdf lists.\n",
			root
		);
	}
}

// How to set Steam's launch option, after the account's current one if it has another; for
// `install`, also how to check that Steam has saved it, and that `fm24ctl launch` starts the game
// directly until then.
static void describeLaunchOption(const char *launcher, bool forInstall) {
	char current[PATH_LENGTH];
	if (readLaunchOption(current, sizeof current) && current[0]) {
		printf("\nSteam's launch option for Football Manager 2024 is now: %s\n", current);
	}
	printf(
		"\nTo start Football Manager 2024 from Steam with the harness, set its launch option once:\n"
		"  Steam > Library > Football Manager 2024 > Properties > General > Launch Options:\n"
		"  /bin/sh \"%s\" %%command%%\n%s",
		launcher,
		forInstall
			? "Keep any arguments of your own after %command%. Steam may save the option only when it "
				"quits: run `fm24ctl install` again until it says that Steam's launch option uses this "
				"launcher, quitting Steam if it does not. Until then, `fm24ctl launch` starts the game "
				"without the launch option, which has not been verified: the game crashed in its one "
				"attempt.\n"
			: ""
	);
}
