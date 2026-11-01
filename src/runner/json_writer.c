// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json_writer.h"

#include "platform.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>


static void append(JsonWriter *writer, const void *bytes, size_t size);
static bool beginValue(JsonWriter *writer, const char *key);
static void beginContainer(JsonWriter *writer, const char *key, bool object);
static void endContainer(JsonWriter *writer, bool object);
static void appendText(JsonWriter *writer, const char *text);
static void writeRaw(JsonWriter *writer, const char *key, const char *text);
static size_t validSequence(const unsigned char *text);

void jsonWriter_start(JsonWriter *writer, ByteBuffer *out) {
	*writer = (JsonWriter){.out = out};
}

void jsonWriter_beginObject(JsonWriter *writer, const char *key) {
	beginContainer(writer, key, true);
}

void jsonWriter_endObject(JsonWriter *writer) {
	endContainer(writer, true);
}

void jsonWriter_beginArray(JsonWriter *writer, const char *key) {
	beginContainer(writer, key, false);
}

void jsonWriter_endArray(JsonWriter *writer) {
	endContainer(writer, false);
}

void jsonWriter_string(JsonWriter *writer, const char *key, const char *text) {
	if (!text) {
		jsonWriter_null(writer, key);
	} else if (beginValue(writer, key)) {
		appendText(writer, text);
	}
}

void jsonWriter_unsigned(JsonWriter *writer, const char *key, uint64_t value) {
	char text[24];
	(void)snprintf(text, sizeof text, "%" PRIu64, value);
	writeRaw(writer, key, text);
}

void jsonWriter_signed(JsonWriter *writer, const char *key, int64_t value) {
	char text[24];
	(void)snprintf(text, sizeof text, "%" PRId64, value);
	writeRaw(writer, key, text);
}

void jsonWriter_decimal(JsonWriter *writer, const char *key, double value, int decimals) {
	if (decimals < 0 || decimals > 9) {
		writer->failed = true;
		return;
	}
	if (!isfinite(value)) {
		jsonWriter_null(writer, key);
		return;
	}
	char text[64];
	int written = snprintf(text, sizeof text, "%.*f", decimals, value);
	if (written < 0 || (size_t)written >= sizeof text) {
		(void)snprintf(text, sizeof text, "%.17g", value); // too large for fixed digits
	}
	writeRaw(writer, key, text);
}

void jsonWriter_bool(JsonWriter *writer, const char *key, bool value) {
	writeRaw(writer, key, value ? "true" : "false");
}

void jsonWriter_null(JsonWriter *writer, const char *key) {
	writeRaw(writer, key, "null");
}

void jsonWriter_utcTime(JsonWriter *writer, const char *key, time_t seconds) {
	struct tm utc;
	char text[32];
	if (!platform_utcTime(seconds, &utc) ||
			!strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc)) {
		jsonWriter_null(writer, key);
	} else {
		jsonWriter_string(writer, key, text);
	}
}

bool jsonWriter_finish(const JsonWriter *writer) {
	return !writer->failed && writer->started && writer->depth == 0;
}

static void append(JsonWriter *writer, const void *bytes, size_t size) {
	if (!writer->failed && !buffer_append(writer->out, bytes, size)) {
		writer->failed = true;
	}
}

// Writes what comes before a value: a comma after an earlier one, and its key. False, with the
// writer failed, if the key is missing or not allowed there.
static bool beginValue(JsonWriter *writer, const char *key) {
	if (writer->failed) {
		return false;
	}
	if (!writer->depth) {
		writer->failed = writer->started || key;
		writer->started = true;
		return !writer->failed;
	}
	uint32_t level = 1U << (writer->depth - 1);
	if (((writer->objects & level) != 0) != (key != NULL)) {
		writer->failed = true;
		return false;
	}
	if (writer->filled & level) {
		append(writer, ",", 1);
	}
	writer->filled |= level;
	if (key) {
		appendText(writer, key);
		append(writer, ":", 1);
	}
	return !writer->failed;
}

static void beginContainer(JsonWriter *writer, const char *key, bool object) {
	if (!beginValue(writer, key)) {
		return;
	}
	if (writer->depth == JSON_WRITER_DEPTH) {
		writer->failed = true;
		return;
	}
	uint32_t level = 1U << writer->depth++;
	writer->objects = object ? writer->objects | level : writer->objects & ~level;
	writer->filled &= ~level;
	append(writer, object ? "{" : "[", 1);
}

static void endContainer(JsonWriter *writer, bool object) {
	if (writer->failed) {
		return;
	}
	uint32_t level = writer->depth ? 1U << (writer->depth - 1) : 0;
	if (!level || ((writer->objects & level) != 0) != object) {
		writer->failed = true;
		return;
	}
	--writer->depth;
	append(writer, object ? "}" : "]", 1);
}

// `text` as a JSON string: quotes, backslashes and control characters escaped, valid UTF-8 kept and
// any other byte replaced by U+FFFD.
static void appendText(JsonWriter *writer, const char *text) {
	static const char replacement[] = "\xEF\xBF\xBD";
	append(writer, "\"", 1);
	const unsigned char *next = (const unsigned char *)text;
	while (*next) {
		size_t plain = 0;
		size_t length = 0;
		while (next[plain] && (length = validSequence(next + plain)) != 0 && next[plain] != '"' &&
					 next[plain] != '\\' && next[plain] >= 0x20) {
			plain += length;
		}
		append(writer, next, plain);
		next += plain;
		if (!*next) {
			break;
		}
		char escaped[8];
		unsigned char byte = *next++;
		if (!validSequence(next - 1)) {
			append(writer, replacement, sizeof replacement - 1);
		} else if (byte == '"' || byte == '\\') {
			escaped[0] = '\\';
			escaped[1] = (char)byte;
			append(writer, escaped, 2);
		} else {
			static const char shortForms[] = "btnvfr"; // \b \t \n, no \v, \f \r
			bool hasShort = byte >= '\b' && byte <= '\r' && byte != '\v';
			int written = hasShort ? snprintf(escaped, sizeof escaped, "\\%c", shortForms[byte - '\b'])
														 : snprintf(escaped, sizeof escaped, "\\u%04x", byte);
			append(writer, escaped, (size_t)written);
		}
	}
	append(writer, "\"", 1);
}

static void writeRaw(JsonWriter *writer, const char *key, const char *text) {
	if (beginValue(writer, key)) {
		append(writer, text, strlen(text));
	}
}

// The length of the UTF-8 sequence `text` starts with, 0 if it is not a valid one (overlong, a
// surrogate, beyond U+10FFFF or cut short).
static size_t validSequence(const unsigned char *text) {
	unsigned char lead = text[0];
	if (lead < 0x80) {
		return 1;
	}
	size_t count = 0;
	if (lead >= 0xC2 && lead < 0xE0) {
		count = 2;
	} else if (lead >= 0xE0 && lead < 0xF0) {
		count = 3;
	} else if (lead >= 0xF0 && lead < 0xF5) {
		count = 4;
	} else {
		return 0;
	}
	// The second byte's range rules out overlong forms, surrogates and code points past U+10FFFF.
	unsigned char low = lead == 0xE0 ? 0xA0 : lead == 0xF0 ? 0x90 : 0x80;
	unsigned char high = lead == 0xED ? 0x9F : lead == 0xF4 ? 0x8F : 0xBF;
	if (text[1] < low || text[1] > high) {
		return 0;
	}
	for (size_t i = 2; i < count; ++i) {
		if ((text[i] & 0xC0) != 0x80) {
			return 0;
		}
	}
	return count;
}
