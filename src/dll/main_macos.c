// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// fm24_harness on macOS: runs Football Manager 24's own match engine headless, inside the game.
//
// Loaded when the game starts (DYLD_INSERT_LIBRARIES, see the runner's macos_game.h). Its
// initialiser starts a thread that checks the game build and serves jobs on the harness socket,
// one runner at a time (see protocol.h and job.h). Jobs must only be submitted while the game is
// idle: the engine calls are not otherwise synchronised with the game's own threads.

#include "channel.h"
#include "game.h"
#include "job.h"
#include "log.h"
#include "platform.h"
#include "protocol.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
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
static bool supported;           // game_init accepted the running build

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

// Runs before the game's own initialisers: only start the job thread here.
static void harnessLoaded(void) {
	if (!isGame()) {
		return; // a helper process that inherited DYLD_INSERT_LIBRARIES
	}
	pthread_attr_t attributes;
	pthread_t thread;
	if (pthread_attr_init(&attributes) != 0) {
		return;
	}
	(void)pthread_attr_setstacksize(&attributes, SERVER_STACK_SIZE);
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, serveJobs, NULL) != 0) {
		fputs("fm24 harness: cannot start the job thread\n", stderr);
	}
	pthread_attr_destroy(&attributes);
}

// True in the game's own process: its executable is an app bundle's Contents/MacOS/fm.
static bool isGame(void) {
	char path[4096];
	uint32_t size = sizeof path;
	static const char suffix[] = "/Contents/MacOS/fm";
	size_t length = 0;
	return _NSGetExecutablePath(path, &size) == 0 && (length = strlen(path)) >= sizeof suffix - 1 &&
				 strcmp(path + length - (sizeof suffix - 1), suffix) == 0;
}

static void *serveJobs(void *parameter) {
	(void)parameter;
	int listener = startServing();
	if (listener < 0) {
		return NULL;
	}
	unsigned pauseMs = 0;
	for (;;) {
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
	supported = game_init();
	int listener = openSocket();
	if (listener >= 0) {
		log_message(supported ? "ready; waiting for jobs" : "refusing jobs: unsupported game build");
	}
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
	} else if (!supported) {
		job_writeResult(
			"error: harness %s does not support this game build; see harness.log", HARNESS_PROTOCOL_TEXT
		);
	} else {
		text[length] = 0;
		job_execute(text);
	}
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
