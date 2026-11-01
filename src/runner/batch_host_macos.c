// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The batch's host on macOS: the game through the harness's socket and macos_game.c, and the
// computer through programs of its own: /usr/bin/gzip compresses chunks after they are verified,
// so the game never does, caffeinate keeps the computer awake while the batch runs, pmset tells
// battery from AC power and osascript posts notifications. Each runs in a process group of its
// own, so that Ctrl-C in the terminal reaches only fm24ctl, whose handler decides what it means,
// and with only its standard input, output and error.
// FM24's file-list error logs are trimmed in the data folder macos_game.c looks saves up in.
// (The functions are named host... so that tests can include this file beside macos_game.c.)

#include "batch_policy.h"
#include "batch_run.h"
#include "batch_store.h"
#include "client.h"
#include "game_logs.h"
#include "macos_game.h"
#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>


extern char **environ;

enum {
	HOST_PATH_LENGTH = 4 * HARNESS_PATH_LENGTH,
	HOST_POWER_LENGTH = 4096, // what pmset -g ps prints is read up to this
	HOST_NOTIFICATION_LENGTH = 512,
};

static const char hostGzipPath[] = "/usr/bin/gzip";
static const char hostCaffeinatePath[] = "/usr/bin/caffeinate";
static const char hostPmsetPath[] = "/usr/bin/pmset";
static const char hostOsascriptPath[] = "/usr/bin/osascript";

// A decompressing gzip, and the read end of its output.
typedef struct HostDecompression {
	pid_t child;
} HostDecompression;

// The signal handler's state: how many signals have come, and the control files they create.
static volatile sig_atomic_t hostSignalCount;
static char hostStopFile[HOST_PATH_LENGTH];
static char hostStopNowFile[HOST_PATH_LENGTH];

static bool hostCompress(void *context, const wchar_t *path);
static FILE *hostOpenDecompressed(void *context, const wchar_t *path, void **handle);
static bool hostCloseDecompressed(void *context, FILE *file, void *handle);
static bool hostSyncFile(void *context, FILE *file);
static bool hostSyncDirectory(void *context, const wchar_t *path);
static bool hostFreeBytes(void *context, const wchar_t *path, uint64_t *bytes);
static time_t hostUtcNow(void *context);
static ClientOutcome hostSubmit(
	void *context,
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t size
);
static int hostEnsureSave(
	void *context,
	const SessionNeed *need,
	char *date,
	size_t size,
	SessionOutcome *outcome
);
static uint32_t hostFindGame(void *context);
static bool hostGameRunning(void *context, uint32_t pid);
static uint32_t hostGameSlice(void *context, uint32_t pid);
static bool hostHarnessListening(void *context);
static bool hostStopGame(void *context, uint32_t pid, unsigned graceS);
static uint64_t hostResidentBytes(void *context, uint32_t pid);
static BatchPower hostPowerSource(void *context);
static bool hostKeepAwake(void *context, bool onBattery);
static void hostNotify(void *context, const char *text);
static bool hostWatchSignals(void *context, const wchar_t *stop, const wchar_t *stopNow);
static unsigned hostSignals(void *context);
static uint64_t hostUptimeMs(void *context);
static uint64_t hostMonotonicMs(void *context);
static void hostSleepMs(void *context, unsigned milliseconds);
static void hostTrimGameLogs(void *context, GameLogsTrim *out);
static bool hostOnAppleSilicon(void);
static BatchPower hostReadPower(const char *text);
static bool hostNotificationScript(const char *text, char *out, size_t size);
static void hostOnSignal(int number);
static void hostSay(const char *text);
static int hostRunGzip(bool test, const char *path);
static bool hostPipe(int ends[2]);
static int hostSpawn(
	pid_t *child,
	const char *path,
	int output,
	char *const *arguments,
	char *const *environment
);
static char **hostGzipEnvironment(void);
static int hostWaitForChild(pid_t child);

void batchHost_real(BatchHost *host) {
	*host = (BatchHost){
		.compress = hostCompress,
		.openDecompressed = hostOpenDecompressed,
		.closeDecompressed = hostCloseDecompressed,
		.syncFile = hostSyncFile,
		.syncDirectory = hostSyncDirectory,
		.freeBytes = hostFreeBytes,
		.utcNow = hostUtcNow,
		.submit = hostSubmit,
		.ensureSave = hostEnsureSave,
		.findGame = hostFindGame,
		.gameRunning = hostGameRunning,
		.gameSlice = hostGameSlice,
		.harnessListening = hostHarnessListening,
		.stopGame = hostStopGame,
		.residentBytes = hostResidentBytes,
		.powerSource = hostPowerSource,
		.keepAwake = hostKeepAwake,
		.notify = hostNotify,
		.watchSignals = hostWatchSignals,
		.signals = hostSignals,
		.uptimeMs = hostUptimeMs,
		.monotonicMs = hostMonotonicMs,
		.sleepMs = hostSleepMs,
		.trimGameLogs = hostTrimGameLogs,
	};
}

// gzip -n -6 replaces the file with <file>.gz, without a name or time in it, so that the same
// bytes always compress the same; gzip -t then reads it back.
static bool hostCompress(void *context, const wchar_t *path) {
	(void)context;
	char utf8[HOST_PATH_LENGTH];
	char compressed[HOST_PATH_LENGTH + 4];
	if (!platform_toUtf8(path, utf8, sizeof utf8) ||
			snprintf(compressed, sizeof compressed, "%s.gz", utf8) <= 0) {
		fprintf(stderr, "cannot compress %ls: the path is too long\n", path);
		return false;
	}
	int compressedStatus = hostRunGzip(false, utf8);
	int tested = compressedStatus == 0 ? hostRunGzip(true, compressed) : -1;
	if (compressedStatus != 0 || tested != 0) {
		fprintf(stderr, "gzip could not %s %s\n", compressedStatus ? "compress" : "check", utf8);
		return false;
	}
	return true;
}

static FILE *hostOpenDecompressed(void *context, const wchar_t *path, void **handle) {
	(void)context;
	char utf8[HOST_PATH_LENGTH];
	char program[] = "gzip";
	char option[] = "-dc";
	int ends[2] = {-1, -1};
	HostDecompression *decompression = malloc(sizeof *decompression);
	if (!decompression || !platform_toUtf8(path, utf8, sizeof utf8) || !hostPipe(ends)) {
		free(decompression);
		fprintf(stderr, "cannot decompress %ls\n", path);
		return NULL;
	}
	char *arguments[] = {program, option, utf8, NULL};
	char **environment = hostGzipEnvironment();
	int error = environment
								? hostSpawn(&decompression->child, hostGzipPath, ends[1], arguments, environment)
								: ENOMEM;
	free(environment);
	close(ends[1]);
	FILE *file = error == 0 ? fdopen(ends[0], "rb") : NULL;
	if (!file) {
		close(ends[0]);
		if (error == 0) {
			(void)hostWaitForChild(decompression->child);
		}
		free(decompression);
		fprintf(stderr, "cannot run gzip to decompress %s\n", utf8);
		return NULL;
	}
	*handle = decompression;
	return file;
}

static bool hostCloseDecompressed(void *context, FILE *file, void *handle) {
	(void)context;
	HostDecompression *decompression = handle;
	bool ok = fclose(file) == 0;
	ok = hostWaitForChild(decompression->child) == 0 && ok;
	free(decompression);
	return ok;
}

static bool hostSyncFile(void *context, FILE *file) {
	(void)context;
	return platform_syncFile(file);
}

static bool hostSyncDirectory(void *context, const wchar_t *path) {
	(void)context;
	return platform_syncDirectory(path);
}

static bool hostFreeBytes(void *context, const wchar_t *path, uint64_t *bytes) {
	(void)context;
	return platform_freeBytes(path, bytes);
}

static time_t hostUtcNow(void *context) {
	(void)context;
	return time(NULL);
}

static ClientOutcome hostSubmit(
	void *context,
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t size
) {
	(void)context;
	return client_submitWatched(job, timeoutMs, watch, result, size);
}

// The x86_64 slice is Rosetta's only on Apple silicon; an Intel Mac runs it natively.
static int hostEnsureSave(
	void *context,
	const SessionNeed *need,
	char *date,
	size_t size,
	SessionOutcome *outcome
) {
	(void)context;
	SessionNeed adjusted = *need;
	adjusted.rosetta = need->rosetta && hostOnAppleSilicon();
	return macosGame_checkSave(&adjusted, date, size, outcome);
}

static uint32_t hostFindGame(void *context) {
	(void)context;
	char path[4096];
	pid_t pid = macosGame_findRunning(path, sizeof path);
	return pid > 0 ? (uint32_t)pid : 0;
}

static bool hostGameRunning(void *context, uint32_t pid) {
	(void)context;
	return macosGame_running((pid_t)pid);
}

static uint32_t hostGameSlice(void *context, uint32_t pid) {
	(void)context;
	return hostOnAppleSilicon() && !macosGame_underRosetta((pid_t)pid) ? BATCH_SLICE_ARM64
																																		 : BATCH_SLICE_X86_64;
}

static bool hostHarnessListening(void *context) {
	(void)context;
	return macosGame_harnessListening();
}

static bool hostStopGame(void *context, uint32_t pid, unsigned graceS) {
	(void)context;
	return macosGame_stop((pid_t)pid, graceS);
}

static uint64_t hostResidentBytes(void *context, uint32_t pid) {
	(void)context;
	return macosGame_residentBytes((pid_t)pid);
}

// What `pmset -g ps` says the computer draws from.
static BatchPower hostPowerSource(void *context) {
	(void)context;
	char program[] = "pmset";
	char get[] = "-g";
	char sources[] = "ps";
	char *arguments[] = {program, get, sources, NULL};
	int ends[2] = {-1, -1};
	if (!hostPipe(ends)) {
		return BATCH_POWER_UNKNOWN;
	}
	pid_t child = 0;
	int error = hostSpawn(&child, hostPmsetPath, ends[1], arguments, environ);
	close(ends[1]);
	char text[HOST_POWER_LENGTH];
	size_t length = 0;
	for (ssize_t got = 1; error == 0 && got > 0 && length < sizeof text - 1;) {
		got = read(ends[0], text + length, sizeof text - 1 - length);
		if (got < 0 && errno == EINTR) {
			got = 1;
		} else if (got > 0) {
			length += (size_t)got;
		}
	}
	text[length] = 0;
	close(ends[0]);
	if (error != 0 || hostWaitForChild(child) != 0) {
		return BATCH_POWER_UNKNOWN;
	}
	return hostReadPower(text);
}

// caffeinate keeps the computer from sleeping until this process exits, which it watches (-w).
// -s holds sleep off only on AC power, so that the Mac can still sleep on battery power, where the
// batch pauses; with `onBattery` (pause_on_battery=0, playing on), -i holds idle sleep off on
// battery power too. Closing the lid still sleeps it.
static bool hostKeepAwake(void *context, bool onBattery) {
	(void)context;
	char program[] = "caffeinate";
	char idle[] = "-i";
	char disk[] = "-m";
	char system[] = "-s";
	char watch[] = "-w";
	char pid[24];
	(void)snprintf(pid, sizeof pid, "%lu", platform_processId());
	char *always[] = {program, disk, system, watch, pid, NULL};
	char *battery[] = {program, idle, disk, system, watch, pid, NULL};
	pid_t child = 0;
	return hostSpawn(&child, hostCaffeinatePath, -1, onBattery ? battery : always, environ) == 0;
}

static void hostNotify(void *context, const char *text) {
	(void)context;
	char script[HOST_NOTIFICATION_LENGTH];
	if (!hostNotificationScript(text, script, sizeof script)) {
		return;
	}
	char program[] = "osascript";
	char option[] = "-e";
	char *arguments[] = {program, option, script, NULL};
	pid_t child = 0;
	if (hostSpawn(&child, hostOsascriptPath, -1, arguments, environ) == 0) {
		(void)hostWaitForChild(child);
	}
}

// SIGINT and SIGTERM: the first creates `stop`, the second `stopNow`, and the third exits at once
// with 4, leaving the job running in the game for the next batch run to adopt.
static bool hostWatchSignals(void *context, const wchar_t *stop, const wchar_t *stopNow) {
	(void)context;
	if (!platform_toUtf8(stop, hostStopFile, sizeof hostStopFile) ||
			!platform_toUtf8(stopNow, hostStopNowFile, sizeof hostStopNowFile)) {
		return false;
	}
	hostSignalCount = 0;
	struct sigaction action = {.sa_handler = hostOnSignal};
	(void)sigemptyset(&action.sa_mask);
	return sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0;
}

static unsigned hostSignals(void *context) {
	(void)context;
	return (unsigned)hostSignalCount;
}

static uint64_t hostUptimeMs(void *context) {
	(void)context;
	return platform_uptimeTicks() / (platform_tickFrequency() / 1000);
}

static uint64_t hostMonotonicMs(void *context) {
	(void)context;
	return platform_ticks() / (platform_tickFrequency() / 1000);
}

static void hostSleepMs(void *context, unsigned milliseconds) {
	(void)context;
	platform_sleep(milliseconds);
}

// The Logs folder of the data folder a batch's save is looked up in (macosGame_dataFolder), which
// FM24 writes its logs in: the usual one, or the one Steam's launch option gives the game.
static void hostTrimGameLogs(void *context, GameLogsTrim *out) {
	(void)context;
	char data[GAME_LOGS_PATH_LENGTH];
	char logs[GAME_LOGS_PATH_LENGTH];
	MacosDataFolder found = macosGame_dataFolder(data, sizeof data);
	int written = found == MACOS_DATA_FOUND ? snprintf(logs, sizeof logs, "%s/Logs", data) : -1;
	if (written <= 0 || (size_t)written >= sizeof logs) {
		(void)snprintf(
			out->failed[GAME_LOGS_FOLDER],
			sizeof out->failed[GAME_LOGS_FOLDER],
			"%s",
			found == MACOS_DATA_UNCLEAR
				? "Steam's launch option sets it (user_data_location) in a way fm24ctl does not follow"
			: found == MACOS_DATA_NO_HOME ? "HOME is not set, or too long"
																		: "its path is too long"
		);
		return;
	}
	gameLogs_trim(logs, out);
}

static bool hostOnAppleSilicon(void) {
	int arm64 = 0;
	size_t size = sizeof arm64;
	return sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64 == 1;
}

// pmset -g ps's first line: "Now drawing from 'AC Power'" or "'Battery Power'" (or 'UPS Power').
static BatchPower hostReadPower(const char *text) {
	const char *source = strstr(text, "drawing from '");
	if (!source) {
		return BATCH_POWER_UNKNOWN;
	}
	source += strlen("drawing from '");
	if (strncmp(source, "Battery Power'", 14) == 0) {
		return BATCH_POWER_BATTERY;
	}
	return strncmp(source, "AC Power'", 9) == 0 || strncmp(source, "UPS Power'", 10) == 0
					 ? BATCH_POWER_AC
					 : BATCH_POWER_UNKNOWN;
}

// The AppleScript that posts `text` as a notification from fm24ctl, with '"' and '\' escaped and
// control characters left out.
static bool hostNotificationScript(const char *text, char *out, size_t size) {
	static const char start[] = "display notification \"";
	static const char end[] = "\" with title \"fm24ctl\"";
	size_t length = sizeof start - 1;
	if (size < sizeof start + sizeof end) {
		return false;
	}
	memcpy(out, start, length);
	for (const char *c = text; *c; ++c) {
		if ((unsigned char)*c < 0x20) {
			continue;
		}
		bool escaped = *c == '"' || *c == '\\';
		if (length + 1 + escaped + sizeof end > size) {
			return false;
		}
		if (escaped) {
			out[length++] = '\\';
		}
		out[length++] = *c;
	}
	memcpy(out + length, end, sizeof end);
	return true;
}

// Only async-signal-safe calls here: open, close, write and _exit.
static void hostOnSignal(int number) {
	(void)number;
	int count = hostSignalCount + 1;
	hostSignalCount = count;
	if (count >= 3) {
		hostSay("exiting: the job keeps running in the game, and the next batch run adopts it\n");
		_exit(BATCH_EXIT_STOPPED);
	}
	int file =
		open(count == 1 ? hostStopFile : hostStopNowFile, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (file >= 0) {
		(void)close(file);
	}
	hostSay(
		count == 1 ? "stopping once this chunk is committed; again to stop sooner\n"
							 : "stopping now: matches under way finish and are kept; again to exit at once\n"
	);
}

static void hostSay(const char *text) {
	(void)!write(STDERR_FILENO, text, strlen(text));
}

// Runs gzip -n -6 on a file, or gzip -t; its exit status, or -1 if it could not run.
static int hostRunGzip(bool test, const char *path) {
	char program[] = "gzip";
	char noName[] = "-n";
	char level[] = "-6";
	char check[] = "-t";
	char file[HOST_PATH_LENGTH + 4];
	(void)snprintf(file, sizeof file, "%s", path);
	char *compressing[] = {program, noName, level, file, NULL};
	char *testing[] = {program, check, file, NULL};
	char **arguments = test ? testing : compressing;
	char **environment = hostGzipEnvironment();
	pid_t child = 0;
	int error = environment ? hostSpawn(&child, hostGzipPath, -1, arguments, environment) : ENOMEM;
	free(environment);
	if (error != 0) {
		fprintf(stderr, "cannot run %s (%s)\n", hostGzipPath, platform_errorText(error).text);
		return -1;
	}
	return hostWaitForChild(child);
}

// A pipe whose ends the programs this process starts do not inherit, above the standard
// descriptors, which they do (should this process have one of those closed).
static bool hostPipe(int ends[2]) {
	int made[2];
	if (pipe(made) != 0) {
		return false;
	}
	for (size_t i = 0; i < 2; ++i) {
		ends[i] = fcntl(made[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
		close(made[i]);
	}
	if (ends[0] < 0 || ends[1] < 0) {
		for (size_t i = 0; i < 2; ++i) {
			if (ends[i] >= 0) {
				close(ends[i]);
			}
		}
		return false;
	}
	return true;
}

// posix_spawn into a process group of the child's own. The terminal sends Ctrl-C to its
// foreground group, fm24ctl's, where it would end caffeinate (and with it keeping the computer
// awake) and a commit's gzip, while fm24ctl itself only notes the first two. SIGTTIN and SIGTTOU
// are blocked, so that a program in the background group writes to the terminal as before, and
// one that reads from it fails rather than stops. The child gets this process's standard input
// and error, its standard output or `output` (unless -1), and no other descriptor: a gzip that
// held another's pipe would keep that one from ending when its reader stops early.
static int hostSpawn(
	pid_t *child,
	const char *path,
	int output,
	char *const *arguments,
	char *const *environment
) {
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	int error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		return error;
	}
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		return error;
	}
	for (int descriptor = STDIN_FILENO; error == 0 && descriptor <= STDERR_FILENO; ++descriptor) {
		if (descriptor == STDOUT_FILENO && output >= 0) {
			error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
		} else if (fcntl(descriptor, F_GETFD) != -1) {
			error = posix_spawn_file_actions_addinherit_np(&actions, descriptor);
		}
	}
	sigset_t blocked;
	(void)sigemptyset(&blocked);
	(void)sigaddset(&blocked, SIGTTIN);
	(void)sigaddset(&blocked, SIGTTOU);
	short flags =
		(short)(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_CLOEXEC_DEFAULT);
	error = error == 0 ? posix_spawnattr_setflags(&attributes, flags) : error;
	error = error == 0 ? posix_spawnattr_setpgroup(&attributes, 0) : error;
	error = error == 0 ? posix_spawnattr_setsigmask(&attributes, &blocked) : error;
	error =
		error == 0 ? posix_spawn(child, path, &actions, &attributes, arguments, environment) : error;
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);
	return error;
}

// This process's environment for gzip, without GZIP: gzip adds the options it holds to those it is
// given, and -k (keep the file) or -S (another suffix) would undo a commit. Free it; NULL if memory
// runs out.
static char **hostGzipEnvironment(void) {
	size_t count = 0;
	while (environ[count]) {
		++count;
	}
	char **kept = malloc((count + 1) * sizeof *kept);
	size_t length = 0;
	for (size_t i = 0; kept && i < count; ++i) {
		if (strncmp(environ[i], "GZIP=", 5) != 0) {
			kept[length++] = environ[i];
		}
	}
	if (kept) {
		kept[length] = NULL;
	}
	return kept;
}

static int hostWaitForChild(pid_t child) {
	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
