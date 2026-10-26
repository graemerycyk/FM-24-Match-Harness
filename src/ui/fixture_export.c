// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_export.h"

#include <stdio.h>
#include <string.h>


// What a team name's byte becomes in a filename, or 0 to drop it.
static char filenameCharacter(unsigned char c) {
	if (c < ' ' || c > '~' || strchr("<>:\"/\\|?*", c)) {
		return 0;
	}
	return (char)(c == ' ' ? '-' : c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}

static void teamFilename(const char *name, wchar_t *out) {
	for (; *name; ++name) {
		char c = filenameCharacter((unsigned char)*name);
		if (c) {
			*out++ = (wchar_t)c;
		}
	}
	*out = 0;
}

bool fixtureExport_filename(
	const FixtureData *data,
	uint64_t timestamp,
	wchar_t *out,
	size_t size
) {
	wchar_t home[FIXTURE_TEXT];
	wchar_t away[FIXTURE_TEXT];
	teamFilename(data->teams[0].name, home);
	teamFilename(data->teams[1].name, away);
	wchar_t stamp[32];
	if (swprintf(stamp, 32, L"%llu", (unsigned long long)timestamp) < 0) {
		return false;
	}
	// MinGW's swprintf fails when the output exactly fills the buffer, so check the length first
	// and build the name without it.
	size_t length =
		wcslen(home) + wcslen(L"-v-") + wcslen(away) + wcslen(L"-") + wcslen(stamp) + wcslen(L".json");
	if (length >= size || length >= FIXTURE_EXPORT_FILENAME_LENGTH) {
		return false;
	}
	wcscpy(out, home);
	wcscat(out, L"-v-");
	wcscat(out, away);
	wcscat(out, L"-");
	wcscat(out, stamp);
	wcscat(out, L".json");
	return true;
}

static void teamFilenameUtf8(const char *name, char *out) {
	for (; *name; ++name) {
		char c = filenameCharacter((unsigned char)*name);
		if (c) {
			*out++ = c;
		}
	}
	*out = 0;
}

bool fixtureExport_filenameUtf8(
	const FixtureData *data,
	uint64_t timestamp,
	const char *extension,
	char *out,
	size_t size
) {
	char home[FIXTURE_TEXT];
	char away[FIXTURE_TEXT];
	teamFilenameUtf8(data->teams[0].name, home);
	teamFilenameUtf8(data->teams[1].name, away);
	int length =
		snprintf(out, size, "%s-v-%s-%llu%s", home, away, (unsigned long long)timestamp, extension);
	if (length < 0 || (size_t)length >= size || length >= FIXTURE_EXPORT_FILENAME_LENGTH) {
		if (size) {
			out[0] = 0;
		}
		return false;
	}
	return true;
}
