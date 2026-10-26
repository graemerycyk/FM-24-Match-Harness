// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "saves.h"

#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { STATUS_LINE_LENGTH = 1024 };

static const char extension[] = ".fm";
static const char *const folderNames[SAVES_FOLDER_COUNT] = {"cloud/games", "games"};
static const char *const phaseNames[] = {"starting", "menu", "loading", "busy", "ready", "no_team"};

static bool hasExtension(const char *name, size_t length);
static bool copyText(const char *text, size_t length, char *out, size_t size);
static bool parseStatus(const char *text, GameStatus *out);
static bool readPhase(const char **cursor, StatusPhase *phase);
static bool
readField(const char **cursor, const char *key, long long low, long long high, long long *value);

const char *saves_folderName(SavesFolder folder) {
	return (size_t)folder < SAVES_FOLDER_COUNT ? folderNames[folder] : "";
}

bool saves_isName(const char *text) {
	size_t length = strlen(text);
	size_t suffix = sizeof extension - 1;
	return length > 0 && !strchr(text, '/') &&
				 (length < suffix || platform_compareIgnoringCase(text + length - suffix, extension) != 0);
}

bool saves_nameOfPath(const char *path, char *out, size_t size) {
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	size_t length = strlen(name);
	if (hasExtension(name, length)) {
		length -= sizeof extension - 1;
	}
	return copyText(name, length, out, size);
}

bool saves_isSave(const char *reported, const char *name) {
	char reportedName[SAVES_NAME_LENGTH];
	return saves_nameOfPath(reported, reportedName, sizeof reportedName) &&
				 strcmp(reportedName, name) == 0;
}

void saves_startSearch(SaveSearch *search, const char *name) {
	*search = (SaveSearch){.name = name};
}

void saves_note(SaveSearch *search, SavesFolder folder, const char *fileName) {
	size_t length = strlen(fileName);
	size_t nameLength = strlen(search->name);
	if ((size_t)folder >= SAVES_FOLDER_COUNT || !hasExtension(fileName, length)) {
		return;
	}
	size_t stem = length - (sizeof extension - 1);
	if (stem == nameLength && memcmp(fileName, search->name, stem) == 0) {
		search->found[folder] = true;
		return;
	}
	if (stem < nameLength || platform_compareIgnoringCaseN(fileName, search->name, nameLength) != 0) {
		return;
	}
	++search->similarCount;
	size_t used = strlen(search->similar);
	size_t room = sizeof search->similar - used;
	int written =
		snprintf(search->similar + used, room, "%s%.*s", used ? ", " : "", (int)stem, fileName);
	if (written > 0 && (size_t)written < room) {
		++search->similarListed;
	} else {
		search->similar[used] = 0; // only whole names
	}
}

SavesResult saves_result(const SaveSearch *search, SavesFolder *folder) {
	size_t found = 0;
	for (size_t i = SAVES_FOLDER_COUNT; i-- > 0;) {
		if (search->found[i]) {
			*folder = (SavesFolder)i;
			++found;
		}
	}
	return found == 1 ? SAVES_FOUND : found ? SAVES_AMBIGUOUS : SAVES_MISSING;
}

bool saves_path(const char *data, SavesFolder folder, const char *name, char *out, size_t size) {
	int written = snprintf(out, size, "%s/%s/%s%s", data, saves_folderName(folder), name, extension);
	return written > 0 && (size_t)written < size;
}

bool saves_autoplayJson(const char *path, char *out, size_t size) {
	static const char start[] = "{\"LOAD_GAME\": \"";
	static const char end[] = "\"}";
	if (!path[0] || size < sizeof start + sizeof end) {
		return false;
	}
	memcpy(out, start, sizeof start - 1);
	size_t used = sizeof start - 1;
	for (const char *c = path; *c; ++c) {
		bool escaped = *c == '"' || *c == '\\';
		if ((unsigned char)*c < 0x20 || used + escaped + 1 + sizeof end > size) {
			return false;
		}
		if (escaped) {
			out[used++] = '\\';
		}
		out[used++] = *c;
	}
	memcpy(out + used, end, sizeof end);
	return true;
}

bool saves_parseStatus(const char *line, GameStatus *out) {
	size_t length = strcspn(line, "\r\n");
	const char *ending = line + length;
	char text[STATUS_LINE_LENGTH];
	bool ok =
		(!*ending || strcmp(ending, "\n") == 0 || strcmp(ending, "\r\n") == 0) && length < sizeof text;
	if (ok) {
		memcpy(text, line, length);
		text[length] = 0;
		ok = parseStatus(text, out);
	}
	if (!ok) {
		*out = (GameStatus){.kind = -1};
	}
	return ok;
}

const char *saves_phaseName(StatusPhase phase) {
	return (size_t)phase < sizeof phaseNames / sizeof phaseNames[0] ? phaseNames[phase] : "unknown";
}

// True if the file name ends with ".fm" (as the game writes it) after at least one character.
static bool hasExtension(const char *name, size_t length) {
	size_t suffix = sizeof extension - 1;
	return length > suffix && strcmp(name + length - suffix, extension) == 0;
}

static bool copyText(const char *text, size_t length, char *out, size_t size) {
	if (length == 0 || length >= size) {
		return false;
	}
	memcpy(out, text, length);
	out[length] = 0;
	return true;
}

// The status line without its line ending (see job_status.c for its keys).
static bool parseStatus(const char *text, GameStatus *out) {
	static const struct {
		const char *key;
		long long minimum;
		long long maximum;
	} fields[] = {
		{"in_progress", 0, 1},
		{"processing", 0, 1},
		{"kind", INT_MIN, INT_MAX},
		{"people", 0, LLONG_MAX},
		{"human_team", 0, 1},
		{"menu_seen", 0, 1},
		{"autoplay", 0, 1},
		{"autoplay_read", 0, 1},
	};
	enum { FIELD_COUNT = sizeof fields / sizeof fields[0] };
	static const char prefix[] = "ok: ";
	static const char dateKey[] = " date=";
	static const char saveKey[] = " save=";
	*out = (GameStatus){.kind = -1};
	const char *c = text + sizeof prefix - 1;
	long long values[FIELD_COUNT];
	if (strncmp(text, prefix, sizeof prefix - 1) != 0 || !readPhase(&c, &out->phase)) {
		return false;
	}
	for (size_t i = 0; i < FIELD_COUNT; ++i) {
		if (!readField(&c, fields[i].key, fields[i].minimum, fields[i].maximum, &values[i])) {
			return false;
		}
	}
	out->inProgress = values[0] != 0;
	out->processing = values[1] != 0;
	out->kind = (int)values[2];
	out->people = (unsigned long long)values[3];
	out->humanTeam = values[4] != 0;
	out->menuSeen = values[5] != 0;
	out->autoPlay = values[6] != 0;
	out->autoPlayRead = values[7] != 0;
	// The date has spaces; it ends where the save starts. The save runs to the end of the line.
	if (strncmp(c, dateKey, sizeof dateKey - 1) == 0) {
		c += sizeof dateKey - 1;
		const char *end = strstr(c, saveKey);
		size_t length = end ? (size_t)(end - c) : strlen(c);
		if (!copyText(c, length, out->date, sizeof out->date)) {
			return false;
		}
		c += length;
	}
	if (strncmp(c, saveKey, sizeof saveKey - 1) == 0) {
		c += sizeof saveKey - 1;
		size_t length = strlen(c);
		if (!copyText(c, length, out->save, sizeof out->save)) {
			return false;
		}
		c += length;
	}
	return *c == 0;
}

// The phase name at `*cursor`, which moves past it.
static bool readPhase(const char **cursor, StatusPhase *phase) {
	size_t length = strcspn(*cursor, " ");
	for (size_t i = 0; i < sizeof phaseNames / sizeof phaseNames[0]; ++i) {
		if (strlen(phaseNames[i]) == length && strncmp(*cursor, phaseNames[i], length) == 0) {
			*phase = (StatusPhase)i;
			*cursor += length;
			return true;
		}
	}
	return false;
}

// " key=N" at `*cursor`, which moves past it, with N a decimal integer from `low` to `high`.
static bool
readField(const char **cursor, const char *key, long long low, long long high, long long *value) {
	size_t length = strlen(key);
	const char *c = *cursor;
	if (c[0] != ' ' || strncmp(c + 1, key, length) != 0 || c[length + 1] != '=') {
		return false;
	}
	c += length + 2;
	bool negative = c[0] == '-';
	if (c[negative] < '0' || c[negative] > '9') {
		return false;
	}
	char *end = NULL;
	errno = 0;
	*value = strtoll(c, &end, 10);
	*cursor = end;
	return errno == 0 && *value >= low && *value <= high && (!*end || *end == ' ');
}
