// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "batch_season.h"

#include "match_config.h"
#include "run_job.h"

#include <stdio.h>
#include <string.h>


enum { LINE_LENGTH = BATCH_ARM_LENGTH + 2 * SHA256_HEX_LENGTH + 96 };

static const char fixturesFormat[] = "fm24-season-fixtures/1";
static const char planKey[] = "plan_sha256=";
static const char fixtureKey[] = "fixture=";
static const char hashKey[] = "fixtures_sha256=";

static bool nextLine(const char **at, const char *end, char *line, size_t size);
static bool readUnsigned(const char *text, const char **end, uint64_t *value);
static void formatTeam(uint32_t team, char *out, size_t size);

size_t batchSeason_fixtures(size_t teams) {
	return teams * (teams - 1);
}

void batchSeason_pair(size_t teams, size_t fixture, size_t *home, size_t *away) {
	*home = fixture / (teams - 1);
	size_t other = fixture % (teams - 1);
	*away = other < *home ? other : other + 1;
}

size_t batchSeason_fixture(size_t teams, size_t home, size_t away) {
	return home * (teams - 1) + (away < home ? away : away - 1);
}

bool batchSeason_isCheckFixture(size_t teams, size_t fixture) {
	size_t home = 0;
	size_t away = 0;
	batchSeason_pair(teams, fixture, &home, &away);
	if (home % 2 == 0 && away == home + 1) {
		return true;
	}
	return teams % 2 == 1 && home == teams - 1 && away == 0;
}

void batchSeason_id(size_t fixture, char id[BATCH_ARM_LENGTH]) {
	(void)snprintf(id, BATCH_ARM_LENGTH, "f%04zu", fixture + 1);
}

bool batchSeason_formatFixtures(
	const BatchPlan *plan,
	ByteBuffer *out,
	char hash[SHA256_HEX_LENGTH]
) {
	size_t start = out->length;
	char line[LINE_LENGTH];
	int written =
		snprintf(line, sizeof line, "format=%s\n%s%s\n", fixturesFormat, planKey, plan->planSha256);
	bool ok = written > 0 && buffer_append(out, line, (size_t)written);
	for (size_t arm = 0; ok && arm < plan->armCount; ++arm) {
		char hashes[LINE_LENGTH];
		batchPlan_formatHashes(&plan->arms[arm], hashes, sizeof hashes);
		written = snprintf(line, sizeof line, "%s%s %s\n", fixtureKey, plan->arms[arm].id, hashes);
		ok = written > 0 && (size_t)written < sizeof line && buffer_append(out, line, (size_t)written);
	}
	if (!ok) {
		return false;
	}
	sha256_hexOf(out->data + start, out->length - start, hash);
	written = snprintf(line, sizeof line, "%s%s\n", hashKey, hash);
	return written > 0 && buffer_append(out, line, (size_t)written);
}

bool batchSeason_readFixtures(
	const char *text,
	size_t length,
	BatchPlan *plan,
	char hash[SHA256_HEX_LENGTH],
	char *error,
	size_t errorSize
) {
	const char *at = text;
	const char *end = text + length;
	char line[LINE_LENGTH];
	char expected[LINE_LENGTH];
	(void)snprintf(expected, sizeof expected, "%s%s", planKey, plan->planSha256);
	bool ok =
		nextLine(&at, end, line, sizeof line) && strcmp(line, "format=fm24-season-fixtures/1") == 0;
	if (!ok || !nextLine(&at, end, line, sizeof line) || strcmp(line, expected) != 0) {
		(void)snprintf(
			error, errorSize, ok ? "it is of another plan" : "it is not fm24-season-fixtures/1"
		);
		return false;
	}
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		BatchArm *fixture = &plan->arms[arm];
		size_t id = strlen(fixture->id);
		if (!nextLine(&at, end, line, sizeof line) ||
				strncmp(line, fixtureKey, sizeof fixtureKey - 1) != 0 ||
				strncmp(line + sizeof fixtureKey - 1, fixture->id, id) != 0 ||
				line[sizeof fixtureKey - 1 + id] != ' ' ||
				!batchPlan_readHashes(line + sizeof fixtureKey + id, fixture)) {
			(void)snprintf(error, errorSize, "it has no line for fixture %s", fixture->id);
			return false;
		}
	}
	size_t body = (size_t)(at - text);
	char actual[SHA256_HEX_LENGTH];
	sha256_hexOf(text, body, actual);
	(void)snprintf(expected, sizeof expected, "%s%s", hashKey, actual);
	if (!nextLine(&at, end, line, sizeof line) || strcmp(line, expected) != 0 || at != end) {
		(void)snprintf(
			error, errorSize, "it has been changed: its last line is not the hash of the rest"
		);
		return false;
	}
	memcpy(hash, actual, SHA256_HEX_LENGTH);
	return true;
}

bool batchSeason_formatReplace(
	bool away,
	const char *input,
	uint32_t team,
	const uint32_t teams[2],
	const char *output,
	char *out,
	size_t size
) {
	char home[16];
	char other[16];
	(void)snprintf(home, sizeof home, "%.0u", teams[0]);
	(void)snprintf(other, sizeof other, "%.0u", teams[1]);
	int written = snprintf(
		out,
		size,
		"command=replace_%s\ninput=%s\nteam=%u\nhome_team=%s\naway_team=%s\noutput=%s\nfull=0\n",
		away ? "away" : "home",
		input,
		team,
		home,
		other,
		output
	);
	return written > 0 && (size_t)written < size;
}

bool batchSeason_checkReply(const char *result, uint32_t team, char *why, size_t size) {
	static const char marker[] = " (team ";
	const char *last = NULL;
	for (const char *at = strstr(result, marker); at; at = strstr(at + 1, marker)) {
		last = at;
	}
	uint64_t named = 0;
	uint64_t kept = 0;
	const char *after = NULL;
	bool read = strncmp(result, "ok: ", 4) == 0 && last &&
							readUnsigned(last + sizeof marker - 1, &after, &named) &&
							strncmp(after, "); ", 3) == 0 && readUnsigned(after + 3, &after, &kept) &&
							strncmp(after, " edits kept", 11) == 0;
	if (!read) {
		(void)snprintf(why, size, "not a replacement's reply: %s", result);
	} else if (named != team) {
		(void)snprintf(why, size, "it built team %llu, not %u", (unsigned long long)named, team);
	} else if (kept) {
		(void)snprintf(
			why,
			size,
			"it kept %llu edits of the base (export it again unedited)",
			(unsigned long long)kept
		);
	}
	return read && named == team && !kept;
}

bool batchSeason_checkBuilt(
	const BatchPlan *plan,
	const ByteBuffer *json,
	unsigned side,
	uint32_t team,
	const uint32_t input[2],
	ByteBuffer *bin,
	uint32_t teams[2],
	char *why,
	size_t size
) {
	ConfigHeader header;
	SaveCheck save = {0};
	char error[256] = "";
	const char *text = buffer_string(json);
	if (!matchConfig_readHeader(text, json->length, &header) ||
			strcmp(header.format, "fm24-match-config/1") != 0) {
		(void)snprintf(why, size, "it is not a fixture (fm24-match-config/1)");
		return false;
	}
	runJob_readFixtureSave(json, &save);
	if (header.gameBuild != plan->gameBuild) {
		(void)snprintf(why, size, "it is of game build %u, not %u", header.gameBuild, plan->gameBuild);
		return false;
	}
	if (plan->saveName[0] && strcmp(save.save, plan->saveName) != 0) {
		(void)snprintf(why, size, "it is of save \"%s\", not %s", save.save, plan->saveName);
		return false;
	}
	if (plan->gameDate[0] && strcmp(save.date, plan->gameDate) != 0) {
		(void)snprintf(why, size, "it is of in-game date \"%s\", not %s", save.date, plan->gameDate);
		return false;
	}
	if (!matchConfig_encode(text, json->length, bin, teams, error, sizeof error)) {
		(void)snprintf(why, size, "it does not encode: %s", error);
		return false;
	}
	char built[2][16];
	formatTeam(teams[0], built[0], sizeof built[0]);
	formatTeam(teams[1], built[1], sizeof built[1]);
	if ((teams[side] != team && teams[side] != 0) || teams[1 - side] != input[1 - side]) {
		(void)snprintf(why, size, "its team overrides are home %s and away %s", built[0], built[1]);
		return false;
	}
	return true;
}

// The next line from `*at`, without its newline, if it is complete and fits.
static bool nextLine(const char **at, const char *end, char *line, size_t size) {
	const char *newline = memchr(*at, '\n', (size_t)(end - *at));
	size_t length = newline ? (size_t)(newline - *at) : 0;
	if (!newline || length >= size || memchr(*at, 0, length)) {
		return false;
	}
	memcpy(line, *at, length);
	line[length] = 0;
	*at = newline + 1;
	return true;
}

// A decimal number at `text`, up to 2^32 - 1; `*end` is the character after it.
static bool readUnsigned(const char *text, const char **end, uint64_t *value) {
	*value = 0;
	const char *digit = text;
	for (; *digit >= '0' && *digit <= '9'; ++digit) {
		*value = *value * 10 + (uint64_t)(*digit - '0');
		if (*value > UINT32_MAX) {
			return false;
		}
	}
	*end = digit;
	return digit != text;
}

static void formatTeam(uint32_t team, char *out, size_t size) {
	if (team) {
		(void)snprintf(out, size, "%u", team);
	} else {
		(void)snprintf(out, size, "none");
	}
}
