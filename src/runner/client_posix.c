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

// What waitFor saw.
typedef enum Wait {
	WAIT_READY,
	WAIT_TIMED_OUT,
	WAIT_ERROR,
	WAIT_ABANDONED, // the tick stopped the wait
} Wait;

// One job's exchange with the harness: what it waits for, when it ticks, and why a step failed.
typedef struct Exchange {
	ClientWatch *watch; // NULL: no ticks
	ClientPhase phase;
	uint64_t nextTick;
	uint64_t acceptedAt; // when the harness took the job, 0 until it has
	ClientOutcome outcome;
} Exchange;

static const char waitingNotice[] =
	"the harness is running another job; waiting for it to finish\n";

static bool validResult(char *result, size_t length);
static int connectHarness(uint64_t deadline, bool *noticed, Exchange *exchange);
static bool retryConnect(int error, const char *path, uint64_t deadline, uint64_t *missingSince);
static ClientOutcome reportConnectFailure(int error, const char *path);
static int tryConnect(const struct sockaddr_un *address, int *error);
static bool ticking(const Exchange *exchange);
static bool keepWaiting(Exchange *exchange, uint64_t now);
static Wait waitFor(int socket, short events, uint64_t deadline, Exchange *exchange);
static bool waitAccepted(int socket, uint64_t deadline, bool noticed, Exchange *exchange);
static bool
sendAll(int socket, const char *data, size_t size, uint64_t deadline, Exchange *exchange);
static bool receiveAll(
	int socket,
	char *data,
	size_t capacity,
	uint64_t deadline,
	size_t *done,
	Exchange *exchange
);
static bool connectionLost(int error);
static uint64_t nowMs(void);

bool client_submit(const char *job, unsigned timeoutMs, char *result, size_t resultSize) {
	return client_submitWatched(job, timeoutMs, NULL, result, resultSize) == CLIENT_REPLIED;
}

ClientOutcome client_submitWatched(
	const char *job,
	unsigned timeoutMs,
	ClientWatch *watch,
	char *result,
	size_t resultSize
) {
	char reply[HARNESS_RESULT_LENGTH];
	if (!result) {
		result = reply;
		resultSize = sizeof reply;
	}
	if (watch) {
		watch->accepted = false;
		watch->queuedMs = 0;
		watch->runMs = 0;
	}
	if (resultSize < 2) {
		fprintf(stderr, "result buffer is too small\n");
		return CLIENT_FAILED;
	}
	result[0] = 0;
	size_t length = strlen(job);
	if (length > HARNESS_JOB_LENGTH) {
		fprintf(stderr, "the job is too long\n");
		return CLIENT_FAILED;
	}
	uint64_t started = nowMs();
	uint64_t deadline = started + timeoutMs;
	Exchange exchange = {.watch = watch, .phase = CLIENT_CONNECTING, .outcome = CLIENT_FAILED};
	exchange.nextTick = started + (watch ? watch->tickMs : 0);
	bool noticed = watch && watch->quiet;
	int connection = connectHarness(deadline, &noticed, &exchange);
	size_t count = 0;
	bool ok = connection >= 0 && waitAccepted(connection, deadline, noticed, &exchange) &&
						sendAll(connection, job, length, deadline, &exchange);
	if (ok && shutdown(connection, SHUT_WR) != 0) {
		int error = errno;
		fprintf(stderr, "cannot finish sending the job (%s)\n", platform_errorText(error).text);
		exchange.outcome = connectionLost(error) ? CLIENT_LOST : CLIENT_FAILED;
		ok = false;
	}
	ok = ok && receiveAll(connection, result, resultSize - 1, deadline, &count, &exchange);
	if (connection >= 0) {
		close(connection);
	}
	if (ok && validResult(result, count)) {
		exchange.outcome = CLIENT_REPLIED;
	} else if (ok) {
		exchange.outcome = count ? CLIENT_FAILED : CLIENT_LOST; // the harness closed without a reply
	} else {
		result[0] = 0;
	}
	if (watch) {
		uint64_t ended = nowMs();
		watch->accepted = exchange.acceptedAt != 0;
		watch->queuedMs = (watch->accepted ? exchange.acceptedAt : ended) - started;
		watch->runMs = watch->accepted ? ended - exchange.acceptedAt : 0;
	}
	return exchange.outcome;
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
static int connectHarness(uint64_t deadline, bool *noticed, Exchange *exchange) {
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
		if (!keepWaiting(exchange, nowMs())) {
			return -1;
		}
		platform_sleep(CONNECT_RETRY_MS);
		connection = tryConnect(&address, &error);
	}
	if (connection < 0) {
		exchange->outcome = reportConnectFailure(error, address.sun_path);
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

// Prints why the connection failed, and returns that outcome.
static ClientOutcome reportConnectFailure(int error, const char *path) {
	if (error != ENOENT && error != ECONNREFUSED) {
		fprintf(stderr, "cannot connect to the harness (%s)\n", platform_errorText(error).text);
		return CLIENT_FAILED;
	}
	if (!channel_served(path)) {
		(void)inject_harness(); // explains how to load it into the game, which starts it
		return CLIENT_NO_HARNESS;
	}
	if (error == ECONNREFUSED) {
		fprintf(stderr, "timed out waiting for the other job\n");
		return CLIENT_QUEUE_TIMEOUT;
	}
	fprintf(stderr, "the harness has no job socket (see harness.log); restart the game\n");
	return CLIENT_NO_HARNESS;
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

static bool ticking(const Exchange *exchange) {
	return exchange->watch && exchange->watch->tick && exchange->watch->tickMs;
}

// Ticks if a tick is due at `now`; false (CLIENT_ABANDONED) if the tick stops the wait.
static bool keepWaiting(Exchange *exchange, uint64_t now) {
	if (!ticking(exchange) || now < exchange->nextTick) {
		return true;
	}
	ClientWatch *watch = exchange->watch;
	exchange->nextTick = now + watch->tickMs;
	if (watch->tick(watch->context, exchange->phase)) {
		return true;
	}
	exchange->outcome = CLIENT_ABANDONED;
	return false;
}

// Polls for `events` until `deadline`, ticking between polls.
static Wait waitFor(int socket, short events, uint64_t deadline, Exchange *exchange) {
	for (;;) {
		uint64_t now = nowMs();
		if (!keepWaiting(exchange, now)) {
			return WAIT_ABANDONED;
		}
		if (now >= deadline) {
			return WAIT_TIMED_OUT;
		}
		uint64_t until =
			ticking(exchange) && exchange->nextTick < deadline ? exchange->nextTick : deadline;
		uint64_t left = until > now ? until - now : 0;
		struct pollfd entry = {.fd = socket, .events = events};
		int ready = poll(&entry, 1, left > 60000 ? 60000 : (int)left);
		if (ready > 0) {
			return WAIT_READY;
		}
		if (ready < 0 && errno != EINTR) {
			return WAIT_ERROR;
		}
	}
}

// Waits for the harness to take the connection, which it does once any earlier job has finished.
static bool waitAccepted(int socket, uint64_t deadline, bool noticed, Exchange *exchange) {
	exchange->phase = CLIENT_QUEUED;
	uint64_t notice = nowMs() + ACCEPT_NOTICE_MS;
	Wait ready = waitFor(socket, POLLIN, notice < deadline ? notice : deadline, exchange);
	if (ready == WAIT_TIMED_OUT && nowMs() < deadline) {
		if (!noticed) {
			fputs(waitingNotice, stderr);
		}
		ready = waitFor(socket, POLLIN, deadline, exchange);
	}
	if (ready == WAIT_ABANDONED) {
		return false;
	}
	if (ready == WAIT_TIMED_OUT) {
		fprintf(stderr, "timed out waiting for the other job\n");
		exchange->outcome = CLIENT_QUEUE_TIMEOUT;
		return false;
	}
	char accepted = 0;
	ssize_t got = ready == WAIT_READY ? read(socket, &accepted, 1) : -1;
	int error = got < 0 && ready == WAIT_READY ? errno : 0;
	if (got != 1 || accepted != HARNESS_SOCKET_ACCEPTED) {
		fprintf(stderr, "the harness did not accept the job; see harness.log\n");
		// A harness that stops serving closes the connections still waiting for it.
		exchange->outcome = got == 0 || connectionLost(error) ? CLIENT_NO_HARNESS : CLIENT_FAILED;
		return false;
	}
	exchange->phase = CLIENT_RUNNING;
	exchange->acceptedAt = nowMs();
	return true;
}

static bool
sendAll(int socket, const char *data, size_t size, uint64_t deadline, Exchange *exchange) {
	size_t sent = 0;
	while (sent < size) {
		Wait ready = waitFor(socket, POLLOUT, deadline, exchange);
		if (ready == WAIT_ABANDONED) {
			return false;
		}
		if (ready != WAIT_READY) {
			fprintf(
				stderr,
				ready == WAIT_ERROR ? "cannot write to the harness\n" : "timed out sending the job\n"
			);
			exchange->outcome = ready == WAIT_ERROR ? CLIENT_FAILED : CLIENT_RUN_TIMEOUT;
			return false;
		}
		ssize_t written = write(socket, data + sent, size - sent);
		if (written < 0 && errno != EINTR && errno != EAGAIN) {
			int error = errno;
			fprintf(
				stderr,
				"cannot write to the harness (%s); see harness.log\n",
				platform_errorText(error).text
			);
			exchange->outcome = connectionLost(error) ? CLIENT_LOST : CLIENT_FAILED;
			return false;
		}
		sent += written > 0 ? (size_t)written : 0;
	}
	return true;
}

// Reads the reply until the harness closes the connection. A timeout leaves the job running.
static bool receiveAll(
	int socket,
	char *data,
	size_t capacity,
	uint64_t deadline,
	size_t *done,
	Exchange *exchange
) {
	*done = 0;
	for (;;) {
		Wait ready = waitFor(socket, POLLIN, deadline, exchange);
		if (ready == WAIT_ABANDONED) {
			return false;
		}
		if (ready == WAIT_TIMED_OUT) {
			fprintf(stderr, "timed out; the harness may still be running\n");
			exchange->outcome = CLIENT_RUN_TIMEOUT;
			return false;
		}
		if (ready == WAIT_ERROR) {
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
			int error = errno;
			fprintf(
				stderr,
				"cannot read from the harness (%s); see harness.log\n",
				platform_errorText(error).text
			);
			exchange->outcome = connectionLost(error) ? CLIENT_LOST : CLIENT_FAILED;
			return false;
		}
		if (*done >= capacity) {
			fprintf(stderr, "the harness's result is too long\n");
			return false;
		}
		*done += (size_t)got;
	}
}

// True if `error` means the harness closed the connection (as when the game exits).
static bool connectionLost(int error) {
	return error == ECONNRESET || error == EPIPE || error == ENOTCONN;
}

static uint64_t nowMs(void) {
	return platform_ticks() / (platform_tickFrequency() / 1000);
}
