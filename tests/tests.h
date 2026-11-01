// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FM24_TESTS_H
#define FM24_TESTS_H

#include "buffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

void fixtureTest_config(ByteBuffer *out);

// A synthetic GAME_MATCH (dll_test.c) whose players' statistics follow from `seed`, or NULL if
// memory runs out; its seed and score; and freeing it.
uint8_t *dllTest_makeMatch(uint32_t seed);
void dllTest_matchScore(const uint8_t *match, uint32_t *seed, unsigned goals[2]);
void dllTest_freeMatch(uint8_t *match);
// The per-match player and team rows the harness writes for that match (matchStats_formatRows),
// appended to `players` and `teams`.
bool dllTest_formatRows(uint32_t seed, uint64_t index, ByteBuffer *players, ByteBuffer *teams);
// The harness's per-match headers (match_stats.c).
extern const char matchStats_playerRowsHeader[];
extern const char matchStats_teamRowsHeader[];

// The result line job_run.c gives with report=1 for a summary with every field at its longest, or
// for one of nothing from a build of unknown commit (job_run_test.c).
void jobRunTest_reportLine(bool longest, char *out, size_t size);

// For batch run against the stand-in harness (macos_test.c), from batch_test.c: a new batch of
// the standard two arms and settings, then `settings`, in <folder>/batch (`directory`); the files
// and result line of a run job, as batch_test.c's fake game plays it for game process `pid`, loaded
// at `loaded` and given `jobs` jobs, with report=1 (false if `atSeed` stopped it at an index, with
// that seed's results row half written, as a crash would); and whether a finished batch has every
// index once, verify --deep's exit code, through `host`, and batch stop's (`now`: --now).
typedef struct BatchTestGame {
	uint32_t pid;
	int64_t loaded;
	uint64_t jobs;
	bool (*atSeed)(void *context, uint64_t index); // NULL, or false to stop there
	void *context;
} BatchTestGame;
struct BatchHost;
bool batchTest_createBatch(const char *settings, wchar_t *folder, wchar_t *directory);
// The same for a season of three teams (11, 22 and 33) from a base of 11 at home to 22, seasons
// 0-3 in blocks of 2, and `settings`.
bool batchTest_createSeasonBatch(const char *settings, wchar_t *folder, wchar_t *directory);
bool batchTest_playJob(const char *text, const BatchTestGame *game, char *result, size_t size);
bool batchTest_checkFinished(const wchar_t *directory, const struct BatchHost *host);
int batchTest_verify(const wchar_t *directory, const struct BatchHost *host);
int batchTest_stop(const wchar_t *directory, bool now);

void registerJobTests(void);
void registerDllTests(void);
void registerJobRunTests(void);
void registerCommonTests(void);
void registerFixtureDataTests(void);
void registerFixtureBaselineTests(const char *path);
void registerRunnerMainTests(void);
void registerRunnerTests(void);
void registerBatchTests(void);
// The season tests that need no fake game, which registerBatchTests runs (batch_season_test.c).
void registerBatchSeasonTests(void);
void registerFixtureExportTests(void);
void registerFixtureViewTests(void);
void registerFixtureWindowTests(void);
void registerFixtureLoadTests(void);
void registerPlayerInjectTests(void);
void registerFixtureLineupTests(void);
void registerMacosTests(void);

#endif
