// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_edit.h"

#include "match_config.h"

#include <stdio.h>


bool fixtureEdit_apply(
	const char *json,
	size_t length,
	const FixturePlayerEdits *players,
	const FixtureLineup *lineup,
	const FixtureNumber values[VENUE_FIELD_COUNT],
	bool binary,
	ByteBuffer *out,
	char *error,
	size_t errorSize
) {
	buffer_clear(out);
	ByteBuffer patched = {0};
	ByteBuffer ordered = {0};
	ByteBuffer edited = {0};
	const char *text = json;
	size_t textLength = length;
	bool ok = true;
	if (fixtureData_hasPlayerEdits(players)) {
		ok = fixtureData_writePlayers(text, textLength, players, &patched, error, errorSize);
		text = buffer_string(&patched);
		textLength = patched.length;
	}
	// The Max patches name the blocks as they were, so the order comes after them.
	if (ok && lineup && (lineup->changed[0] || lineup->changed[1])) {
		FixtureLineupResult result;
		char problem[256] = "";
		ok = fixtureLineup_apply(text, textLength, lineup, &ordered, &result, problem, sizeof problem);
		if (!ok) {
			(void)snprintf(error, errorSize, "Cannot write the new line-up: %s.", problem);
		}
		text = buffer_string(&ordered);
		textLength = ordered.length;
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
	buffer_free(&ordered);
	buffer_free(&patched);
	return ok;
}
