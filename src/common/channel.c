// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "channel.h"

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/un.h>
#include <unistd.h>


bool channel_socketPath(char *out, size_t size) {
	char directory[1024];
	size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof directory);
	if (length == 0 || length > sizeof directory) {
		return false;
	}
	length = strlen(directory);
	const char *separator = length && directory[length - 1] == '/' ? "" : "/";
	struct sockaddr_un address;
	int written = snprintf(out, size, "%s%s%s", directory, separator, HARNESS_SOCKET_NAME);
	// The harness binds the socket as "<path>.new" first.
	return written > 0 && (size_t)written < size &&
				 (size_t)written + sizeof ".new" - 1 < sizeof address.sun_path;
}

bool channel_lockPath(const char *socketPath, char *out, size_t size) {
	int written = snprintf(out, size, "%s.lock", socketPath);
	return written > 0 && (size_t)written < size;
}

bool channel_served(const char *socketPath) {
	char lockPath[1100];
	if (!channel_lockPath(socketPath, lockPath, sizeof lockPath)) {
		return false;
	}
	int lock = open(lockPath, O_RDONLY | O_CLOEXEC);
	if (lock < 0) {
		return false;
	}
	// A shared lock is refused only while the harness holds its exclusive one. The probe's own lock
	// is released straight away (by closing), and the harness retries a refused lock briefly.
	bool held = flock(lock, LOCK_SH | LOCK_NB) != 0 && errno == EWOULDBLOCK;
	close(lock);
	return held;
}
