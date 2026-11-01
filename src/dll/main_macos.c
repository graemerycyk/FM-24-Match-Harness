// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The harness on macOS: runs the game's own match engine headless, inside the game.
//
// Loaded when the game starts (DYLD_INSERT_LIBRARIES, see the runner's macos_game.h). Its
// initialiser starts a thread that checks the game build and serves jobs on the harness socket,
// one runner at a time (see protocol.h and job.h). Jobs must only be submitted while the game is
// idle: the engine calls are not otherwise synchronised with the game's own threads.
//
// Where the engine is a plugin the game loads once started (EDITION_ENGINE_PLUGIN), the thread
// serves at once, reporting the game as starting until the plugin is loaded and accepted, and
// notes the plugin's unloading as the game quits (game.h). Between jobs, it samples the game's
// state (game_state.h). In log-only mode, which a file next to the dylib sets
// (HARNESS_LOG_ONLY_FILE), it only logs that, for a first check of a game build.

#include "channel.h"
#include "edition.h"
#include "game.h"
#if EDITION_ENGINE_PLUGIN
#include "game_state.h"
#include "info_plist.h"
#endif
#include "job.h"
#include "log.h"
#include "platform.h"
#include "protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>


enum {
	SERVER_STACK_SIZE = 16 * 1024 * 1024, // jobs run engine code on this thread
	RECEIVE_TIMEOUT_S = 60,               // a runner must send its whole job within this
	LOCK_ATTEMPTS = 40,                   // 50 ms apart: runners probe the lock for an instant
	ACCEPT_RETRY_MAX_MS = 1000,           // the longest pause after a transient accept() error
};

static wchar_t harnessDirectory[HARNESS_PATH_LENGTH]; // where the dylib and its log live
static char socketPath[sizeof(((struct sockaddr_un *)0)->sun_path)];
static atomic_int lockFile = -1; // held (exclusively locked) while this harness serves socketPath
static bool supported; // game_init (or, for an engine plugin, game_update) accepted the build

static void harnessLoaded(void) __attribute__((constructor));
static bool isGame(void);
static void *serveJobs(void *parameter);
static int startServing(void);
static bool acceptFailed(int error, unsigned *pauseMs);
static bool lockSocket(void);
static int openSocket(void);
static void stopServing(void);
static void serve(int connection);
static bool sendAll(int connection, const char *data, size_t size);
static void setDirectory(void);
#if EDITION_ENGINE_PLUGIN
static bool loggingOnly;                             // HARNESS_LOG_ONLY_FILE is next to the dylib
static GameEngineState noted = GAME_ENGINE_STARTING; // as noteEngine last logged it

static bool hasLogOnlyFile(void);
static void watchOnly(void);
static bool awaitConnection(int listener);
static void sampleGame(void);
static void noteEngine(GameEngineState engine);
static void runPluginJob(char *text);
#endif

// Runs before the game's own initialisers: only start the job thread here (and, for an engine
// plugin, watch for it).
static void harnessLoaded(void) {
	if (!isGame()) {
		return; // a helper process that inherited DYLD_INSERT_LIBRARIES
	}
#if EDITION_ENGINE_PLUGIN
	if (!game_watch()) {
		fputs(EDITION_LOWER " harness: cannot watch for the game's engine\n", stderr);
		return;
	}
#endif
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0) {
		return;
	}
	(void)pthread_attr_setstacksize(&attributes, SERVER_STACK_SIZE);
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, serveJobs, NULL) != 0) {
		fputs(EDITION_LOWER " harness: cannot start the job thread\n", stderr);
	}
	pthread_attr_destroy(&attributes);
}

// True in the game's own process: its executable is an app bundle's Contents/MacOS/<the game's>.
#if EDITION_ENGINE_PLUGIN
// Or the executable its bundle's Info.plist names, in a bundle with the game's identifier: a build
// that renamed it is then refused for its build, not left without the harness.
#endif
static bool isGame(void) {
	char path[4096];
	uint32_t size = sizeof path;
	static const char suffix[] = "/Contents/MacOS/" EDITION_GAME_EXECUTABLE;
	size_t length = 0;
#if EDITION_ENGINE_PLUGIN
	static const char folder[] = "/Contents/MacOS/";
	if (_NSGetExecutablePath(path, &size) != 0) {
		return false;
	}
	length = strlen(path);
	if (length >= sizeof suffix - 1 && strcmp(path + length - (sizeof suffix - 1), suffix) == 0) {
		return true;
	}
	const char *name = strrchr(path, '/');
	size_t bundle = name ? (size_t)(name - path) + 1 : 0; // up to <bundle>/Contents/MacOS/
	if (bundle < sizeof folder ||
			strncmp(path + bundle - (sizeof folder - 1), folder, sizeof folder - 1) != 0) {
		return false;
	}
	char app[sizeof path];
	char named[sizeof path];
	(void)snprintf(app, sizeof app, "%.*s", (int)(bundle - (sizeof folder - 1)), path);
	return infoPlist_executable(
					 app, EDITION_GAME_BUNDLE_ID, EDITION_GAME_EXECUTABLE, named, sizeof named
				 ) &&
				 strcmp(named, path) == 0;
#else
	return _NSGetExecutablePath(path, &size) == 0 && (length = strlen(path)) >= sizeof suffix - 1 &&
				 strcmp(path + length - (sizeof suffix - 1), suffix) == 0;
#endif
}

static void *serveJobs(void *parameter) {
	(void)parameter;
	int listener = startServing();
	if (listener < 0) {
		return NULL;
	}
	unsigned pauseMs = 0;
	for (;;) {
#if EDITION_ENGINE_PLUGIN
		if (!awaitConnection(listener)) {
			continue;
		}
#endif
		int connection = accept(listener, NULL, NULL);
		if (connection < 0) {
			if (acceptFailed(errno, &pauseMs)) {
				continue;
			}
			break;
		}
		(void)fcntl(connection, F_SETFD, FD_CLOEXEC); // keep it from processes the game starts
		if (pauseMs) {
			log_message("accepting jobs again");
			pauseMs = 0;
		}
		uid_t user = 0;
		gid_t group = 0;
		if (getpeereid(connection, &user, &group) == 0 && user == getuid()) {
			serve(connection);
		} else {
			log_message("refused a connection from another user");
		}
		close(connection);
	}
	// Runners now find no harness and say to restart the game, instead of waiting for this one.
	stopServing();
	close(listener);
	return NULL;
}

// Opens the log, checks the game build and opens the job socket. Returns the listener, or -1.
static int startServing(void) {
	setDirectory();
	log_open(harnessDirectory);
	log_message("harness %s loaded (pid %d)", HARNESS_PROTOCOL_TEXT, (int)getpid());
#if EDITION_ENGINE_PLUGIN
	log_message("starting: waiting for " EDITION_GAME " to load its engine, " EDITION_ENGINE_PATH);
	loggingOnly = hasLogOnlyFile();
	if (loggingOnly) {
		log_message(
			"log-only mode (%ls/" HARNESS_LOG_ONLY_FILE "): logging the engine's loading and "
			"unloading, and serving no jobs",
			harnessDirectory
		);
		watchOnly();
		return -1;
	}
	int listener = openSocket();
	if (listener >= 0) {
		log_message("serving jobs: status reports the game starting until then");
	}
	noteEngine(game_update());
#else
	supported = game_init();
	int listener = openSocket();
	if (listener >= 0) {
		log_message(supported ? "ready; waiting for jobs" : "refusing jobs: unsupported game build");
	}
#endif
	return listener;
}

// Handles a failed accept(): true to accept again (after a pause for errors that pass, such as the
// game running out of file descriptors for a moment), false if the socket is unusable.
static bool acceptFailed(int error, unsigned *pauseMs) {
	if (error == EINTR || error == ECONNABORTED) {
		return true;
	}
	if (error == EMFILE || error == ENFILE || error == ENOMEM || error == ENOBUFS) {
		if (!*pauseMs) {
			log_message("cannot accept a job connection (%s); retrying", platform_errorText(error).text);
		}
		// The connection stays queued, so accepting again at once would only spin.
		*pauseMs = *pauseMs ? *pauseMs * 2 : 100;
		*pauseMs = *pauseMs > ACCEPT_RETRY_MAX_MS ? ACCEPT_RETRY_MAX_MS : *pauseMs;
		platform_sleep(*pauseMs);
		return true;
	}
	log_message(
		"job loop stopped (%s): restart the game to use the harness", platform_errorText(error).text
	);
	return false;
}

// Takes the socket's lock, which keeps a second copy of this version (in another game process) from
// taking over the socket of the first. Runners probe it for an instant (channel_served), so a
// refusal is retried briefly before giving up.
static bool lockSocket(void) {
	char lockPath[sizeof socketPath + 8];
	if (!channel_lockPath(socketPath, lockPath, sizeof lockPath)) {
		return false;
	}
	int lock = open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	for (int attempt = 0; lock >= 0 && attempt < LOCK_ATTEMPTS; ++attempt) {
		if (flock(lock, LOCK_EX | LOCK_NB) == 0) {
			atomic_store(&lockFile, lock);
			return true;
		}
		if (errno != EWOULDBLOCK && errno != EINTR) {
			break;
		}
		platform_sleep(50);
	}
	if (lock >= 0) {
		close(lock);
	}
	return false;
}

// Creates and listens on the harness socket. It is bound under a temporary name and renamed into
// place, replacing any socket file a game that did not exit cleanly left, so a runner never finds
// no socket while the harness starts.
static int openSocket(void) {
	if (!channel_socketPath(socketPath, sizeof socketPath)) {
		log_message("cannot resolve the job socket path");
		return -1;
	}
	if (!lockSocket()) {
		log_message("another game process serves %s; staying idle", socketPath);
		return -1;
	}
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int written = snprintf(address.sun_path, sizeof address.sun_path, "%s.new", socketPath);
	int listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (listener >= 0) {
		(void)fcntl(listener, F_SETFD, FD_CLOEXEC); // keep it from processes the game starts
		// Connections inherit it, from before they are accepted: writing to a runner that has gone
		// away then fails with EPIPE instead of raising SIGPIPE, which would end the game.
		int on = 1;
		(void)setsockopt(listener, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
	}
	bool named = written > 0 && (size_t)written < sizeof address.sun_path;
	if (named) {
		(void)unlink(address.sun_path);
	}
	mode_t mask = umask(0077);
	bool bound = named && listener >= 0 &&
							 bind(listener, (const struct sockaddr *)&address, sizeof address) == 0;
	(void)umask(mask);
	if (!bound || listen(listener, SOMAXCONN) != 0 || rename(address.sun_path, socketPath) != 0) {
		log_message("cannot create the job socket %s (%s)", socketPath, platform_errorText(errno).text);
		if (bound) {
			(void)unlink(address.sun_path);
		}
		if (listener >= 0) {
			close(listener);
		}
		close(atomic_exchange(&lockFile, -1));
		return -1;
	}
	if (atexit(stopServing) != 0) {
		log_message("cannot arrange to remove the job socket at exit");
	}
	return listener;
}

// Removes the socket and releases its lock, once, while this harness still owns them: a socket that
// another game process has taken over since is left alone.
static void stopServing(void) {
	int lock = atomic_exchange(&lockFile, -1);
	if (lock >= 0) {
		(void)unlink(socketPath);
		close(lock);
	}
}

// Reads one job from a connected runner (up to its end of sending), runs it and replies with its
// result line.
static void serve(int connection) {
	static char text[HARNESS_JOB_LENGTH + 1];
	// The listener's SO_NOSIGPIPE should already be set; macOS refuses to set options on a
	// connection both ends have closed: a runner that gave up waiting for an earlier job.
	int on = 1;
	if (setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on) != 0) {
		log_message("the runner went away before its job was read");
		return;
	}
	struct timeval timeout = {.tv_sec = RECEIVE_TIMEOUT_S};
	(void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
	char accepted = HARNESS_SOCKET_ACCEPTED;
	if (!sendAll(connection, &accepted, 1)) {
		log_message("the runner went away before its job was read");
		return;
	}
	size_t length = 0;
	bool tooLong = false;
	bool ended = false;
	while (!ended) {
		char discard[256];
		bool room = length < HARNESS_JOB_LENGTH;
		ssize_t got = room ? read(connection, text + length, HARNESS_JOB_LENGTH - length)
											 : read(connection, discard, sizeof discard);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got < 0) {
			log_message("cannot read the job (%s)", platform_errorText(errno).text);
			return;
		}
		ended = got == 0;
		if (room) {
			length += (size_t)got;
		} else {
			tooLong = tooLong || got > 0;
		}
	}
	if (tooLong) {
		log_message("cannot read the job: it is too long");
		job_writeResult("error: the job is too long");
	} else if (memchr(text, 0, length)) {
		log_message("the job contains a NUL byte");
		job_writeResult("error: cannot read the job; see harness.log");
#if EDITION_ENGINE_PLUGIN
	} else {
		text[length] = 0;
		runPluginJob(text);
	}
#else
	} else if (!supported) {
		job_writeResult(
			"error: harness %s does not support this game build; see harness.log", HARNESS_PROTOCOL_TEXT
		);
	} else {
		text[length] = 0;
		job_execute(text);
	}
#endif
	const char *result = job_takeResult();
	if (!result[0]) {
		result = "error: the job reported no result; see harness.log";
	}
	if (!sendAll(connection, result, strlen(result))) {
		log_message("cannot send the job result (%s): %s", platform_errorText(errno).text, result);
	}
}

static bool sendAll(int connection, const char *data, size_t size) {
	size_t sent = 0;
	while (sent < size) {
		ssize_t written = write(connection, data + sent, size - sent);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return false;
		}
		sent += (size_t)written;
	}
	return true;
}

// The directory this dylib was loaded from (the runner installs it with its log next to it).
static void setDirectory(void) {
	Dl_info info;
	char resolved[4096];
	harnessDirectory[0] = 0;
	if (!dladdr((const void *)&harnessDirectory, &info) || !info.dli_fname ||
			!realpath(info.dli_fname, resolved)) {
		return;
	}
	char *slash = strrchr(resolved, '/');
	if (slash) {
		*slash = 0;
	}
	if (!platform_fromUtf8(resolved, harnessDirectory, HARNESS_PATH_LENGTH)) {
		harnessDirectory[0] = 0;
	}
}

#if EDITION_ENGINE_PLUGIN
static bool hasLogOnlyFile(void) {
	char directory[HARNESS_PATH_LENGTH * 4];
	char path[sizeof directory + sizeof HARNESS_LOG_ONLY_FILE + 1];
	if (!platform_toUtf8(harnessDirectory, directory, sizeof directory)) {
		return false;
	}
	int written = snprintf(path, sizeof path, "%s/%s", directory, HARNESS_LOG_ONLY_FILE);
	return written > 0 && (size_t)written < sizeof path && access(path, F_OK) == 0;
}

// Log-only mode: notes what the game loads and unloads for as long as the game runs.
static void watchOnly(void) {
	noteEngine(game_update());
	struct pollfd watch = {.fd = game_watchDescriptor(), .events = POLLIN};
	for (;;) {
		int ready = poll(&watch, 1, -1);
		if (ready < 0 && errno != EINTR) {
			log_message("cannot watch for the engine (%s)", platform_errorText(errno).text);
			return;
		}
		if (ready > 0 && !(watch.revents & POLLIN)) {
			log_message("cannot watch for the engine");
			return;
		}
		if (ready > 0) {
			noteEngine(game_update());
		}
	}
}

// Waits until a runner connects (true), noting what the game loads and unloads meanwhile and, while
// its engine is accepted, sampling the game every GAME_STATE_SAMPLE_MS. A failure to wait is left
// for accept() to report.
static bool awaitConnection(int listener) {
	struct pollfd waits[] = {
		{.fd = listener, .events = POLLIN},
		{.fd = game_watchDescriptor(), .events = POLLIN},
	};
	int ready = poll(waits, 2, supported ? GAME_STATE_SAMPLE_MS : -1);
	if (ready == 0) {
		sampleGame();
		return false;
	}
	if (ready < 0) {
		return errno != EINTR;
	}
	if (waits[1].revents) {
		noteEngine(game_update());
	}
	return waits[0].revents != 0;
}

// Samples the game's state, holding its engine as a job does.
static void sampleGame(void) {
	if (supported && game_hold()) {
		gameState_sample();
		game_release();
	}
}

// Logs what a change of the engine's state means for jobs.
static void noteEngine(GameEngineState engine) {
	static const char *const serving[] = {
		[GAME_ENGINE_ACCEPTED] = "accepted: ready; waiting for jobs",
		[GAME_ENGINE_REFUSED] = "refusing jobs: unsupported game build",
		[GAME_ENGINE_GONE] = "gone: refusing jobs until the game loads its engine again",
	};
	static const char *const logging[] = {
		[GAME_ENGINE_ACCEPTED] = "accepted",
		[GAME_ENGINE_REFUSED] = "refused: unsupported game build",
		[GAME_ENGINE_GONE] = "gone",
	};
	supported = engine == GAME_ENGINE_ACCEPTED;
	if (engine == noted) {
		return;
	}
	noted = engine;
	if (engine != GAME_ENGINE_STARTING) {
		log_message(
			loggingOnly ? "%s (log-only mode: serving no jobs)" : "%s",
			(loggingOnly ? logging : serving)[engine]
		);
	}
}

// A job once the engine is accepted runs holding it, so that the game's unloading of it waits for
// the job. Before the engine is loaded, job_execute answers status (the game is starting) and
// refuses the rest; once it is refused or unloaded, every job is refused.
static void runPluginJob(char *text) {
	GameEngineState engine = game_engineState();
	if (supported && game_hold()) {
		job_execute(text);
		game_release();
	} else if (supported || engine == GAME_ENGINE_GONE) {
		job_writeResult("error: " EDITION_PRODUCT " is quitting: it has unloaded its match engine");
	} else if (engine == GAME_ENGINE_STARTING) {
		job_execute(text);
	} else {
		job_writeResult(
			"error: harness %s does not support this game build; see harness.log", HARNESS_PROTOCOL_TEXT
		);
	}
}
#endif
