// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game_logs.h"
#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


enum { NAME_LENGTH = 256 }; // a file name (NAME_MAX) and its NUL

static const char logPrefix[] = "file llist errors_";
static const char logSuffix[] = ".txt";

// A log as the folder held it when it was read.
typedef struct GameLog {
	char name[NAME_LENGTH];
	struct timespec modified;
	uint64_t bytes; // on the disk
} GameLog;

static bool readLogs(int folder, GameLogsTrim *out, GameLog **logs, size_t *count);
static int addLog(
	int folder,
	const char *name,
	GameLogsTrim *out,
	GameLog **logs,
	size_t *count,
	size_t *capacity
);
static bool newer(const GameLog *a, const GameLog *b);
static void emptyLog(int folder, const GameLog *log, GameLogsTrim *out);
static void deleteLog(int folder, const GameLog *log, GameLogsTrim *out);
static uint64_t diskBytes(const struct stat *status);
static void fail(GameLogsTrim *out, GameLogsProblem problem, int error);

bool gameLogs_isLogName(const char *name) {
	size_t length = strlen(name);
	size_t prefix = sizeof logPrefix - 1;
	size_t suffix = sizeof logSuffix - 1;
	return length >= prefix + suffix && strncmp(name, logPrefix, prefix) == 0 &&
				 strcmp(name + length - suffix, logSuffix) == 0;
}

// The folder is opened once and every entry used through it, so all of them are in that folder,
// whatever happens to its path meanwhile.
void gameLogs_trim(const char *folder, GameLogsTrim *out) {
	(void)snprintf(out->folder, sizeof out->folder, "%s", folder);
	int directory = open(folder, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory < 0) {
		if (errno != ENOENT) {
			fail(out, GAME_LOGS_READ, errno);
		}
		return;
	}
	GameLog *logs = NULL;
	size_t count = 0;
	if (readLogs(directory, out, &logs, &count) && count) {
		size_t newest = 0;
		for (size_t i = 1; i < count; ++i) {
			newest = newer(&logs[i], &logs[newest]) ? i : newest;
		}
		for (size_t i = 0; i < count; ++i) {
			if (i != newest) {
				deleteLog(directory, &logs[i], out);
			}
		}
		emptyLog(directory, &logs[newest], out);
	}
	free(logs);
	(void)close(directory);
}

// The logs directly in `folder`: regular files with a log's name, as lstat sees them. False (with
// why) if the folder cannot be read to its end; nothing is then trimmed.
static bool readLogs(int folder, GameLogsTrim *out, GameLog **logs, size_t *count) {
	int copy = fcntl(folder, F_DUPFD_CLOEXEC, 0);
	DIR *entries = copy >= 0 ? fdopendir(copy) : NULL;
	if (!entries) {
		fail(out, GAME_LOGS_READ, errno);
		if (copy >= 0) {
			(void)close(copy);
		}
		return false;
	}
	size_t capacity = 0;
	int error = 0;
	for (;;) {
		errno = 0;
		// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
		struct dirent *entry = readdir(entries);
		error = entry ? addLog(folder, entry->d_name, out, logs, count, &capacity) : errno;
		if (!entry || error) {
			break;
		}
	}
	(void)closedir(entries);
	if (error) {
		fail(out, GAME_LOGS_READ, error);
		return false;
	}
	return true;
}

// Adds the entry `name` of `folder` to `logs` if it is a log. An errno value if memory runs out,
// else 0.
static int addLog(
	int folder,
	const char *name,
	GameLogsTrim *out,
	GameLog **logs,
	size_t *count,
	size_t *capacity
) {
	size_t length = strlen(name);
	struct stat status;
	if (!gameLogs_isLogName(name) || length >= NAME_LENGTH) {
		return 0;
	}
	if (fstatat(folder, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
		if (errno != ENOENT) {
			fail(out, GAME_LOGS_READ, errno);
		}
		return 0;
	}
	if (!S_ISREG(status.st_mode)) {
		return 0;
	}
	if (*count == *capacity) {
		size_t grown = *capacity ? 2 * *capacity : 16;
		GameLog *more = realloc(*logs, grown * sizeof *more);
		if (!more) {
			return ENOMEM;
		}
		*logs = more;
		*capacity = grown;
	}
	GameLog *log = &(*logs)[(*count)++];
	memcpy(log->name, name, length + 1);
	log->modified = status.st_mtimespec;
	log->bytes = diskBytes(&status);
	return 0;
}

// Later modified, or as late with a later name (names hold the date and time).
static bool newer(const GameLog *a, const GameLog *b) {
	if (a->modified.tv_sec != b->modified.tv_sec) {
		return a->modified.tv_sec > b->modified.tv_sec;
	}
	if (a->modified.tv_nsec != b->modified.tv_nsec) {
		return a->modified.tv_nsec > b->modified.tv_nsec;
	}
	return strcmp(a->name, b->name) > 0;
}

// Truncated in place rather than deleted, as FM24 may have it open: its next write lands at the
// end if it appends, or at its own offset after a hole, which APFS does not store. Opened without
// following a link, and emptied only if it is still a regular file.
static void emptyLog(int folder, const GameLog *log, GameLogsTrim *out) {
	int file = openat(folder, log->name, O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (file < 0) {
		if (errno != ENOENT && errno != ELOOP) {
			fail(out, GAME_LOGS_EMPTY, errno);
		}
		return;
	}
	struct stat status;
	if (fstat(file, &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0) {
		if (ftruncate(file, 0) == 0) {
			++out->emptied;
			out->freedBytes += diskBytes(&status);
		} else {
			fail(out, GAME_LOGS_EMPTY, errno);
		}
	}
	(void)close(file);
}

static void deleteLog(int folder, const GameLog *log, GameLogsTrim *out) {
	if (unlinkat(folder, log->name, 0) == 0) {
		++out->deleted;
		out->freedBytes += log->bytes;
	} else if (errno != ENOENT) {
		fail(out, GAME_LOGS_DELETE, errno);
	}
}

static uint64_t diskBytes(const struct stat *status) {
	return status->st_blocks > 0 ? (uint64_t)status->st_blocks * 512U : 0;
}

// Keeps the first failure of each kind.
static void fail(GameLogsTrim *out, GameLogsProblem problem, int error) {
	if (!out->failed[problem][0]) {
		(void)snprintf(
			out->failed[problem], sizeof out->failed[problem], "%s", platform_errorText(error).text
		);
	}
}
