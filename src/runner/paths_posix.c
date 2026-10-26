// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "paths.h"
#include "platform.h"
#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


enum { UTF8_PATH_LENGTH = 4096 };

static bool fromSystem(const char *path, wchar_t *out, size_t size);
static bool normalise(wchar_t *path);

bool paths_executableDirectory(wchar_t *out, size_t size) {
	char raw[UTF8_PATH_LENGTH];
	char resolved[PATH_MAX];
	uint32_t length = sizeof raw;
	if (_NSGetExecutablePath(raw, &length) != 0 || !realpath(raw, resolved)) {
		return false;
	}
	char *slash = strrchr(resolved, '/');
	if (!slash) {
		return false;
	}
	*slash = 0;
	return fromSystem(resolved[0] ? resolved : "/", out, size);
}

bool paths_jobDirectory(wchar_t *out, size_t size) {
	char directory[UTF8_PATH_LENGTH];
	size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof directory);
	if (length == 0 || length > sizeof directory) {
		return false;
	}
	length = strlen(directory);
	while (length > 1 && directory[length - 1] == '/') {
		directory[--length] = 0;
	}
	return fromSystem(directory, out, size);
}

bool paths_join(wchar_t *out, size_t size, const wchar_t *directory, const wchar_t *name) {
	int written = swprintf(out, size, L"%ls/%ls", directory, name);
	return written > 0 && (size_t)written < size;
}

bool paths_absolute(const wchar_t *path, wchar_t *out, size_t size) {
	if (!path[0]) {
		return false;
	}
	if (path[0] == L'/') {
		if (wcslen(path) >= size) {
			return false;
		}
		wcscpy(out, path);
	} else {
		char cwd[UTF8_PATH_LENGTH];
		wchar_t directory[HARNESS_PATH_LENGTH];
		if (!getcwd(cwd, sizeof cwd) || !fromSystem(cwd, directory, HARNESS_PATH_LENGTH) ||
				!paths_join(out, size, directory, path)) {
			return false;
		}
	}
	return normalise(out);
}

PathsFolder paths_parentFolder(const wchar_t *path) {
	char file[UTF8_PATH_LENGTH];
	char folder[UTF8_PATH_LENGTH];
	if (!paths_toUtf8(path, file, sizeof file)) {
		return PATHS_FOLDER_UNCHECKED;
	}
	memcpy(folder, file, sizeof folder);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		return PATHS_FOLDER_UNCHECKED;
	}
	slash[slash == folder ? 1 : 0] = 0; // "/name" is in "/"
	struct stat status;
	if (stat(folder, &status) != 0) {
		switch (errno) {
			case ENOENT: return PATHS_FOLDER_MISSING;
			case ENOTDIR: return PATHS_FOLDER_NOT_FOLDER;
			case EACCES: return PATHS_FOLDER_DENIED;
			case EPERM: return PATHS_FOLDER_PRIVACY; // as for Documents without privacy access
			default: return PATHS_FOLDER_UNCHECKED;
		}
	}
	if (!S_ISDIR(status.st_mode)) {
		return PATHS_FOLDER_NOT_FOLDER;
	}
	// The game and fm24ctl write a file in place ("wb"), as the same user: an existing file needs
	// the user's permission to write it, a new one their permission to write in the folder and
	// search it (stat needed only the folders above it).
	struct stat existing;
	bool exists = stat(file, &existing) == 0;
	if (exists && S_ISDIR(existing.st_mode)) {
		return PATHS_FILE_IS_FOLDER; // which "wb" cannot open
	}
	if (access(exists ? file : folder, exists ? W_OK : W_OK | X_OK) == 0) {
		return PATHS_FOLDER_FOUND;
	}
	// A lock (Finder's Get Info > Locked, or an append-only flag) keeps every app out, the game
	// too; otherwise "operation not permitted" is macOS's privacy protection, which judges each app
	// on its own.
	uint32_t locks = UF_IMMUTABLE | SF_IMMUTABLE | UF_APPEND | SF_APPEND;
	switch (errno) {
		case EACCES:
			// W_OK on a folder also asks to add folders to it, which an access control list can refuse
			// alone: a new file needs only to be added (_WRITE_OK) where the user may search.
			if (!exists && access(folder, _WRITE_OK | _EXECUTE_OK) == 0) {
				return PATHS_FOLDER_FOUND;
			}
			return exists ? PATHS_FILE_NOT_WRITABLE : PATHS_FOLDER_NOT_WRITABLE;
		case EPERM:
			if (((exists ? existing : status).st_flags & locks) == 0) {
				return PATHS_FOLDER_PRIVACY;
			}
			return exists ? PATHS_FILE_LOCKED : PATHS_FOLDER_LOCKED;
		default: return PATHS_FOLDER_UNCHECKED;
	}
}

bool paths_toUtf8(const wchar_t *text, char *out, size_t size) {
	return platform_toUtf8(text, out, size);
}

static bool fromSystem(const char *path, wchar_t *out, size_t size) {
	return platform_fromUtf8(path, out, size);
}

// Collapses repeated slashes and resolves "." and ".." in an absolute path, without touching the
// file system (like Windows' GetFullPathNameW, the path need not exist yet).
static bool normalise(wchar_t *path) {
	size_t length = wcslen(path);
	wchar_t *result = malloc((length + 2) * sizeof *result);
	if (!result) {
		return false;
	}
	size_t written = 0;
	const wchar_t *part = path;
	while (*part) {
		while (*part == L'/') {
			++part;
		}
		const wchar_t *end = part;
		while (*end && *end != L'/') {
			++end;
		}
		size_t partLength = (size_t)(end - part);
		if (partLength == 0 || (partLength == 1 && part[0] == L'.')) {
			// nothing to add
		} else if (partLength == 2 && part[0] == L'.' && part[1] == L'.') {
			while (written > 0 && result[written - 1] != L'/') {
				--written;
			}
			if (written > 0) {
				--written; // the separator before the removed part
			}
		} else {
			result[written++] = L'/';
			wmemcpy(result + written, part, partLength);
			written += partLength;
		}
		part = end;
	}
	if (written == 0) {
		result[written++] = L'/';
	}
	result[written] = 0;
	wcscpy(path, result);
	free(result);
	return true;
}
