// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Season batches' plans, fixtures and seeds, and the checks of what batch run builds: pure, with no
// folder or game (batch_test.c runs season batches against its fake game).
#include "batch_plan.h"
#include "batch_season.h"
#include "sha256.h"
#include "test.h"
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const BatchDefaults seasonDefaults = {7, BATCH_COMPRESS_GZIP};

// A season plan of `teams` teams from 1001 on, then `extra`.
static void seasonPlanText(size_t teams, const char *extra, char *out, size_t size) {
	size_t length = (size_t)snprintf(
		out, size, "format=fm24-batch-plan/1\nname=epl\nmode=season\nbase=fixtures/next.json\n"
	);
	for (size_t i = 0; i < teams && length < size; ++i) {
		length += (size_t)snprintf(out + length, size - length, "team=%zu\n", 1001 + i);
	}
	(void)snprintf(out + length, size - length, "%s", extra);
}

static bool
parseSeason(const char *text, bool frozen, BatchPlan *plan, char *error, size_t errorSize) {
	return batchPlan_parse(text, strlen(text), frozen, &seasonDefaults, plan, error, errorSize);
}

TEST(reads_a_season_plan_with_its_defaults) {
	static BatchPlan plan;
	static char text[4096];
	char error[256] = "";
	seasonPlanText(20, "seeds=season:20261027:0-999\nsave=LIV_COPY\n", text, sizeof text);
	bool parsed = parseSeason(text, false, &plan, error, sizeof error);
	if (!parsed) {
		fprintf(stderr, "%s\n", error);
	}
	ASSERT_BOOL_EQ(parsed, true);
	ASSERT_BOOL_EQ(plan.mode == BATCH_MODE_SEASON && plan.teamCount == 20, true);
	ASSERT_BOOL_EQ(plan.teams[0] == 1001 && plan.teams[19] == 1020, true);
	ASSERT_INT_EQ(strcmp(plan.base.path, "fixtures/next.json"), 0);
	ASSERT_BOOL_EQ(plan.seeds.kind == BATCH_SEEDS_SEASON && plan.seeds.key == 20261027, true);
	ASSERT_BOOL_EQ(plan.seeds.first == 0 && plan.seeds.last == 999 && plan.seeds.stride == 380, true);
	// A season's own defaults: 100 seasons a chunk, one canary season, five failed seeds a fixture.
	ASSERT_BOOL_EQ(plan.chunk == 100 && plan.canary == 1 && plan.warmup == 1, true);
	ASSERT_BOOL_EQ(!plan.maxSeedErrors.percent && plan.maxSeedErrors.value == 5, true);
	ASSERT_BOOL_EQ(plan.order == BATCH_ORDER_INTERLEAVE && plan.threads == 7, true);
	ASSERT_BOOL_EQ(plan.armCount == 380 && batchPlan_chunksPerArm(&plan) == 10, true);
	ASSERT_INT_EQ(strcmp(plan.arms[0].id, "f0001"), 0);
	ASSERT_BOOL_EQ(plan.arms[0].pair[0] == 1001 && plan.arms[0].pair[1] == 1002, true);
	ASSERT_INT_EQ(strcmp(plan.arms[19].id, "f0020"), 0);
	ASSERT_BOOL_EQ(plan.arms[19].pair[0] == 1002 && plan.arms[19].pair[1] == 1001, true);
	ASSERT_INT_EQ(strcmp(plan.arms[379].id, "f0380"), 0);
	ASSERT_BOOL_EQ(plan.arms[379].pair[0] == 1020 && plan.arms[379].pair[1] == 1019, true);
	ASSERT_BOOL_EQ(plan.arms[5].teams[0] == 0 && plan.arms[5].path[0] == 0, true);
	batchPlan_free(&plan);
	// Settings given keep their values.
	seasonPlanText(
		3,
		"seeds=season:1:10-19\nchunk=4\ncanary=2\nmax_seed_errors=1%\nmode=season\n",
		text,
		sizeof text
	);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), false);
	ASSERT_BOOL_EQ(strstr(error, "mode is given twice") != NULL, true);
	seasonPlanText(
		3, "seeds=season:1:10-19\nchunk=4\ncanary=2\nmax_seed_errors=1%\n", text, sizeof text
	);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	ASSERT_BOOL_EQ(plan.chunk == 4 && plan.canary == 2 && plan.maxSeedErrors.percent, true);
	ASSERT_BOOL_EQ(plan.armCount == 6 && batchPlan_chunksPerArm(&plan) == 3, true);
	batchPlan_free(&plan);
	// A match plan may say mode=match, and is unchanged by it.
	static const char match[] = "format=fm24-batch-plan/1\nname=m\nmode=match\narm=a a.json\n"
															"seeds=range:1-10\n";
	ASSERT_BOOL_EQ(parseSeason(match, false, &plan, error, sizeof error), true);
	ASSERT_BOOL_EQ(plan.mode == BATCH_MODE_MATCH && plan.chunk == 1000 && plan.canary == 3, true);
	ASSERT_BOOL_EQ(plan.maxSeedErrors.percent && plan.maxSeedErrors.value == 1000, true);
	batchPlan_free(&plan);
}

TEST(refuses_bad_season_plans) {
	static BatchPlan plan;
	static const struct {
		size_t teams;
		const char *extra; // after the teams
		const char *error;
	} cases[] = {
		{20, "seeds=season:1:0-9\narm=a a.json\n", "not arm="},
		{1, "seeds=season:1:0-9\n", "a season needs 2-40 team= lines"},
		{0, "seeds=season:1:0-9\n", "a season needs 2-40 team= lines"},
		{41, "seeds=season:1:0-9\n", "a season has at most 40 teams"},
		{3, "seeds=season:1:0-9\nteam=1002\n", "team 1002 is given twice"},
		{3, "seeds=season:1:0-9\nteam=0\n", "a team is team=<unique ID>"},
		{3, "seeds=season:1:0-9\nteam=Arsenal\n", "a team is team=<unique ID>"},
		{3, "seeds=scatter:1:0-9\n", "a season's seeds are season:"},
		{3, "seeds=range:0-9\n", "a season's seeds are season:"},
		{3, "seeds=season:1:0-9\norder=sequential\n", "order=sequential"},
		{3, "seeds=season:1:0-9\nkeep_build=1\n", "keep_build=1"},
		{3, "seeds=season:1:0-9\nhuman_control=1\n", "human_control=1"},
		{3, "seeds=season:1:0-9\nwarmup=0\n", "warmup=0"},
		{3, "seeds=season:1:0-9\nbase=b.bin\n", "base is given twice"},
		{3, "seeds=season:1:x-9\n", "seeds must be"},
		{3, "seeds=season:1:0-9\nbase_fixture=x\n", "only in a batch's plan.txt"},
		{3, "seeds=season:1:0-9\npair=f0001 home_team=1001 away_team=1002\n", "unknown key pair"},
		// 20 teams play 380 fixtures a season: 2^32 / 380 seasons is the most.
		{20, "seeds=season:1:0-11302545\n", "the last season can be at most 11302544"},
		{40, "seeds=season:1:0-2753184\n", "the last season can be at most 2753183"},
	};
	static char text[8192];
	char error[256];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		seasonPlanText(cases[i].teams, cases[i].extra, text, sizeof text);
		error[0] = 0;
		bool parsed = parseSeason(text, false, &plan, error, sizeof error);
		bool named = strstr(error, cases[i].error) != NULL;
		if (parsed || !named) {
			fprintf(stderr, "case %zu: %s\n", i, error);
		}
		ASSERT_BOOL_EQ(parsed, false);
		ASSERT_BOOL_EQ(named, true);
		ASSERT_BOOL_EQ(plan.arms == NULL, true);
	}
	seasonPlanText(20, "seeds=season:1:0-11302544\n", text, sizeof text);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	batchPlan_free(&plan);
	// No base, and a match plan's season keys.
	static const struct {
		const char *text;
		const char *error;
	} others[] = {
		{"format=fm24-batch-plan/1\nname=s\nmode=season\nteam=1\nteam=2\nseeds=season:1:0-9\n",
			"a season needs base="},
		{"format=fm24-batch-plan/1\nname=m\narm=a a.json\nseeds=season:1:0-9\n",
			"for a season (mode=season)"},
		{"format=fm24-batch-plan/1\nname=m\narm=a a.json\nteam=5\nseeds=range:0-9\n",
			"for a season (mode=season)"},
		{"format=fm24-batch-plan/1\nname=m\narm=a a.json\nbase=b.json\nseeds=range:0-9\n",
			"for a season (mode=season)"},
		{"format=fm24-batch-plan/1\nname=s\nmode=season\nbase=b.json\nteam=1\nteam=2\n",
			"the plan has no seeds="},
		{"format=fm24-batch-plan/1\nname=s\nmode=tournament\n", "mode must be match or season"},
	};
	for (size_t i = 0; i < sizeof others / sizeof others[0]; ++i) {
		error[0] = 0;
		bool parsed = parseSeason(others[i].text, false, &plan, error, sizeof error);
		bool named = strstr(error, others[i].error) != NULL;
		if (parsed || !named) {
			fprintf(stderr, "other case %zu: %s\n", i, error);
		}
		ASSERT_BOOL_EQ(parsed, false);
		ASSERT_BOOL_EQ(named, true);
	}
}

// Fills in what create adds to a plan.
static void fillCreated(BatchPlan *plan) {
	(void)snprintf(plan->base.path, sizeof plan->base.path, "/x/fixtures/next.json");
	memset(plan->base.jsonSha256, 'a', SHA256_HEX_LENGTH - 1);
	memset(plan->base.binSha256, 'b', SHA256_HEX_LENGTH - 1);
	plan->base.teams[1] = 677;
	plan->gameBuild = 2081827;
	strcpy(plan->saveName, "LIV_COPY");
	strcpy(plan->gameDate, "2024 day 123 15:00");
}

TEST(freezes_a_season_plan_that_reads_back) {
	static BatchPlan plan;
	static BatchPlan again;
	static char text[4096];
	char error[256] = "";
	seasonPlanText(3, "seeds=season:20261027:0-3\nchunk=2\nsave=LIV_COPY\n", text, sizeof text);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	fillCreated(&plan);
	ByteBuffer frozen = {0};
	ByteBuffer twice = {0};
	ASSERT_BOOL_EQ(batchPlan_format(&plan, &frozen), true);
	const char *body = buffer_string(&frozen);
	static const char *const lines[] = {
		"\nname=epl\nmode=season\nbase=/x/fixtures/next.json\nteam=1001\nteam=1002\nteam=1003\n"
		"seeds=season:20261027:0-3\nchunk=2\n",
		"\ncanary=1\n",
		"\nmax_seed_errors=5\n",
		"\nsave=LIV_COPY\nbase_fixture=json_sha256=aaaa",
		" home_team=- away_team=677\npair=f0001 home_team=1001 away_team=1002\n"
		"pair=f0002 home_team=1001 away_team=1003\npair=f0003 home_team=1002 away_team=1001\n",
		"\npair=f0006 home_team=1003 away_team=1002\nsave_name=LIV_COPY\n",
	};
	for (size_t i = 0; i < sizeof lines / sizeof lines[0]; ++i) {
		bool found = strstr(body, lines[i]) != NULL;
		if (!found) {
			fprintf(stderr, "frozen plan lacks %s:\n%s", lines[i], body);
		}
		ASSERT_BOOL_EQ(found, true);
	}
	ASSERT_BOOL_EQ(strstr(body, "\narm=") == NULL && strstr(body, "\nfixture=") == NULL, true);
	ASSERT_BOOL_EQ(parseSeason(body, true, &again, error, sizeof error), true);
	ASSERT_BOOL_EQ(batchPlan_format(&again, &twice), true);
	ASSERT_BOOL_EQ(
		frozen.length == twice.length && memcmp(frozen.data, twice.data, frozen.length) == 0, true
	);
	ASSERT_INT_EQ(strcmp(again.planSha256, plan.planSha256), 0);
	ASSERT_BOOL_EQ(again.armCount == 6 && again.base.teams[1] == 677 && again.chunk == 2, true);
	ASSERT_INT_EQ(strcmp(again.base.jsonSha256, plan.base.jsonSha256), 0);
	batchPlan_free(&again);
	// A pair line that is not the teams' fixture, or a missing one, is refused even with the hash
	// made again.
	static const struct {
		const char *from;
		const char *to;
	} edits[] = {
		{"pair=f0002 home_team=1001 away_team=1003\n", "pair=f0002 home_team=1003 away_team=1001\n"},
		{"pair=f0006 home_team=1003 away_team=1002\n", ""},
		{"base_fixture=", "base_fixtures="},
	};
	for (size_t i = 0; i < sizeof edits / sizeof edits[0]; ++i) {
		static char edited[8192];
		const char *at = strstr(body, edits[i].from);
		ASSERT_BOOL_EQ(at != NULL, true);
		if (!at) {
			continue;
		}
		size_t before = (size_t)(at - body);
		size_t hashLine = sizeof "plan_sha256=" - 1 + SHA256_HEX_LENGTH;
		int length = snprintf(
			edited,
			sizeof edited,
			"%.*s%s%.*s",
			(int)before,
			body,
			edits[i].to,
			(int)(frozen.length - hashLine - before - strlen(edits[i].from)),
			at + strlen(edits[i].from)
		);
		char hash[SHA256_HEX_LENGTH];
		sha256_hexOf(edited, (size_t)length, hash);
		(void)snprintf(edited + length, sizeof edited - (size_t)length, "plan_sha256=%s\n", hash);
		error[0] = 0;
		ASSERT_BOOL_EQ(parseSeason(edited, true, &again, error, sizeof error), false);
		ASSERT_BOOL_EQ(again.arms == NULL, true);
	}
	buffer_free(&frozen);
	buffer_free(&twice);
	batchPlan_free(&plan);
}

// A match plan's frozen text is as it was before seasons: no mode, base or pair lines.
TEST(freezes_a_match_plan_as_before) {
	static BatchPlan plan;
	char error[256];
	static const char text[] = "format=fm24-batch-plan/1\nname=n\narm=a /x/a.json\n"
														 "seeds=scatter:9:0-99\nchunk=10\nsave=LIV_COPY\n";
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	memset(plan.arms[0].jsonSha256, 'a', SHA256_HEX_LENGTH - 1);
	memset(plan.arms[0].binSha256, 'b', SHA256_HEX_LENGTH - 1);
	plan.arms[0].teams[1] = 677;
	plan.gameBuild = 2081827;
	strcpy(plan.saveName, "LIV_COPY");
	strcpy(plan.gameDate, "2024 day 123 15:00");
	ByteBuffer frozen = {0};
	ASSERT_BOOL_EQ(batchPlan_format(&plan, &frozen), true);
	static const char expected[] =
		"format=fm24-batch-plan/1\n"
		"# Frozen by fm24ctl batch create; do not edit. The last line is the SHA-256 of the lines "
		"above it.\n"
		"name=n\narm=a /x/a.json\nseeds=scatter:9:0-99\nchunk=10\nthreads=7\nper_match=1\n"
		"keep_build=0\nhuman_control=0\nseed_errors=skip\nmax_seed_errors=1%\norder=interleave\n"
		"compress=gzip\nwarmup=1\ncanary=3\ncanary_every=50\non_canary_mismatch=stop\nslice=arm64\n"
		"launch=1\nany_save=0\nkill_game=0\nhang_minutes=10\nqueue_wait_minutes=360\n"
		"busy_wait_minutes=15\nmax_crashes_per_hour=4\nmax_poison=3\npause_on_battery=1\n"
		"min_free_gb=20\nnotify=1\ntrim_game_logs=1\nsave=LIV_COPY\n"
		"fixture=a json_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa "
		"bin_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb home_team=- "
		"away_team=677\n"
		"save_name=LIV_COPY\ngame_build=2081827\ngame_date=2024 day 123 15:00\n";
	char hash[SHA256_HEX_LENGTH];
	sha256_hexOf(expected, sizeof expected - 1, hash);
	bool same =
		frozen.length == sizeof expected - 1 + sizeof "plan_sha256=" - 1 + SHA256_HEX_LENGTH &&
		memcmp(frozen.data, expected, sizeof expected - 1) == 0 && strcmp(plan.planSha256, hash) == 0;
	if (!same) {
		fprintf(stderr, "%s", buffer_string(&frozen));
	}
	ASSERT_BOOL_EQ(same, true);
	buffer_free(&frozen);
	batchPlan_free(&plan);
}

TEST(numbers_a_seasons_fixtures) {
	for (size_t teams = 2; teams <= BATCH_MAX_TEAMS; ++teams) {
		size_t fixtures = batchSeason_fixtures(teams);
		bool ok = fixtures == teams * (teams - 1);
		static bool seen[BATCH_MAX_TEAMS][BATCH_MAX_TEAMS];
		memset(seen, 0, sizeof seen);
		unsigned covered[BATCH_MAX_TEAMS] = {0};
		size_t checks = 0;
		for (size_t fixture = 0; ok && fixture < fixtures; ++fixture) {
			size_t home = 0;
			size_t away = 0;
			batchSeason_pair(teams, fixture, &home, &away);
			ok = home < teams && away < teams && home != away && !seen[home][away] &&
					 batchSeason_fixture(teams, home, away) == fixture;
			seen[home][away] = true;
			if (batchSeason_isCheckFixture(teams, fixture)) {
				++checks;
				++covered[home];
				++covered[away];
			}
		}
		for (size_t team = 0; ok && team < teams; ++team) {
			ok = covered[team] >= 1 && (covered[team] == 1 || (teams % 2 && team == 0));
		}
		ok = ok && checks == (teams + 1) / 2;
		if (!ok) {
			fprintf(stderr, "%zu teams\n", teams);
		}
		ASSERT_BOOL_EQ(ok, true);
	}
	size_t home = 0;
	size_t away = 0;
	batchSeason_pair(20, 0, &home, &away);
	ASSERT_BOOL_EQ(home == 0 && away == 1, true);
	batchSeason_pair(20, 19, &home, &away);
	ASSERT_BOOL_EQ(home == 1 && away == 0, true);
	batchSeason_pair(20, 379, &home, &away);
	ASSERT_BOOL_EQ(home == 19 && away == 18, true);
	// Three teams: t0 v t1, and t2 v t0 for the third.
	ASSERT_BOOL_EQ(batchSeason_isCheckFixture(3, 0) && batchSeason_isCheckFixture(3, 4), true);
	ASSERT_BOOL_EQ(!batchSeason_isCheckFixture(3, 1) && !batchSeason_isCheckFixture(3, 5), true);
	char id[BATCH_ARM_LENGTH];
	batchSeason_id(0, id);
	ASSERT_INT_EQ(strcmp(id, "f0001"), 0);
	batchSeason_id(1559, id);
	ASSERT_INT_EQ(strcmp(id, "f1560"), 0);
}

static int compareSeedValues(const void *a, const void *b) {
	uint32_t left = *(const uint32_t *)a;
	uint32_t right = *(const uint32_t *)b;
	return (left > right) - (left < right);
}

TEST(plays_each_season_fixture_its_own_seeds) {
	static BatchPlan plan;
	static char text[4096];
	char error[256];
	seasonPlanText(20, "seeds=season:20261027:0-999\n", text, sizeof text);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	// The goldens tools/batch/fm24seeds.py shares: (season, fixture) of key 20261027, 380 fixtures.
	static const struct {
		uint64_t season;
		size_t fixture;
		uint32_t seed;
	} goldens[] = {
		{0, 0, 2818942921U}, {0, 1, 828994977U}, {1, 0, 3825868651U}, {999, 379, 999216404U}};
	for (size_t i = 0; i < sizeof goldens / sizeof goldens[0]; ++i) {
		BatchSeeds seeds = batchPlan_armSeeds(&plan, goldens[i].fixture);
		ASSERT_BOOL_EQ(batchSeeds_at(&seeds, goldens[i].season) == goldens[i].seed, true);
	}
	// Every match of the batch plays a seed of its own.
	uint32_t *all = malloc(380000 * sizeof *all);
	ASSERT_BOOL_EQ(all != NULL, true);
	if (all) {
		for (size_t fixture = 0; fixture < 380; ++fixture) {
			BatchSeeds seeds = batchPlan_armSeeds(&plan, fixture);
			for (uint64_t season = 0; season < 1000; ++season) {
				all[season * 380 + fixture] = batchSeeds_at(&seeds, season);
			}
		}
		qsort(all, 380000, sizeof *all, compareSeedValues);
		bool distinct = true;
		for (size_t i = 1; i < 380000; ++i) {
			distinct = distinct && all[i] != all[i - 1];
		}
		ASSERT_BOOL_EQ(distinct, true);
		free(all);
	}
	// A part's seeds are listed, as scattered ones are.
	BatchSeeds seeds = batchPlan_armSeeds(&plan, 1);
	char listed[64];
	ASSERT_BOOL_EQ(batchSeeds_format(&seeds, 0, 0, listed, sizeof listed), true);
	ASSERT_INT_EQ(strcmp(listed, "828994977"), 0);
	// A match plan's arms play its seeds.
	BatchSeeds match = {.kind = BATCH_SEEDS_SCATTER, .key = 20261025, .last = 9};
	BatchPlan matchPlan = {.seeds = match};
	BatchSeeds same = batchPlan_armSeeds(&matchPlan, 3);
	ASSERT_BOOL_EQ(batchSeeds_at(&same, 0) == 4210832071U, true);
	ASSERT_BOOL_EQ(batchPlan_isCheckArm(&matchPlan, 3), true);
	batchPlan_free(&plan);
	// 40 teams: f1560's jobs, with the longest overrides, fit as a scattered batch's do.
	seasonPlanText(40, "seeds=season:1:0-2753183\n", text, sizeof text);
	bool parsed = parseSeason(text, false, &plan, error, sizeof error);
	ASSERT_BOOL_EQ(parsed && plan.armCount == 1560, true);
	if (!parsed) {
		return;
	}
	ASSERT_INT_EQ(strcmp(plan.arms[1559].id, "f1560"), 0);
	static const char directory[] =
		"/Users/you/fm24-batches/liv-ars-2026-10-with-a-rather-long-name-for-its-folder-to-check-cap";
	uint32_t largest = batchPlan_largestChunk(&plan, directory);
	ASSERT_BOOL_EQ(largest > 1400 && largest < 1460, true);
	ASSERT_BOOL_EQ(batchPlan_longestJob(&plan, directory, largest) < HARNESS_JOB_LENGTH, true);
	static char job[HARNESS_JOB_LENGTH];
	static char many[HARNESS_JOB_LENGTH];
	size_t length = 0;
	for (uint32_t i = 0; i < largest; ++i) {
		length += (size_t)snprintf(many + length, sizeof many - length, "%s4294967295", i ? "," : "");
	}
	BatchPartPaths paths;
	plan.arms[1559].teams[0] = UINT32_MAX;
	plan.arms[1559].teams[1] = UINT32_MAX;
	ASSERT_BOOL_EQ(batchPlan_partPaths(directory, "f1560", 4294967295ULL, UINT32_MAX, &paths), true);
	ASSERT_BOOL_EQ(
		batchPlan_formatJob(&plan, 1559, &paths, many, 2753183, 16, job, sizeof job), true
	);
	ASSERT_BOOL_EQ(strlen(job) <= batchPlan_longestJob(&plan, directory, largest), true);
	ASSERT_BOOL_EQ(batchPlan_isCheckArm(&plan, 0) && !batchPlan_isCheckArm(&plan, 1), true);
	batchPlan_free(&plan);
}

TEST(writes_and_reads_inputs_fixtures_txt) {
	static BatchPlan plan;
	static char text[4096];
	char error[256] = "";
	seasonPlanText(3, "seeds=season:1:0-3\n", text, sizeof text);
	ASSERT_BOOL_EQ(parseSeason(text, false, &plan, error, sizeof error), true);
	memset(plan.planSha256, 'c', SHA256_HEX_LENGTH - 1);
	for (size_t arm = 0; arm < plan.armCount; ++arm) {
		memset(plan.arms[arm].jsonSha256, (int)('0' + arm), SHA256_HEX_LENGTH - 1);
		memset(plan.arms[arm].binSha256, (int)('a' + arm), SHA256_HEX_LENGTH - 1);
		plan.arms[arm].teams[0] = arm == 0 ? 0 : plan.arms[arm].pair[0];
		plan.arms[arm].teams[1] = plan.arms[arm].pair[1];
	}
	ByteBuffer out = {0};
	char hash[SHA256_HEX_LENGTH];
	ASSERT_BOOL_EQ(batchSeason_formatFixtures(&plan, &out, hash), true);
	const char *written = buffer_string(&out);
	ASSERT_BOOL_EQ(
		strncmp(written, "format=fm24-season-fixtures/1\nplan_sha256=cccc", 46) == 0, true
	);
	ASSERT_BOOL_EQ(
		strstr(written, "\nfixture=f0001 json_sha256=0000") != NULL &&
			strstr(written, " home_team=- away_team=1002\nfixture=f0002 ") != NULL,
		true
	);
	char expected[SHA256_HEX_LENGTH];
	size_t body = out.length - (sizeof "fixtures_sha256=" - 1) - SHA256_HEX_LENGTH;
	sha256_hexOf(out.data, body, expected);
	ASSERT_INT_EQ(strcmp(expected, hash), 0);
	// Read into a fresh plan, it gives the same.
	static BatchPlan fresh;
	ASSERT_BOOL_EQ(parseSeason(text, false, &fresh, error, sizeof error), true);
	memcpy(fresh.planSha256, plan.planSha256, SHA256_HEX_LENGTH);
	char read[SHA256_HEX_LENGTH] = "";
	ASSERT_BOOL_EQ(
		batchSeason_readFixtures(written, out.length, &fresh, read, error, sizeof error), true
	);
	ASSERT_INT_EQ(strcmp(read, hash), 0);
	bool same = true;
	for (size_t arm = 0; arm < plan.armCount; ++arm) {
		same = same && strcmp(fresh.arms[arm].jsonSha256, plan.arms[arm].jsonSha256) == 0 &&
					 strcmp(fresh.arms[arm].binSha256, plan.arms[arm].binSha256) == 0 &&
					 fresh.arms[arm].teams[0] == plan.arms[arm].teams[0] &&
					 fresh.arms[arm].teams[1] == plan.arms[arm].teams[1];
	}
	ASSERT_BOOL_EQ(same, true);
	// A changed byte, another plan's, a missing line and anything after the hash are refused.
	ByteBuffer copy = {0};
	ASSERT_BOOL_EQ(buffer_append(&copy, out.data, out.length), true);
	copy.data[60] ^= 1;
	ASSERT_BOOL_EQ(
		batchSeason_readFixtures(buffer_string(&copy), copy.length, &fresh, read, error, sizeof error),
		false
	);
	copy.data[60] ^= 1;
	fresh.planSha256[0] = 'd';
	ASSERT_BOOL_EQ(
		batchSeason_readFixtures(buffer_string(&copy), copy.length, &fresh, read, error, sizeof error),
		false
	);
	ASSERT_BOOL_EQ(strstr(error, "another plan") != NULL, true);
	fresh.planSha256[0] = 'c';
	ASSERT_BOOL_EQ(buffer_append(&copy, "x\n", 2), true);
	ASSERT_BOOL_EQ(
		batchSeason_readFixtures(buffer_string(&copy), copy.length, &fresh, read, error, sizeof error),
		false
	);
	ASSERT_BOOL_EQ(
		batchSeason_readFixtures(written, body / 2, &fresh, read, error, sizeof error), false
	);
	buffer_free(&copy);
	buffer_free(&out);
	batchPlan_free(&fresh);
	batchPlan_free(&plan);
}

TEST(formats_and_checks_replacements) {
	char job[1024];
	static const uint32_t none[2] = {0, 677};
	ASSERT_BOOL_EQ(
		batchSeason_formatReplace(
			false, "/b/inputs/base.bin", 1131, none, "/b/x.json", job, sizeof job
		),
		true
	);
	ASSERT_INT_EQ(
		strcmp(
			job,
			"command=replace_home\ninput=/b/inputs/base.bin\nteam=1131\nhome_team=\naway_team=677\n"
			"output=/b/x.json\nfull=0\n"
		),
		0
	);
	static const uint32_t both[2] = {1131, 677};
	ASSERT_BOOL_EQ(
		batchSeason_formatReplace(true, "/i.bin", 7, both, "/o.json", job, sizeof job), true
	);
	ASSERT_BOOL_EQ(strstr(job, "command=replace_away\n") == job, true);
	ASSERT_BOOL_EQ(strstr(job, "\nhome_team=1131\naway_team=677\n") != NULL, true);
	ASSERT_BOOL_EQ(batchSeason_formatReplace(true, "/i.bin", 7, both, "/o.json", job, 20), false);
	char why[256];
	static const char good[] =
		"ok: Liverpool vs Arsenal, 2024 day 123 15:00, seed 42 (team 1131); 0 edits kept, 3 edits to "
		"the old home team dropped";
	ASSERT_BOOL_EQ(batchSeason_checkReply(good, 1131, why, sizeof why), true);
	ASSERT_BOOL_EQ(batchSeason_checkReply(good, 1132, why, sizeof why), false);
	ASSERT_BOOL_EQ(strstr(why, "built team 1131, not 1132") != NULL, true);
	static const char kept[] =
		"ok: Liverpool vs Arsenal, 2024 day 123 15:00, seed 42 (team 1131); 2 edits kept, 0 edits to "
		"the old home team dropped";
	ASSERT_BOOL_EQ(batchSeason_checkReply(kept, 1131, why, sizeof why), false);
	ASSERT_BOOL_EQ(strstr(why, "kept 2 edits") != NULL, true);
	ASSERT_BOOL_EQ(
		batchSeason_checkReply("error: no team matches 1131", 1131, why, sizeof why), false
	);
	// A club named like the reply's end does not fool it: the last "(team" counts.
	static const char named[] =
		"ok: X (team 5); 0 edits kept, vs Arsenal, day 1, seed 4 (team 9); 1 edits kept, 0 edits to "
		"the old away team dropped";
	ASSERT_BOOL_EQ(batchSeason_checkReply(named, 5, why, sizeof why), false);
	ASSERT_BOOL_EQ(batchSeason_checkReply(named, 9, why, sizeof why), false);
}

static void fixtureJson(ByteBuffer *out, const char *save, const char *date, const char *teams) {
	buffer_clear(out);
	char text[512];
	int length = snprintf(
		text,
		sizeof text,
		"{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"save\":\"%s\",\"game_date\":"
		"\"%s\"%s,\"fields\":[{\"u8\":42}]}",
		save,
		date,
		teams
	);
	(void)buffer_append(out, text, (size_t)length);
}

TEST(checks_the_fixtures_a_replacement_builds) {
	static BatchPlan plan;
	plan.gameBuild = 2081827;
	strcpy(plan.saveName, "LIV_COPY");
	strcpy(plan.gameDate, "2024 day 123 15:00");
	ByteBuffer json = {0};
	ByteBuffer bin = {0};
	uint32_t teams[2] = {0};
	char why[256];
	static const uint32_t base[2] = {0, 677};
	static const char date[] = "2024 day 123 15:00";
	// Home replaced with 1131, the away override kept; then a team that was the side's own.
	fixtureJson(&json, "LIV_COPY", date, ",\"home_team\":1131,\"away_team\":677");
	ASSERT_BOOL_EQ(
		batchSeason_checkBuilt(&plan, &json, 0, 1131, base, &bin, teams, why, sizeof why), true
	);
	ASSERT_BOOL_EQ(teams[0] == 1131 && teams[1] == 677 && bin.length == 1, true);
	buffer_clear(&bin);
	fixtureJson(&json, "LIV_COPY", date, ",\"away_team\":677");
	ASSERT_BOOL_EQ(
		batchSeason_checkBuilt(&plan, &json, 0, 1131, base, &bin, teams, why, sizeof why), true
	);
	ASSERT_BOOL_EQ(teams[0] == 0, true);
	static const struct {
		const char *save;
		const char *date;
		const char *teams;
		unsigned side;
		const char *why;
	} bad[] = {
		{"OTHER", date, ",\"home_team\":1131,\"away_team\":677", 0, "of save \"OTHER\""},
		{"LIV_COPY", "2024 day 124 15:00", ",\"home_team\":1131,\"away_team\":677", 0, "date"},
		{"LIV_COPY", date, ",\"home_team\":1132,\"away_team\":677", 0, "overrides are home 1132"},
		{"LIV_COPY", date, ",\"home_team\":1131", 0, "away none"},
		{"LIV_COPY", date, ",\"home_team\":1131,\"away_team\":677", 1, "overrides"},
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		fixtureJson(&json, bad[i].save, bad[i].date, bad[i].teams);
		buffer_clear(&bin);
		why[0] = 0;
		bool ok =
			batchSeason_checkBuilt(&plan, &json, bad[i].side, 1131, base, &bin, teams, why, sizeof why);
		if (ok || !strstr(why, bad[i].why)) {
			fprintf(stderr, "case %zu: %s\n", i, why);
		}
		ASSERT_BOOL_EQ(ok, false);
		ASSERT_BOOL_EQ(strstr(why, bad[i].why) != NULL, true);
	}
	plan.gameBuild = 2081828;
	fixtureJson(&json, "LIV_COPY", date, ",\"home_team\":1131,\"away_team\":677");
	ASSERT_BOOL_EQ(
		batchSeason_checkBuilt(&plan, &json, 0, 1131, base, &bin, teams, why, sizeof why), false
	);
	ASSERT_BOOL_EQ(strstr(why, "game build 2081827, not 2081828") != NULL, true);
	plan.gameBuild = 2081827;
	buffer_clear(&json);
	static const char notFixture[] = "{\"format\":\"fm24-player/1\",\"fields\":[]}";
	(void)buffer_append(&json, notFixture, sizeof notFixture - 1);
	ASSERT_BOOL_EQ(
		batchSeason_checkBuilt(&plan, &json, 0, 1131, base, &bin, teams, why, sizeof why), false
	);
	buffer_clear(&json);
	static const char unencodable[] =
		"{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"save\":\"LIV_COPY\","
		"\"game_date\":\"2024 day 123 "
		"15:00\",\"home_team\":1131,\"away_team\":677,\"fields\":[{\"u7\":1}]}";
	(void)buffer_append(&json, unencodable, sizeof unencodable - 1);
	ASSERT_BOOL_EQ(
		batchSeason_checkBuilt(&plan, &json, 0, 1131, base, &bin, teams, why, sizeof why), false
	);
	ASSERT_BOOL_EQ(strstr(why, "does not encode") != NULL, true);
	buffer_free(&json);
	buffer_free(&bin);
}

void registerBatchSeasonTests(void) {
	test_run("reads_a_season_plan_with_its_defaults", test_reads_a_season_plan_with_its_defaults);
	test_run("refuses_bad_season_plans", test_refuses_bad_season_plans);
	test_run("freezes_a_season_plan_that_reads_back", test_freezes_a_season_plan_that_reads_back);
	test_run("freezes_a_match_plan_as_before", test_freezes_a_match_plan_as_before);
	test_run("numbers_a_seasons_fixtures", test_numbers_a_seasons_fixtures);
	test_run("plays_each_season_fixture_its_own_seeds", test_plays_each_season_fixture_its_own_seeds);
	test_run("writes_and_reads_inputs_fixtures_txt", test_writes_and_reads_inputs_fixtures_txt);
	test_run("formats_and_checks_replacements", test_formats_and_checks_replacements);
	test_run(
		"checks_the_fixtures_a_replacement_builds", test_checks_the_fixtures_a_replacement_builds
	);
}
