// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Encodes an "fm24-match-config/1" JSON config (written by the DLL) back into the serialised
// GAME_MATCH bytes the game's deserialiser expects.
//
// Each entry of "fields" carries exactly one value, emitted in order:
//   u8/u16/u32/u64  little-endian unsigned integer
//   str                   u32 byte length, then the UTF-8 bytes
//   hex                   raw bytes
//   u8map                 object of u8 values, emitted in key order (a player's ratings)
// Other keys ("site", "p", "role", "name", "player", "member", "flags", ...) are annotations and
// are skipped.
#pragma once

#include "buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	CONFIG_SAVE_LENGTH = 256,   // a "save" of up to 255 UTF-8 bytes and its NUL
	CONFIG_DATE_LENGTH = 32,    // a "game_date" and its NUL
	CONFIG_FORMAT_LENGTH = 64,  // a "format" and its NUL
	CONFIG_VERSION_LENGTH = 32, // an "engine_version" and its NUL
	CONFIG_SOURCE_LENGTH = 512, // a "source" and its NUL
};

typedef enum ConfigValueKind {
	CONFIG_U8 = 1,
	CONFIG_U16 = 2,
	CONFIG_U32 = 4,
	CONFIG_U64 = 8,
	CONFIG_STRING,
	CONFIG_HEX,
	CONFIG_RATINGS,
} ConfigValueKind;

typedef struct ConfigField {
	const char *name;
	const char *player;
	ConfigValueKind kind;
	const uint8_t *bytes; // encoded value, including the length prefix for strings
	size_t length;
	size_t valueOffset; // value token's span in the source JSON, for lossless edits
	size_t valueLength;
	size_t playerOffset; // the "player" annotation's string token (length 0 if none)
	size_t playerLength;
	size_t objectOffset; // the field's object, from its '{' to its '}'
	size_t objectLength;
	size_t nameOffset; // the "name" annotation's string token (length 0 if none)
	size_t nameLength;
} ConfigField;

typedef struct ConfigVisitor {
	void *context;
	void (*field)(void *context, const ConfigField *field);
	void (*source)(void *context, const char *source);
} ConfigVisitor;

// The save a config was exported from and the in-game date then: its "save" and "game_date" header
// keys (see docs/FIXTURE.md). Each text is "" unless the key is a string that fits, without control
// characters. `lineup` holds the "home_lineup" and "away_lineup" markers, set when they are true.
typedef struct ConfigOrigin {
	bool hasSave; // the config has a "save" key, usable or not
	char save[CONFIG_SAVE_LENGTH];
	char gameDate[CONFIG_DATE_LENGTH];
	bool lineup[2]; // the side's players were reordered (fixture_lineup.h)
} ConfigOrigin;

// A config's header, the keys around "fields" that describe it (see docs/FIXTURE.md), as a run's
// manifest records it. Each text is "" unless the key is a string that fits, without control
// characters; each number is 0 unless the key is an unsigned integer that fits.
typedef struct ConfigHeader {
	char format[CONFIG_FORMAT_LENGTH];
	uint32_t gameBuild;
	char engineVersion[CONFIG_VERSION_LENGTH];
	char source[CONFIG_SOURCE_LENGTH];
	ConfigOrigin origin; // "save" and "game_date"
	uint32_t teams[2];   // "home_team" and "away_team", the team overrides
} ConfigHeader;

// Fills `out` and optional home/away replacement IDs (`teams`, 0 for unchanged).
// On failure clears both IDs and returns false with a message in `error`.
bool matchConfig_encode(
	const char *json,
	size_t length,
	ByteBuffer *out,
	uint32_t teams[2],
	char *error,
	size_t errorSize
);

// Validates values with the same encoder, visiting each field once. Borrowed strings and bytes
// remain valid only during the callback. Annotation strings are read only for this operation.
bool matchConfig_visit(
	const char *json,
	size_t length,
	const ConfigVisitor *visitor,
	char *error,
	size_t errorSize
);

// Reads the origin keys of a config's top-level object, skipping everything else. False (with
// `origin` cleared) if the text is not a JSON object.
bool matchConfig_readOrigin(const char *json, size_t length, ConfigOrigin *origin);

// As matchConfig_readOrigin, for the whole header.
bool matchConfig_readHeader(const char *json, size_t length, ConfigHeader *header);

// As matchConfig_visit, for a file of the same field syntax with another "format"
// (e.g. "fm24-player/1", a player's database values from `fm24ctl get-player`).
bool matchConfig_visitFormat(
	const char *json,
	size_t length,
	const char *format,
	const ConfigVisitor *visitor,
	char *error,
	size_t errorSize
);
