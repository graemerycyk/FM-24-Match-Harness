// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The macOS job channel shared by fm24ctl and the harness dylib (see protocol.h).
#pragma once

#include <stdbool.h>
#include <stddef.h>


// The job socket's path: HARNESS_SOCKET_NAME in the user's temporary directory, which the game and
// fm24ctl share. False if it cannot be resolved or does not fit a Unix socket address.
bool channel_socketPath(char *out, size_t size);

// The lock file of the socket at `socketPath` ("<socketPath>.lock"). The harness holds an exclusive
// flock on it for as long as it serves (or is about to serve) that socket.
bool channel_lockPath(const char *socketPath, char *out, size_t size);

// True if a harness holds the lock of the socket at `socketPath`: a running game serves it. A
// socket file left behind by a game that crashed or was force-quit does not count.
bool channel_served(const char *socketPath);
