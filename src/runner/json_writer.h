// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Compact JSON text for manifests, appended to a byte buffer. Every value call takes the member's
// key inside an object and NULL elsewhere, and the writer puts in the commas. A misuse (a missing
// or extra key, an end that does not match, nesting deeper than JSON_WRITER_DEPTH, a second
// top-level value) or running out of memory is remembered, and jsonWriter_finish reports it.
#pragma once

#include "buffer.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>


enum { JSON_WRITER_DEPTH = 32 };

typedef struct JsonWriter {
	ByteBuffer *out;
	unsigned depth;   // open objects and arrays
	uint32_t objects; // bit d: the container at depth d + 1 is an object
	uint32_t filled;  // bit d: that container has a value already
	bool started;     // the top-level value has begun
	bool failed;
} JsonWriter;

// Starts one top-level value at the end of `out`.
void jsonWriter_start(JsonWriter *writer, ByteBuffer *out);
void jsonWriter_beginObject(JsonWriter *writer, const char *key);
void jsonWriter_endObject(JsonWriter *writer);
void jsonWriter_beginArray(JsonWriter *writer, const char *key);
void jsonWriter_endArray(JsonWriter *writer);
// UTF-8 text, escaped as JSON requires; each byte that is not part of valid UTF-8 becomes U+FFFD.
// NULL writes null.
void jsonWriter_string(JsonWriter *writer, const char *key, const char *text);
void jsonWriter_unsigned(JsonWriter *writer, const char *key, uint64_t value);
void jsonWriter_signed(JsonWriter *writer, const char *key, int64_t value);
// `value` with `decimals` digits after the point (0-9); null if it is not finite.
void jsonWriter_decimal(JsonWriter *writer, const char *key, double value, int decimals);
void jsonWriter_bool(JsonWriter *writer, const char *key, bool value);
void jsonWriter_null(JsonWriter *writer, const char *key);
// A time as UTC text, "2026-10-25T09:23:18Z"; null if it cannot be converted.
void jsonWriter_utcTime(JsonWriter *writer, const char *key, time_t seconds);
// True if one complete value was written and nothing failed.
bool jsonWriter_finish(const JsonWriter *writer);
