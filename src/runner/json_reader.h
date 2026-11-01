// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// A minimal streaming JSON reader: enough to walk a match config, but it accepts any valid JSON.
#pragma once

#include "buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>


typedef struct JsonReader {
	const char *begin;
	const char *position;
	const char *end;
	char error[192]; // the first error, with its byte offset
} JsonReader;

void jsonReader_init(JsonReader *reader, const char *text, size_t length);

// Records an error (the first one wins) and returns false.
bool jsonReader_fail(JsonReader *reader, const char *message);

// Skips whitespace, then consumes `c` if it is next.
bool jsonReader_consume(JsonReader *reader, char c);

// Reads a string into `out` (decoding escapes to UTF-8), or validates it if `out` is NULL.
bool jsonReader_readString(JsonReader *reader, ByteBuffer *out);

// Reads a number; `isUnsigned` says whether it was a plain non-negative integer (then `value` holds
// it, unless `value` is NULL).
bool jsonReader_readNumber(JsonReader *reader, unsigned long long *value, bool *isUnsigned);

int jsonReader_hexDigit(uint8_t c);

// Skips any value.
bool jsonReader_skipValue(JsonReader *reader);

// A whole JSON document as its scalar values by path, for reading back small documents such as
// manifests: "rows.results" is member "results" of member "rows", "parts.0.count" member "count" of
// the first element of "parts". Keys with a '.' cannot be told apart.
typedef struct JsonFlat {
	ByteBuffer entries; // each: its path, NUL, its kind, its text, NUL
} JsonFlat;

// Reads `text`, replacing what `flat` held. False if it is not one JSON value, nests too deeply or
// has a string with a NUL.
bool jsonFlat_read(JsonFlat *flat, const char *text, size_t length);
void jsonFlat_free(JsonFlat *flat);
// The text of the value at `path` ("" for null) and its kind: 's' a string, 'n' a number, 'b'
// true or false, 'z' null. NULL if there is none.
const char *jsonFlat_find(const JsonFlat *flat, const char *path, char *kind);
// A non-negative integer up to UINT64_MAX.
bool jsonFlat_unsigned(const JsonFlat *flat, const char *path, uint64_t *value);
// A string that fits in `size` with its NUL.
bool jsonFlat_string(const JsonFlat *flat, const char *path, char *out, size_t size);
bool jsonFlat_bool(const JsonFlat *flat, const char *path, bool *value);
bool jsonFlat_isNull(const JsonFlat *flat, const char *path);
// A UTC time as jsonWriter_utcTime writes it ("2026-10-25T09:23:18Z").
bool jsonFlat_time(const JsonFlat *flat, const char *path, time_t *seconds);
