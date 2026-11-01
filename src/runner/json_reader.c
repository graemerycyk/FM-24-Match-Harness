// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "json_reader.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>


enum {
	MAX_DEPTH = 64,
	FLAT_PATH_LENGTH = 160, // the longest path jsonFlat_read keeps
	UTC_TIME_LENGTH = 20,   // "2026-10-25T09:23:18Z"
};

// A document being flattened.
typedef struct Flattening {
	JsonReader reader;
	JsonFlat *flat;
	char path[FLAT_PATH_LENGTH];
	ByteBuffer text;
} Flattening;

static void skipWhitespace(JsonReader *reader);
static bool appendUtf8(ByteBuffer *out, unsigned long codePoint);
static bool readHex4(JsonReader *reader, unsigned long *value);
static bool readUnicodeEscape(JsonReader *reader, ByteBuffer *out);
static bool readEscape(JsonReader *reader, ByteBuffer *out);
static bool readUtf8(JsonReader *reader);
static bool readDigits(JsonReader *reader);
static bool
convertUnsigned(JsonReader *reader, const char *start, const char *end, unsigned long long *value);
static bool skipLiteral(JsonReader *reader);
static bool skipContainer(JsonReader *reader, bool isObject, int depth);
static bool skipNested(JsonReader *reader, int depth);
static bool flattenValue(Flattening *state, size_t pathLength, int depth);
static bool flattenMembers(Flattening *state, size_t pathLength, bool isObject, int depth);
static bool flattenLiteral(Flattening *state, size_t pathLength);
static bool
addEntry(Flattening *state, size_t pathLength, char kind, const char *text, size_t length);
static bool readDecimalText(const char *text, size_t length, uint64_t *value);
static int64_t daysFromCivil(int64_t year, unsigned month, unsigned day);

void jsonReader_init(JsonReader *reader, const char *text, size_t length) {
	*reader = (JsonReader){.begin = text, .position = text, .end = text + length};
}

bool jsonReader_fail(JsonReader *reader, const char *message) {
	if (reader->error[0] == 0) {
		(void)snprintf(
			reader->error,
			sizeof reader->error,
			"%s at byte %td",
			message,
			reader->position - reader->begin
		);
	}
	return false;
}

bool jsonReader_consume(JsonReader *reader, char c) {
	skipWhitespace(reader);
	if (reader->position < reader->end && *reader->position == c) {
		++reader->position;
		return true;
	}
	return false;
}

bool jsonReader_readString(JsonReader *reader, ByteBuffer *out) {
	if (out) {
		buffer_clear(out);
	}
	if (!jsonReader_consume(reader, '"')) {
		return jsonReader_fail(reader, "expected string");
	}
	while (reader->position < reader->end && *reader->position != '"') {
		const char *start = reader->position;
		while (
			reader->position < reader->end && *reader->position != '"' && *reader->position != '\\') {
			uint8_t c = (uint8_t)*reader->position;
			if (c < 0x20) {
				return jsonReader_fail(reader, "unescaped control character in string");
			}
			if (c < 0x80) {
				++reader->position;
			} else if (!readUtf8(reader)) {
				return false;
			}
		}
		if (out && !buffer_append(out, start, (size_t)(reader->position - start))) {
			return jsonReader_fail(reader, "out of memory");
		}
		if (reader->position < reader->end && *reader->position == '\\') {
			++reader->position;
			if (!readEscape(reader, out)) {
				return jsonReader_fail(reader, "out of memory");
			}
		}
	}
	if (reader->position >= reader->end) {
		return jsonReader_fail(reader, "unterminated string");
	}
	++reader->position;
	return true;
}

static bool readUtf8(JsonReader *reader) {
	uint8_t first = (uint8_t)*reader->position;
	unsigned length = first >= 0xC2 && first <= 0xDF   ? 2
										: first >= 0xE0 && first <= 0xEF ? 3
										: first >= 0xF0 && first <= 0xF4 ? 4
																										 : 0;
	if (!length || (size_t)(reader->end - reader->position) < length) {
		return jsonReader_fail(reader, "invalid UTF-8");
	}
	uint32_t codePoint = first & (0x7FU >> length);
	for (unsigned i = 1; i < length; ++i) {
		uint8_t next = (uint8_t)reader->position[i];
		if ((next & 0xC0) != 0x80) {
			return jsonReader_fail(reader, "invalid UTF-8");
		}
		codePoint = (codePoint << 6) | (next & 0x3F);
	}
	if ((length == 3 && codePoint < 0x800) || (length == 4 && codePoint < 0x10000) ||
			(codePoint >= 0xD800 && codePoint <= 0xDFFF) || codePoint > 0x10FFFF) {
		return jsonReader_fail(reader, "invalid UTF-8");
	}
	reader->position += length;
	return true;
}

static bool readDigits(JsonReader *reader) {
	const char *start = reader->position;
	while (reader->position < reader->end && *reader->position >= '0' && *reader->position <= '9') {
		++reader->position;
	}
	return reader->position != start;
}

bool jsonReader_readNumber(JsonReader *reader, unsigned long long *value, bool *isUnsigned) {
	skipWhitespace(reader);
	const char *start = reader->position;
	bool negative = reader->position < reader->end && *reader->position == '-';
	if (negative) {
		++reader->position;
	}
	if (reader->position < reader->end && *reader->position == '0') {
		++reader->position;
	} else if (!readDigits(reader)) {
		return jsonReader_fail(reader, "expected a number");
	}
	const char *integerEnd = reader->position;
	*isUnsigned = !negative;
	if (reader->position < reader->end && *reader->position == '.') {
		*isUnsigned = false;
		++reader->position;
		if (!readDigits(reader)) {
			return jsonReader_fail(reader, "invalid JSON fraction");
		}
	}
	if (reader->position < reader->end && (*reader->position == 'e' || *reader->position == 'E')) {
		*isUnsigned = false;
		++reader->position;
		if (reader->position < reader->end && (*reader->position == '+' || *reader->position == '-')) {
			++reader->position;
		}
		if (!readDigits(reader)) {
			return jsonReader_fail(reader, "invalid JSON exponent");
		}
	}
	if (reader->position < reader->end && *reader->position &&
			strchr("+-0123456789.eE", *reader->position)) {
		return jsonReader_fail(reader, "invalid JSON number");
	}
	if (!value) {
		return true;
	}
	*value = 0;
	return !*isUnsigned || convertUnsigned(reader, start, integerEnd, value);
}

static bool
convertUnsigned(JsonReader *reader, const char *start, const char *end, unsigned long long *value) {
	for (const char *digit = start; digit < end; ++digit) {
		unsigned number = (unsigned)(*digit - '0');
		if (*value > (ULLONG_MAX - number) / 10) {
			return jsonReader_fail(reader, "integer out of range");
		}
		*value = *value * 10 + number;
	}
	return true;
}

int jsonReader_hexDigit(uint8_t c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

bool jsonReader_skipValue(JsonReader *reader) {
	return skipNested(reader, 0);
}

static void skipWhitespace(JsonReader *reader) {
	while (
		reader->position < reader->end && strchr(" \t\r\n", *reader->position) && *reader->position) {
		++reader->position;
	}
}

static bool appendUtf8(ByteBuffer *out, unsigned long codePoint) {
	if (!out) {
		return true;
	}
	uint8_t bytes[4];
	size_t length = 0;
	if (codePoint < 0x80) {
		bytes[length++] = (uint8_t)codePoint;
	} else if (codePoint < 0x800) {
		bytes[length++] = (uint8_t)(0xC0 | (codePoint >> 6));
		bytes[length++] = (uint8_t)(0x80 | (codePoint & 0x3F));
	} else if (codePoint < 0x10000) {
		bytes[length++] = (uint8_t)(0xE0 | (codePoint >> 12));
		bytes[length++] = (uint8_t)(0x80 | ((codePoint >> 6) & 0x3F));
		bytes[length++] = (uint8_t)(0x80 | (codePoint & 0x3F));
	} else {
		bytes[length++] = (uint8_t)(0xF0 | (codePoint >> 18));
		bytes[length++] = (uint8_t)(0x80 | ((codePoint >> 12) & 0x3F));
		bytes[length++] = (uint8_t)(0x80 | ((codePoint >> 6) & 0x3F));
		bytes[length++] = (uint8_t)(0x80 | (codePoint & 0x3F));
	}
	return buffer_append(out, bytes, length);
}

static bool readHex4(JsonReader *reader, unsigned long *value) {
	if (reader->end - reader->position < 4) {
		return jsonReader_fail(reader, "truncated \\u escape");
	}
	*value = 0;
	for (unsigned i = 0; i < 4; ++i) {
		int digit = jsonReader_hexDigit((uint8_t)*reader->position++);
		if (digit < 0) {
			return jsonReader_fail(reader, "bad \\u escape");
		}
		*value = (*value << 4) | (unsigned)digit;
	}
	return true;
}

static bool readUnicodeEscape(JsonReader *reader, ByteBuffer *out) {
	unsigned long codePoint = 0;
	if (!readHex4(reader, &codePoint)) {
		return false;
	}
	if (codePoint >= 0xD800 && codePoint < 0xDC00) { // surrogate pair
		unsigned long low = 0;
		if (reader->end - reader->position < 2 || reader->position[0] != '\\' ||
				reader->position[1] != 'u') {
			return jsonReader_fail(reader, "lone surrogate");
		}
		reader->position += 2;
		if (!readHex4(reader, &low) || low < 0xDC00 || low > 0xDFFF) {
			return jsonReader_fail(reader, "bad surrogate");
		}
		codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
	} else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
		return jsonReader_fail(reader, "lone surrogate");
	}
	return appendUtf8(out, codePoint);
}

static bool readEscape(JsonReader *reader, ByteBuffer *out) {
	static const char from[] = "\"\\/bfnrt";
	static const char to[] = "\"\\/\b\f\n\r\t";
	if (reader->position >= reader->end) {
		return jsonReader_fail(reader, "truncated escape");
	}
	char c = *reader->position++;
	if (c == 'u') {
		return readUnicodeEscape(reader, out);
	}
	const char *found = c ? strchr(from, c) : NULL;
	if (!found) {
		return jsonReader_fail(reader, "unknown escape");
	}
	return !out || buffer_append(out, &to[found - from], 1);
}

static bool skipLiteral(JsonReader *reader) {
	static const char *const literals[] = {"true", "false", "null"};
	for (size_t i = 0; i < sizeof literals / sizeof literals[0]; ++i) {
		size_t length = strlen(literals[i]);
		if ((size_t)(reader->end - reader->position) >= length &&
				memcmp(reader->position, literals[i], length) == 0) {
			reader->position += length;
			return true;
		}
	}
	bool isUnsigned = false;
	return jsonReader_readNumber(reader, NULL, &isUnsigned);
}

// Skips the members of an object or array whose opening bracket was consumed.
static bool skipContainer(JsonReader *reader, bool isObject, int depth) {
	char close = isObject ? '}' : ']';
	if (jsonReader_consume(reader, close)) {
		return true;
	}
	bool ok = true;
	do {
		if (isObject) {
			ok = jsonReader_readString(reader, NULL) &&
					 (jsonReader_consume(reader, ':') || jsonReader_fail(reader, "expected :"));
		}
		ok = ok && skipNested(reader, depth + 1);
	} while (ok && jsonReader_consume(reader, ','));
	return ok && (jsonReader_consume(reader, close) ||
								 jsonReader_fail(reader, "expected , or closing bracket"));
}

static bool skipNested(JsonReader *reader, int depth) {
	if (depth > MAX_DEPTH) {
		return jsonReader_fail(reader, "nested too deeply");
	}
	skipWhitespace(reader);
	if (reader->position >= reader->end) {
		return jsonReader_fail(reader, "unexpected end");
	}
	if (*reader->position == '"') {
		return jsonReader_readString(reader, NULL);
	}
	if (jsonReader_consume(reader, '{')) {
		return skipContainer(reader, true, depth);
	}
	if (jsonReader_consume(reader, '[')) {
		return skipContainer(reader, false, depth);
	}
	return skipLiteral(reader);
}

bool jsonFlat_read(JsonFlat *flat, const char *text, size_t length) {
	buffer_clear(&flat->entries);
	Flattening state = {.flat = flat};
	jsonReader_init(&state.reader, text, length);
	bool ok = flattenValue(&state, 0, 0);
	skipWhitespace(&state.reader);
	ok = ok && state.reader.position == state.reader.end;
	buffer_free(&state.text);
	if (!ok) {
		buffer_clear(&flat->entries);
	}
	return ok;
}

void jsonFlat_free(JsonFlat *flat) {
	buffer_free(&flat->entries);
}

const char *jsonFlat_find(const JsonFlat *flat, const char *path, char *kind) {
	const char *at = (const char *)flat->entries.data;
	const char *end = at + flat->entries.length;
	while (at && at < end) {
		size_t pathLength = strlen(at);
		const char *value = at + pathLength + 2;
		if (strcmp(at, path) == 0) {
			if (kind) {
				*kind = at[pathLength + 1];
			}
			return value;
		}
		at = value + strlen(value) + 1;
	}
	return NULL;
}

bool jsonFlat_unsigned(const JsonFlat *flat, const char *path, uint64_t *value) {
	char kind = 0;
	const char *text = jsonFlat_find(flat, path, &kind);
	*value = 0;
	return text && kind == 'n' && readDecimalText(text, strlen(text), value);
}

bool jsonFlat_string(const JsonFlat *flat, const char *path, char *out, size_t size) {
	char kind = 0;
	const char *text = jsonFlat_find(flat, path, &kind);
	size_t length = text ? strlen(text) : 0;
	if (!text || kind != 's' || length >= size) {
		if (size) {
			out[0] = 0;
		}
		return false;
	}
	memcpy(out, text, length + 1);
	return true;
}

bool jsonFlat_bool(const JsonFlat *flat, const char *path, bool *value) {
	char kind = 0;
	const char *text = jsonFlat_find(flat, path, &kind);
	*value = text && kind == 'b' && strcmp(text, "true") == 0;
	return text && kind == 'b';
}

bool jsonFlat_isNull(const JsonFlat *flat, const char *path) {
	char kind = 0;
	return jsonFlat_find(flat, path, &kind) && kind == 'z';
}

bool jsonFlat_time(const JsonFlat *flat, const char *path, time_t *seconds) {
	char text[UTC_TIME_LENGTH + 1];
	*seconds = 0;
	if (!jsonFlat_string(flat, path, text, sizeof text) || strlen(text) != UTC_TIME_LENGTH ||
			text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':' ||
			text[19] != 'Z') {
		return false;
	}
	static const size_t starts[] = {0, 5, 8, 11, 14, 17};
	static const size_t lengths[] = {4, 2, 2, 2, 2, 2};
	static const uint64_t limits[] = {9999, 12, 31, 23, 59, 60};
	uint64_t parts[6] = {0};
	for (size_t i = 0; i < 6; ++i) {
		if (!readDecimalText(text + starts[i], lengths[i], &parts[i]) || parts[i] > limits[i]) {
			return false;
		}
	}
	if (parts[1] < 1 || parts[2] < 1) {
		return false;
	}
	int64_t days = daysFromCivil((int64_t)parts[0], (unsigned)parts[1], (unsigned)parts[2]);
	*seconds = (time_t)(days * 86400 + (int64_t)(parts[3] * 3600 + parts[4] * 60 + parts[5]));
	return true;
}

static bool flattenValue(Flattening *state, size_t pathLength, int depth) {
	JsonReader *reader = &state->reader;
	if (depth > MAX_DEPTH) {
		return jsonReader_fail(reader, "nested too deeply");
	}
	if (jsonReader_consume(reader, '{')) {
		return flattenMembers(state, pathLength, true, depth);
	}
	if (jsonReader_consume(reader, '[')) {
		return flattenMembers(state, pathLength, false, depth);
	}
	if (reader->position < reader->end && *reader->position == '"') {
		return jsonReader_readString(reader, &state->text) &&
					 addEntry(state, pathLength, 's', buffer_string(&state->text), state->text.length);
	}
	return flattenLiteral(state, pathLength);
}

// The members of an object or array whose opening bracket was consumed, each at the path
// extended by its key or position.
static bool flattenMembers(Flattening *state, size_t pathLength, bool isObject, int depth) {
	JsonReader *reader = &state->reader;
	char close = isObject ? '}' : ']';
	if (jsonReader_consume(reader, close)) {
		return true;
	}
	size_t position = 0;
	bool ok = true;
	do {
		char number[24];
		const char *name = number;
		size_t length = 0;
		if (isObject) {
			ok = jsonReader_readString(reader, &state->text) &&
					 (jsonReader_consume(reader, ':') || jsonReader_fail(reader, "expected :"));
			name = buffer_string(&state->text);
			length = state->text.length;
		} else {
			length = (size_t)snprintf(number, sizeof number, "%zu", position++);
		}
		size_t extended = pathLength + (pathLength ? 1 : 0) + length;
		if (ok && extended >= FLAT_PATH_LENGTH) {
			ok = jsonReader_fail(reader, "path too long");
		}
		if (ok) {
			if (pathLength) {
				state->path[pathLength] = '.';
			}
			memcpy(state->path + extended - length, name, length);
			ok = flattenValue(state, extended, depth + 1);
		}
	} while (ok && jsonReader_consume(reader, ','));
	return ok && (jsonReader_consume(reader, close) ||
								 jsonReader_fail(reader, "expected , or closing bracket"));
}

static bool flattenLiteral(Flattening *state, size_t pathLength) {
	JsonReader *reader = &state->reader;
	static const struct {
		const char *text;
		char kind;
	} literals[] = {{"true", 'b'}, {"false", 'b'}, {"null", 'z'}};
	for (size_t i = 0; i < sizeof literals / sizeof literals[0]; ++i) {
		size_t length = strlen(literals[i].text);
		if ((size_t)(reader->end - reader->position) >= length &&
				memcmp(reader->position, literals[i].text, length) == 0) {
			reader->position += length;
			const char *text = literals[i].kind == 'z' ? "" : literals[i].text;
			return addEntry(state, pathLength, literals[i].kind, text, strlen(text));
		}
	}
	const char *start = reader->position;
	bool isUnsigned = false;
	return jsonReader_readNumber(reader, NULL, &isUnsigned) &&
				 addEntry(state, pathLength, 'n', start, (size_t)(reader->position - start));
}

static bool
addEntry(Flattening *state, size_t pathLength, char kind, const char *text, size_t length) {
	ByteBuffer *entries = &state->flat->entries;
	if (memchr(text, 0, length)) {
		return jsonReader_fail(&state->reader, "NUL in a string");
	}
	char separator[2] = {0, kind};
	bool ok = buffer_append(entries, state->path, pathLength) &&
						buffer_append(entries, separator, sizeof separator) &&
						buffer_append(entries, text, length) && buffer_append(entries, "", 1);
	return ok || jsonReader_fail(&state->reader, "out of memory");
}

// `length` decimal digits, at most UINT64_MAX.
static bool readDecimalText(const char *text, size_t length, uint64_t *value) {
	*value = 0;
	if (!length) {
		return false;
	}
	for (size_t i = 0; i < length; ++i) {
		unsigned digit = (unsigned)(text[i] - '0');
		if (text[i] < '0' || text[i] > '9' || *value > (UINT64_MAX - digit) / 10) {
			return false;
		}
		*value = *value * 10 + digit;
	}
	return true;
}

// Days from 1970-01-01 to a date of the proleptic Gregorian calendar.
static int64_t daysFromCivil(int64_t year, unsigned month, unsigned day) {
	year -= month <= 2;
	int64_t era = (year >= 0 ? year : year - 399) / 400;
	unsigned yearOfEra = (unsigned)(year - era * 400);
	unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
	return era * 146097 + (int64_t)dayOfEra - 719468;
}
