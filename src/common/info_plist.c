// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "info_plist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static char *readPlist(const char *bundle);
static bool decode(const char *text, size_t length, char *out, size_t size);

bool infoPlist_find(const char *text, const char *key, char *out, size_t size) {
	static const char keyStart[] = "<key>";
	static const char keyEnd[] = "</key>";
	static const char stringStart[] = "<string>";
	static const char stringEnd[] = "</string>";
	size_t keyLength = strlen(key);
	for (const char *at = strstr(text, keyStart); at; at = strstr(at + 1, keyStart)) {
		const char *name = at + sizeof keyStart - 1;
		if (strncmp(name, key, keyLength) != 0 ||
				strncmp(name + keyLength, keyEnd, sizeof keyEnd - 1) != 0) {
			continue;
		}
		const char *value = name + keyLength + sizeof keyEnd - 1;
		value += strspn(value, " \t\r\n");
		if (strncmp(value, stringStart, sizeof stringStart - 1) != 0) {
			return false;
		}
		value += sizeof stringStart - 1;
		const char *end = strchr(value, '<');
		return end && strncmp(end, stringEnd, sizeof stringEnd - 1) == 0 &&
					 decode(value, (size_t)(end - value), out, size);
	}
	return false;
}

bool infoPlist_read(const char *bundle, const char *key, char *out, size_t size) {
	char *text = readPlist(bundle);
	bool found = text && infoPlist_find(text, key, out, size);
	free(text);
	return found;
}

bool infoPlist_executable(
	const char *bundle,
	const char *identifier,
	const char *fallback,
	char *out,
	size_t size
) {
	char *text = readPlist(bundle);
	char found[256] = "";
	char name[256] = "";
	bool ours = text && infoPlist_find(text, "CFBundleIdentifier", found, sizeof found) &&
							strcmp(found, identifier) == 0;
	if (ours && !infoPlist_find(text, "CFBundleExecutable", name, sizeof name)) {
		(void)snprintf(name, sizeof name, "%s", fallback);
	}
	free(text);
	int written = ours ? snprintf(out, size, "%s/Contents/MacOS/%s", bundle, name) : -1;
	return written > 0 && (size_t)written < size && name[0] && !strchr(name, '/') &&
				 strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

// The bundle's Contents/Info.plist as text, to free, or NULL.
static char *readPlist(const char *bundle) {
	char path[4096];
	int written = snprintf(path, sizeof path, "%s/Contents/Info.plist", bundle);
	char *text = written > 0 && (size_t)written < sizeof path ? malloc(INFO_PLIST_MAX + 1) : NULL;
	FILE *file = text ? fopen(path, "rb") : NULL;
	if (!file) {
		free(text);
		return NULL;
	}
	size_t length = fread(text, 1, INFO_PLIST_MAX, file);
	bool read = !ferror(file);
	(void)fclose(file);
	if (!read) {
		free(text);
		return NULL;
	}
	text[length] = 0;
	return text;
}

// The text of an XML element, with &amp; &lt; &gt; &quot; and &apos; decoded.
static bool decode(const char *text, size_t length, char *out, size_t size) {
	static const struct {
		const char *entity;
		char character;
	} entities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
	size_t used = 0;
	for (size_t i = 0; i < length;) {
		char next = text[i];
		size_t skip = 1;
		for (size_t k = 0; next == '&' && k < sizeof entities / sizeof entities[0]; ++k) {
			size_t entity = strlen(entities[k].entity);
			if (i + entity <= length && strncmp(text + i, entities[k].entity, entity) == 0) {
				next = entities[k].character;
				skip = entity;
				break;
			}
		}
		if (used + 1 >= size) {
			return false;
		}
		out[used++] = next;
		i += skip;
	}
	out[used] = 0;
	return used > 0;
}
