// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// `fm24ctl batch`: creates a batch from a plan, and runs, shows, verifies and stops one (see
// docs/CLI.md "Batches"). batch_plan.c reads plans, batch_verify.c checks rows, batch_store.c keeps
// a batch's folder and batch_run.c plays it.

#include "batch_plan.h"
#include "batch_run.h"
#include "batch_season.h"
#include "batch_store.h"
#include "commands.h"
#include "files.h"
#include "fixture_data.h"
#include "match_config.h"
#include "paths.h"
#include "platform.h"
#include "run_job.h"
#include "saves.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#ifndef _WIN32
#include <limits.h>
#include <sys/stat.h>
#endif


enum {
	EXIT_OK = 0,
	EXIT_FAILED = 1,
	EXIT_USAGE = 2,      // also when a batch's frozen files do not match their hashes
	SIBLING_GROWTH = 14, // "results.csv" -> "results-match-players.csv", the longest sibling
	MANY_CHUNKS = 20000, // more than a season's batch should have: each commit reads them all
};

static const char usage[] = "usage: fm24ctl batch create <plan.txt> <dir>\n"
														"       fm24ctl batch run <dir> [--threads N] [--until HH:MM] "
														"[--max-chunks N] [--allow-harness-change]\n"
														"       fm24ctl batch status <dir>\n"
														"       fm24ctl batch verify <dir> [--deep]\n"
														"       fm24ctl batch stop <dir> [--now]\n";

#ifdef _WIN32
#define SEPARATOR '\\'
#else
#define SEPARATOR '/'
#endif

// An arm's fixture, or a season's base, as create reads it.
typedef struct Fixture {
	ByteBuffer json;
	ByteBuffer bin;
	ConfigHeader header;
	SaveCheck save;
} Fixture;

// A batch being created, all in memory until it is frozen.
typedef struct Creation {
	BatchPlan plan;
	char planPath[BATCH_PATH_LENGTH];
	char planFolder[BATCH_PATH_LENGTH];
	wchar_t directory[HARNESS_PATH_LENGTH];
	char directoryUtf8[BATCH_PATH_LENGTH];
	Fixture fixtures[BATCH_MAX_ARMS]; // a season's base is the first

	ByteBuffer listText;
	uint32_t *list;
} Creation;

static int createBatch(const wchar_t *planArgument, const wchar_t *folder, const BatchHost *host);
static int readPlan(Creation *creation, const wchar_t *planArgument, const wchar_t *folder);
static bool resolvePath(const Creation *creation, char *path, size_t size);
static bool isAbsolute(const char *path);
static bool readFixtures(Creation *creation);
static bool readFixture(Creation *creation, BatchArm *entry, Fixture *fixture);
static bool checkLineup(const Creation *creation, const BatchArm *entry, const Fixture *fixture);
static bool checkFixtures(Creation *creation);
static bool checkSave(Creation *creation);
static bool readSaveName(Creation *creation, char *name, size_t size);
static bool hasSaveExtension(const char *path);
static bool isFile(const char *path);
static bool readList(Creation *creation);
static bool checkSizes(Creation *creation);
static bool checkFolder(const Creation *creation);
static int freeze(Creation *creation, const BatchHost *host);
static bool writeInputs(const Creation *creation, const BatchHost *host);
static bool writeInput(
	const Creation *creation,
	const BatchHost *host,
	const char *name,
	const ByteBuffer *bytes
);
static bool startBatch(Creation *creation, const BatchHost *host);
static void undoFreeze(const Creation *creation);
static void warnIfLowOnSpace(const Creation *creation, const BatchHost *host);
static bool makeFolders(const wchar_t *path);
static bool countEntry(void *context, const wchar_t *name, bool isDirectory);
static bool batchPath(const wchar_t *directory, const wchar_t *name, wchar_t *out);
static int openBatch(Batch *batch, const wchar_t *folder, const BatchHost *host);
static int showStatus(const wchar_t *folder, const BatchHost *host, FILE *out);
static void printArms(const Batch *batch, FILE *out);
static void printSeasonHead(const Batch *batch, FILE *out);
static void printSeasons(const Batch *batch, FILE *out);
static void printNext(Batch *batch, FILE *out);
static void printIncidents(const BatchProgress *progress, FILE *out);
static void printState(const Batch *batch, FILE *out);
static int verifyBatch(const wchar_t *folder, bool deep, const BatchHost *host, FILE *out);
static int stopBatch(const wchar_t *folder, bool now);
static int runBatch(int argc, wchar_t **argv, const BatchHost *host);
static bool readRunOptions(int argc, wchar_t **argv, const BatchHost *host, BatchRunOptions *out);
static bool readCount(const wchar_t *text, uint64_t limit, uint64_t *value);
static void freeCreation(Creation *creation);
#ifndef _WIN32
static bool isProtectedFolder(const char *path);
static void resolveExisting(const char *path, char *out, size_t size);

// Where macOS mounts volumes (tests point it at a folder of their own).
static const char *volumesFolder = "/Volumes";
#endif

int commands_batch(int argc, wchar_t **argv) {
	const wchar_t *command = argc > 2 ? argv[2] : L"";
	bool flagged = argc == 5;
	BatchHost host;
	batchHost_real(&host);
	if (wcscmp(command, L"create") == 0 && argc == 5) {
		return createBatch(argv[3], argv[4], &host);
	}
	if (wcscmp(command, L"run") == 0 && argc >= 4) {
		return runBatch(argc, argv, &host);
	}
	if (wcscmp(command, L"status") == 0 && argc == 4) {
		return showStatus(argv[3], &host, stdout);
	}
	if (wcscmp(command, L"verify") == 0 &&
			(argc == 4 || (flagged && wcscmp(argv[4], L"--deep") == 0))) {
		return verifyBatch(argv[3], flagged, &host, stdout);
	}
	if (wcscmp(command, L"stop") == 0 && (argc == 4 || (flagged && wcscmp(argv[4], L"--now") == 0))) {
		return stopBatch(argv[3], flagged);
	}
	fputs(usage, stderr);
	return EXIT_USAGE;
}

// Validates the plan and everything it names, all before writing anything, then freezes it.
static int createBatch(const wchar_t *planArgument, const wchar_t *folder, const BatchHost *host) {
	Creation *creation = calloc(1, sizeof *creation);
	if (!creation) {
		fputs("out of memory\n", stderr);
		return EXIT_FAILED;
	}
	int status = readPlan(creation, planArgument, folder);
	bool valid = status == EXIT_OK && readFixtures(creation) && checkFixtures(creation) &&
							 checkSave(creation) && readList(creation) && checkSizes(creation) &&
							 checkFolder(creation);
	if (status == EXIT_OK) {
		status = valid ? freeze(creation, host) : EXIT_FAILED;
	}
	freeCreation(creation);
	return status;
}

static int readPlan(Creation *creation, const wchar_t *planArgument, const wchar_t *folder) {
	wchar_t planPath[HARNESS_PATH_LENGTH];
	if (!paths_absolute(planArgument, planPath, HARNESS_PATH_LENGTH) ||
			!platform_toUtf8(planPath, creation->planPath, sizeof creation->planPath) ||
			!paths_absolute(folder, creation->directory, HARNESS_PATH_LENGTH) ||
			!platform_toUtf8(
				creation->directory, creation->directoryUtf8, sizeof creation->directoryUtf8
			)) {
		fputs("a path is too long or not valid text\n", stderr);
		return EXIT_FAILED;
	}
	ByteBuffer text = {0};
	char error[512];
	BatchDefaults defaults = {
		.threads = (uint32_t)commands_defaultThreads(),
#ifdef _WIN32
		.compress = BATCH_COMPRESS_NONE,
#else
		.compress = BATCH_COMPRESS_GZIP,
#endif
	};
	bool ok = files_read(planPath, &text);
	if (!ok) {
		fprintf(stderr, "cannot read %s\n", creation->planPath);
	} else if (!(ok = batchPlan_parse(
								 buffer_string(&text),
								 text.length,
								 false,
								 &defaults,
								 &creation->plan,
								 error,
								 sizeof error
							 ))) {
		fprintf(stderr, "%s: %s\n", creation->planPath, error);
	}
	buffer_free(&text);
	(void)snprintf(creation->planFolder, sizeof creation->planFolder, "%s", creation->planPath);
	char *last = strrchr(creation->planFolder, SEPARATOR);
	if (last) {
		*last = 0;
	}
#ifdef _WIN32
	if (ok && creation->plan.compress == BATCH_COMPRESS_GZIP) {
		fputs("Windows has no gzip: use compress=none\n", stderr);
		ok = false;
	}
#endif
	return ok ? EXIT_OK : EXIT_FAILED;
}

// A plan's path made absolute: as it is if it is, else from the plan's folder.
static bool resolvePath(const Creation *creation, char *path, size_t size) {
	char joined[BATCH_PATH_LENGTH];
	wchar_t wide[HARNESS_PATH_LENGTH];
	wchar_t absolute[HARNESS_PATH_LENGTH];
	const char *source = path;
	if (!isAbsolute(path)) {
		int written = snprintf(joined, sizeof joined, "%s%c%s", creation->planFolder, SEPARATOR, path);
		if (written <= 0 || (size_t)written >= sizeof joined) {
			return false;
		}
		source = joined;
	}
	return platform_fromUtf8(source, wide, HARNESS_PATH_LENGTH) &&
				 paths_absolute(wide, absolute, HARNESS_PATH_LENGTH) &&
				 platform_toUtf8(absolute, path, size);
}

static bool isAbsolute(const char *path) {
#ifdef _WIN32
	return path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':');
#else
	return path[0] == '/';
#endif
}

static bool readFixtures(Creation *creation) {
	BatchPlan *plan = &creation->plan;
	if (plan->mode == BATCH_MODE_SEASON) {
		return readFixture(creation, &plan->base, &creation->fixtures[0]);
	}
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		if (!readFixture(creation, &plan->arms[arm], &creation->fixtures[arm])) {
			return false;
		}
	}
	return true;
}

// An arm's fixture, or a season's base: a JSON config that encodes, with its hashes and team
// overrides.
static bool readFixture(Creation *creation, BatchArm *entry, Fixture *fixture) {
	wchar_t path[HARNESS_PATH_LENGTH];
	char error[256] = "";
	if (!resolvePath(creation, entry->path, sizeof entry->path) ||
			!platform_fromUtf8(entry->path, path, HARNESS_PATH_LENGTH) ||
			!files_read(path, &fixture->json)) {
		fprintf(stderr, "arm %s: cannot read %s\n", entry->id, entry->path);
		return false;
	}
	const char *json = buffer_string(&fixture->json);
	if (!matchConfig_readHeader(json, fixture->json.length, &fixture->header) ||
			strcmp(fixture->header.format, "fm24-match-config/1") != 0) {
		fprintf(stderr, "arm %s: %s is not a fixture (fm24-match-config/1)\n", entry->id, entry->path);
		return false;
	}
	if (!matchConfig_encode(
				json, fixture->json.length, &fixture->bin, entry->teams, error, sizeof error
			)) {
		fprintf(stderr, "arm %s: %s: %s\n", entry->id, entry->path, error);
		return false;
	}
	runJob_readFixtureSave(&fixture->json, &fixture->save);
	if (fixture->save.badSave) {
		fprintf(stderr, "arm %s: the fixture's \"save\" is not a save's name\n", entry->id);
		return false;
	}
	sha256_hexOf(json, fixture->json.length, entry->jsonSha256);
	sha256_hexOf(fixture->bin.data, fixture->bin.length, entry->binSha256);
	return checkLineup(creation, entry, fixture);
}

// A side whose players `fm24ctl lineup` reordered (its "<side>_lineup" marker) plays its new order
// only on the build as it is (keep_build=1), and, if its manager is the user, without
// human_control=1, which may play the manager's own selection instead. A season's base may not
// have one: each fixture is built from it by replacement, which drops line-ups.
static bool checkLineup(const Creation *creation, const BatchArm *entry, const Fixture *fixture) {
	for (unsigned side = 0; side < 2; ++side) {
		const char *name = side ? "away" : "home";
		if (!fixture->header.origin.lineup[side]) {
			continue;
		}
		if (creation->plan.mode == BATCH_MODE_SEASON) {
			fprintf(
				stderr,
				"base: %s has an edited %s line-up (%s_lineup), which a season's fixtures would not "
				"keep: export the base again, unedited\n",
				entry->path,
				name,
				name
			);
			return false;
		}
		if (!creation->plan.keepBuild) {
			fprintf(
				stderr,
				"arm %s: %s has an edited %s line-up (%s_lineup), which rebuilding each seed puts back: "
				"set keep_build=1 to play it\n",
				entry->id,
				entry->path,
				name,
				name
			);
			return false;
		}
		if (!creation->plan.humanControl) {
			continue;
		}
		FixtureData *data = malloc(sizeof *data);
		char error[256] = "out of memory";
		bool read =
			data && fixtureData_read(
								buffer_string(&fixture->json), fixture->json.length, data, error, sizeof error
							);
		bool managed = read && data->teams[side].lineupKnown;
		free(data);
		if (!read) {
			fprintf(stderr, "arm %s: %s: %s\n", entry->id, entry->path, error);
			return false;
		}
		if (managed) {
			fprintf(
				stderr,
				"arm %s: %s has an edited %s line-up (%s_lineup), which human_control=1 may replace with "
				"the manager's own selection: set human_control=0\n",
				entry->id,
				entry->path,
				name,
				name
			);
			return false;
		}
	}
	return true;
}

// Every arm must come from the same save and game build, and the same in-game date unless each
// seed is played on the build as it is (keep_build=1).
static bool checkFixtures(Creation *creation) {
	BatchPlan *plan = &creation->plan;
	const Fixture *first = &creation->fixtures[0];
	bool sameDate = true;
	size_t fixtures = plan->mode == BATCH_MODE_SEASON ? 1 : plan->armCount;
	for (size_t arm = 1; arm < fixtures; ++arm) {
		const Fixture *fixture = &creation->fixtures[arm];
		const char *names[] = {plan->arms[0].id, plan->arms[arm].id};
		if (strcmp(fixture->save.save, first->save.save) != 0) {
			fprintf(
				stderr,
				"arms %s and %s are from different saves (\"%s\" and \"%s\")\n",
				names[0],
				names[1],
				first->save.save,
				fixture->save.save
			);
			return false;
		}
		if (fixture->header.gameBuild != first->header.gameBuild) {
			fprintf(
				stderr,
				"arms %s and %s are from different game builds (%u and %u)\n",
				names[0],
				names[1],
				first->header.gameBuild,
				fixture->header.gameBuild
			);
			return false;
		}
		sameDate = sameDate && strcmp(fixture->save.date, first->save.date) == 0;
		if (!sameDate && !plan->keepBuild) {
			fprintf(
				stderr,
				"arms %s and %s are from different in-game dates (\"%s\" and \"%s\"); only keep_build=1 "
				"allows that\n",
				names[0],
				names[1],
				first->save.date,
				fixture->save.date
			);
			return false;
		}
	}
	plan->gameBuild = first->header.gameBuild;
	(void)snprintf(plan->gameDate, sizeof plan->gameDate, "%s", sameDate ? first->save.date : "");
	return true;
}

// save=: a save's name, or a .fm file of that name to relaunch the game with; by default the arms'
// save. A batch without one cannot launch the game.
static bool checkSave(Creation *creation) {
	BatchPlan *plan = &creation->plan;
	const char *armSave = creation->fixtures[0].save.save;
	char name[BATCH_TEXT_LENGTH] = "";
	if (plan->save[0] && !readSaveName(creation, name, sizeof name)) {
		fprintf(stderr, "save= must be a save's name or an existing .fm file: %s\n", plan->save);
		return false;
	}
	if (name[0] && armSave[0] && strcmp(name, armSave) != 0) {
		fprintf(stderr, "save= names %s, but the arms' fixtures are from %s\n", name, armSave);
		return false;
	}
	(void)snprintf(plan->saveName, sizeof plan->saveName, "%s", name[0] ? name : armSave);
	if (!plan->saveName[0] && plan->launch) {
		fputs(
			"a batch without a save cannot launch the game: give the fixtures a \"save\", set save=, or "
			"set launch=0\n",
			stderr
		);
		return false;
	}
	return true;
}

// save='s name: as given, or of the .fm file it names, which must exist.
static bool readSaveName(Creation *creation, char *name, size_t size) {
	BatchPlan *plan = &creation->plan;
	if (!strchr(plan->save, '/') && !strchr(plan->save, '\\') && !hasSaveExtension(plan->save)) {
		(void)snprintf(name, size, "%s", plan->save);
		return runJob_isSaveName(name);
	}
	return resolvePath(creation, plan->save, sizeof plan->save) && hasSaveExtension(plan->save) &&
				 isFile(plan->save) && saves_nameOfPath(plan->save, name, size) && runJob_isSaveName(name);
}

static bool hasSaveExtension(const char *path) {
	size_t length = strlen(path);
	return length > 3 && platform_compareIgnoringCase(path + length - 3, ".fm") == 0;
}

static bool isFile(const char *path) {
#ifdef _WIN32
	wchar_t wide[HARNESS_PATH_LENGTH];
	DWORD attributes = platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) ? GetFileAttributesW(wide)
																																				: INVALID_FILE_ATTRIBUTES;
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat status;
	return stat(path, &status) == 0 && S_ISREG(status.st_mode);
#endif
}

// list:<file>: one distinct seed per line.
static bool readList(Creation *creation) {
	BatchPlan *plan = &creation->plan;
	if (plan->seeds.kind != BATCH_SEEDS_LIST) {
		return true;
	}
	wchar_t path[HARNESS_PATH_LENGTH];
	char error[256];
	size_t count = 0;
	if (!resolvePath(creation, plan->listPath, sizeof plan->listPath) ||
			!platform_fromUtf8(plan->listPath, path, HARNESS_PATH_LENGTH) ||
			!files_read(path, &creation->listText)) {
		fprintf(stderr, "cannot read the seed list %s\n", plan->listPath);
		return false;
	}
	if (!batchSeeds_readList(
				buffer_string(&creation->listText),
				creation->listText.length,
				&creation->list,
				&count,
				error,
				sizeof error
			)) {
		fprintf(stderr, "%s: %s\n", plan->listPath, error);
		return false;
	}
	plan->seeds.list = creation->list;
	plan->seeds.first = 0;
	plan->seeds.last = count - 1;
	sha256_hexOf(creation->listText.data, creation->listText.length, plan->listSha256);
	return true;
}

// The warm-up and canary fit the seed set and a chunk, and a chunk's job fits the harness.
static bool checkSizes(Creation *creation) {
	const BatchPlan *plan = &creation->plan;
	uint64_t count = batchSeeds_count(&plan->seeds);
	if (plan->warmup > count || plan->warmup > plan->chunk || plan->canary > count ||
			plan->canary > plan->chunk) {
		fprintf(stderr, "warmup= and canary= can be at most chunk= and the number of seeds\n");
		return false;
	}
	BatchPartPaths paths;
	wchar_t wide[HARNESS_PATH_LENGTH];
	size_t longestArm = 0;
	for (size_t arm = 1; arm < plan->armCount; ++arm) {
		longestArm = strlen(plan->arms[arm].id) > strlen(plan->arms[longestArm].id) ? arm : longestArm;
	}
	if (!batchPlan_partPaths(
				creation->directoryUtf8, plan->arms[longestArm].id, UINT32_MAX, UINT32_MAX, &paths
			) ||
			!platform_fromUtf8(paths.output, wide, HARNESS_PATH_LENGTH) ||
			wcslen(wide) + SIBLING_GROWTH >= HARNESS_PATH_LENGTH) {
		fprintf(stderr, "the batch folder's path is too long for the harness\n");
		return false;
	}
	uint32_t largest = batchPlan_largestChunk(plan, creation->directoryUtf8);
	if (plan->chunk > largest) {
		fprintf(
			stderr,
			"chunk=%u makes a job longer than the harness accepts here; the largest chunk that fits is "
			"%u\n",
			plan->chunk,
			largest
		);
		return false;
	}
	return true;
}

// The game writes the batch's files, and macOS asks before it writes to some folders, which would
// stop an unattended batch.
static bool checkFolder(const Creation *creation) {
#ifdef _WIN32
	(void)creation;
#else
	if (isProtectedFolder(creation->directoryUtf8)) {
		fprintf(
			stderr,
			"macOS asks before the game writes to Desktop, Documents, Downloads or a volume under "
			"/Volumes, which would stop an unattended batch: use another folder for %s\n",
			creation->directoryUtf8
		);
		return false;
	}
#endif
	return true;
}

// Writes the batch's folder: the inputs and plan.txt, then index.csv, events.log and batch.json.
static int freeze(Creation *creation, const BatchHost *host) {
	unsigned entries = 0;
	if (!makeFolders(creation->directory) ||
			!platform_listDirectory(creation->directory, countEntry, &entries)) {
		return EXIT_FAILED;
	}
	if (entries) {
		fprintf(
			stderr, "%s is not empty: a batch needs a new or empty folder\n", creation->directoryUtf8
		);
		return EXIT_FAILED;
	}
	warnIfLowOnSpace(creation, host);
	if (!writeInputs(creation, host) || !startBatch(creation, host)) {
		undoFreeze(creation);
		return EXIT_FAILED;
	}
	const BatchPlan *plan = &creation->plan;
	char indices[32];
	char chunks[32];
	char size[32];
	batch_formatCount(batchSeeds_count(&plan->seeds), indices, sizeof indices);
	uint64_t chunkCount = batchPlan_chunksPerArm(plan) * plan->armCount;
	batch_formatCount(chunkCount, chunks, sizeof chunks);
	batch_formatCount(plan->chunk, size, sizeof size);
	if (plan->mode == BATCH_MODE_SEASON) {
		char matches[32];
		batch_formatCount(batchSeeds_count(&plan->seeds) * plan->armCount, matches, sizeof matches);
		printf(
			"created season batch %s in %s: %zu teams, %zu fixtures, seasons %llu-%llu (%s matches), "
			"in %s chunks of up to %s seasons\n",
			plan->name,
			creation->directoryUtf8,
			plan->teamCount,
			plan->armCount,
			(unsigned long long)plan->seeds.first,
			(unsigned long long)plan->seeds.last,
			matches,
			chunks,
			size
		);
		if (chunkCount > MANY_CHUNKS) {
			fprintf(
				stderr,
				"warning: %s chunks; status, verify and each commit read them all, so a larger chunk= "
				"would be quicker\n",
				chunks
			);
		}
	} else {
		printf(
			"created batch %s in %s: %zu arm%s of %s indices, in %s chunk%s of up to %s\n",
			plan->name,
			creation->directoryUtf8,
			plan->armCount,
			plan->armCount == 1 ? "" : "s",
			indices,
			chunks,
			chunkCount == 1 ? "" : "s",
			size
		);
	}
	printf("plan_sha256 %s\n", plan->planSha256);
	return EXIT_OK;
}

static bool writeInputs(const Creation *creation, const BatchHost *host) {
	static const wchar_t *const folders[] = {L"inputs", L"attempts", L"chunks", L"control"};
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
		wchar_t path[HARNESS_PATH_LENGTH];
		if (!batchPath(creation->directory, folders[i], path) || !platform_makeDirectory(path)) {
			return false;
		}
	}
	const BatchPlan *plan = &creation->plan;
	if (plan->mode == BATCH_MODE_SEASON) {
		// The fixtures are built from the base by batch run.
		return writeInput(creation, host, "base.json", &creation->fixtures[0].json) &&
					 writeInput(creation, host, "base.bin", &creation->fixtures[0].bin);
	}
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		char name[BATCH_ARM_LENGTH + 8];
		(void)snprintf(name, sizeof name, "%s.json", plan->arms[arm].id);
		bool ok = writeInput(creation, host, name, &creation->fixtures[arm].json);
		(void)snprintf(name, sizeof name, "%s.bin", plan->arms[arm].id);
		if (!ok || !writeInput(creation, host, name, &creation->fixtures[arm].bin)) {
			return false;
		}
	}
	return plan->seeds.kind != BATCH_SEEDS_LIST ||
				 writeInput(creation, host, "seeds.txt", &creation->listText);
}

static bool writeInput(
	const Creation *creation,
	const BatchHost *host,
	const char *name,
	const ByteBuffer *bytes
) {
	wchar_t path[HARNESS_PATH_LENGTH];
	wchar_t wide[BATCH_ARM_LENGTH + 16];
	size_t length = strlen(name);
	for (size_t i = 0; i <= length; ++i) {
		wide[i] = (wchar_t)(unsigned char)name[i];
	}
	wchar_t inputs[HARNESS_PATH_LENGTH];
	bool ok = batchPath(creation->directory, L"inputs", inputs) && batchPath(inputs, wide, path) &&
						batch_writeFile(host, path, bytes->data, bytes->length);
	if (!ok) {
		fprintf(stderr, "cannot write inputs/%s\n", name);
	}
	return ok;
}

// plan.txt, which makes the folder a batch, then the files made from the batch's state.
static bool startBatch(Creation *creation, const BatchHost *host) {
	BatchPlan *plan = &creation->plan;
	ByteBuffer text = {0};
	wchar_t path[HARNESS_PATH_LENGTH];
	bool ok = batchPlan_format(plan, &text) && batchPath(creation->directory, L"plan.txt", path) &&
						batch_writeFile(host, path, text.data, text.length);
	buffer_free(&text);
	Batch *batch = ok ? malloc(sizeof *batch) : NULL;
	ok = batch && batch_open(batch, creation->directory, host) == BATCH_OPENED;
	if (ok) {
		char indices[32];
		batch_formatCount(batchSeeds_count(&plan->seeds) * plan->armCount, indices, sizeof indices);
		time_t now = host->utcNow(host->context);
		BatchOutcome outcome = {"created", NULL, -1, now};
		ok = batch_writeIndex(batch);
		batch_logEvent(
			batch,
			"created name=%s arms=%zu indices=%llu chunks=%llu plan_sha256=%s",
			plan->name,
			plan->armCount,
			(unsigned long long)(batchSeeds_count(&plan->seeds) * plan->armCount),
			(unsigned long long)(batchPlan_chunksPerArm(plan) * plan->armCount),
			plan->planSha256
		);
		ok = batch_writeManifest(batch, &outcome) == BATCH_STATUS_OK && ok;
		batch_close(batch);
	}
	free(batch);
	if (!ok) {
		fprintf(stderr, "cannot write the batch's files in %s\n", creation->directoryUtf8);
	}
	return ok;
}

// After a failure, leaves the folder empty again.
static void undoFreeze(const Creation *creation) {
	static const wchar_t *const folders[] = {L"inputs", L"attempts", L"chunks", L"control"};
	static const wchar_t *const files[] = {
		L"plan.txt",
		L"plan.txt.tmp",
		L"index.csv",
		L"index.csv.tmp",
		L"events.log",
		L"batch.json",
		L"batch.json.tmp",
	};
	wchar_t path[HARNESS_PATH_LENGTH];
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
		if (batchPath(creation->directory, folders[i], path)) {
			(void)batch_removeFolder(path);
		}
	}
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		if (batchPath(creation->directory, files[i], path)) {
			(void)platform_removeFile(path);
		}
	}
}

// Warns if the folder's volume lacks room for the batch's files, kept compressed if they are, and
// the two raw chunks in flight, as well as min_free_gb.
static void warnIfLowOnSpace(const Creation *creation, const BatchHost *host) {
	const BatchPlan *plan = &creation->plan;
	static const uint64_t storedPerMatch[2][2] = {{60, 6800}, {30, 1700}}; // [gzip][per_match]
	uint64_t matches = batchSeeds_count(&plan->seeds) * plan->armCount;
	uint64_t needed =
		matches * storedPerMatch[plan->compress == BATCH_COMPRESS_GZIP][plan->perMatch != 0] +
		2ULL * plan->chunk * 6800 + ((uint64_t)plan->minFreeGb << 30);
	if (plan->mode == BATCH_MODE_SEASON) {
		// Each fixture's inputs: its encoding and its JSON, which gzip makes about a fourteenth.
		const Fixture *base = &creation->fixtures[0];
		uint64_t json =
			plan->compress == BATCH_COMPRESS_GZIP ? base->json.length / 14 : base->json.length;
		needed += plan->armCount * (base->bin.length + json);
	}
	uint64_t available = 0;
	if (host->freeBytes(host->context, creation->directory, &available) && available < needed) {
		fprintf(
			stderr,
			"warning: %.1f GB free, but the batch needs about %.1f GB with min_free_gb=%u\n",
			(double)available / (double)(1ULL << 30),
			(double)needed / (double)(1ULL << 30),
			plan->minFreeGb
		);
	}
}

// Makes a folder and those above it that are missing.
static bool makeFolders(const wchar_t *path) {
	wchar_t partial[HARNESS_PATH_LENGTH];
	size_t length = wcslen(path);
	size_t start = 1;
#ifdef _WIN32
	start = path[0] == L'\\' && path[1] == L'\\' ? 2 : (length > 2 && path[1] == L':' ? 3 : 1);
	for (int skipped = 0; start == 2 && skipped < 2 && start < length; ++start) {
		skipped += path[start] == L'\\'; // a UNC path's server and share
	}
#endif
	for (size_t i = start; i < length; ++i) {
		if (path[i] != PLATFORM_PATH_SEPARATOR) {
			continue;
		}
		wmemcpy(partial, path, i);
		partial[i] = 0;
		if (!platform_makeDirectory(partial)) {
			return false;
		}
	}
	return platform_makeDirectory(path);
}

// Counts a folder's entries, but not the .DS_Store the Finder leaves.
static bool countEntry(void *context, const wchar_t *name, bool isDirectory) {
	(void)isDirectory;
	*(unsigned *)context += wcscmp(name, L".DS_Store") != 0;
	return true;
}

static bool batchPath(const wchar_t *directory, const wchar_t *name, wchar_t *out) {
	return paths_join(out, HARNESS_PATH_LENGTH, directory, name);
}

// Opens the batch in `folder` for reading: 0, or the exit code.
static int openBatch(Batch *batch, const wchar_t *folder, const BatchHost *host) {
	wchar_t directory[HARNESS_PATH_LENGTH];
	if (!paths_absolute(folder, directory, HARNESS_PATH_LENGTH)) {
		fprintf(stderr, "bad path: %ls\n", folder);
		return EXIT_FAILED;
	}
	switch (batch_open(batch, directory, host)) {
		case BATCH_OPENED: return EXIT_OK;
		case BATCH_OPEN_CHANGED: return EXIT_USAGE;
		default: return EXIT_FAILED;
	}
}

static int showStatus(const wchar_t *folder, const BatchHost *host, FILE *out) {
	Batch *batch = malloc(sizeof *batch);
	int status = batch ? openBatch(batch, folder, host) : EXIT_FAILED;
	if (status != EXIT_OK) {
		free(batch);
		return status;
	}
	const BatchPlan *plan = &batch->plan;
	time_t now = host->utcNow(host->context);
	BatchProgress progress;
	batch_progress(batch, now, &progress);
	char text[3][32];
	bool season = plan->mode == BATCH_MODE_SEASON;
	batch_formatCount(plan->chunk, text[0], sizeof text[0]);
	if (season) {
		printSeasonHead(batch, out);
	} else {
		fprintf(
			out,
			"batch %s: %zu arm%s, chunks of %s, %s order\n",
			plan->name,
			plan->armCount,
			plan->armCount == 1 ? "" : "s",
			text[0],
			plan->order == BATCH_ORDER_SEQUENTIAL ? "sequential" : "interleaved"
		);
	}
	batch_formatCount(progress.done, text[0], sizeof text[0]);
	batch_formatCount(progress.planned, text[1], sizeof text[1]);
	batch_formatCount(progress.chunks, text[2], sizeof text[2]);
	fprintf(
		out,
		"progress: %s of %s %s (%.1f%%) in %llu of %s chunk%s",
		text[0],
		text[1],
		season ? "matches" : "indices",
		progress.planned ? 100.0 * (double)progress.done / (double)progress.planned : 0.0,
		(unsigned long long)progress.chunksDone,
		text[2],
		progress.chunks == 1 ? "" : "s"
	);
	if (progress.inParts) {
		batch_formatCount(progress.inParts, text[0], sizeof text[0]);
		fprintf(out, ", and %s in parts of chunks not yet committed", text[0]);
	}
	fputc('\n', out);
	const BatchCommitted *last = NULL;
	for (size_t i = 0; i < batch->committedCount; ++i) {
		const BatchCommitted *chunk = &batch->committed[i];
		last = !last || chunk->committedAt >= last->committedAt ? chunk : last;
	}
	if (last) {
		char line[512];
		batch_progressLine(batch, last, now, line, sizeof line);
		fprintf(out, "last: %s\n", line);
	}
	printNext(batch, out);
	if (season) {
		printSeasons(batch, out);
	} else {
		printArms(batch, out);
	}
	printIncidents(&progress, out);
	if (batch->leftoverCount) {
		fprintf(
			out,
			"to tidy: %zu folder%s a stopped process left, which the next batch run removes\n",
			batch->leftoverCount,
			batch->leftoverCount == 1 ? "" : "s"
		);
	}
	if (batch->problem[0]) {
		fprintf(out, "problem: %s\n", batch->problem);
	}
	printState(batch, out);
	batch_close(batch);
	free(batch);
	return EXIT_OK;
}

static void printArms(const Batch *batch, FILE *out) {
	const BatchPlan *plan = &batch->plan;
	for (size_t arm = 0; arm < plan->armCount; ++arm) {
		BatchArmTotals total;
		char results[32];
		batch_armTotals(batch, arm, &total);
		batch_formatCount(total.rows[BATCH_RESULTS], results, sizeof results);
		fprintf(
			out,
			"arm %s: %llu of %llu chunk%s; %s results, %llu skipped, %llu excluded; "
			"home %llu, draw %llu, away %llu\n",
			plan->arms[arm].id,
			(unsigned long long)total.chunks,
			(unsigned long long)batchPlan_chunksPerArm(plan),
			batchPlan_chunksPerArm(plan) == 1 ? "" : "s",
			results,
			(unsigned long long)total.rows[BATCH_FAILED],
			(unsigned long long)total.excluded,
			(unsigned long long)total.summary.home,
			(unsigned long long)total.summary.draw,
			(unsigned long long)total.summary.away
		);
	}
}

// A season's teams, fixtures and seasons, and how many of its fixtures are built.
static void printSeasonHead(const Batch *batch, FILE *out) {
	const BatchPlan *plan = &batch->plan;
	char block[32];
	batch_formatCount(plan->chunk, block, sizeof block);
	fprintf(
		out,
		"batch %s: season run of %zu teams, %zu fixtures, seasons %llu-%llu in blocks of %s\n",
		plan->name,
		plan->teamCount,
		plan->armCount,
		(unsigned long long)plan->seeds.first,
		(unsigned long long)plan->seeds.last,
		block
	);
	fprintf(
		out,
		"fixtures: %zu of %zu built%s\n",
		batch->fixturesBuilt,
		plan->armCount,
		batch->fixturesFrozen ? ", frozen in inputs/fixtures.txt"
													: "; batch run builds the others before the first chunk"
	);
}

// The seasons complete (those of every block whose fixtures are all committed), the block under
// way, and the matches complete blocks lack: failed or excluded seeds, each costing its season.
static void printSeasons(const Batch *batch, FILE *out) {
	BatchSeasons seasons;
	batch_seasons(batch, &seasons);
	char done[32];
	char planned[32];
	char range[64] = "";
	batch_formatCount(seasons.seasonsComplete, done, sizeof done);
	batch_formatCount(seasons.seasons, planned, sizeof planned);
	if (seasons.completeCount) {
		(void)snprintf(
			range,
			sizeof range,
			" (%llu-%llu)",
			(unsigned long long)seasons.completeFirst,
			(unsigned long long)(seasons.completeFirst + seasons.completeCount - 1)
		);
	}
	fprintf(out, "seasons: %s of %s in complete blocks%s", done, planned, range);
	if (seasons.hasCurrent) {
		fprintf(
			out,
			"; block %llu: %zu of %zu fixtures",
			(unsigned long long)seasons.current,
			seasons.currentDone,
			batch->plan.armCount
		);
	}
	fprintf(
		out,
		"; %llu match%s missing\n",
		(unsigned long long)seasons.matchesMissing,
		seasons.matchesMissing == 1 ? "" : "es"
	);
}

// The next chunk to run, and how far its parts have got.
static void printNext(Batch *batch, FILE *out) {
	size_t arm = 0;
	uint64_t chunk = 0;
	if (!batch_nextChunk(batch, &arm, &chunk)) {
		fputs("next: nothing; every chunk is committed\n", out);
		return;
	}
	uint64_t first = 0;
	uint64_t last = 0;
	char id[BATCH_CHUNK_ID_LENGTH];
	batchPlan_chunkRange(&batch->plan, chunk, &first, &last);
	batchPlan_chunkId(first, id);
	fprintf(out, "next: chunk %s/%s", batch->plan.arms[arm].id, id);
	const BatchChunk *pending = batch_pending(batch, arm, chunk);
	BatchChain chain;
	if (!pending || !pending->partCount || !batchChain_compute(pending, &chain)) {
		fputs(", not started\n", out);
		return;
	}
	const BatchPart *latest = &pending->parts[pending->partCount - 1];
	fprintf(
		out,
		", %llu of %llu indices done in %zu attempt%s; the last, a%u, ",
		(unsigned long long)(chain.cursor - first),
		(unsigned long long)(last - first + 1),
		pending->partCount,
		pending->partCount == 1 ? "" : "s",
		latest->attempt
	);
	if (latest->evaluated) {
		fprintf(out, "ended %s\n", batch_outcomeName(latest->outcome));
	} else {
		fputs("has not been evaluated (it may still be running)\n", out);
	}
	batchChain_free(&chain);
}

static void printIncidents(const BatchProgress *progress, FILE *out) {
	fprintf(out, "attempts: %u", progress->attempts);
	for (size_t i = 0; i < BATCH_PART_OUTCOMES; ++i) {
		if (progress->outcomes[i]) {
			fprintf(out, ", %u %s", progress->outcomes[i], batch_outcomeName((BatchPartOutcome)i));
		}
	}
	fprintf(
		out,
		"; %llu poison seed%s; %.0f s asleep\n",
		(unsigned long long)progress->excluded,
		progress->excluded == 1 ? "" : "s",
		(double)progress->sleptMs / 1000.0
	);
}

// What batch.json says the batch did last.
static void printState(const Batch *batch, FILE *out) {
	char state[32];
	char reason[256];
	char at[32] = "";
	int exitCode = -1;
	time_t when = 0;
	if (!batch_readOutcome(batch, state, sizeof state, reason, sizeof reason, &exitCode, &when)) {
		fputs("state: unknown (batch.json cannot be read)\n", out);
		return;
	}
	struct tm utc;
	if (platform_utcTime(when, &utc)) {
		(void)snprintf(
			at,
			sizeof at,
			"%04d-%02d-%02dT%02d:%02d:%02dZ",
			utc.tm_year + 1900,
			utc.tm_mon + 1,
			utc.tm_mday,
			utc.tm_hour,
			utc.tm_min,
			utc.tm_sec
		);
	}
	fprintf(out, "state: %s at %s", state, at);
	if (exitCode >= 0) {
		fprintf(out, " (exit code %d)", exitCode);
	}
	fprintf(out, "%s%s\n", reason[0] ? ": " : "", reason);
}

static int verifyBatch(const wchar_t *folder, bool deep, const BatchHost *host, FILE *out) {
	Batch *batch = malloc(sizeof *batch);
	int status = batch ? openBatch(batch, folder, host) : EXIT_FAILED;
	if (status != EXIT_OK) {
		free(batch);
		return status;
	}
	size_t problems = 0;
	uint64_t indices = 0;
	if (batch->plan.mode == BATCH_MODE_SEASON && batch->fixturesFrozen) {
		bool fixtures = batch_verifyFixtures(batch, out);
		problems += !fixtures;
		fprintf(
			out,
			"%s %zu fixtures against inputs/fixtures.txt: %s\n",
			fixtures ? "verified" : "checked",
			batch->plan.armCount,
			fixtures ? "no problems" : "problems found"
		);
	} else if (batch->plan.mode == BATCH_MODE_SEASON) {
		fprintf(
			out,
			"fixtures: %zu of %zu built; not yet frozen, so not checked\n",
			batch->fixturesBuilt,
			batch->plan.armCount
		);
	}
	for (size_t i = 0; i < batch->committedCount; ++i) {
		const BatchCommitted *chunk = &batch->committed[i];
		problems += !batch_verifyChunk(batch, chunk, deep, out);
		indices += chunk->count;
	}
	char text[32];
	batch_formatCount(indices, text, sizeof text);
	fprintf(
		out,
		"%s %zu committed chunk%s (%s indices)%s: %s\n",
		problems ? "checked" : "verified",
		batch->committedCount,
		batch->committedCount == 1 ? "" : "s",
		text,
		deep ? ", decompressed and every row checked" : "",
		problems ? "problems found" : "no problems"
	);
	batch_close(batch);
	free(batch);
	return problems ? EXIT_FAILED : EXIT_OK;
}

// Asks a running batch to stop: after its chunk, or now (the jobs' stop file).
static int stopBatch(const wchar_t *folder, bool now) {
	wchar_t directory[HARNESS_PATH_LENGTH];
	wchar_t path[HARNESS_PATH_LENGTH];
	wchar_t control[HARNESS_PATH_LENGTH];
	if (!paths_absolute(folder, directory, HARNESS_PATH_LENGTH) ||
			!batchPath(directory, L"plan.txt", path)) {
		fprintf(stderr, "bad path: %ls\n", folder);
		return EXIT_FAILED;
	}
	ByteBuffer plan = {0};
	bool frozen = files_read(path, &plan) && batchPlan_isFrozen(buffer_string(&plan), plan.length);
	buffer_free(&plan);
	if (!frozen) {
		fprintf(stderr, "%ls is not a batch: it has no plan.txt that batch create froze\n", directory);
		return EXIT_FAILED;
	}
	// A line of this request's own, by which a batch run tells a new stop-now from one an earlier
	// run left for its job.
	char line[64];
	int length = snprintf(
		line,
		sizeof line,
		"pid=%lu ticks=%llu\n",
		platform_processId(),
		(unsigned long long)platform_ticks()
	);
	if (!batchPath(directory, L"control", control) || !platform_makeDirectory(control) ||
			!batchPath(control, now ? L"stop-now" : L"stop", path) || length <= 0 ||
			!files_write(path, line, (size_t)length)) {
		fputs("cannot ask the batch to stop\n", stderr);
		return EXIT_FAILED;
	}
	puts(
		now ? "asked the batch to stop now: no more matches start, and those under way finish and are "
					"kept; batch run stops waiting for a job that makes no progress for 30 seconds"
				: "asked the batch to stop once its current chunk is committed, or sooner if a job hangs "
					"or the game exits"
	);
	return EXIT_OK;
}

// batch run <dir> and its options. Windows has no batch run yet (see docs/CLI.md).
static int runBatch(int argc, wchar_t **argv, const BatchHost *host) {
	BatchRunOptions options;
	if (!readRunOptions(argc, argv, host, &options)) {
		fputs(usage, stderr);
		return EXIT_USAGE;
	}
#ifdef _WIN32
	(void)options;
	fputs("batch run is not available on Windows yet\n", stderr);
	return EXIT_FAILED;
#else
	wchar_t directory[HARNESS_PATH_LENGTH];
	if (!paths_absolute(argv[3], directory, HARNESS_PATH_LENGTH)) {
		fprintf(stderr, "bad path: %ls\n", argv[3]);
		return EXIT_FAILED;
	}
	return batchRun_run(directory, &options, host);
#endif
}

static bool readRunOptions(int argc, wchar_t **argv, const BatchHost *host, BatchRunOptions *out) {
	*out = (BatchRunOptions){0};
	for (int i = 4; i < argc; ++i) {
		const wchar_t *option = argv[i];
		const wchar_t *value = i + 1 < argc ? argv[i + 1] : NULL;
		uint64_t number = 0;
		if (wcscmp(option, L"--allow-harness-change") == 0) {
			out->allowHarnessChange = true;
		} else if (wcscmp(option, L"--threads") == 0 && value &&
							 readCount(value, HARNESS_MAX_THREADS, &number) && number) {
			out->threads = (uint32_t)number;
			++i;
		} else if (wcscmp(option, L"--max-chunks") == 0 && value &&
							 readCount(value, UINT32_MAX, &number) && number) {
			out->maxChunks = number;
			++i;
		} else if (wcscmp(option, L"--until") == 0 && value && wcslen(value) == 5 && value[2] == L':' &&
							 iswdigit(value[0]) && iswdigit(value[1]) && iswdigit(value[3]) &&
							 iswdigit(value[4])) {
			int hour = (value[0] - L'0') * 10 + (value[1] - L'0');
			int minute = (value[3] - L'0') * 10 + (value[4] - L'0');
			if (hour > 23 || minute > 59) {
				return false;
			}
			out->hasUntil = true;
			out->until = batchRun_nextLocalTime(host->utcNow(host->context), hour, minute);
			++i;
		} else {
			fprintf(stderr, "unknown option, or a bad or missing value: %ls\n", option);
			return false;
		}
	}
	return true;
}

// A decimal count from 0 to `limit`.
static bool readCount(const wchar_t *text, uint64_t limit, uint64_t *value) {
	*value = 0;
	if (!*text) {
		return false;
	}
	for (const wchar_t *c = text; *c; ++c) {
		if (*c < L'0' || *c > L'9' || *value > (limit - (uint64_t)(*c - L'0')) / 10) {
			return false;
		}
		*value = *value * 10 + (uint64_t)(*c - L'0');
	}
	return true;
}

static void freeCreation(Creation *creation) {
	for (size_t arm = 0; arm < BATCH_MAX_ARMS; ++arm) {
		buffer_free(&creation->fixtures[arm].json);
		buffer_free(&creation->fixtures[arm].bin);
	}
	buffer_free(&creation->listText);
	free(creation->list);
	batchPlan_free(&creation->plan);
	free(creation);
}

#ifndef _WIN32
// Whether `path` is in a folder macOS's privacy controls guard: Desktop, Documents, Downloads or a
// volume under /Volumes.
static bool isProtectedFolder(const char *path) {
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): fm24ctl has one thread
	char resolved[BATCH_PATH_LENGTH + PATH_MAX];
	resolveExisting(path, resolved, sizeof resolved);
	static const char *const inHome[] = {"Desktop", "Documents", "Downloads"};
	for (size_t i = 0; i <= sizeof inHome / sizeof inHome[0]; ++i) {
		char folder[PATH_MAX];
		char real[BATCH_PATH_LENGTH + PATH_MAX];
		bool named =
			i < sizeof inHome / sizeof inHome[0]
				? home && home[0] && snprintf(folder, sizeof folder, "%s/%s", home, inHome[i]) > 0
				: snprintf(folder, sizeof folder, "%s", volumesFolder) > 0;
		if (!named) {
			continue;
		}
		resolveExisting(folder, real, sizeof real);
		size_t length = strlen(real);
		if (platform_compareIgnoringCaseN(resolved, real, length) == 0 &&
				(resolved[length] == 0 || resolved[length] == '/')) {
			return true;
		}
	}
	return false;
}

// `path` with its longest existing part resolved (links, /tmp as /private/tmp), then the rest.
static void resolveExisting(const char *path, char *out, size_t size) {
	char prefix[BATCH_PATH_LENGTH];
	size_t end = strlen(path);
	if (end >= sizeof prefix) {
		(void)snprintf(out, size, "%s", path);
		return;
	}
	while (end > 0) {
		char real[PATH_MAX];
		memcpy(prefix, path, end);
		prefix[end] = 0;
		if (realpath(prefix, real)) {
			const char *rest = path + end;
			while (*rest == '/') {
				++rest;
			}
			(void)snprintf(out, size, "%s%s%s", real, *rest && strcmp(real, "/") != 0 ? "/" : "", rest);
			return;
		}
		// Back to the end of the part before the last.
		while (end > 0 && path[end - 1] != '/') {
			--end;
		}
		while (end > 1 && path[end - 1] == '/') {
			--end;
		}
	}
	(void)snprintf(out, size, "%s", path);
}
#endif
