// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_edit.h"

#include "match_config.h"

#include <stdio.h>


bool fixtureEdit_apply(
	const char *json,
	size_t length,
	const FixturePlayerEdits *players,
	const FixtureNumber values[VENUE_FIELD_COUNT],
	bool binary,
	ByteBuffer *out,
	char *error,
	size_t errorSize
) {
	buffer_clear(out);
	ByteBuffer patched = {0};
	ByteBuffer edited = {0};
	const char *text = json;
	size_t textLength = length;
	bool ok = true;
	if (fixtureData_hasPlayerEdits(players)) {
		ok = fixtureData_writePlayers(text, textLength, players, &patched, error, errorSize);
		text = buffer_string(&patched);
		textLength = patched.length;
	}
	if (ok && values) {
		ok = fixtureData_writeVenue(text, textLength, values, &edited, error, errorSize);
		text = buffer_string(&edited);
		textLength = edited.length;
	}
	if (ok && binary) {
		ok = matchConfig_encode(text, textLength, out, NULL, error, errorSize);
	} else if (ok && !buffer_append(out, text, textLength)) {
		(void)snprintf(error, errorSize, "Out of memory writing the fixture.");
		ok = false;
	}
	if (!ok) {
		buffer_clear(out);
	}
	buffer_free(&edited);
	buffer_free(&patched);
	return ok;
}
