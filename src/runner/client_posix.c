// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// client.h over the macOS job socket (see protocol.h).

#include "channel.h"
#include "client.h"
#include "inject.h"
#include "platform.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>


enum {
	ACCEPT_NOTICE_MS = 500, // how long the harness may take to pick up a connection before "waiting"
	CONNECT_RETRY_MS = 200, // between attempts while the harness's backlog is full
	SOCKET_GRACE_MS = 2000, // how long a harness holding its lock may have no socket (starting up)
};

static const char waitingNotice[] =
	"the harness is running another job; waiting for it to finish\n";

static bool validResult(char *result, size_t length);
static int connectHarness(uint64_t deadline, bool *noticed);
static bool retryConnect(int error, const char *path, uint64_t deadline, uint64_t *missingSince);
static void reportConnectFailure(int error, const char *path);
static int tryConnect(const struct sockaddr_un *address, int *error);
static int waitFor(int socket, short events, uint64_t deadline);
static bool waitAccepted(int socket, uint64_t deadline, bool noticed);
static bool sendAll(int socket, const char *data, size_t size, uint64_t deadline);
static bool receiveAll(int socket, char *data, size_t capacity, uint64_t deadline, size_t *done);
static uint64_t nowMs(void);

bool client_submit(const char *job, unsigned timeoutMs, char *result, size_t resultSize) {
	char reply[HARNESS_RESULT_LENGTH];
	if (!result) {
		result = reply;
		resultSize = sizeof reply;
	}
	if (resultSize < 2) {
		fprintf(stderr, "result buffer is too small\n");
		return false;
	}
	result[0] = 0;
	size_t length = strlen(job);
	if (length > HARNESS_JOB_LENGTH) {
		fprintf(stderr, "the job is too long\n");
		return false;
	}
	uint64_t deadline = nowMs() + timeoutMs;
	bool noticed = false;
	int connection = connectHarness(deadline, &noticed);
	if (connection < 0) {
		return false;
	}
	size_t count = 0;
	bool ok =
		waitAccepted(connection, deadline, noticed) && sendAll(connection, job, length, deadline);
	if (ok && shutdown(connection, SHUT_WR) != 0) {
		fprintf(stderr, "cannot finish sending the job (%s)\n", platform_errorText(errno).text);
		ok = false;
	}
	ok = ok && receiveAll(connection, result, resultSize - 1, deadline, &count);
	close(connection);
	return ok && validResult(result, count);
}

bool client_succeeded(const char *result) {
	return strncmp(result, "ok", 2) == 0 && (result[2] == 0 || result[2] == ':');
}

// Terminates a reply of `length` bytes; false (and empty) unless it is one non-empty line.
static bool validResult(char *result, size_t length) {
	result[length] = 0;
	bool ok = length > 0 && !memchr(result, 0, length);
	size_t lineLength = strcspn(result, "\r\n");
	if (lineLength < length) {
		size_t ending = result[lineLength] == '\r' && result[lineLength + 1] == '\n' ? 2 : 1;
		ok = ok && lineLength + ending == length;
	}
	if (!ok) {
		result[0] = 0;
		fprintf(stderr, "cannot read a complete result; see harness.log\n");
	}
	return ok;
}

// Connects to the harness socket. While a live harness refuses connections (its backlog of waiting
// runners is full), retries until `deadline`. Without a live harness of this protocol, explains how
// to start the game with it. Prints why and returns -1 on failure.
static int connectHarness(uint64_t deadline, bool *noticed) {
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	if (!channel_socketPath(address.sun_path, sizeof address.sun_path)) {
		fprintf(stderr, "cannot resolve the harness socket path\n");
		return -1;
	}
	int error = 0;
	uint64_t missingSince = 0;
	int connection = tryConnect(&address, &error);
	while (connection < 0 && retryConnect(error, address.sun_path, deadline, &missingSince)) {
		if (error == ECONNREFUSED && !*noticed) {
			fputs(waitingNotice, stderr);
			*noticed = true;
		}
		platform_sleep(CONNECT_RETRY_MS);
		connection = tryConnect(&address, &error);
	}
	if (connection < 0) {
		reportConnectFailure(error, address.sun_path);
		return -1;
	}
	uid_t user = 0;
	gid_t group = 0;
	if (getpeereid(connection, &user, &group) != 0 || user != getuid()) {
		fprintf(stderr, "the harness socket belongs to another user\n");
		close(connection);
		return -1;
	}
	return connection;
}

// True to try connecting again: a live harness (holding the socket's lock) refuses connections
// while its backlog is full, and has no socket for a moment while it starts.
static bool retryConnect(int error, const char *path, uint64_t deadline, uint64_t *missingSince) {
	uint64_t now = nowMs();
	if (error == ENOENT && !*missingSince) {
		*missingSince = now;
	}
	bool passing =
		error == ECONNREFUSED || (error == ENOENT && now - *missingSince < SOCKET_GRACE_MS);
	return passing && now < deadline && channel_served(path);
}

static void reportConnectFailure(int error, const char *path) {
	if (error != ENOENT && error != ECONNREFUSED) {
		fprintf(stderr, "cannot connect to the harness (%s)\n", platform_errorText(error).text);
	} else if (!channel_served(path)) {
		(void)inject_harness(); // explains how to load it into the game, which starts it
	} else if (error == ECONNREFUSED) {
		fprintf(stderr, "timed out waiting for the other job\n");
	} else {
		fprintf(stderr, "the harness has no job socket (see harness.log); restart the game\n");
	}
}

// A connected socket, or -1 with the reason in `error`.
static int tryConnect(const struct sockaddr_un *address, int *error) {
	int connection = socket(AF_UNIX, SOCK_STREAM, 0);
	if (connection < 0) {
		*error = errno;
		return -1;
	}
	int on = 1;
	(void)setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
	if (connect(connection, (const struct sockaddr *)address, sizeof *address) != 0) {
		*error = errno;
		close(connection);
		return -1;
	}
	return connection;
}

// Polls for `events` until `deadline`; 1 if ready, 0 on timeout, -1 on error.
static int waitFor(int socket, short events, uint64_t deadline) {
	for (;;) {
		uint64_t now = nowMs();
		if (now >= deadline) {
			return 0;
		}
		uint64_t left = deadline - now;
		struct pollfd entry = {.fd = socket, .events = events};
		int ready = poll(&entry, 1, left > 60000 ? 60000 : (int)left);
		if (ready > 0) {
			return 1;
		}
		if (ready < 0 && errno != EINTR) {
			return -1;
		}
	}
}

// Waits for the harness to take the connection, which it does once any earlier job has finished.
static bool waitAccepted(int socket, uint64_t deadline, bool noticed) {
	uint64_t notice = nowMs() + ACCEPT_NOTICE_MS;
	int ready = waitFor(socket, POLLIN, notice < deadline ? notice : deadline);
	if (ready == 0 && nowMs() < deadline) {
		if (!noticed) {
			fputs(waitingNotice, stderr);
		}
		ready = waitFor(socket, POLLIN, deadline);
	}
	if (ready == 0) {
		fprintf(stderr, "timed out waiting for the other job\n");
		return false;
	}
	char accepted = 0;
	ssize_t got = ready > 0 ? read(socket, &accepted, 1) : -1;
	if (got != 1 || accepted != HARNESS_SOCKET_ACCEPTED) {
		fprintf(stderr, "the harness did not accept the job; see harness.log\n");
		return false;
	}
	return true;
}

static bool sendAll(int socket, const char *data, size_t size, uint64_t deadline) {
	size_t sent = 0;
	while (sent < size) {
		int ready = waitFor(socket, POLLOUT, deadline);
		if (ready <= 0) {
			fprintf(stderr, ready ? "cannot write to the harness\n" : "timed out sending the job\n");
			return false;
		}
		ssize_t written = write(socket, data + sent, size - sent);
		if (written < 0 && errno != EINTR && errno != EAGAIN) {
			fprintf(
				stderr,
				"cannot write to the harness (%s); see harness.log\n",
				platform_errorText(errno).text
			);
			return false;
		}
		sent += written > 0 ? (size_t)written : 0;
	}
	return true;
}

// Reads the reply until the harness closes the connection. A timeout leaves the job running.
static bool receiveAll(int socket, char *data, size_t capacity, uint64_t deadline, size_t *done) {
	*done = 0;
	for (;;) {
		int ready = waitFor(socket, POLLIN, deadline);
		if (ready == 0) {
			fprintf(stderr, "timed out; the harness may still be running\n");
			return false;
		}
		if (ready < 0) {
			fprintf(
				stderr,
				"cannot read from the harness (%s); see harness.log\n",
				platform_errorText(errno).text
			);
			return false;
		}
		char discard;
		char *target = *done < capacity ? data + *done : &discard;
		size_t room = *done < capacity ? capacity - *done : 1;
		ssize_t got = read(socket, target, room);
		if (got == 0) {
			return true;
		}
		if (got < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			fprintf(
				stderr,
				"cannot read from the harness (%s); see harness.log\n",
				platform_errorText(errno).text
			);
			return false;
		}
		if (*done >= capacity) {
			fprintf(stderr, "the harness's result is too long\n");
			return false;
		}
		*done += (size_t)got;
	}
}

static uint64_t nowMs(void) {
	return platform_ticks() / (platform_tickFrequency() / 1000);
}
