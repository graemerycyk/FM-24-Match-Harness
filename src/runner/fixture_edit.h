// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The pure part of saving a fixture: fixture JSON in memory to the bytes a save writes, with no
// file or game access, so that any thread can call it.
#pragma once

#include "buffer.h"
#include "fixture_data.h"

#include <stdbool.h>
#include <stddef.h>


// Applies `players` (NULL or none: unchanged), then the venue `values` (NULL: unchanged), as
// fixtureData_writePlayers and fixtureData_writeVenue do, then encodes the result as a binary
// fixture if `binary`. `out` receives the JSON or binary bytes; on failure it is empty.
bool fixtureEdit_apply(
	const char *json,
	size_t length,
	const FixturePlayerEdits *players,
	const FixtureNumber values[VENUE_FIELD_COUNT],
	bool binary,
	ByteBuffer *out,
	char *error,
	size_t errorSize
);
