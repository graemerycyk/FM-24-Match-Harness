// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "files.h"

#include "platform.h"
#include "protocol.h"

#include <stdio.h>
#include <wchar.h>


bool files_read(const wchar_t *path, ByteBuffer *out) {
	buffer_clear(out);
	FILE *file = platform_openFile(path, "rb");
	if (!file) {
		return false;
	}
	uint8_t chunk[1 << 16];
	size_t got = 0;
	bool ok = true;
	while (ok && (got = fread(chunk, 1, sizeof chunk, file)) > 0) {
		ok = buffer_append(out, chunk, got);
	}
	ok = ok && !ferror(file);
	return fclose(file) == 0 && ok;
}

bool files_write(const wchar_t *path, const void *bytes, size_t size) {
	FILE *file = platform_openFile(path, "wb");
	if (!file) {
		return false;
	}
	bool ok = fwrite(bytes, 1, size, file) == size;
	return fclose(file) == 0 && ok;
}

bool files_replace(const wchar_t *path, const void *bytes, size_t size) {
	wchar_t temporary[HARNESS_PATH_LENGTH + 32];
	wchar_t folder[HARNESS_PATH_LENGTH] = L".";
	char utf8[3 * HARNESS_PATH_LENGTH];
	size_t length = wcslen(path);
	if (length >= HARNESS_PATH_LENGTH) {
		fprintf(
			stderr,
			"cannot write %s: the path is too long\n",
			platform_toUtf8(path, utf8, sizeof utf8) ? utf8 : "a file"
		);
		return false;
	}
	// Not one swprintf of the path, which fails on macOS for characters the C locale lacks.
	wmemcpy(temporary, path, length);
	(void)swprintf(
		temporary + length,
		sizeof temporary / sizeof temporary[0] - length,
		L".%lu.tmp",
		platform_processId()
	);
	// The folder is the path up to its last separator, or the root if that is the first character.
	const wchar_t *name = wcsrchr(path, PLATFORM_PATH_SEPARATOR);
	if (name) {
		size_t folderLength = name == path ? 1 : (size_t)(name - path);
		wmemcpy(folder, path, folderLength);
		folder[folderLength] = 0;
	}
	FILE *file = platform_openFile(temporary, "wb");
	bool ok = file && fwrite(bytes, 1, size, file) == size && platform_syncFile(file);
	ok = file && fclose(file) == 0 && ok;
	if (!ok) {
		fprintf(
			stderr, "cannot write %s\n", platform_toUtf8(temporary, utf8, sizeof utf8) ? utf8 : "a file"
		);
	}
	ok = ok && platform_replaceFile(temporary, path) && platform_syncDirectory(folder);
	if (!ok) {
		(void)platform_removeFile(temporary);
	}
	return ok;
}

bool files_hasExtension(const wchar_t *path, const wchar_t *suffix) {
	size_t pathLength = wcslen(path);
	size_t suffixLength = wcslen(suffix);
	return pathLength >= suffixLength &&
				 platform_compareWideIgnoringCase(path + pathLength - suffixLength, suffix) == 0;
}
