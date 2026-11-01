// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "build_info.h"
#include "client.h"
#include "embedded.h"
#include "files.h"
#include "fixture_data.h"
#include "fixture_lineup.h"
#include "fixture_samples.h"
#include "host_info.h"
#include "json_writer.h"
#include "manifest.h"
#include "paths.h"
#include "platform.h"
#include "saves.h"
#include "session.h"
#include "sha256.h"
#include "test.h"
#include "tests.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include "macos_game.h"

#include <membership.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif


static bool readConfig(const wchar_t *path, ByteBuffer *out);
static bool writeConfig(const wchar_t *path, const void *bytes, size_t length);
static bool submitJob(const char *job, unsigned timeout, char *result, size_t resultSize);
static ClientOutcome submitWatchedJob(
	const char *job,
	unsigned timeout,
	ClientWatch *watch,
	char *result,
	size_t resultSize
);
#ifndef _WIN32
static int ensureTestSave(const SessionNeed *need, char *date, size_t size);
static pid_t findTestGame(char *path, size_t size);
static bool testGameUnderRosetta(pid_t pid);
static uint64_t testGameBytes(pid_t pid);
#endif
static PathsFolder testParentFolder(const wchar_t *path);
static bool removeTestFile(const wchar_t *path);

#define commands_defaultThreads runnerTest_defaultThreads
#define commands_run runnerTest_run
#define commands_runWithProgress runnerTest_runWithProgress
#define commands_getNextFixture runnerTest_getNextFixture
#define commands_replaceTeam runnerTest_replaceTeam
#define commands_replaceTeamWithResult runnerTest_replaceTeamWithResult
#define commands_listTeams runnerTest_listTeams
#define commands_listTeamsWithResult runnerTest_listTeamsWithResult
#define commands_listPlayers runnerTest_listPlayers
#define commands_listPlayersWithResult runnerTest_listPlayersWithResult
#define commands_encode runnerTest_encode
#define commands_decode runnerTest_decode
#define commands_getPlayer runnerTest_getPlayer
#define commands_injectPlayer runnerTest_injectPlayer
#define commands_injectPlayerWithResult runnerTest_injectPlayerWithResult
#define commands_lineup runnerTest_lineup
#define commands_status runnerTest_status
#define commands_version runnerTest_version
#define commands_probe runnerTest_probe
#define files_read readConfig
#define files_write writeConfig
#define client_submit submitJob
#define client_submitWatched submitWatchedJob
#define macosGame_ensureSave ensureTestSave
#define macosGame_findRunning findTestGame
#define macosGame_underRosetta testGameUnderRosetta
#define macosGame_residentBytes testGameBytes
#define paths_parentFolder testParentFolder
#define platform_removeFile removeTestFile
#include "../src/runner/commands.c" // NOLINT(bugprone-suspicious-include): exercise private parsers.
#include "../src/runner/run_job.c" // NOLINT(bugprone-suspicious-include): with the save check above.
#undef files_read
#undef files_write
#undef client_submit
#undef client_submitWatched
#undef macosGame_ensureSave
#undef macosGame_findRunning
#undef macosGame_underRosetta
#undef macosGame_residentBytes
#undef paths_parentFolder
#undef platform_removeFile

#ifdef _WIN32
#include "../src/runner/client.c" // NOLINT(bugprone-suspicious-include): check result replies.
#include "../src/runner/probe.c"  // NOLINT(bugprone-suspicious-include): test in-memory PE images.
#else
#include "../src/runner/client_posix.c" // NOLINT(bugprone-suspicious-include): check result replies.
#include "../src/runner/probe_macos.c"  // NOLINT(bugprone-suspicious-include): fake Mach-O files.
#endif


static const char plainConfig[] =
	"{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u8\":42}],\"home_team\":678,\"away_team\":677}";
static const char *configText = plainConfig; // what every file read returns
static unsigned configReads;
static ByteBuffer writtenConfig;
static char submittedJob[HARNESS_JOB_LENGTH];
static const char *jobReply = "ok";
static unsigned expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
static bool submitFails; // as without a harness
static bool readFails;   // every file read fails
static bool writeFails;  // every file write fails
// Only while folderChecked is set does paths_parentFolder look at the folder: otherwise every
// folder is open, so that the outputs of tests in the working directory pass wherever the suite
// runs. While foldersClosed is set, a folder named "fm24-closed-folder", which no folder the suite
// runs in is called, is one macOS's privacy protection keeps fm24ctl out of.
static bool folderChecked;
static bool foldersClosed;
static unsigned removedJobInputs; // the encoded inputs commands have removed
// While jobOutput is set, a submitted job writes it: every file read after the job returns it, and
// writtenConfig keeps only what is written after the job. The flags after it break reads or writes
// after the job.
static const char *jobOutput;
static bool readsFailAfterJob;
static bool writesFailAfterJob;

static bool submitJob(const char *job, unsigned timeout, char *result, size_t resultSize) {
	ASSERT_BOOL_EQ(timeout == expectedTimeout, true);
	(void)snprintf(submittedJob, sizeof submittedJob, "%s", job);
	(void)snprintf(result, resultSize, "%s", jobReply);
	if (jobOutput) {
		configText = jobOutput;
		buffer_clear(&writtenConfig);
		readFails = readFails || readsFailAfterJob;
		writeFails = writeFails || writesFailAfterJob;
	}
	return !submitFails;
}

static ClientOutcome watchedOutcome = CLIENT_REPLIED; // how `run`'s jobs end

// client_submitWatched for `run`: as submitJob, ending as `watchedOutcome` says, with the harness
// taking the job after 12 ms and finishing it 4 s later.
static ClientOutcome submitWatchedJob(
	const char *job,
	unsigned timeout,
	ClientWatch *watch,
	char *result,
	size_t resultSize
) {
	ClientOutcome outcome =
		submitJob(job, timeout, result, resultSize) ? watchedOutcome : CLIENT_NO_HARNESS;
	bool accepted = outcome != CLIENT_NO_HARNESS && outcome != CLIENT_QUEUE_TIMEOUT;
	*watch = (ClientWatch){.accepted = accepted, .queuedMs = 12, .runMs = accepted ? 4000 : 0};
	if (outcome != CLIENT_REPLIED) {
		result[0] = 0;
	}
	return outcome;
}

static bool readConfig(const wchar_t *path, ByteBuffer *out) {
	(void)path;
	++configReads;
	return !readFails && buffer_append(out, configText, strlen(configText));
}

static bool writeConfig(const wchar_t *path, const void *bytes, size_t length) {
	(void)path;
	return !writeFails && buffer_append(&writtenConfig, bytes, length);
}

// paths_parentFolder for commands.c: a closed folder's answer, the real one, or an open folder's.
static PathsFolder testParentFolder(const wchar_t *path) {
	if (foldersClosed && wcsstr(path, L"/fm24-closed-folder/")) {
		return PATHS_FOLDER_PRIVACY;
	}
	return folderChecked ? paths_parentFolder(path) : PATHS_FOLDER_FOUND;
}

// platform_removeFile for commands.c: counts the job inputs it removes (files_write wrote none).
static bool removeTestFile(const wchar_t *path) {
	if (wcsstr(path, L"_job_input-")) {
		++removedJobInputs;
	}
	return platform_removeFile(path);
}

#ifndef _WIN32
static unsigned ensures;                    // how often commands checked the game's save
static SessionNeed ensured;                 // what the last check was for
static char ensuredSave[SAVES_NAME_LENGTH]; // its save
static int ensureResult;                    // what checks return
static const char *ensureDate = "";         // and the save's in-game date they report

static pid_t testGame; // the game a manifest finds running, 0 for none

static pid_t findTestGame(char *path, size_t size) {
	(void)snprintf(path, size, "%s", testGame ? "/fm.app/Contents/MacOS/fm" : "");
	return testGame;
}

static bool testGameUnderRosetta(pid_t pid) {
	return pid && pid == testGame;
}

static uint64_t testGameBytes(pid_t pid) {
	return pid && pid == testGame ? 6123456789U : 0;
}

// macosGame_ensureSave for commands.c. It clears the submitted job, so one submitted after it
// shows.
static int ensureTestSave(const SessionNeed *need, char *date, size_t size) {
	++ensures;
	ensured = *need;
	(void)snprintf(ensuredSave, sizeof ensuredSave, "%s", need->save ? need->save : "");
	ensured.save = ensuredSave;
	if (date) {
		(void)snprintf(date, size, "%s", ensureDate);
	}
	submittedJob[0] = 0;
	return ensureResult;
}
#endif

bool inject_harness(void) {
	return false;
}

unsigned long inject_findGame(void) {
	return 0;
}

TEST(encodes_input_and_metadata_in_one_read) {
	Path input = {.wide = L"input.json", .utf8 = "input.json"};
	Path output = {.wide = L"output.bin", .utf8 = "output.bin"};
	uint32_t teams[2] = {0};
	size_t size = 0;
	SaveCheck check = {.save = "stale", .date = "stale"};
	bool lineup[2] = {true, true};
	configReads = 0;
	ASSERT_BOOL_EQ(encodeToFile(&input, &output, &size, teams, &check, NULL, lineup, NULL), true);
	ASSERT_INT_EQ(configReads, 1);
	ASSERT_BOOL_EQ(size == 1 && writtenConfig.length == 1 && writtenConfig.data[0] == 42, true);
	ASSERT_BOOL_EQ(teams[0] == 678 && teams[1] == 677, true);
	ASSERT_BOOL_EQ(!check.save[0] && !check.date[0] && !check.badSave, true);
	ASSERT_BOOL_EQ(!lineup[0] && !lineup[1], true);
	buffer_free(&writtenConfig);
	// The save and date, and the line-up markers, come from the same read.
	configText = "{\"format\":\"fm24-match-config/1\",\"save\":\"LIV\",\"game_date\":\"2024 day 1 "
							 "09:00\",\"away_lineup\":true,\"fields\":[{\"u8\":42}]}";
	configReads = 0;
	ASSERT_BOOL_EQ(encodeToFile(&input, &output, &size, teams, &check, NULL, lineup, NULL), true);
	ASSERT_INT_EQ(configReads, 1);
	ASSERT_BOOL_EQ(strcmp(check.save, "LIV") == 0 && strcmp(check.date, "2024 day 1 09:00") == 0, true);
	ASSERT_BOOL_EQ(!lineup[0] && lineup[1], true);
	configText = plainConfig;
	buffer_free(&writtenConfig);
}

TEST(replaces_either_team_and_preserves_both_overrides) {
	for (unsigned side = 0; side < 2; ++side) {
		wchar_t *arguments[] = {L"fm24ctl.exe",
			side ? L"replace-away" : L"replace-home",
			L"input.json",
			L"Norwich City",
			L"output.json",
			L"--full",
			NULL};
		configReads = 0;
		char result[CLIENT_RESULT_LENGTH];
		ASSERT_INT_EQ(commands_replaceTeamWithResult(6, arguments, result, sizeof result), EXIT_OK);
		ASSERT_INT_EQ(configReads, 1);
		ASSERT_BOOL_EQ(
			strstr(submittedJob, side ? "command=replace_away\n" : "command=replace_home\n") != NULL, true
		);
		ASSERT_BOOL_EQ(
			strstr(submittedJob, "\nteam=Norwich City\nhome_team=678\naway_team=677\n") != NULL, true
		);
		ASSERT_BOOL_EQ(strstr(submittedJob, "\nfull=1\n") != NULL, true);
		buffer_free(&writtenConfig);
	}
}

TEST(decodes_binary_fixtures_through_the_inspect_job) {
	wchar_t *arguments[] = {L"fm24ctl", L"decode", L"next.bin", L"next.json", L"--full", NULL};
	for (int full = 0; full < 2; ++full) {
		submittedJob[0] = 0;
		ASSERT_INT_EQ(commands_decode(4 + full, arguments), EXIT_OK);
		ASSERT_BOOL_EQ(strncmp(submittedJob, "command=inspect\ninput=", 22) == 0, true);
		ASSERT_BOOL_EQ(strstr(submittedJob, "next.bin\noutput=") != NULL, true);
		ASSERT_BOOL_EQ(
			strstr(submittedJob, full ? "next.json\nfull=1\n" : "next.json\nfull=0\n") != NULL, true
		);
	}
	jobReply = "error: cannot read input or invalid FM24 binary header";
	ASSERT_INT_EQ(commands_decode(4, arguments), EXIT_FAILED);
	jobReply = "ok";
	struct {
		int argc;
		wchar_t *argv[6];
	} invalid[] = {
		{3, {L"fm24ctl", L"decode", L"next.bin"}},
		{5, {L"fm24ctl", L"decode", L"next.bin", L"next.json", L"--seed"}},
		{6, {L"fm24ctl", L"decode", L"next.bin", L"next.json", L"--full", L"--full"}},
		{4, {L"fm24ctl", L"decode", L"next.bin", L"--full"}},
		{4, {L"fm24ctl", L"decode", L"Next.JSON", L"copy.json"}},
		{4, {L"fm24ctl", L"decode", L"next.bin", L"copy.bin"}},
		{4, {L"fm24ctl", L"decode", L"next.bin", L"next.txt"}},
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		submittedJob[0] = 0;
		ASSERT_INT_EQ(commands_decode(invalid[i].argc, invalid[i].argv), EXIT_USAGE);
		ASSERT_BOOL_EQ(submittedJob[0] == 0, true);
	}
}

TEST(reports_whether_the_game_is_ready) {
	wchar_t *arguments[] = {L"fm24ctl", L"status", L"extra", NULL};
	static const struct {
		const char *reply;
		int status;
	} cases[] = {
		{"ok: ready in_progress=1 processing=0 kind=-1 people=3 human_team=1 menu_seen=1 autoplay=1 "
		 "autoplay_read=1 date=2024 day 123 15:00 save=LIV MAIN",
			EXIT_OK},
		{"ok: ready", EXIT_OK},
		{"ok: readying", EXIT_WRONG_STATE},
		{"ok: menu in_progress=0 processing=0 kind=-1 people=0 human_team=0 menu_seen=1 autoplay=0 "
		 "autoplay_read=0",
			EXIT_WRONG_STATE},
		{"ok: busy in_progress=1 processing=1 kind=7", EXIT_WRONG_STATE},
		{"ok: no_team in_progress=1", EXIT_WRONG_STATE},
		{"error: status is not available on Windows yet", EXIT_FAILED},
		{"error: harness 1.3.0 does not support this game build; see harness.log", EXIT_FAILED},
	};
	expectedTimeout = CLIENT_STATUS_TIMEOUT_MS;
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		jobReply = cases[i].reply;
		submittedJob[0] = 0;
		ASSERT_INT_EQ(commands_status(2, arguments), cases[i].status);
		ASSERT_INT_EQ(strcmp(submittedJob, "command=status\n"), 0);
	}
	submitFails = true;
	ASSERT_INT_EQ(commands_status(2, arguments), EXIT_FAILED);
	submitFails = false;
	submittedJob[0] = 0;
	ASSERT_INT_EQ(commands_status(3, arguments), EXIT_USAGE);
	ASSERT_INT_EQ(submittedJob[0], 0);
	jobReply = "ok";
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
}

TEST(encodes_configs_alike_with_or_without_the_save_and_date) {
	static const char plain[] =
		"{\"format\":\"fm24-match-config/1\",\"source\":\"A vs B, 2024 day 130 15:00, seed 1\","
		"\"home_team\":678,\"fields\":[{\"u8\":42},{\"str\":\"ab\"}]}";
	static const char annotated[] =
		"{\"format\":\"fm24-match-config/1\",\"source\":\"A vs B, 2024 day 130 15:00, seed 1\","
		"\"save\":\"LIV \\\"MAIN\\\" \\u00d8\",\"game_date\":\"2024 day 123 15:00\","
		"\"home_team\":678,\"fields\":[{\"u8\":42},{\"str\":\"ab\"}]}";
	ByteBuffer bytes[2] = {{0}};
	uint32_t teams[2][2] = {{0}};
	char error[256] = "";
	ASSERT_BOOL_EQ(
		matchConfig_encode(plain, sizeof plain - 1, &bytes[0], teams[0], error, sizeof error), true
	);
	ASSERT_BOOL_EQ(
		matchConfig_encode(annotated, sizeof annotated - 1, &bytes[1], teams[1], error, sizeof error),
		true
	);
	ASSERT_BOOL_EQ(
		bytes[0].length == 7 && bytes[1].length == bytes[0].length &&
			memcmp(bytes[0].data, bytes[1].data, bytes[0].length) == 0,
		true
	);
	ASSERT_BOOL_EQ(teams[1][0] == 678 && teams[1][1] == 0, true);
	buffer_free(&bytes[0]);
	buffer_free(&bytes[1]);
	// The keys are read on their own.
	ConfigOrigin origin;
	ASSERT_BOOL_EQ(matchConfig_readOrigin(annotated, sizeof annotated - 1, &origin), true);
	ASSERT_BOOL_EQ(origin.hasSave && strcmp(origin.save, "LIV \"MAIN\" \xC3\x98") == 0, true);
	ASSERT_INT_EQ(strcmp(origin.gameDate, "2024 day 123 15:00"), 0);
	ASSERT_BOOL_EQ(matchConfig_readOrigin(plain, sizeof plain - 1, &origin), true);
	ASSERT_BOOL_EQ(!origin.hasSave && !origin.save[0] && !origin.gameDate[0], true);
}

TEST(reads_the_save_and_date_a_fixture_was_exported_from) {
	ConfigOrigin origin;
	// A "save" inside a field is not the header's.
	static const char nested[] =
		"{\"format\":\"fm24-match-config/1\",\"fields\":[{\"save\":\"inner\",\"u8\":1}],"
		"\"game_date\":\"2024 day 1 09:00\",\"save\":\"Outer\"}";
	ASSERT_BOOL_EQ(matchConfig_readOrigin(nested, sizeof nested - 1, &origin), true);
	ASSERT_BOOL_EQ(origin.hasSave && strcmp(origin.save, "Outer") == 0, true);
	ASSERT_INT_EQ(strcmp(origin.gameDate, "2024 day 1 09:00"), 0);
	// Keys that are not usable text are there, but empty.
	static const char *const unusable[] = {
		"{\"save\":5,\"game_date\":[1]}",
		"{\"save\":\"\",\"game_date\":\"\"}",
		"{\"save\":\"bell\\u0007\",\"game_date\":\"tab\\t\"}",
		"{\"save\":null,\"game_date\":\"2024 day 123 15:00, and far too long for a date\"}",
		"{\"save\":\"nul\\u0000\",\"game_date\":{\"day\":1}}",
	};
	for (size_t i = 0; i < sizeof unusable / sizeof unusable[0]; ++i) {
		ASSERT_BOOL_EQ(matchConfig_readOrigin(unusable[i], strlen(unusable[i]), &origin), true);
		ASSERT_BOOL_EQ(origin.hasSave && !origin.save[0] && !origin.gameDate[0], true);
	}
	// A save of 255 bytes fits; one more does not.
	char text[300];
	int prefix = snprintf(text, sizeof text, "{\"save\":\"");
	for (int extra = 0; extra < 2; ++extra) {
		memset(text + prefix, 'S', 255 + (size_t)extra);
		(void)snprintf(text + prefix + 255 + extra, sizeof text - (size_t)(prefix + 255 + extra), "\"}");
		ASSERT_BOOL_EQ(matchConfig_readOrigin(text, strlen(text), &origin) && origin.hasSave, true);
		ASSERT_INT_EQ((int)strlen(origin.save), extra ? 0 : 255);
	}
	// Text that is not a JSON object has no origin.
	static const char *const invalid[] = {
		"",
		"[]",
		"{\"save\":\"LIV\"",
		"{\"save\" \"LIV\"}",
		"{\"save\":\"LIV\",}",
		"{\"save\":\"\xC0\xAF\"}",
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		ASSERT_BOOL_EQ(matchConfig_readOrigin(invalid[i], strlen(invalid[i]), &origin), false);
		ASSERT_BOOL_EQ(!origin.hasSave && !origin.save[0] && !origin.gameDate[0], true);
	}
}

TEST(parses_player_ids_and_blocks) {
	char text[64];
	static const wchar_t *const invalidIds[] = {L"", L"0", L"-1", L" 1", L"1x", L"4294967296"};
	for (size_t i = 0; i < sizeof invalidIds / sizeof invalidIds[0]; ++i) {
		ASSERT_BOOL_EQ(parsePersonId(invalidIds[i], text, sizeof text), false);
	}
	ASSERT_BOOL_EQ(parsePersonId(L"2000040347", text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "2000040347"), 0);
	static const wchar_t *const invalidBlocks[] = {
		L"home.player[]",
		L"home.player[1",
		L"home.player[x]",
		L"home.player[1].ratings",
		L"both.player[1]",
		L"home.staff[1]",
	};
	for (size_t i = 0; i < sizeof invalidBlocks / sizeof invalidBlocks[0]; ++i) {
		ASSERT_BOOL_EQ(parsePlayerBlock(invalidBlocks[i], text, sizeof text), false);
	}
	ASSERT_BOOL_EQ(parsePlayerBlock(L"away.player[17]", text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "away.player[17]"), 0);
	ASSERT_BOOL_EQ(parsePlayerBlock(L"home.player[3]", text, 14), false);
}

TEST(rejects_malformed_cli_numbers) {
	static const wchar_t *const invalid[] = {
		L"",
		L"-1",
		L"+1",
		L" 1",
		L"1 ",
		L"1x",
		L"4294967296",
		L"999999999999999999999999999",
	};
	char seed[32];
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		ASSERT_BOOL_EQ(parseSeedValue(invalid[i], seed, sizeof seed), false);
	}
	ASSERT_BOOL_EQ(parseSeedValue(L"4294967295", seed, sizeof seed), true);
	ASSERT_INT_EQ(strcmp(seed, "4294967295"), 0);
	static wchar_t *const threads[] = {L"0", L"-1", L"+1", L" 1", L"17", L"99999999999999999999"};
	for (size_t i = 0; i < sizeof threads / sizeof threads[0]; ++i) {
		wchar_t *arguments[] = {L"--threads", threads[i]};
		int index = 0;
		RunOptions options = {0};
		ASSERT_BOOL_EQ(parseRunOption(2, arguments, &index, &options), false);
	}
	unsigned long values[3];
	ASSERT_BOOL_EQ(parseCsvNumbers("4294967295,1,2,100,1,123,5.5\n", values, 3), true);
	ASSERT_BOOL_EQ(values[0] == UINT32_MAX && values[1] == 1 && values[2] == 2, true);
	static const char *const invalidRows[] = {
		"-1,1,2",
		"+1,1,2",
		" 1,1,2",
		"1,2,3junk",
		"1,2,",
		"4294967296,1,2",
		"1,2x,3",
		"1,2,+3",
		"1,2, 3",
	};
	for (size_t i = 0; i < sizeof invalidRows / sizeof invalidRows[0]; ++i) {
		ASSERT_BOOL_EQ(parseCsvNumbers(invalidRows[i], values, 3), false);
	}
	Totals totals = {.runs = SUMMARY_SHOWN, .homeGoals = UINT32_MAX, .awayGoals = UINT32_MAX};
	addResult(&totals, 42, 2, 1);
	ASSERT_BOOL_EQ(totals.homeGoals == (uint64_t)UINT32_MAX + 2, true);
	ASSERT_BOOL_EQ(totals.awayGoals == (uint64_t)UINT32_MAX + 1, true);
}

static void checkResult(const void *data, size_t length, bool expected) {
	char result[32] = "";
	memcpy(result, data, length);
	ASSERT_BOOL_EQ(validResult(result, length), expected);
	ASSERT_BOOL_EQ(result[0] != 0, expected);
}

TEST(rejects_missing_or_incomplete_results) {
	ASSERT_BOOL_EQ(client_succeeded("ok"), true);
	ASSERT_BOOL_EQ(client_succeeded("ok: complete"), true);
	ASSERT_BOOL_EQ(client_succeeded("okay"), false);
	ASSERT_BOOL_EQ(client_succeeded("okerror"), false);
	ASSERT_BOOL_EQ(client_succeeded("o"), false);
	ASSERT_BOOL_EQ(client_succeeded(""), false);
	checkResult("ok: done", 8, true);
	checkResult("ok: done\n", 9, true);
	checkResult("ok: done\r\n", 10, true);
	checkResult("ok: done\nerror", 14, false);
	checkResult("", 0, false);
	checkResult("ok\0error", 8, false);
}

#define TEST_STATUS_FLAGS                                                                          \
	"in_progress=1 processing=0 kind=-1 people=152340 human_team=1 menu_seen=1 autoplay=1 "         \
	"autoplay_read=1"

TEST(parses_the_harness_status_line) {
	GameStatus status;
	// The save runs to the end of the line, whatever it holds.
	ASSERT_BOOL_EQ(
		saves_parseStatus(
			"ok: ready " TEST_STATUS_FLAGS " date=2024 day 123 15:00 save=LIV MAIN (v02) date=x", &status
		),
		true
	);
	ASSERT_BOOL_EQ(status.phase == STATUS_READY && status.inProgress && !status.processing, true);
	ASSERT_BOOL_EQ(status.kind == -1 && status.people == 152340 && status.humanTeam, true);
	ASSERT_BOOL_EQ(status.menuSeen && status.autoPlay && status.autoPlayRead, true);
	ASSERT_INT_EQ(strcmp(status.date, "2024 day 123 15:00"), 0);
	ASSERT_INT_EQ(strcmp(status.save, "LIV MAIN (v02) date=x"), 0);
	// The date and the save are left out when the harness cannot read them; a line ending is fine.
	ASSERT_BOOL_EQ(
		saves_parseStatus(
			"ok: busy in_progress=0 processing=1 kind=3 people=0 human_team=0 menu_seen=0 autoplay=1 "
			"autoplay_read=0\r\n",
			&status
		),
		true
	);
	ASSERT_BOOL_EQ(status.phase == STATUS_BUSY && status.processing && status.kind == 3, true);
	ASSERT_BOOL_EQ(!status.inProgress && !status.menuSeen && !status.date[0] && !status.save[0], true);
	ASSERT_BOOL_EQ(saves_parseStatus("ok: no_team " TEST_STATUS_FLAGS " save=a save=b\n", &status), true);
	ASSERT_BOOL_EQ(status.phase == STATUS_NO_TEAM && strcmp(status.save, "a save=b") == 0, true);
	static const char *const phases[] = {"starting", "menu", "loading", "busy", "ready", "no_team"};
	for (size_t i = 0; i < sizeof phases / sizeof phases[0]; ++i) {
		char line[256];
		(void)snprintf(line, sizeof line, "ok: %s " TEST_STATUS_FLAGS, phases[i]);
		ASSERT_BOOL_EQ(saves_parseStatus(line, &status) && status.phase == (StatusPhase)i, true);
		ASSERT_INT_EQ(strcmp(saves_phaseName(status.phase), phases[i]), 0);
	}
	// A save of 255 bytes fits; one more does not.
	char line[512];
	int prefix = snprintf(line, sizeof line, "ok: ready " TEST_STATUS_FLAGS " save=");
	memset(line + prefix, 'S', 255);
	line[prefix + 255] = 0;
	ASSERT_BOOL_EQ(saves_parseStatus(line, &status) && strlen(status.save) == 255, true);
	line[prefix + 255] = 'S';
	line[prefix + 256] = 0;
	ASSERT_BOOL_EQ(saves_parseStatus(line, &status), false);
	static const char *const garbled[] = {
		"error: FM24 is busy (loading); wait until it is idle",
		"error: status is not available on Windows yet",
		"ok: 21 bytes",
		"ok: ready",
		"ok: sleeping " TEST_STATUS_FLAGS,
		"ok: ready in_progress=1 processing=0 kind=-1 people=1 human_team=1 menu_seen=1 autoplay=1",
		"ok: ready processing=0 in_progress=1 kind=-1 people=1 human_team=1 menu_seen=1 autoplay=1 "
		"autoplay_read=1",
		"ok: ready in_progress=2 processing=0 kind=-1 people=1 human_team=1 menu_seen=1 autoplay=1 "
		"autoplay_read=1",
		"ok: ready in_progress=1 processing=0 kind=x people=1 human_team=1 menu_seen=1 autoplay=1 "
		"autoplay_read=1",
		"ok: ready in_progress=1 processing=0 kind=-1 people=-1 human_team=1 menu_seen=1 autoplay=1 "
		"autoplay_read=1",
		"ok: ready in_progress=1  processing=0 kind=-1 people=1 human_team=1 menu_seen=1 autoplay=1 "
		"autoplay_read=1",
		"ok: ready " TEST_STATUS_FLAGS " date=",
		"ok: ready " TEST_STATUS_FLAGS " save=",
		"ok: ready " TEST_STATUS_FLAGS " extra=1",
		"ok: ready " TEST_STATUS_FLAGS "\nsave=X",
		"ok: ready " TEST_STATUS_FLAGS " date=2024 day 123 15:00 save=X\n\n",
	};
	for (size_t i = 0; i < sizeof garbled / sizeof garbled[0]; ++i) {
		ASSERT_BOOL_EQ(saves_parseStatus(garbled[i], &status), false);
		ASSERT_BOOL_EQ(status.kind == -1 && !status.inProgress && !status.save[0], true);
	}
}

TEST(finds_a_save_by_its_exact_name) {
	ASSERT_BOOL_EQ(saves_isName("LIV_MAIN_FM24") && saves_isName("LIV MAIN (v02)"), true);
	static const char *const files[] = {"LIV_MAIN_FM24.fm", "Copy.FM", "saves/LIV", ".fm", ""};
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		ASSERT_BOOL_EQ(saves_isName(files[i]), false);
	}
	// A file's name without ".fm", which is all the harness strips.
	char name[SAVES_NAME_LENGTH];
	ASSERT_BOOL_EQ(saves_nameOfPath("/a b/cloud/games/LIV (v02).fm", name, sizeof name), true);
	ASSERT_INT_EQ(strcmp(name, "LIV (v02)"), 0);
	ASSERT_BOOL_EQ(saves_nameOfPath("/x/Copy.FM", name, sizeof name), true);
	ASSERT_INT_EQ(strcmp(name, "Copy.FM"), 0);
	ASSERT_BOOL_EQ(saves_nameOfPath("/x/", name, sizeof name), false);
	ASSERT_BOOL_EQ(saves_nameOfPath("/x/LIV.fm", name, 3), false);
	// The save the harness reports, or a path ending in it; no other save, case or prefix.
	ASSERT_BOOL_EQ(saves_isSave("LIV_MAIN_FM24", "LIV_MAIN_FM24"), true);
	ASSERT_BOOL_EQ(saves_isSave("/Users/x/cloud/games/LIV_MAIN_FM24", "LIV_MAIN_FM24"), true);
	ASSERT_BOOL_EQ(saves_isSave("LIV_MAIN_FM24_FINAL", "LIV_MAIN_FM24"), false);
	ASSERT_BOOL_EQ(saves_isSave("LIV_MAIN_FM24 (v02)", "LIV_MAIN_FM24"), false);
	ASSERT_BOOL_EQ(saves_isSave("liv_main_fm24", "LIV_MAIN_FM24"), false);
	ASSERT_BOOL_EQ(saves_isSave("", "LIV_MAIN_FM24"), false);
	static const char *const cloud[] = {
		"LIV_MAIN_FM24.fm",
		"LIV_MAIN_FM24 (v02).fm",
		"LIV_MAIN_FM24_FINAL.fm",
		"last save overwrite backup.fm",
		"LIV_MAIN_FM24.fm.bak",
		"steam_autocloud.vdf",
	};
	SaveSearch search;
	SavesFolder folder = SAVES_LOCAL;
	// Only in the cloud folder, with longer names beside it.
	saves_startSearch(&search, "LIV_MAIN_FM24");
	for (size_t i = 0; i < sizeof cloud / sizeof cloud[0]; ++i) {
		saves_note(&search, SAVES_CLOUD, cloud[i]);
	}
	saves_note(&search, SAVES_LOCAL, "config.xml");
	ASSERT_BOOL_EQ(saves_result(&search, &folder) == SAVES_FOUND && folder == SAVES_CLOUD, true);
	// Only in games, then in both.
	saves_startSearch(&search, "Sandbox Copy");
	saves_note(&search, SAVES_CLOUD, "LIV_MAIN_FM24.fm");
	saves_note(&search, SAVES_LOCAL, "Sandbox Copy.fm");
	ASSERT_BOOL_EQ(saves_result(&search, &folder) == SAVES_FOUND && folder == SAVES_LOCAL, true);
	saves_note(&search, SAVES_CLOUD, "Sandbox Copy.fm");
	ASSERT_BOOL_EQ(saves_result(&search, &folder) == SAVES_AMBIGUOUS, true);
	// Missing: saves that start with the name, in any case, are listed.
	saves_startSearch(&search, "liv_main_fm24");
	for (size_t i = 0; i < sizeof cloud / sizeof cloud[0]; ++i) {
		saves_note(&search, SAVES_CLOUD, cloud[i]);
	}
	ASSERT_BOOL_EQ(saves_result(&search, &folder) == SAVES_MISSING, true);
	ASSERT_INT_EQ(strcmp(search.similar, "LIV_MAIN_FM24, LIV_MAIN_FM24 (v02), LIV_MAIN_FM24_FINAL"), 0);
	ASSERT_BOOL_EQ(search.similarCount == 3 && search.similarListed == 3, true);
	saves_startSearch(&search, "Nothing");
	saves_note(&search, SAVES_CLOUD, "LIV_MAIN_FM24.fm");
	ASSERT_BOOL_EQ(saves_result(&search, &folder) == SAVES_MISSING && !search.similar[0], true);
	// A list that does not fit keeps whole names and counts the rest.
	saves_startSearch(&search, "S");
	for (int i = 0; i < 200; ++i) {
		char file[64];
		(void)snprintf(file, sizeof file, "Save %03d with a long name.fm", i);
		saves_note(&search, SAVES_LOCAL, file);
	}
	ASSERT_BOOL_EQ(search.similarCount == 200 && search.similarListed > 0, true);
	ASSERT_BOOL_EQ(search.similarListed < 200 && strlen(search.similar) < sizeof search.similar, true);
	ASSERT_BOOL_EQ(strcmp(search.similar + strlen(search.similar) - 4, "name") == 0, true);
	char path[64];
	ASSERT_BOOL_EQ(saves_path("/Users/a b/FM", SAVES_CLOUD, "LIV (v02)", path, sizeof path), true);
	ASSERT_INT_EQ(strcmp(path, "/Users/a b/FM/cloud/games/LIV (v02).fm"), 0);
	ASSERT_BOOL_EQ(saves_path("/Users/a b/FM", SAVES_LOCAL, "LIV (v02)", path, sizeof path), true);
	ASSERT_INT_EQ(strcmp(path, "/Users/a b/FM/games/LIV (v02).fm"), 0);
	ASSERT_BOOL_EQ(saves_path("/Users/a b/FM", SAVES_LOCAL, "LIV (v02)", path, 24), false);
}

TEST(writes_only_load_game_into_the_autoplay_file) {
	char json[128];
	ASSERT_BOOL_EQ(saves_autoplayJson("/Users/a b/cloud/games/LIV.fm", json, sizeof json), true);
	ASSERT_INT_EQ(strcmp(json, "{\"LOAD_GAME\": \"/Users/a b/cloud/games/LIV.fm\"}"), 0);
	// Quotes and backslashes are escaped, and other text, UTF-8 included, is kept as it is.
	ASSERT_BOOL_EQ(saves_autoplayJson("/x/\"\xC3\x98\" \\ \xE2\x82\xAC.fm", json, sizeof json), true);
	ASSERT_INT_EQ(strcmp(json, "{\"LOAD_GAME\": \"/x/\\\"\xC3\x98\\\" \\\\ \xE2\x82\xAC.fm\"}"), 0);
	// No file for control characters or an empty path, or when it does not fit.
	ASSERT_BOOL_EQ(saves_autoplayJson("/x/a\nb.fm", json, sizeof json), false);
	ASSERT_BOOL_EQ(saves_autoplayJson("/x/\t.fm", json, sizeof json), false);
	ASSERT_BOOL_EQ(saves_autoplayJson("", json, sizeof json), false);
	size_t exact = sizeof "{\"LOAD_GAME\": \"/\\\"\"}";
	ASSERT_BOOL_EQ(saves_autoplayJson("/\"", json, exact), true);
	ASSERT_INT_EQ(strcmp(json, "{\"LOAD_GAME\": \"/\\\"\"}"), 0);
	ASSERT_BOOL_EQ(saves_autoplayJson("/\"", json, exact - 1), false);
}

// A sample at `at` in which the harness reports `phase` with `save` ("" for none).
static SessionSample statusSample(time_t at, StatusPhase phase, const char *save) {
	SessionSample sample = {.at = at, .running = true, .answer = SESSION_ANSWER_STATUS};
	sample.status.phase = phase;
	sample.status.kind = -1;
	sample.status.inProgress = phase == STATUS_READY || phase == STATUS_NO_TEAM;
	sample.status.menuSeen = phase != STATUS_STARTING;
	(void)snprintf(sample.status.save, sizeof sample.status.save, "%s", save);
	return sample;
}

// Steps through samples one second apart from `at`, in which the harness reports `phases`, each with
// `save`. The outcome of the last.
static SessionOutcome stepThrough(
	SessionWait *wait,
	time_t at,
	const StatusPhase *phases,
	size_t count,
	const char *save
) {
	SessionOutcome outcome = SESSION_WAITING;
	for (size_t i = 0; i < count; ++i) {
		SessionSample sample = statusSample(at + (time_t)i, phases[i], save);
		outcome = session_step(wait, &sample);
		if (i + 1 < count) {
			ASSERT_INT_EQ(outcome, SESSION_WAITING);
		}
	}
	return outcome;
}

TEST(waits_until_the_save_is_loaded_and_stays_idle) {
	enum { START = 1000, TIMEOUT = 900 }; // a clock time: never 0, like a real one
	static const StatusPhase loading[] = {
		STATUS_STARTING, STATUS_LOADING, STATUS_READY, STATUS_READY, STATUS_READY
	};
	static const StatusPhase flicker[] = {
		STATUS_READY, STATUS_READY, STATUS_BUSY, STATUS_READY, STATUS_READY, STATUS_READY
	};
	static const StatusPhase ready[] = {STATUS_READY, STATUS_READY, STATUS_READY};
	static const StatusPhase busy[] = {STATUS_BUSY, STATUS_READY, STATUS_READY, STATUS_READY};
	SessionWait wait;
	// After a launch: ready once three samples in a row are ready with the save.
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, loading, 5, "LIV"), SESSION_READY);
	ASSERT_BOOL_EQ(wait.answered && wait.last.phase == STATUS_READY, true);
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, flicker, 6, "LIV"), SESSION_READY);
	// The save changes: counted again, then reported as another save. One the harness cannot name
	// is taken to be the one asked for.
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, ready, 2, "LIV"), SESSION_WAITING);
	ASSERT_INT_EQ(stepThrough(&wait, START + 2, ready, 2, "LIV_FINAL"), SESSION_WAITING);
	ASSERT_INT_EQ(stepThrough(&wait, START + 4, ready, 1, "LIV_FINAL"), SESSION_OTHER_SAVE);
	ASSERT_INT_EQ(strcmp(wait.last.save, "LIV_FINAL"), 0);
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, ready, 3, ""), SESSION_READY);
	// A game that was running already is decided by its first sample, unless it was busy.
	session_start(&wait, "LIV", false, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, ready, 1, "LIV"), SESSION_READY);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, ready, 1, "LIV (v02)"), SESSION_OTHER_SAVE);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	SessionSample sample = statusSample(START, STATUS_MENU, "");
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_AT_MENU);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, busy, 4, "LIV"), SESSION_READY);
	// A missing answer also breaks a run of ready samples.
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START, ready, 2, "LIV"), SESSION_WAITING);
	sample = (SessionSample){.at = START + 2, .running = true, .answer = SESSION_ANSWER_NONE};
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	ASSERT_INT_EQ(stepThrough(&wait, START + 3, ready, 2, "LIV"), SESSION_WAITING);
	ASSERT_INT_EQ(stepThrough(&wait, START + 5, ready, 1, "LIV"), SESSION_READY);
	// The game exits, or the harness refuses the status job.
	session_start(&wait, "LIV", true, TIMEOUT, START);
	sample = statusSample(START, STATUS_READY, "LIV");
	sample.running = false;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_EXITED);
	sample.running = true;
	sample.answer = SESSION_ANSWER_ERROR;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_REFUSED);
	// Still at the start screen SESSION_MENU_GRACE_S after a launch: the game did not read the
	// auto-play file, or read it without loading the save. Loading in between starts it again.
	for (int read = 0; read < 2; ++read) {
		session_start(&wait, "LIV", true, TIMEOUT, START);
		sample = statusSample(START + 5, STATUS_MENU, "");
		sample.status.autoPlay = read;
		sample.status.autoPlayRead = read;
		ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
		sample.at = START + 5 + SESSION_MENU_GRACE_S - 1;
		ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
		sample.at = START + 5 + SESSION_MENU_GRACE_S;
		ASSERT_INT_EQ(session_step(&wait, &sample), read ? SESSION_NOT_LOADED : SESSION_NO_AUTOPLAY);
	}
	session_start(&wait, "LIV", true, TIMEOUT, START);
	sample = statusSample(START, STATUS_MENU, "");
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	SessionSample load = statusSample(START + 10, STATUS_LOADING, "");
	ASSERT_INT_EQ(session_step(&wait, &load), SESSION_WAITING);
	sample.at = START + 11;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	sample.at = START + 11 + SESSION_MENU_GRACE_S - 1;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	// A game without the human manager's team for SESSION_NO_TEAM_GRACE_S.
	session_start(&wait, "LIV", false, TIMEOUT, START);
	sample = statusSample(START, STATUS_NO_TEAM, "LIV");
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	sample.at = START + SESSION_NO_TEAM_GRACE_S - 1;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	sample.at = START + SESSION_NO_TEAM_GRACE_S;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_NO_TEAM);
	// The time runs out, unless the last sample decides.
	session_start(&wait, "LIV", true, TIMEOUT, START);
	sample = statusSample(START + TIMEOUT - 1, STATUS_LOADING, "");
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_WAITING);
	sample.at = START + TIMEOUT;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_TIMED_OUT);
	session_start(&wait, "LIV", true, TIMEOUT, START);
	ASSERT_INT_EQ(stepThrough(&wait, START + TIMEOUT - 2, ready, 3, "LIV"), SESSION_READY);
	// A batch's stop ends a wait that a sample does not decide: for a busy game, or for an answer
	// behind another runner's job. A sample that decides it, or a game that exits, comes first.
	session_start(&wait, "LIV", false, TIMEOUT, START);
	sample = statusSample(START, STATUS_BUSY, "LIV");
	sample.stopped = true;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_STOPPED);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	sample = (SessionSample){
		.at = START + TIMEOUT, .running = true, .answer = SESSION_ANSWER_NONE, .stopped = true};
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_STOPPED);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	sample = statusSample(START, STATUS_READY, "LIV");
	sample.stopped = true;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_READY);
	session_start(&wait, "LIV", false, TIMEOUT, START);
	sample = statusSample(START, STATUS_READY, "LIV_FINAL");
	sample.stopped = true;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_OTHER_SAVE);
	sample.running = false;
	ASSERT_INT_EQ(session_step(&wait, &sample), SESSION_EXITED);
}


TEST(decides_what_happens_before_a_job_for_a_save) {
	enum { START = 1000 }; // a clock time
	SessionNeed need = {.save = "LIV", .launch = true, .timeoutMs = CLIENT_SHORT_TIMEOUT_MS};
	// No game: start it with the save, unless --no-launch leaves the job to fail as before.
	ASSERT_INT_EQ(session_plan(&need, false, false), SESSION_PLAN_LAUNCH);
	need.launch = false;
	ASSERT_INT_EQ(session_plan(&need, false, false), SESSION_PLAN_SUBMIT);
	// A running game is checked if it serves this harness; without it, or with another version of
	// it, the job explains as before.
	for (int launch = 0; launch < 2; ++launch) {
		need.launch = launch;
		ASSERT_INT_EQ(session_plan(&need, true, true), SESSION_PLAN_CHECK);
		ASSERT_INT_EQ(session_plan(&need, true, false), SESSION_PLAN_SUBMIT);
	}
	// A job without a save is submitted as before.
	static const char *const none[] = {NULL, ""};
	for (size_t i = 0; i < sizeof none / sizeof none[0]; ++i) {
		SessionNeed any = {.save = none[i], .launch = true};
		for (int state = 0; state < 4; ++state) {
			ASSERT_INT_EQ(session_plan(&any, state & 1, state & 2), SESSION_PLAN_SUBMIT);
		}
	}
	// After the wait: the job goes ahead with the save, and with a harness that answers no status;
	// with another save only given --any-save; otherwise not.
	static const struct {
		SessionOutcome outcome;
		bool submits;
		bool withAnySave;
	} outcomes[] = {
		{SESSION_READY, true, true},
		{SESSION_REFUSED, true, true},
		{SESSION_OTHER_SAVE, false, true},
		{SESSION_AT_MENU, false, false},
		{SESSION_NO_AUTOPLAY, false, false},
		{SESSION_NOT_LOADED, false, false},
		{SESSION_NO_TEAM, false, false},
		{SESSION_EXITED, false, false},
		{SESSION_TIMED_OUT, false, false},
		{SESSION_STOPPED, false, false},
		{SESSION_WAITING, false, false},
	};
	for (size_t i = 0; i < sizeof outcomes / sizeof outcomes[0]; ++i) {
		need.anySave = false;
		ASSERT_BOOL_EQ(session_submits(&need, outcomes[i].outcome), outcomes[i].submits);
		need.anySave = true;
		ASSERT_BOOL_EQ(session_submits(&need, outcomes[i].outcome), outcomes[i].withAnySave);
	}
	// A loaded save the harness cannot name is taken to be the one wanted, so the job goes ahead;
	// another one only with --any-save.
	need.anySave = false;
	SessionWait wait;
	session_start(&wait, "LIV", false, 900, START);
	SessionSample sample = statusSample(START, STATUS_READY, "");
	ASSERT_BOOL_EQ(session_submits(&need, session_step(&wait, &sample)), true);
	session_start(&wait, "LIV", false, 900, START);
	sample = statusSample(START, STATUS_READY, "LIV_FINAL");
	SessionOutcome other = session_step(&wait, &sample);
	ASSERT_BOOL_EQ(!session_submits(&need, other) && other == SESSION_OTHER_SAVE, true);
	need.anySave = true;
	ASSERT_BOOL_EQ(session_submits(&need, other), true);
}

typedef int Command(int argc, wchar_t **argv);

// The exit code of `command` with the arguments up to a NULL, after clearing the submitted job.
static int invoke(Command *command, wchar_t **arguments) {
	int argc = 0;
	while (arguments[argc]) {
		++argc;
	}
	submittedJob[0] = 0;
#ifndef _WIN32
	ensures = 0;
#endif
	return command(argc, arguments);
}

enum { ANY_RESULT = -1 }; // a command ran, whatever came of it

// A command's arguments, its exit code and a part of the job it submitted (NULL for none).
typedef struct ArgumentCase {
	Command *command;
	wchar_t *arguments[10];
	int status;
	const char *job;
} ArgumentCase;

// Runs each case; `refused` cases must be usage errors without a job instead.
static void checkArguments(const ArgumentCase *cases, size_t count, bool refused) {
	for (size_t i = 0; i < count; ++i) {
		int expected = refused ? EXIT_USAGE : cases[i].status;
		const char *job = refused ? NULL : cases[i].job;
		int status = invoke(cases[i].command, (wchar_t **)cases[i].arguments);
		if (expected != ANY_RESULT) {
			ASSERT_INT_EQ(status, expected);
		}
		ASSERT_BOOL_EQ(job ? strstr(submittedJob, job) != NULL : !submittedJob[0], true);
	}
}

TEST(keeps_every_argument_form_and_takes_the_save_options) {
	static const ArgumentCase cases[] = {
		// What has always worked works as before.
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-home", L"next.json", L"677", L"out.json"},
			EXIT_OK,
			"\nfull=0\n"},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-home", L"next.json", L"677", L"out.json", L"--full"},
			EXIT_OK,
			"\nfull=1\n"},
		{commands_listTeams, {L"fm24ctl", L"list-teams", L"teams.txt"}, EXIT_OK, "command=list_teams\n"},
		{commands_listPlayers,
			{L"fm24ctl", L"list-players", L"players.txt"},
			EXIT_OK,
			"command=list_players\n"},
		{commands_getPlayer,
			{L"fm24ctl", L"get-player", L"2000040347", L"p.json"},
			EXIT_OK,
			"command=get_player\nperson=2000040347\n"},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--seed", L"42", L"--full"},
			EXIT_OK,
			"\nseed=42\nfull=1\n"},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--full", L"--seed", L"7", L"--seed", L"8"},
			EXIT_OK,
			"\nseed=8\nfull=1\n"},
		{commands_injectPlayer,
			{L"fm24ctl", L"inject-player", L"next.json", L"home.player[0]", L"2000040347", L"out.json"},
			ANY_RESULT,
			"command=get_player\n"},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"out.json",
				L"--allow-duplicate"},
			ANY_RESULT,
			"command=get_player\n"},
		// And what has always been refused is refused.
		{commands_replaceTeam, {L"fm24ctl", L"replace-home", L"next.json", L"677"}, EXIT_USAGE, NULL},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-home", L"next.json", L"677", L"out.json", L"--full", L"--full"},
			EXIT_USAGE,
			NULL},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-home", L"next.json", L"677", L"out.json", L"extra.json"},
			EXIT_USAGE,
			NULL},
		{commands_listTeams, {L"fm24ctl", L"list-teams", L"teams.txt", L"extra"}, EXIT_USAGE, NULL},
		{commands_listPlayers, {L"fm24ctl", L"list-players", L"a.txt", L"--full"}, EXIT_USAGE, NULL},
		{commands_getPlayer, {L"fm24ctl", L"get-player", L"1", L"p.json", L"x"}, EXIT_USAGE, NULL},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--seed"},
			EXIT_USAGE,
			NULL},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"extra"},
			EXIT_USAGE,
			NULL},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"out.json",
				L"--allow-duplicate",
				L"--allow-duplicate"},
			EXIT_USAGE,
			NULL},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"out.json",
				L"--full"},
			EXIT_USAGE,
			NULL},
		{commands_decode,
			{L"fm24ctl", L"decode", L"next.bin", L"next.json", L"--save", L"LIV"},
			EXIT_USAGE,
			NULL},
		// --no-launch and --any-save need --save where there is no fixture, and names must be names.
		{commands_listTeams, {L"fm24ctl", L"list-teams", L"teams.txt", L"--no-launch"}, EXIT_USAGE, NULL},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--any-save"},
			EXIT_USAGE,
			NULL},
		{commands_listTeams, {L"fm24ctl", L"list-teams", L"teams.txt", L"--save"}, EXIT_USAGE, NULL},
		{commands_listTeams,
			{L"fm24ctl", L"list-teams", L"teams.txt", L"--save", L"LIV", L"--save", L"LIV"},
			EXIT_USAGE,
			NULL},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-away", L"next.json", L"677", L"out.json", L"--save", L"LIV"},
			EXIT_USAGE,
			NULL},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-away", L"next.json", L"677", L"out.json", L"--any-save", L"--any-save"},
			EXIT_USAGE,
			NULL},
	};
	checkArguments(cases, sizeof cases / sizeof cases[0], false);
	// The save options follow the fixed arguments, in any order, once each; Windows has none.
	static const ArgumentCase saveOptions[] = {
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-away", L"next.json", L"677", L"out.json", L"--no-launch", L"--full"},
			EXIT_OK,
			"\nfull=1\n"},
		{commands_listTeams,
			{L"fm24ctl", L"list-teams", L"teams.txt", L"--any-save", L"--save", L"LIV MAIN (v02)"},
			EXIT_OK,
			"command=list_teams\n"},
		{commands_listPlayers,
			{L"fm24ctl", L"list-players", L"a.txt", L"--save", L"\u00D8degaard \u20AC", L"--no-launch"},
			EXIT_OK,
			"command=list_players\n"},
		{commands_getPlayer,
			{L"fm24ctl", L"get-player", L"7", L"p.json", L"--save", L"LIV", L"--any-save"},
			EXIT_OK,
			"command=get_player\nperson=7\n"},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--save", L"LIV", L"--seed", L"3"},
			EXIT_OK,
			"\nseed=3\nfull=0\n"},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"out.json",
				L"--any-save",
				L"--allow-duplicate",
				L"--no-launch"},
			ANY_RESULT,
			"command=get_player\n"},
	};
	checkArguments(saveOptions, sizeof saveOptions / sizeof saveOptions[0], SAVE_OPTIONS == 0);
	// run takes the save flags anywhere, like its others.
	wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", L"--no-launch", L"1-10", L"--any-save", NULL};
	RunOptions options;
	ASSERT_BOOL_EQ(parseRunArguments(6, run, &options), SAVE_OPTIONS != 0);
	ASSERT_BOOL_EQ(!SAVE_OPTIONS || (options.check.noLaunch && options.check.anySave), true);
	ASSERT_BOOL_EQ(!SAVE_OPTIONS || wcscmp(options.seeds, L"1-10") == 0, true);
	ASSERT_BOOL_EQ(parseRunArguments(5, run, &options) && !options.check.anySave, SAVE_OPTIONS != 0);
	ASSERT_BOOL_EQ(parseRunArguments(3, run, &options) && !options.check.noLaunch, true);
	// A --save value is a save's name: its file name without .fm, with no folder or control
	// characters.
	char name[SAVES_NAME_LENGTH];
	static const wchar_t *const names[] = {L"LIV_MAIN_FM24", L"LIV MAIN (v02)", L"\u00D8degaard"};
	for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
		ASSERT_BOOL_EQ(parseSaveName(names[i], name, sizeof name), true);
	}
	ASSERT_INT_EQ(strcmp(name, "\xC3\x98" "degaard"), 0);
	wchar_t tooLong[SAVES_NAME_LENGTH + 1];
	wmemset(tooLong, L'S', SAVES_NAME_LENGTH);
	tooLong[SAVES_NAME_LENGTH] = 0;
	static const wchar_t *const notNames[] = {L"", L"a/b", L"LIV.fm", L"LIV.FM", L"tab\there", L"\x1B[2J"};
	for (size_t i = 0; i < sizeof notNames / sizeof notNames[0]; ++i) {
		ASSERT_BOOL_EQ(parseSaveName(notNames[i], name, sizeof name), false);
		ASSERT_INT_EQ(name[0], 0);
	}
	ASSERT_BOOL_EQ(parseSaveName(tooLong, name, sizeof name), false);
	tooLong[SAVES_NAME_LENGTH - 1] = 0;
	ASSERT_BOOL_EQ(parseSaveName(tooLong, name, sizeof name), true);
}

TEST(stops_bad_seeds_and_missing_output_folders_before_the_game) {
	// Seed patterns the harness takes, and those it would refuse, which fm24ctl now refuses itself,
	// before it checks the save or submits a job.
	wchar_t longest[SEED_PATTERN_LENGTH + 2];
	for (size_t i = 0; i < SEED_PATTERN_LENGTH; ++i) {
		longest[i] = i % 2 ? L',' : L'1';
	}
	longest[SEED_PATTERN_LENGTH] = 0;
	static const wchar_t *const good[] = {
		L"keep", L"42", L"1-1000", L"1,5,10-20", L"42,42,42", L"0-10000000", L"4294967295",
	};
	RunOptions options;
	for (size_t i = 0; i <= sizeof good / sizeof good[0]; ++i) {
		wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", i < 1 ? longest : (wchar_t *)good[i - 1]};
		ASSERT_BOOL_EQ(parseRunArguments(4, run, &options), true);
	}
	longest[SEED_PATTERN_LENGTH] = L'1';
	longest[SEED_PATTERN_LENGTH + 1] = 0;
	static const wchar_t *const bad[] = {
		L"10-1",
		L"1,,5",
		L"1,",
		L",1",
		L"1-2-3",
		L"-5",
		L"0-10000001",
		L"4294967296",
		L"1-4294967296",
		L"4294967290-4294967296",
		L"99999999999999999999999",
	};
	for (size_t i = 0; i <= sizeof bad / sizeof bad[0]; ++i) {
		wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", i < 1 ? longest : (wchar_t *)bad[i - 1], NULL};
		ASSERT_BOOL_EQ(parseRunArguments(4, run, &options), false);
		ASSERT_INT_EQ(invoke(commands_run, run), EXIT_USAGE);
		ASSERT_INT_EQ(submittedJob[0], 0);
#ifndef _WIN32
		ASSERT_INT_EQ((int)ensures, 0);
#endif
	}
	// An output in a folder that does not exist, or that is a folder itself, stops the command
	// before it checks the save or submits a job: the game would only report a failed write.
	static const ArgumentCase missingFolder[] = {
		{commands_getNextFixture, {L"fm24ctl", L"get-next-fixture", L"."}, EXIT_FAILED, NULL},
		{commands_run, {L"fm24ctl", L"run", L"next.json", L"1-10", L".."}, EXIT_FAILED, NULL},
		{commands_run,
			{L"fm24ctl", L"run", L"next.json", L"1-10", L"fm24-no-such-folder/results.csv"},
			EXIT_FAILED,
			NULL},
		{commands_run,
			{L"fm24ctl", L"run", L"next.json", L"--profile", L"fm24-no-such-folder/samples.txt"},
			EXIT_FAILED,
			NULL},
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"fm24-no-such-folder/next.json"},
			EXIT_FAILED,
			NULL},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-away", L"next.json", L"677", L"fm24-no-such-folder/out.json"},
			EXIT_FAILED,
			NULL},
		{commands_listTeams,
			{L"fm24ctl", L"list-teams", L"fm24-no-such-folder/teams.txt"},
			EXIT_FAILED,
			NULL},
		{commands_listPlayers,
			{L"fm24ctl", L"list-players", L"fm24-no-such-folder/players.txt"},
			EXIT_FAILED,
			NULL},
		{commands_getPlayer,
			{L"fm24ctl", L"get-player", L"7", L"fm24-no-such-folder/p.json"},
			EXIT_FAILED,
			NULL},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"fm24-no-such-folder/out.json"},
			EXIT_FAILED,
			NULL},
		{commands_decode,
			{L"fm24ctl", L"decode", L"next.bin", L"fm24-no-such-folder/next.json"},
			EXIT_FAILED,
			NULL},
		{commands_lineup,
			{L"fm24ctl",
				L"lineup",
				L"next.json",
				L"home",
				L"player[10]",
				L"player[19]",
				L"fm24-no-such-folder/out.json"},
			EXIT_FAILED,
			NULL},
	};
	folderChecked = true;
	for (size_t i = 0; i < sizeof missingFolder / sizeof missingFolder[0]; ++i) {
		configReads = 0;
		checkArguments(&missingFolder[i], 1, false);
#ifndef _WIN32
		ASSERT_INT_EQ((int)ensures, 0);
#endif
		// lineup reads its input only once its output can be written.
		ASSERT_BOOL_EQ(missingFolder[i].command != commands_lineup || configReads == 0, true);
	}
	folderChecked = false;
}

#ifndef _WIN32
// A fixture exported from the save LIV_MAIN_FM24 at 2024 day 123 15:00.
static const char savedConfig[] =
	"{\"format\":\"fm24-match-config/1\",\"source\":\"A vs B, 2024 day 130 15:00, seed 1\","
	"\"save\":\"LIV_MAIN_FM24\",\"game_date\":\"2024 day 123 15:00\",\"home_team\":678,"
	"\"fields\":[{\"u8\":42}]}";

// As invoke, writing what the command printed to stderr to `out`.
static int invokeCapturingErrors(Command *command, wchar_t **arguments, char *out, size_t size) {
	char printed[] = "/tmp/fm24-runner-errors-XXXXXX";
	int file = mkstemp(printed);
	int saved = dup(STDERR_FILENO);
	int status = -1;
	out[0] = 0;
	if (file >= 0 && saved >= 0 && dup2(file, STDERR_FILENO) >= 0) {
		status = invoke(command, arguments);
		(void)dup2(saved, STDERR_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	if (file >= 0) {
		ssize_t length = pread(file, out, size - 1, 0);
		out[length > 0 ? length : 0] = 0;
		close(file);
		(void)unlink(printed);
	}
	return status;
}

// Each refused seed pattern's reason comes first, then the usage line.
TEST(explains_refused_seeds) {
	static const struct {
		const wchar_t *seeds;
		const char *reason;
	} cases[] = {
		{L"10-1", "the seed range 10-1 runs backwards: write 1-10\n"},
		{L"1,5-1", "the seed range 5-1 runs backwards: write 1-5\n"},
		{L"1,,5", "a seed is missing next to a comma in 1,,5\n"},
		{L"1,", "a seed is missing next to a comma in 1,\n"},
		{L",1", "a seed is missing next to a comma in ,1\n"},
		{L"1-2-3", "1-2-3 is not a seed or a range of seeds (first-last)\n"},
		{L"-5", "-5 is not a seed or a range of seeds (first-last)\n"},
		{L"1,2-", "2- is not a seed or a range of seeds (first-last)\n"},
		{L"0-10000001", "the seed range 0-10000001 has more than 10000001 seeds: split it\n"},
		{L"4294967296", "seeds must be numbers from 0 to 4294967295, not 4294967296\n"},
		{L"4294967290-4294967296",
			"seeds must be numbers from 0 to 4294967295, not 4294967290-4294967296\n"},
		{L"99999999999999999999999",
			"seeds must be numbers from 0 to 4294967295, not 99999999999999999999999\n"},
	};
	char errors[2048];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", (wchar_t *)cases[i].seeds, NULL};
		ASSERT_INT_EQ(invokeCapturingErrors(commands_run, run, errors, sizeof errors), EXIT_USAGE);
		ASSERT_INT_EQ(strncmp(errors, cases[i].reason, strlen(cases[i].reason)), 0);
		ASSERT_BOOL_EQ(strstr(errors, "\nusage: fm24ctl run ") != NULL, true);
	}
	wchar_t longest[SEED_PATTERN_LENGTH + 2];
	wmemset(longest, L'1', SEED_PATTERN_LENGTH + 1);
	longest[SEED_PATTERN_LENGTH + 1] = 0;
	wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", longest, NULL};
	ASSERT_INT_EQ(invokeCapturingErrors(commands_run, run, errors, sizeof errors), EXIT_USAGE);
	static const char tooLong[] = "the seed pattern must fit in 255 characters (this one has 256)\n";
	ASSERT_INT_EQ(strncmp(errors, tooLong, sizeof tooLong - 1), 0);
}

// A folder access list that lets this user add files to it and search it, but not add folders.
static bool allowFilesOnly(const char *path) {
	uuid_t user;
	acl_t list = acl_init(1);
	acl_entry_t entry = NULL;
	acl_permset_t permissions = NULL;
	bool ok =
		list && mbr_uid_to_uuid(getuid(), user) == 0 && acl_create_entry(&list, &entry) == 0 &&
		acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 && acl_set_qualifier(entry, user) == 0 &&
		acl_get_permset(entry, &permissions) == 0 && acl_add_perm(permissions, ACL_ADD_FILE) == 0 &&
		acl_add_perm(permissions, ACL_SEARCH) == 0 && acl_set_permset(entry, permissions) == 0 &&
		acl_set_file(path, ACL_TYPE_EXTENDED, list) == 0;
	if (list) {
		acl_free(list);
	}
	return ok;
}

// An output that is a folder, or whose folder is missing, is a file, cannot be opened, written in
// or searched, or is locked, and an existing output the user may not write or that is locked (or
// append-only): why, before the save is checked or a job submitted, whether the game writes it or
// fm24ctl opens it too. One the user may write goes on to the job, also one that exists in a
// folder they may not add files to or that is locked, or a new one in a folder whose access list
// lets them add files but not folders, or that is append-only.
TEST(explains_output_folders_it_cannot_write_in) {
	char directory[] = "/tmp/fm24-outputs-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char path[1100];
	static const char *const folders[] = {"locked",
		"locked/sub",
		"readonly",
		"nosearch",
		"listed",
		"frozen",
		"appending",
		"open",
		"open/folder.csv"};
	static const char *const files[] = {"afile",
		"readonly/results.csv",
		"frozen/results.csv",
		"open/readonly.csv",
		"open/frozen.csv",
		"open/appending.csv",
		"open/stats-teams.csv",
		"open/next.bin"};
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(mkdir(path, 0700), 0);
	}
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, files[i]);
		FILE *file = fopen(path, "wb");
		ASSERT_BOOL_EQ(file != NULL && fclose(file) == 0, true);
	}
	static const struct {
		const char *path;
		mode_t mode;
		unsigned flags;
	} setups[] = {
		{"locked", 0, 0},
		{"readonly", 0555, 0},
		{"nosearch", 0600, 0},
		{"listed", 0555, 0},
		{"frozen", 0700, UF_IMMUTABLE},
		{"appending", 0700, UF_APPEND},
		{"open/readonly.csv", 0444, 0},
		{"open/frozen.csv", 0600, UF_IMMUTABLE},
		{"open/appending.csv", 0600, UF_APPEND},
		{"open/stats-teams.csv", 0444, 0},
	};
	for (size_t i = 0; i < sizeof setups / sizeof setups[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, setups[i].path);
		ASSERT_INT_EQ(chmod(path, setups[i].mode), 0);
		ASSERT_INT_EQ(chflags(path, setups[i].flags), 0);
	}
	(void)snprintf(path, sizeof path, "%s/listed", directory);
	ASSERT_BOOL_EQ(allowFilesOnly(path), true);
	static const char isFolder[] = "it is a folder (name a file in it)\n";
	static const char missing[] = "its folder does not exist (create it first)\n";
	static const char aFile[] = "its folder, or one above it, is a file\n";
	static const char denied[] =
		"you have no permission to open its folder, or one above it (choose another folder, or "
		"change the permissions)\n";
	static const char notWritable[] =
		"you have no permission to write in its folder (choose another folder, or change its "
		"permissions)\n";
	static const char folderLocked[] =
		"its folder is locked (choose another folder, or unlock it with Get Info in Finder)\n";
	static const char fileNotWritable[] =
		"you have no permission to write it (choose another file, or change its permissions)\n";
	static const char fileLocked[] =
		"it is locked (choose another file, or unlock it with Get Info in Finder)\n";
	bool root = geteuid() == 0; // root may open and write in any folder, but not a locked one
	const struct {
		const char *output;
		const char *problem; // NULL: the job goes ahead
	} cases[] = {
		{"open/folder.csv", isFolder},
		{"open", isFolder},
		{"missing/next.csv", missing},
		{"afile/next.csv", aFile},
		{"afile/sub/next.csv", aFile},
		{"locked/sub/next.csv", root ? NULL : denied},
		{"locked/next.csv", root ? NULL : notWritable},
		{"readonly/next.csv", root ? NULL : notWritable},
		{"nosearch/next.csv", root ? NULL : notWritable},
		{"frozen/next.csv", folderLocked},
		{"open/readonly.csv", root ? NULL : fileNotWritable},
		{"open/frozen.csv", fileLocked},
		{"open/appending.csv", fileLocked},
		{"readonly/results.csv", NULL},
		{"frozen/results.csv", NULL},
		{"listed/next.csv", NULL},
		{"appending/next.csv", NULL},
		{"open/next.csv", NULL},
	};
	char errors[2048];
	char expected[2048];
	wchar_t output[1100];
	folderChecked = true;
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, cases[i].output);
		ASSERT_BOOL_EQ(platform_fromUtf8(path, output, 1100), true);
		// The game writes get-next-fixture's output; fm24ctl reads back run's results.
		wchar_t *next[] = {L"fm24ctl", L"get-next-fixture", output, NULL};
		wchar_t *run[] = {L"fm24ctl", L"run", L"next.json", L"1-10", output, NULL};
		for (int opened = 0; opened < 2; ++opened) {
			expectedTimeout = opened ? CLIENT_LONG_TIMEOUT_MS : CLIENT_SHORT_TIMEOUT_MS;
			Command *command = opened ? commands_run : commands_getNextFixture;
			int status = invokeCapturingErrors(command, opened ? run : next, errors, sizeof errors);
			if (!cases[i].problem) {
				const char *job = opened ? "command=run\n" : "command=build_next\n";
				ASSERT_BOOL_EQ(strstr(submittedJob, job) != NULL, true);
				ASSERT_BOOL_EQ(opened || (status == EXIT_OK && !errors[0]), true);
				continue;
			}
			ASSERT_INT_EQ(status, EXIT_FAILED);
			(void)snprintf(expected, sizeof expected, "cannot write %s: %s", path, cases[i].problem);
			ASSERT_INT_EQ(strcmp(errors, expected), 0);
			ASSERT_BOOL_EQ(!submittedJob[0] && ensures == 0, true);
		}
	}
	// run's default results, beside its input, are checked as given ones are, once the input has
	// been read (a missing .bin says so instead); and both detailed reports beside the results,
	// which may be new files. A JSON input's encoded copy is removed either way.
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	const struct {
		const char *input;
		const char *output;  // NULL: the default
		const char *refused; // NULL: the job goes ahead
		const char *problem; // NULL: the input cannot be read
	} runs[] = {
		{"readonly/next.json", NULL, root ? NULL : "readonly/next.csv", notWritable},
		{"open/next.json", NULL, NULL, NULL},
		{"missing/next.bin", NULL, "missing/next.bin", NULL},
		{"open/next.bin", NULL, NULL, NULL},
		{"next.json",
			"readonly/results.csv",
			root ? NULL : "readonly/results-players.csv",
			notWritable},
		{"next.json", "open/stats.csv", root ? NULL : "open/stats-teams.csv", fileNotWritable},
		{"next.json", "open/results.csv", NULL, NULL},
	};
	for (size_t i = 0; i < sizeof runs / sizeof runs[0]; ++i) {
		unsigned removed = removedJobInputs;
		bool json = strstr(runs[i].input, ".json") != NULL;
		wchar_t input[1100];
		(void)snprintf(path, sizeof path, "%s/%s", directory, runs[i].input);
		ASSERT_BOOL_EQ(platform_fromUtf8(path, input, 1100), true);
		(void)snprintf(path, sizeof path, "%s/%s", directory, runs[i].output ? runs[i].output : "");
		ASSERT_BOOL_EQ(platform_fromUtf8(path, output, 1100), true);
		wchar_t *run[] = {L"fm24ctl", L"run", input, L"1-10", L"--detailed", NULL, NULL};
		if (runs[i].output) {
			run[5] = output;
		}
		int status = invokeCapturingErrors(commands_run, run, errors, sizeof errors);
		ASSERT_INT_EQ((int)(removedJobInputs - removed), json ? 1 : 0);
		if (!runs[i].refused) {
			ASSERT_BOOL_EQ(strncmp(submittedJob, "command=run\n", 12) == 0, true);
			continue;
		}
		ASSERT_INT_EQ(status, EXIT_FAILED);
		if (runs[i].problem) {
			(void)snprintf(
				expected,
				sizeof expected,
				"cannot write %s/%s: %s",
				directory,
				runs[i].refused,
				runs[i].problem
			);
		} else {
			(void)snprintf(expected, sizeof expected, "cannot read %s/%s\n", directory, runs[i].refused);
		}
		ASSERT_INT_EQ(strcmp(errors, expected), 0);
		ASSERT_BOOL_EQ(!submittedJob[0] && ensures == 0, true);
	}
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	folderChecked = false;
	buffer_free(&writtenConfig);
	for (size_t i = 0; i < sizeof setups / sizeof setups[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, setups[i].path);
		ASSERT_INT_EQ(chflags(path, 0), 0);
		ASSERT_INT_EQ(chmod(path, 0700), 0);
	}
	for (size_t i = sizeof files / sizeof files[0]; i-- > 0;) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, files[i]);
		ASSERT_INT_EQ(unlink(path), 0);
	}
	for (size_t i = sizeof folders / sizeof folders[0]; i-- > 0;) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(rmdir(path), 0);
	}
	ASSERT_INT_EQ(rmdir(directory), 0);
}

// The files run writes beside its results are checked once the input has been read, before the
// save is checked or a job submitted: --per-match's rows and --keep-going's failed seeds as the
// game writes them, and --manifest's as fm24ctl does, as a new file renamed over any manifest
// there, which an append-only folder refuses. A refused one stops run without a manifest; a JSON
// input's encoded copy is removed.
TEST(explains_outputs_beside_the_results_it_cannot_write) {
	char directory[] = "/tmp/fm24-siblings-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char path[1100];
	static const char *const folders[] = {"open",
		"readonly",
		"frozen",
		"appending",
		"open/players-match-players.csv",
		"open/teams-match-teams.csv",
		"open/failed-failed.csv",
		"open/folder-manifest.json"};
	static const char *const files[] = {"readonly/results.csv",
		"readonly/results-manifest.json",
		"frozen/results.csv",
		"frozen/results-manifest.json",
		"open/kept-manifest.json",
		"open/locked-manifest.json"};
	for (size_t i = 0; i < sizeof folders / sizeof folders[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(mkdir(path, 0700), 0);
	}
	for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, files[i]);
		FILE *file = fopen(path, "wb");
		ASSERT_BOOL_EQ(file != NULL && fclose(file) == 0, true);
	}
	static const struct {
		const char *path;
		mode_t mode;
		unsigned flags;
	} setups[] = {
		{"readonly", 0555, 0},
		{"frozen", 0700, UF_IMMUTABLE},
		{"appending", 0700, UF_APPEND},
		{"open/kept-manifest.json", 0444, 0},
		{"open/locked-manifest.json", 0600, UF_IMMUTABLE},
	};
	for (size_t i = 0; i < sizeof setups / sizeof setups[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, setups[i].path);
		ASSERT_INT_EQ(chmod(path, setups[i].mode), 0);
		ASSERT_INT_EQ(chflags(path, setups[i].flags), 0);
	}
	static const char isFolder[] = "it is a folder (name a file in it)\n";
	static const char notWritable[] =
		"you have no permission to write in its folder (choose another folder, or change its "
		"permissions)\n";
	static const char folderLocked[] =
		"its folder is locked (choose another folder, or unlock it with Get Info in Finder)\n";
	static const char fileLocked[] =
		"it is locked (choose another file, or unlock it with Get Info in Finder)\n";
	bool root = geteuid() == 0; // root may write in any folder, but not a locked one
	const struct {
		const char *output;
		const wchar_t *flag;
		const char *refused; // NULL: the job goes ahead
		const char *problem;
	} cases[] = {
		{"open/players.csv", L"--per-match", "open/players-match-players.csv", isFolder},
		{"open/teams.csv", L"--per-match", "open/teams-match-teams.csv", isFolder},
		{"open/failed.csv", L"--keep-going", "open/failed-failed.csv", isFolder},
		{"open/folder.csv", L"--manifest", "open/folder-manifest.json", isFolder},
		{"open/locked.csv", L"--manifest", "open/locked-manifest.json", fileLocked},
		// Existing results go ahead in a folder you may not add files to, or that is locked; the
		// new files beside them do not, nor a manifest, which is replaced by a new file.
		{"readonly/results.csv",
			L"--per-match",
			root ? NULL : "readonly/results-match-players.csv",
			notWritable},
		{"readonly/results.csv",
			L"--keep-going",
			root ? NULL : "readonly/results-failed.csv",
			notWritable},
		{"readonly/results.csv",
			L"--manifest",
			root ? NULL : "readonly/results-manifest.json",
			notWritable},
		{"frozen/results.csv", L"--manifest", "frozen/results-manifest.json", folderLocked},
		{"frozen/results.csv", L"--detailed", "frozen/results-players.csv", folderLocked},
		// An append-only folder takes new files, but not the manifest's rename.
		{"appending/results.csv", L"--manifest", "appending/results-manifest.json", folderLocked},
		{"appending/results.csv", L"--per-match", NULL, NULL},
		// A manifest you may not write is replaced all the same.
		{"open/kept.csv", L"--manifest", NULL, NULL},
		{"open/results.csv", L"--per-match", NULL, NULL},
		{"open/results.csv", L"--keep-going", NULL, NULL},
		{"open/results.csv", L"--manifest", NULL, NULL},
	};
	char errors[2048];
	char expected[2048];
	wchar_t output[1100];
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	folderChecked = true;
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, cases[i].output);
		ASSERT_BOOL_EQ(platform_fromUtf8(path, output, 1100), true);
		wchar_t *run[] = {
			L"fm24ctl", L"run", L"next.json", L"1-10", output, (wchar_t *)cases[i].flag, NULL};
		unsigned removed = removedJobInputs;
		int status = invokeCapturingErrors(commands_run, run, errors, sizeof errors);
		ASSERT_INT_EQ((int)(removedJobInputs - removed), 1);
		if (!cases[i].refused) {
			ASSERT_BOOL_EQ(strncmp(submittedJob, "command=run\n", 12) == 0, true);
			ASSERT_BOOL_EQ(strstr(errors, "manifest") == NULL, true);
			continue;
		}
		ASSERT_INT_EQ(status, EXIT_FAILED);
		(void)snprintf(
			expected,
			sizeof expected,
			"cannot write %s/%s: %s",
			directory,
			cases[i].refused,
			cases[i].problem
		);
		ASSERT_INT_EQ(strcmp(errors, expected), 0);
		ASSERT_BOOL_EQ(!submittedJob[0] && ensures == 0, true);
	}
	// The manifests of the runs that went ahead were written; none was left half written.
	static const char *const written[] = {"open/kept-manifest.json", "open/results-manifest.json"};
	for (size_t i = 0; i < sizeof written / sizeof written[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, written[i]);
		FILE *file = fopen(path, "rb");
		ASSERT_BOOL_EQ(file != NULL && fgetc(file) == '{', true);
		if (file) {
			(void)fclose(file);
		}
	}
	folderChecked = false;
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	buffer_free(&writtenConfig);
	for (size_t i = 0; i < sizeof setups / sizeof setups[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, setups[i].path);
		ASSERT_INT_EQ(chflags(path, 0), 0);
		ASSERT_INT_EQ(chmod(path, 0700), 0);
	}
	(void)snprintf(path, sizeof path, "%s/open/results-manifest.json", directory);
	ASSERT_INT_EQ(unlink(path), 0);
	for (size_t i = sizeof files / sizeof files[0]; i-- > 0;) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, files[i]);
		ASSERT_INT_EQ(unlink(path), 0);
	}
	for (size_t i = sizeof folders / sizeof folders[0]; i-- > 0;) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(rmdir(path), 0);
	}
	ASSERT_INT_EQ(rmdir(directory), 0);
}

// macOS's privacy protection keeps out the app fm24ctl runs in, not the game: a folder it closes to
// fm24ctl (EPERM) stops only run's results, given or by default, which fm24ctl reads back, and the
// outputs of inject-player and lineup, which it writes. The outputs only the game writes go on to
// the job.
TEST(leaves_folders_closed_to_fm24ctl_alone_for_outputs_the_game_writes) {
	static const char privacy[] =
		"macOS does not let fm24ctl into its folder (operation not permitted): choose another folder "
		"or, for a folder macOS protects, allow the app fm24ctl runs in (such as Terminal) under "
		"System Settings > Privacy & Security\n";
	static const ArgumentCase gameWrites[] = {
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"fm24-closed-folder/next.json"},
			EXIT_OK,
			"command=build_next\n"},
		{commands_replaceTeam,
			{L"fm24ctl", L"replace-away", L"next.json", L"677", L"fm24-closed-folder/out.json"},
			EXIT_OK,
			"command=replace_away\n"},
		{commands_listTeams,
			{L"fm24ctl", L"list-teams", L"fm24-closed-folder/teams.txt"},
			EXIT_OK,
			"command=list_teams\n"},
		{commands_listPlayers,
			{L"fm24ctl", L"list-players", L"fm24-closed-folder/players.txt"},
			EXIT_OK,
			"command=list_players\n"},
		{commands_getPlayer,
			{L"fm24ctl", L"get-player", L"7", L"fm24-closed-folder/p.json"},
			EXIT_OK,
			"command=get_player\n"},
		{commands_decode,
			{L"fm24ctl", L"decode", L"next.bin", L"fm24-closed-folder/next.json"},
			EXIT_OK,
			"command=inspect\n"},
	};
	static const struct {
		Command *command;
		wchar_t *arguments[8];
		const char *output;
	} fm24ctlOpens[] = {
		{commands_run,
			{L"fm24ctl", L"run", L"next.json", L"1-10", L"fm24-closed-folder/results.csv"},
			"fm24-closed-folder/results.csv"},
		{commands_run,
			{L"fm24ctl", L"run", L"fm24-closed-folder/next.json", L"1-10"},
			"fm24-closed-folder/next.csv"},
		{commands_injectPlayer,
			{L"fm24ctl",
				L"inject-player",
				L"next.json",
				L"home.player[0]",
				L"2000040347",
				L"fm24-closed-folder/out.json"},
			"fm24-closed-folder/out.json"},
		{commands_lineup,
			{L"fm24ctl",
				L"lineup",
				L"next.json",
				L"home",
				L"player[10]",
				L"player[19]",
				L"fm24-closed-folder/out.json"},
			"fm24-closed-folder/out.json"},
	};
	static const ArgumentCase profile = {
		commands_run,
		{L"fm24ctl", L"run", L"next.json", L"--profile", L"fm24-closed-folder/samples.txt"},
		ANY_RESULT,
		"\nprofile=",
	};
	foldersClosed = true;
	checkArguments(gameWrites, sizeof gameWrites / sizeof gameWrites[0], false);
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS; // run's
	checkArguments(&profile, 1, false);
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	char errors[2048];
	char expected[2048];
	wchar_t wide[1100];
	Path output;
	for (size_t i = 0; i < sizeof fm24ctlOpens / sizeof fm24ctlOpens[0]; ++i) {
		ASSERT_BOOL_EQ(platform_fromUtf8(fm24ctlOpens[i].output, wide, 1100), true);
		ASSERT_BOOL_EQ(absolutePath(wide, &output), true);
		int status = invokeCapturingErrors(
			fm24ctlOpens[i].command, (wchar_t **)fm24ctlOpens[i].arguments, errors, sizeof errors
		);
		ASSERT_INT_EQ(status, EXIT_FAILED);
		(void)snprintf(expected, sizeof expected, "cannot write %s: %s", output.utf8, privacy);
		ASSERT_INT_EQ(strcmp(errors, expected), 0);
		ASSERT_BOOL_EQ(!submittedJob[0] && ensures == 0, true);
	}
	foldersClosed = false;
}

TEST(checks_the_fixtures_save_before_its_job) {
	configText = savedConfig;
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	// run checks the fixture's save first, and lets the check wait as long as the run would.
	wchar_t *run[] = {
		L"fm24ctl", L"run", L"next.json", L"1-10", L"fm24-save-check-test.csv", NULL, NULL, NULL
	};
	(void)invoke(commands_run, run);
	ASSERT_BOOL_EQ(ensures == 1 && strcmp(ensured.save, "LIV_MAIN_FM24") == 0, true);
	ASSERT_BOOL_EQ(ensured.launch && !ensured.anySave, true);
	ASSERT_BOOL_EQ(ensured.timeoutMs == CLIENT_LONG_TIMEOUT_MS, true);
	ASSERT_BOOL_EQ(strncmp(submittedJob, "command=run\n", 12) == 0, true);
	run[5] = L"--no-launch";
	run[6] = L"--any-save";
	(void)invoke(commands_run, run);
	ASSERT_BOOL_EQ(ensures == 1 && !ensured.launch && ensured.anySave, true);
	// A game in the wrong state, or a failed check, stops the job with the check's exit code.
	static const int stops[] = {EXIT_WRONG_STATE, EXIT_FAILED};
	for (size_t i = 0; i < sizeof stops / sizeof stops[0]; ++i) {
		ensureResult = stops[i];
		ASSERT_INT_EQ(invoke(commands_run, run), stops[i]);
		ASSERT_BOOL_EQ(ensures == 1 && !submittedJob[0], true);
	}
	ensureResult = EXIT_OK;
	// The save at another date than the export: a warning, and the run goes ahead.
	char errors[1024];
	ensureDate = "2024 day 130 09:00";
	(void)invokeCapturingErrors(commands_run, run, errors, sizeof errors);
	ASSERT_BOOL_EQ(
		strstr(
			errors,
			"warning: the save is at 2024 day 130 09:00, the fixture was exported at 2024 day 123 15:00"
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(strncmp(submittedJob, "command=run\n", 12) == 0, true);
	static const char *const sameOrUnknown[] = {"2024 day 123 15:00", ""};
	for (size_t i = 0; i < sizeof sameOrUnknown / sizeof sameOrUnknown[0]; ++i) {
		ensureDate = sameOrUnknown[i];
		(void)invokeCapturingErrors(commands_run, run, errors, sizeof errors);
		ASSERT_BOOL_EQ(strstr(errors, "warning: the save is at") == NULL, true);
	}
	// A fixture without a save, and a .bin, are played as before.
	run[5] = NULL;
	configText = plainConfig;
	(void)invoke(commands_run, run);
	ASSERT_BOOL_EQ(ensures == 0 && strncmp(submittedJob, "command=run\n", 12) == 0, true);
	configText = savedConfig;
	char bin[] = "/tmp/fm24-runner-input-XXXXXX.bin"; // fm24ctl checks that a .bin is there
	int made = mkstemps(bin, 4);
	ASSERT_BOOL_EQ(made >= 0 && close(made) == 0, true);
	wchar_t binary[64];
	ASSERT_BOOL_EQ(platform_fromUtf8(bin, binary, 64), true);
	run[2] = binary;
	(void)invoke(commands_run, run);
	ASSERT_BOOL_EQ(ensures == 0 && strncmp(submittedJob, "command=run\n", 12) == 0, true);
	ASSERT_INT_EQ(unlink(bin), 0);
	run[2] = L"next.json";
	// Only the last part of a path counts; a "save" that is no save's name stops the run first.
	configText = "{\"format\":\"fm24-match-config/1\",\"save\":\"/x/cloud/games/LIV (v02)\","
							 "\"fields\":[]}";
	(void)invoke(commands_run, run);
	ASSERT_BOOL_EQ(ensures == 1 && strcmp(ensured.save, "LIV (v02)") == 0, true);
	static const char *const badSaves[] = {
		"{\"format\":\"fm24-match-config/1\",\"save\":\"\",\"fields\":[]}",
		"{\"format\":\"fm24-match-config/1\",\"save\":\"x/\",\"fields\":[]}",
		"{\"format\":\"fm24-match-config/1\",\"save\":7,\"fields\":[]}",
		"{\"format\":\"fm24-match-config/1\",\"save\":\"a\\u001b[2J\",\"fields\":[]}",
	};
	for (size_t i = 0; i < sizeof badSaves / sizeof badSaves[0]; ++i) {
		configText = badSaves[i];
		ASSERT_INT_EQ(invoke(commands_run, run), EXIT_FAILED);
		ASSERT_BOOL_EQ(ensures == 0 && !submittedJob[0], true);
	}
	configText = savedConfig;
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	// replace-home and replace-away check the fixture's save before the rebuild.
	wchar_t *replace[] = {
		L"fm24ctl", L"replace-away", L"next.json", L"677", L"out.json", L"--any-save", L"--full", NULL
	};
	ASSERT_INT_EQ(invoke(commands_replaceTeam, replace), EXIT_OK);
	ASSERT_BOOL_EQ(ensures == 1 && strcmp(ensured.save, "LIV_MAIN_FM24") == 0, true);
	ASSERT_BOOL_EQ(ensured.anySave && ensured.launch && ensured.timeoutMs == CLIENT_SHORT_TIMEOUT_MS, true);
	ASSERT_BOOL_EQ(strstr(submittedJob, "command=replace_away\n") != NULL, true);
	ensureResult = EXIT_WRONG_STATE;
	ASSERT_INT_EQ(invoke(commands_replaceTeam, replace), EXIT_WRONG_STATE);
	ASSERT_BOOL_EQ(!submittedJob[0], true);
	ensureResult = EXIT_OK;
	// inject-player checks before it fetches a player by ID, and not for a player file.
	wchar_t *inject[] = {
		L"fm24ctl", L"inject-player", L"next.json", L"home.player[0]", L"2000040347", L"out.json",
		L"--no-launch", NULL
	};
	(void)invoke(commands_injectPlayer, inject);
	ASSERT_BOOL_EQ(ensures == 1 && !ensured.launch, true);
	ASSERT_BOOL_EQ(strncmp(submittedJob, "command=get_player\n", 19) == 0, true);
	ensureResult = EXIT_WRONG_STATE;
	ASSERT_INT_EQ(invoke(commands_injectPlayer, inject), EXIT_WRONG_STATE);
	ASSERT_BOOL_EQ(!submittedJob[0], true);
	ensureResult = EXIT_OK;
	inject[4] = L"player.json";
	(void)invoke(commands_injectPlayer, inject);
	ASSERT_BOOL_EQ(ensures == 0 && !submittedJob[0], true);
	// The commands without a fixture check --save's save, and nothing without it.
	static const struct {
		Command *command;
		wchar_t *arguments[8];
		int fixed; // the arguments before the options
	} named[] = {
		{commands_getNextFixture,
			{L"fm24ctl", L"get-next-fixture", L"next.json", L"--save", L"LIV (v02)", L"--seed", L"42"},
			3},
		{commands_listTeams, {L"fm24ctl", L"list-teams", L"teams.txt", L"--save", L"LIV (v02)"}, 3},
		{commands_listPlayers,
			{L"fm24ctl", L"list-players", L"players.txt", L"--no-launch", L"--save", L"LIV (v02)"},
			3},
		{commands_getPlayer, {L"fm24ctl", L"get-player", L"7", L"p.json", L"--save", L"LIV (v02)"}, 4},
	};
	for (size_t i = 0; i < sizeof named / sizeof named[0]; ++i) {
		ASSERT_INT_EQ(invoke(named[i].command, (wchar_t **)named[i].arguments), EXIT_OK);
		ASSERT_BOOL_EQ(ensures == 1 && strcmp(ensured.save, "LIV (v02)") == 0 && submittedJob[0], true);
		ASSERT_BOOL_EQ(ensured.timeoutMs == CLIENT_SHORT_TIMEOUT_MS, true);
		ensureResult = EXIT_WRONG_STATE;
		ASSERT_INT_EQ(invoke(named[i].command, (wchar_t **)named[i].arguments), EXIT_WRONG_STATE);
		ASSERT_BOOL_EQ(!submittedJob[0], true);
		ensureResult = EXIT_OK;
		wchar_t *fixed[8] = {NULL};
		memcpy(fixed, named[i].arguments, (size_t)named[i].fixed * sizeof fixed[0]);
		ASSERT_INT_EQ(invoke(named[i].command, fixed), EXIT_OK);
		ASSERT_BOOL_EQ(ensures == 0 && submittedJob[0], true);
	}
	// decode never starts or checks the game.
	wchar_t *decode[] = {L"fm24ctl", L"decode", L"next.bin", L"next.json", NULL};
	ASSERT_INT_EQ(invoke(commands_decode, decode), EXIT_OK);
	ASSERT_BOOL_EQ(ensures == 0 && submittedJob[0], true);
	ensureDate = "";
	configText = plainConfig;
	buffer_free(&writtenConfig);
}
#endif

// Absolute paths, which the commands pass on as they are.
#ifdef _WIN32
#define TEST_FOLDER "C:\\fm24\\"
#define TEST_WIDE_FOLDER L"C:\\fm24\\"
#else
#define TEST_FOLDER "/fm24/"
#define TEST_WIDE_FOLDER L"/fm24/"
#endif

// A .bin input that is there, as run checks (see binaryFound): a new, empty file in the temporary
// directory, for platform_removeFile to remove.
static bool makeBinaryInput(Path *input) {
#ifdef _WIN32
	wchar_t directory[MAX_PATH + 1];
	DWORD length = GetTempPathW(MAX_PATH + 1, directory);
	if (!length || length > MAX_PATH) {
		return false;
	}
	int written = swprintf(
		input->wide,
		HARNESS_PATH_LENGTH,
		L"%lsfm24-runner-input-%lu.bin",
		directory,
		platform_processId()
	);
	FILE *file =
		written > 0 && written < HARNESS_PATH_LENGTH ? platform_openFile(input->wide, "wb") : NULL;
#else
	char made[] = "/tmp/fm24-runner-input-XXXXXX.bin";
	int descriptor = mkstemps(made, 4);
	if (descriptor < 0) {
		return false;
	}
	FILE *file = fdopen(descriptor, "wb");
	if (!file) {
		(void)close(descriptor);
	}
	if (!platform_fromUtf8(made, input->wide, HARNESS_PATH_LENGTH)) {
		if (file) {
			(void)fclose(file);
		}
		(void)unlink(made);
		return false;
	}
#endif
	return file && fclose(file) == 0 && paths_toUtf8(input->wide, input->utf8, sizeof input->utf8);
}

TEST(submits_the_run_job_as_it_always_has) {
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	configText = plainConfig;
	Path bin;
	ASSERT_BOOL_EQ(makeBinaryInput(&bin), true);
	wchar_t *all[] = {L"fm24ctl",
		L"run",
		bin.wide,
		L"1-10,15",
		TEST_WIDE_FOLDER L"out.csv",
		L"--threads",
		L"3",
		L"--profile",
		TEST_WIDE_FOLDER L"samples.txt",
		L"--detailed",
		L"--human-control",
		L"--keep-build",
		NULL};
	submittedJob[0] = 0;
	(void)commands_runWithProgress(12, all, (uintptr_t)0x123456789U);
	char expected[512 + 2 * UTF8_PATH_LENGTH];
	(void)snprintf(
		expected,
		sizeof expected,
		"command=run\ninput=%s\nseeds=1-10,15\noutput=" TEST_FOLDER "out.csv\nthreads=3\n"
		"profile=" TEST_FOLDER "samples.txt\ndetailed=1\nhuman_control=1\nkeep_build=1\n"
		"home_team=\naway_team=\nprogress_window=4886718345\n",
		bin.utf8
	);
	ASSERT_INT_EQ(strcmp(submittedJob, expected), 0);
	// The defaults: the input's own seed, its .csv, and as many threads as processors less one.
	wchar_t *plain[] = {L"fm24ctl", L"run", bin.wide, NULL};
	size_t stem = strlen(bin.utf8) - strlen(".bin");
	(void)snprintf(
		expected,
		sizeof expected,
		"command=run\ninput=%s\nseeds=keep\noutput=%.*s.csv\nthreads=%lu\nprofile=\ndetailed=0\n"
		"human_control=0\nkeep_build=0\nhome_team=\naway_team=\nprogress_window=0\n",
		bin.utf8,
		(int)stem,
		bin.utf8,
		commands_defaultThreads()
	);
	submittedJob[0] = 0;
	(void)commands_run(3, plain);
	ASSERT_INT_EQ(strcmp(submittedJob, expected), 0);
	ASSERT_BOOL_EQ(platform_removeFile(bin.wide), true);
	// A JSON input is played as an encoded copy, with its team overrides.
	plain[2] = TEST_WIDE_FOLDER L"next.json";
	submittedJob[0] = 0;
	(void)commands_run(3, plain);
	ASSERT_BOOL_EQ(strstr(submittedJob, "_job_input-") != NULL, true);
	ASSERT_BOOL_EQ(
		strstr(submittedJob, ".bin\nseeds=keep\noutput=" TEST_FOLDER "next.csv\n") != NULL, true
	);
	ASSERT_BOOL_EQ(
		strstr(submittedJob, "\nhome_team=678\naway_team=677\nprogress_window=0\n") != NULL, true
	);
	buffer_free(&writtenConfig);
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
}

TEST(formats_run_jobs_and_refuses_ones_too_long) {
	RunJob settings = {
		.input = "/fm24/next.bin",
		.seeds = "1-10",
		.output = "/fm24/out.csv",
		.threads = 7,
		.detailed = true,
		.keepBuild = true,
		.teams = {0, 677},
		.progressWindow = UINT64_MAX,
	};
	static char job[HARNESS_JOB_LENGTH + 16];
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	ASSERT_INT_EQ(
		strcmp(
			job,
			"command=run\ninput=/fm24/next.bin\nseeds=1-10\noutput=/fm24/out.csv\nthreads=7\n"
			"profile=\ndetailed=1\nhuman_control=0\nkeep_build=1\nhome_team=\naway_team=677\n"
			"progress_window=18446744073709551615\n"
		),
		0
	);
	// The longest job a harness reads, with its NUL, is HARNESS_JOB_LENGTH bytes.
	static char seeds[HARNESS_JOB_LENGTH];
	settings.seeds = "";
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	size_t fixed = strlen(job);
	memset(seeds, '1', HARNESS_JOB_LENGTH - 1 - fixed);
	settings.seeds = seeds;
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	ASSERT_BOOL_EQ(strlen(job) == HARNESS_JOB_LENGTH - 1, true);
	seeds[HARNESS_JOB_LENGTH - 1 - fixed] = '1';
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), false);
	// So is any job that does not fit the buffer it is written to.
	settings.seeds = "1-10";
	ASSERT_BOOL_EQ(runJob_format(&settings, job, 64), false);
	ASSERT_BOOL_EQ(runJob_format(&settings, job, 0), false);
}

// Protocol 1.4.0's keys follow the others, each only when it is set.
TEST(adds_the_per_match_keys_only_when_set) {
	static const char fixed[] =
		"command=run\ninput=/b/inputs/baseline.bin\nseeds=37000-37999\noutput=/b/a1/results.csv\n"
		"threads=4\nprofile=\ndetailed=0\nhuman_control=0\nkeep_build=0\nhome_team=\naway_team=677\n"
		"progress_window=0\n";
	RunJob settings = {
		.input = "/b/inputs/baseline.bin",
		.seeds = "37000-37999",
		.output = "/b/a1/results.csv",
		.threads = 4,
		.teams = {0, 677},
		.stopFile = "",
	};
	static char job[HARNESS_JOB_LENGTH + 16];
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	ASSERT_INT_EQ(strcmp(job, fixed), 0);
	settings.perMatch = true;
	settings.skipSeedErrors = true;
	settings.report = true;
	settings.indexBase = UINT64_MAX;
	settings.stopFile = "/b/control/stop-now";
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	ASSERT_BOOL_EQ(strncmp(job, fixed, sizeof fixed - 1) == 0, true);
	ASSERT_INT_EQ(
		strcmp(
			job + sizeof fixed - 1,
			"per_match=1\nindex_base=18446744073709551615\nseed_errors=skip\n"
			"stop_file=/b/control/stop-now\nreport=1\n"
		),
		0
	);
	static const struct {
		bool perMatch, skip, report;
		uint64_t indexBase;
		const char *stopFile;
		const char *added;
	} each[] = {
		{true, false, false, 0, NULL, "per_match=1\n"},
		{false, true, false, 0, NULL, "seed_errors=skip\n"},
		{false, false, true, 0, NULL, "report=1\n"},
		{false, false, false, 37000, NULL, "index_base=37000\n"},
		{false, false, false, 0, "/stop", "stop_file=/stop\n"},
	};
	for (size_t i = 0; i < sizeof each / sizeof each[0]; ++i) {
		settings.perMatch = each[i].perMatch;
		settings.skipSeedErrors = each[i].skip;
		settings.report = each[i].report;
		settings.indexBase = each[i].indexBase;
		settings.stopFile = each[i].stopFile;
		ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
		ASSERT_INT_EQ(strcmp(job + sizeof fixed - 1, each[i].added), 0);
	}
	// The new keys count towards the limit.
	settings = (RunJob){.input = "/in.bin", .seeds = "", .output = "/out.csv", .threads = 1};
	settings.perMatch = true;
	settings.report = true;
	settings.stopFile = "/b/control/stop-now";
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	size_t used = strlen(job);
	static char seeds[HARNESS_JOB_LENGTH];
	memset(seeds, '1', HARNESS_JOB_LENGTH - 1 - used);
	settings.seeds = seeds;
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), true);
	ASSERT_BOOL_EQ(strlen(job) == HARNESS_JOB_LENGTH - 1, true);
	seeds[HARNESS_JOB_LENGTH - 1 - used] = '1';
	ASSERT_BOOL_EQ(runJob_format(&settings, job, sizeof job), false);
	ASSERT_BOOL_EQ(runJob_format(&settings, job, HARNESS_JOB_LENGTH - 1), false);
}

#ifndef _WIN32
// commands_run's exit code, with what it printed in `out`.
static int runPrinting(int argc, wchar_t **arguments, char *out, size_t size) {
	char printed[] = "/tmp/fm24-run-out-XXXXXX";
	out[0] = 0;
	int file = mkstemp(printed);
	if (file < 0) {
		return -1;
	}
	(void)fflush(stdout);
	int saved = dup(STDOUT_FILENO);
	int status = -1;
	if (saved >= 0 && dup2(file, STDOUT_FILENO) >= 0) {
		status = commands_run(argc, arguments);
		(void)fflush(stdout);
		(void)dup2(saved, STDOUT_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	ssize_t length = pread(file, out, size - 1, 0);
	out[length > 0 ? length : 0] = 0;
	close(file);
	(void)unlink(printed);
	return status;
}

// `run --per-match --keep-going` sends the keys and names the files the harness writes.
TEST(runs_with_per_match_rows_and_keeps_going) {
	char folder[] = "/tmp/fm24-run-XXXXXX";
	char csv[64];
	char bin[64]; // the input, which run checks is there
	wchar_t output[64];
	wchar_t input[64];
	bool ok = mkdtemp(folder) && snprintf(csv, sizeof csv, "%s/out.csv", folder) > 0 &&
						snprintf(bin, sizeof bin, "%s/next.bin", folder) > 0 &&
						platform_fromUtf8(csv, output, 64) && platform_fromUtf8(bin, input, 64);
	static const char rows[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
														 "7,2,1,21600,1,0123456789abcdef,2000.0\n";
	FILE *file = ok ? fopen(csv, "wb") : NULL;
	ok = file && fputs(rows, file) >= 0;
	ok = file && fclose(file) == 0 && ok;
	file = ok ? fopen(bin, "wb") : NULL;
	ok = file && fclose(file) == 0 && ok;
	ASSERT_BOOL_EQ(ok, true);
	if (!ok) {
		return;
	}
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	wchar_t *arguments[] = {L"fm24ctl",
		L"run",
		input,
		L"5-12",
		output,
		L"--per-match",
		L"--keep-going",
		L"--threads",
		L"2",
		NULL};
	static const struct {
		const char *reply;
		int status;
		const char *skipped; // the line that follows the per-match rows' paths, or ""
	} cases[] = {
		{"ok: 8/8 runs, 3 skipped (rebuild failed), each seed rebuilt by the game",
			EXIT_OK,
			"3 seeds skipped; see "},
		{"ok: 8/8 runs, 1 skipped (rebuild failed), each seed rebuilt by the game",
			EXIT_OK,
			"1 seed skipped; see "},
		{"ok: 8/8 runs, each seed rebuilt by the game", EXIT_OK, ""},
		{"error: 3/8 runs, 1 skipped (rebuild failed), each seed rebuilt by the game; see harness.log",
			EXIT_FAILED,
			NULL},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		jobReply = cases[i].reply;
		submittedJob[0] = 0;
		char printed[2048];
		ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), cases[i].status);
		char expected[2048];
		(void)snprintf(
			expected,
			sizeof expected,
			"command=run\ninput=%s\nseeds=5-12\noutput=%s\nthreads=2\nprofile=\ndetailed=0\n"
			"human_control=0\nkeep_build=0\nhome_team=\naway_team=\nprogress_window=0\nper_match=1\n"
			"seed_errors=skip\n",
			bin,
			csv
		);
		ASSERT_INT_EQ(strcmp(submittedJob, expected), 0);
		if (cases[i].skipped) {
			(void)snprintf(
				expected,
				sizeof expected,
				"%s\nper-match player rows: %s/out-match-players.csv\n"
				"per-match team rows: %s/out-match-teams.csv\n%s%s%s  seed 7: 2-1\n1 runs: home 1 / "
				"draw 0 / away 0, avg 2.00-1.00\n",
				cases[i].reply,
				folder,
				folder,
				cases[i].skipped,
				cases[i].skipped[0] ? folder : "",
				cases[i].skipped[0] ? "/out-failed.csv\n" : ""
			);
		} else {
			(void)snprintf(expected, sizeof expected, "%s\n", cases[i].reply);
		}
		const char *after = strstr(printed, "job submitted; waiting...\n");
		bool same = after && strcmp(after + strlen("job submitted; waiting...\n"), expected) == 0;
		ASSERT_BOOL_EQ(same, true);
		if (!same) {
			fprintf(stderr, "printed:\n%s\nexpected the end:\n%s\n", printed, expected);
		}
	}
	jobReply = "ok";
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	(void)unlink(csv);
	(void)unlink(bin);
	(void)rmdir(folder);
}
#endif

// The text of a finished writer, or "" if it failed.
static const char *jsonText(JsonWriter *writer) {
	return jsonWriter_finish(writer) ? buffer_string(writer->out) : "";
}

TEST(writes_json_with_escapes_and_commas) {
	ByteBuffer out = {0};
	JsonWriter writer;
	jsonWriter_start(&writer, &out);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_string(&writer, "format", "fm24-run-manifest/1");
	jsonWriter_string(
		&writer, "text", "q\"b\\s/\b\f\n\r\t\x01\x1f\x7f \xC3\x98 \xE2\x82\xAC \xF0\x9F\x8E\xB2"
	);
	// Bytes that are not UTF-8: a lead without its continuation, overlong, a surrogate, past
	// U+10FFFF, cut short.
	jsonWriter_string(&writer, "bad", "\xC0\xAF|\xED\xA0\x80|\xF4\x90\x80\x80|\xE2\x82|\xFF");
	jsonWriter_string(&writer, "missing", NULL);
	jsonWriter_beginArray(&writer, "values");
	jsonWriter_string(&writer, NULL, "run");
	jsonWriter_unsigned(&writer, NULL, UINT64_MAX);
	jsonWriter_signed(&writer, NULL, INT64_MIN);
	jsonWriter_bool(&writer, NULL, true);
	jsonWriter_bool(&writer, NULL, false);
	jsonWriter_null(&writer, NULL);
	jsonWriter_beginObject(&writer, NULL);
	jsonWriter_endObject(&writer);
	jsonWriter_beginArray(&writer, NULL);
	jsonWriter_endArray(&writer);
	jsonWriter_endArray(&writer);
	// Each container starts without a comma, whatever its siblings held.
	jsonWriter_beginArray(&writer, "parts");
	for (unsigned part = 1; part <= 2; ++part) {
		jsonWriter_beginObject(&writer, NULL);
		jsonWriter_unsigned(&writer, "attempt", part);
		jsonWriter_beginArray(&writer, "seeds");
		jsonWriter_unsigned(&writer, NULL, part * 10);
		jsonWriter_endArray(&writer);
		jsonWriter_endObject(&writer);
	}
	jsonWriter_endArray(&writer);
	jsonWriter_decimal(&writer, "rate", 1.8, 2);
	jsonWriter_decimal(&writer, "mean", 2201.44, 1);
	jsonWriter_decimal(&writer, "whole", -3.0, 0);
	jsonWriter_decimal(&writer, "not_a_number", NAN, 2);
	jsonWriter_decimal(&writer, "huge", 1e300, 2);
	jsonWriter_utcTime(&writer, "at", 1761384198);
	jsonWriter_utcTime(&writer, "leap", 951782400);
	jsonWriter_endObject(&writer);
	ASSERT_INT_EQ(
		strcmp(
			jsonText(&writer),
			"{\"format\":\"fm24-run-manifest/1\","
			"\"text\":\"q\\\"b\\\\s/\\b\\f\\n\\r\\t\\u0001\\u001f\x7f \xC3\x98 \xE2\x82\xAC "
			"\xF0\x9F\x8E\xB2\","
			"\"bad\":\"\xEF\xBF\xBD\xEF\xBF\xBD|\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD|\xEF\xBF\xBD"
			"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD|\xEF\xBF\xBD\xEF\xBF\xBD|\xEF\xBF\xBD\","
			"\"missing\":null,"
			"\"values\":[\"run\",18446744073709551615,-9223372036854775808,true,false,null,{},[]],"
			"\"parts\":[{\"attempt\":1,\"seeds\":[10]},{\"attempt\":2,\"seeds\":[20]}],"
			"\"rate\":1.80,\"mean\":2201.4,\"whole\":-3,\"not_a_number\":null,"
			"\"huge\":1.0000000000000001e+300,"
			"\"at\":\"2025-10-25T09:23:18Z\",\"leap\":\"2000-02-29T00:00:00Z\"}"
		),
		0
	);
	buffer_free(&out);
	// Keys go with object members only, ends must match, and there is one top-level value.
	for (int misuse = 0; misuse < 8; ++misuse) {
		jsonWriter_start(&writer, &out);
		// In turn: a member without a key, a key in an array, a key at the top, two top-level
		// values, an end of the other kind, an end with nothing open, an object left open, and more
		// digits than allowed.
		switch (misuse) {
			case 0:
				jsonWriter_beginObject(&writer, NULL);
				jsonWriter_string(&writer, NULL, "x");
				jsonWriter_endObject(&writer);
				break;
			case 1:
				jsonWriter_beginArray(&writer, NULL);
				jsonWriter_string(&writer, "key", "x");
				jsonWriter_endArray(&writer);
				break;
			case 2: jsonWriter_string(&writer, "key", "x"); break;
			case 3:
				jsonWriter_string(&writer, NULL, "one");
				jsonWriter_string(&writer, NULL, "two");
				break;
			case 4:
				jsonWriter_beginArray(&writer, NULL);
				jsonWriter_endObject(&writer);
				break;
			case 5:
				jsonWriter_string(&writer, NULL, "x");
				jsonWriter_endArray(&writer);
				break;
			case 6: jsonWriter_beginObject(&writer, NULL); break;
			default:
				jsonWriter_beginObject(&writer, NULL);
				jsonWriter_decimal(&writer, "digits", 1.0, 10);
				jsonWriter_endObject(&writer);
				break;
		}
		ASSERT_BOOL_EQ(jsonWriter_finish(&writer), false);
		buffer_free(&out);
	}
	// Nothing at all is not a document either; JSON_WRITER_DEPTH levels are.
	jsonWriter_start(&writer, &out);
	ASSERT_BOOL_EQ(jsonWriter_finish(&writer), false);
	for (int extra = 0; extra < 2; ++extra) {
		jsonWriter_start(&writer, &out);
		for (int level = 0; level < JSON_WRITER_DEPTH + extra; ++level) {
			jsonWriter_beginArray(&writer, NULL);
		}
		for (int level = 0; level < JSON_WRITER_DEPTH + extra; ++level) {
			jsonWriter_endArray(&writer);
		}
		ASSERT_BOOL_EQ(jsonWriter_finish(&writer), !extra);
		ASSERT_BOOL_EQ(!extra || out.length == JSON_WRITER_DEPTH, true);
		buffer_free(&out);
	}
}

TEST(prints_the_build_it_is) {
	char line[128];
	versionLine(line, sizeof line);
	// "fm24ctl <protocol> <40 hex digits> <clean|dirty> <slice>", or "unknown unknown" for the
	// commit and state of a build from outside a Git checkout.
	char protocol[16] = "";
	char commit[64] = "";
	char state[16] = "";
	char slice[16] = "";
	ASSERT_INT_EQ(sscanf(line, "fm24ctl %15s %63s %15s %15s", protocol, commit, state, slice), 4);
	ASSERT_INT_EQ(strcmp(protocol, HARNESS_PROTOCOL_TEXT), 0);
	ASSERT_INT_EQ(strcmp(commit, buildInfo_commit), 0);
	bool known = strlen(commit) == 40 && strspn(commit, "0123456789abcdef") == 40;
	ASSERT_BOOL_EQ(known || strcmp(commit, "unknown") == 0, true);
	ASSERT_BOOL_EQ(
		known ? buildInfo_dirty == 0 || buildInfo_dirty == 1 : buildInfo_dirty == -1, true
	);
	ASSERT_INT_EQ(strcmp(state, !known ? "unknown" : buildInfo_dirty ? "dirty" : "clean"), 0);
#ifdef __aarch64__
	ASSERT_INT_EQ(strcmp(slice, "arm64"), 0);
#else
	ASSERT_INT_EQ(strcmp(slice, "x86_64"), 0);
#endif
	wchar_t *arguments[] = {L"fm24ctl", L"version", L"extra", NULL};
	ASSERT_INT_EQ(commands_version(3, arguments), EXIT_USAGE);
	ASSERT_INT_EQ(commands_version(2, arguments), EXIT_OK);
}

#ifdef _WIN32
TEST(validates_pe_header_and_section_bounds) {
	uint8_t bytes[512] = {'M', 'Z'};
	uint32_t offset = 64;
	memcpy(bytes + DOS_HEADER_LFANEW, &offset, sizeof offset);
	IMAGE_NT_HEADERS64 nt = {
		.Signature = IMAGE_NT_SIGNATURE,
		.FileHeader =
			{
				.Machine = IMAGE_FILE_MACHINE_AMD64,
				.NumberOfSections = 1,
				.SizeOfOptionalHeader = sizeof nt.OptionalHeader,
			},
		.OptionalHeader = {.Magic = PE32_PLUS_MAGIC, .SizeOfImage = 0x2000},
	};
	IMAGE_SECTION_HEADER section = {
		.VirtualAddress = 0x1000,
		.Misc.VirtualSize = 16,
		.PointerToRawData = 400,
		.SizeOfRawData = 16,
		.Characteristics = IMAGE_SCN_MEM_EXECUTE,
	};
	memcpy(bytes + offset, &nt, sizeof nt);
	memcpy(bytes + offset + sizeof nt, &section, sizeof section);
	Image image = {.view = bytes};
	for (size_t length = 0; length < 416; ++length) {
		image.size = length;
		ASSERT_BOOL_EQ(parseHeaders(&image), false);
	}
	image.size = sizeof bytes;
	ASSERT_BOOL_EQ(parseHeaders(&image), true);
	ASSERT_BOOL_EQ(parseHeaders(&image), true);
	ASSERT_BOOL_EQ(image.sectionCount == 1, true);
	ASSERT_BOOL_EQ(imageBytes(&image, 0x1000, 16) == bytes + 400, true);
	ASSERT_BOOL_EQ(imageBytes(&image, 0x1001, 16) == NULL, true);
	offset = UINT32_MAX;
	memcpy(bytes + DOS_HEADER_LFANEW, &offset, sizeof offset);
	ASSERT_BOOL_EQ(parseHeaders(&image), false);
	offset = 64;
	memcpy(bytes + DOS_HEADER_LFANEW, &offset, sizeof offset);
	section.SizeOfRawData = UINT32_MAX;
	memcpy(bytes + offset + sizeof nt, &section, sizeof section);
	ASSERT_BOOL_EQ(parseHeaders(&image), false);
	nt.FileHeader.SizeOfOptionalHeader = 1;
	memcpy(bytes + offset, &nt, sizeof nt);
	ASSERT_BOOL_EQ(parseHeaders(&image), false);
}

TEST(scans_only_selected_anchors_and_ranks_shifts) {
	Probe *probe = calloc(1, sizeof *probe);
	ASSERT_BOOL_EQ(probe != NULL, true);
	if (!probe) {
		return;
	}
	probe->matches = calloc(probeFunctionCount, sizeof *probe->matches);
	probe->shifts = calloc(probeFunctionCount, sizeof *probe->shifts);
	bool allocated = probe->matches && probe->shifts && probeFunctionCount >= 3;
	ASSERT_BOOL_EQ(allocated, true);
	if (allocated) {
		const ProbeFunction *function = &probeFunctions[0];
		probe->image.sectionCount = 1;
		probe->image.sections[0] = (Section){
			.rva = function->rva,
			.raw = probePatternBytes + function->byteOffset,
			.rawSize = function->size,
			.characteristics = IMAGE_SCN_MEM_EXECUTE,
		};
		ASSERT_BOOL_EQ(buildAnchors(probe), true);
		scan(probe, false);
		chooseAnchors(probe);
		size_t selected = 0;
		for (size_t bucket = 0; bucket < sizeof probe->buckets / sizeof probe->buckets[0]; ++bucket) {
			for (int32_t a = probe->buckets[bucket]; a >= 0; a = probe->anchors[a].next) {
				ASSERT_BOOL_EQ(probe->anchors[a].used, true);
				++selected;
			}
		}
		ASSERT_BOOL_EQ(selected > 0 && selected <= ANCHORS_PER_FUNCTION * probeFunctionCount, true);
		scan(probe, true);
		settle(&probe->matches[0]);
		ASSERT_INT_EQ(probe->matches[0].status, FOUND);
		ASSERT_BOOL_EQ(probe->matches[0].rva == function->rva, true);
		memset(probe->matches, 0, probeFunctionCount * sizeof *probe->matches);
		for (size_t i = 0; i < 3; ++i) {
			probe->matches[i] = (Match){
				.status = FOUND,
				.rva = probeFunctions[i].rva + (i < 2 ? 0x100U : 0x200U),
			};
		}
		rankShifts(probe);
		ASSERT_BOOL_EQ(commonShift(probe, 0) == 0x100, true);
		ASSERT_BOOL_EQ(commonShift(probe, 1) == 0x200, true);
		ASSERT_BOOL_EQ(commonShift(probe, 2) == INT64_MIN, true);
		probe->matches[2] = (Match){
			.status = AMBIGUOUS,
			.tieCount = 2,
			.ties = {probeFunctions[2].rva + 0x200, probeFunctions[2].rva + 0x100},
		};
		resolveTies(probe);
		ASSERT_BOOL_EQ(probe->matches[2].status == FOUND && probe->matches[2].byShift, true);
		ASSERT_BOOL_EQ(probe->matches[2].rva == probeFunctions[2].rva + 0x100, true);
	}
	free(probe->anchors);
	free(probe->matches);
	free(probe->shifts);
	free(probe);
}

TEST(preserves_jump_candidate_overflow) {
	uint8_t bytes[(MAX_CANDIDATES + 1) * JMP_REL32_LENGTH] = {0};
	for (size_t i = 0; i < sizeof bytes; i += JMP_REL32_LENGTH) {
		bytes[i] = JMP_REL32;
		int32_t displacement = (int32_t)(0x2000 - (0x1000 + i + JMP_REL32_LENGTH));
		memcpy(bytes + i + 1, &displacement, sizeof displacement);
	}
	Image image = {.sectionCount = 1};
	image.sections[0] = (Section){
		.rva = 0x1000,
		.raw = bytes,
		.rawSize = sizeof bytes,
		.characteristics = IMAGE_SCN_MEM_EXECUTE,
	};
	Match match = {0};
	collectJumpsTo(&image, 0x2000, &match);
	ASSERT_BOOL_EQ(match.tieCount == MAX_CANDIDATES && match.tooMany, true);
	settle(&match);
	ASSERT_INT_EQ(match.status, AMBIGUOUS);
	image.sections[0].rawSize = 4;
	match = (Match){0};
	collectJumpsTo(&image, 0x2000, &match);
	ASSERT_INT_EQ(match.tieCount, 0);
}

TEST(extracts_the_dll_only_when_it_changed) {
	static const char first[] = "first build";
	static const char second[] = "second build";
	wchar_t path[MAX_PATH];
	DWORD length = GetTempPathW(MAX_PATH, path);
	ASSERT_BOOL_EQ(length > 0 && length < MAX_PATH - 40, true);
	(void)swprintf(
		path + length, MAX_PATH - length, L"fm24-embedded-test-%lu.dll", GetCurrentProcessId()
	);
	ByteBuffer contents = {0};
	ASSERT_BOOL_EQ(embedded_write(path, first, sizeof first), true);
	ASSERT_BOOL_EQ(files_read(path, &contents) && contents.length == sizeof first, true);
	// A read-only file can't be replaced, like a DLL that fm.exe has loaded.
	ASSERT_BOOL_EQ(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY) != 0, true);
	ASSERT_BOOL_EQ(embedded_write(path, first, sizeof first), true);
	ASSERT_BOOL_EQ(embedded_write(path, second, sizeof second), false);
	ASSERT_BOOL_EQ(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL) != 0, true);
	ASSERT_BOOL_EQ(embedded_write(path, second, sizeof second), true);
	ASSERT_BOOL_EQ(
		files_read(path, &contents) && contents.length == sizeof second &&
			memcmp(contents.data, second, sizeof second) == 0,
		true
	);
	buffer_free(&contents);
	(void)DeleteFileW(path);
}
#else
TEST(replaces_an_installed_dylib_atomically) {
	static const char first[] = "first build";
	static const char second[] = "second build";
	char directory[] = "/tmp/fm24-embedded-test-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char narrow[256];
	(void)snprintf(narrow, sizeof narrow, "%s/fm24-harness.dylib", directory);
	wchar_t path[256];
	ASSERT_BOOL_EQ(platform_fromUtf8(narrow, path, 256), true);
	ByteBuffer contents = {0};
	ASSERT_BOOL_EQ(embedded_write(path, first, sizeof first), true);
	struct stat before;
	ASSERT_BOOL_EQ(stat(narrow, &before) == 0 && (before.st_mode & 0111) != 0, true);
	ASSERT_BOOL_EQ(embedded_write(path, first, sizeof first), true);
	struct stat unchanged;
	ASSERT_BOOL_EQ(stat(narrow, &unchanged) == 0 && unchanged.st_ino == before.st_ino, true);
	// A game that loaded the old copy keeps it: the new one is a new file renamed over it, even
	// over a read-only one.
	ASSERT_BOOL_EQ(chmod(narrow, 0444) == 0, true);
	ASSERT_BOOL_EQ(embedded_write(path, second, sizeof second), true);
	struct stat after;
	ASSERT_BOOL_EQ(stat(narrow, &after) == 0 && after.st_ino != before.st_ino, true);
	ASSERT_BOOL_EQ(
		files_read(path, &contents) && contents.length == sizeof second &&
			memcmp(contents.data, second, sizeof second) == 0,
		true
	);
	buffer_free(&contents);
	(void)unlink(narrow);
	(void)rmdir(directory);
}

enum { SLICE_ALIGN = 0x1000 };

typedef struct TestSlice {
	cpu_type_t cpu;
	const uint8_t *uuid; // NULL for a slice without an LC_UUID
} TestSlice;

static bool writeBytes(const char *path, const void *bytes, size_t length) {
	FILE *file = fopen(path, "wb");
	bool ok = file && fwrite(bytes, 1, length, file) == length;
	return file && fclose(file) == 0 && ok;
}

static void putBigEndian32(uint8_t *at, uint32_t value) {
	at[0] = (uint8_t)(value >> 24);
	at[1] = (uint8_t)(value >> 16);
	at[2] = (uint8_t)(value >> 8);
	at[3] = (uint8_t)value;
}

// A minimal executable slice: its header, a __PAGEZERO segment and the slice's LC_UUID. Returns its
// size.
static size_t writeSlice(uint8_t *out, const TestSlice *slice) {
	struct segment_command_64 zero = {
		.cmd = LC_SEGMENT_64,
		.cmdsize = sizeof zero,
		.segname = SEG_PAGEZERO,
		.vmsize = 0x100000000,
	};
	struct uuid_command id = {.cmd = LC_UUID, .cmdsize = sizeof id};
	struct mach_header_64 header = {
		.magic = MH_MAGIC_64,
		.cputype = slice->cpu,
		.filetype = MH_EXECUTE,
		.ncmds = slice->uuid ? 2 : 1,
		.sizeofcmds = (uint32_t)(sizeof zero + (slice->uuid ? sizeof id : 0)),
	};
	memcpy(out, &header, sizeof header);
	memcpy(out + sizeof header, &zero, sizeof zero);
	if (!slice->uuid) {
		return sizeof header + sizeof zero;
	}
	memcpy(id.uuid, slice->uuid, sizeof id.uuid);
	memcpy(out + sizeof header + sizeof zero, &id, sizeof id);
	return sizeof header + sizeof zero + sizeof id;
}

// Writes a universal file of up to two slices to `path`, cut to `length` bytes if that is shorter.
static bool writeFat(const char *path, const TestSlice *slices, uint32_t count, size_t length) {
	static uint8_t file[3 * SLICE_ALIGN];
	memset(file, 0, sizeof file);
	putBigEndian32(file, FAT_MAGIC);
	putBigEndian32(file + 4, count);
	size_t end = sizeof(struct fat_header);
	for (uint32_t i = 0; i < count && i < 2; ++i) {
		uint8_t *entry = file + sizeof(struct fat_header) + i * sizeof(struct fat_arch);
		uint32_t offset = (i + 1) * SLICE_ALIGN;
		size_t size = writeSlice(file + offset, &slices[i]);
		putBigEndian32(entry, (uint32_t)slices[i].cpu);
		putBigEndian32(entry + 8, offset);
		putBigEndian32(entry + 12, (uint32_t)size);
		putBigEndian32(entry + 16, 12);
		end = offset + size;
	}
	return writeBytes(path, file, length < end ? length : end);
}

// `fm24ctl probe <path>`'s exit code, with what it printed in `out`.
static int probeFile(const char *path, char *out, size_t size) {
	wchar_t wide[1100];
	wchar_t *arguments[] = {L"fm24ctl", L"probe", wide, NULL};
	char printed[] = "/tmp/fm24-probe-out-XXXXXX";
	out[0] = 0;
	int file = platform_fromUtf8(path, wide, 1100) ? mkstemp(printed) : -1;
	if (file < 0) {
		return -1;
	}
	(void)fflush(stdout);
	int saved = dup(STDOUT_FILENO);
	int status = -1;
	if (saved >= 0 && dup2(file, STDOUT_FILENO) >= 0) {
		status = commands_probe(3, arguments);
		(void)fflush(stdout);
		(void)dup2(saved, STDOUT_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	ssize_t length = pread(file, out, size - 1, 0);
	out[length > 0 ? length : 0] = 0;
	close(file);
	(void)unlink(printed);
	return status;
}

// The line probe prints for a slice: its UUID in the usual form, and the build if it is supported.
static void sliceLine(char *out, size_t size, const char *architecture, const uint8_t uuid[16]) {
	static const char digits[] = "0123456789ABCDEF";
	char text[40];
	size_t length = 0;
	for (int b = 0; b < 16; ++b) {
		if (b == 4 || b == 6 || b == 8 || b == 10) {
			text[length++] = '-';
		}
		text[length++] = digits[uuid[b] >> 4];
		text[length++] = digits[uuid[b] & 15];
	}
	text[length] = 0;
	const MacosBuild *build = macosBuilds_find(architecture, uuid);
	(void)snprintf(
		out,
		size,
		"\n  %-7s %s  %s%s\n",
		architecture,
		text,
		build ? "supported: " : "not supported",
		build ? build->version : ""
	);
}

static const MacosBuild *supportedBuild(const char *architecture) {
	for (size_t i = 0; i < macosBuildCount; ++i) {
		if (strcmp(macosBuilds[i].architecture, architecture) == 0) {
			return &macosBuilds[i];
		}
	}
	return NULL;
}

TEST(probe_reads_the_uuid_of_each_slice) {
	const MacosBuild *arm = supportedBuild("arm64");
	const MacosBuild *intel = supportedBuild("x86_64");
	char directory[] = "/tmp/fm24-probe-test-XXXXXX";
	bool ready = arm && intel && mkdtemp(directory) != NULL;
	ASSERT_BOOL_EQ(ready, true);
	if (!ready) {
		return;
	}
	uint8_t otherBuild[16];
	memcpy(otherBuild, arm->uuid, sizeof otherBuild);
	otherBuild[0] ^= 1;
	char path[1100];
	char printed[2048];
	char line[160];
	(void)snprintf(path, sizeof path, "%s/fm", directory);
	// The supported game: both slices.
	TestSlice slices[] = {{CPU_TYPE_ARM64, arm->uuid}, {CPU_TYPE_X86_64, intel->uuid}};
	ASSERT_BOOL_EQ(writeFat(path, slices, 2, SIZE_MAX), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 0);
	sliceLine(line, sizeof line, "arm64", arm->uuid);
	ASSERT_BOOL_EQ(strstr(printed, line) != NULL, true);
	sliceLine(line, sizeof line, "x86_64", intel->uuid);
	ASSERT_BOOL_EQ(strstr(printed, line) != NULL, true);
	ASSERT_BOOL_EQ(
		strstr(printed, "\nharness " HARNESS_PROTOCOL_TEXT " supports 2 slice(s) of this build\n") !=
			NULL,
		true
	);
	// Another build's arm64 slice.
	slices[0].uuid = otherBuild;
	ASSERT_BOOL_EQ(writeFat(path, slices, 2, SIZE_MAX), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 0);
	sliceLine(line, sizeof line, "arm64", otherBuild);
	ASSERT_BOOL_EQ(strstr(printed, line) != NULL && strstr(line, "not supported") != NULL, true);
	ASSERT_BOOL_EQ(strstr(printed, " supports 1 slice(s) of this build\n") != NULL, true);
	// Each slice with the other's UUID: supported by neither.
	slices[0].uuid = intel->uuid;
	slices[1].uuid = arm->uuid;
	ASSERT_BOOL_EQ(writeFat(path, slices, 2, SIZE_MAX), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	ASSERT_BOOL_EQ(
		strstr(printed, " does not support this build: please report its UUIDs\n") != NULL, true
	);
	// A universal file whose last slice is cut short, or whose table lists a slice it lacks.
	slices[0].uuid = arm->uuid;
	slices[1].uuid = intel->uuid;
	ASSERT_BOOL_EQ(writeFat(path, slices, 2, 2 * SLICE_ALIGN + 40), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	ASSERT_BOOL_EQ(writeFat(path, slices, 3, SIZE_MAX), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	ASSERT_BOOL_EQ(unlink(path) == 0, true);
	// One slice on its own, named by its bundle.
	static const char *const folders[] = {"Game.app", "Game.app/Contents", "Game.app/Contents/MacOS"};
	for (size_t i = 0; i < 3; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(mkdir(path, 0755), 0);
	}
	uint8_t thin[256];
	(void)snprintf(path, sizeof path, "%s/Game.app/Contents/MacOS/fm", directory);
	ASSERT_BOOL_EQ(writeBytes(path, thin, writeSlice(thin, &slices[1])), true);
	char bundle[1100];
	(void)snprintf(bundle, sizeof bundle, "%s/Game.app/", directory);
	ASSERT_INT_EQ(probeFile(bundle, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(strncmp(printed, path, strlen(path)) == 0 && printed[strlen(path)] == '\n', true);
	ASSERT_BOOL_EQ(strstr(printed, " supports 1 slice(s) of this build\n") != NULL, true);
	// A slice without a UUID, a script, a file too short to be a slice and none at all.
	TestSlice anonymous = {CPU_TYPE_X86_64, NULL};
	ASSERT_BOOL_EQ(writeBytes(path, thin, writeSlice(thin, &anonymous)), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	memset(thin, ' ', sizeof thin);
	memcpy(thin, "#!/bin/sh\n", 10);
	ASSERT_BOOL_EQ(writeBytes(path, thin, sizeof thin), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	ASSERT_BOOL_EQ(writeBytes(path, thin, 16), true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	ASSERT_BOOL_EQ(unlink(path) == 0, true);
	ASSERT_INT_EQ(probeFile(path, printed, sizeof printed), 1);
	wchar_t *extra[] = {L"fm24ctl", L"probe", L"fm.app", L"fm.app", NULL};
	ASSERT_INT_EQ(commands_probe(4, extra), 2);
	// Without a path, and with no game running or in Steam's libraries (none under this HOME), it
	// says it cannot find the game: a failure, not a usage error.
	char running[1100];
	const char *home = getenv("HOME"); // NOLINT(concurrency-mt-unsafe): the tests run one at a time.
	char previous[1100];
	(void)snprintf(previous, sizeof previous, "%s", home ? home : "");
	ASSERT_INT_EQ(setenv("HOME", directory, 1), 0);
	wchar_t *bare[] = {L"fm24ctl", L"probe", NULL};
	if (!macosGame_findRunning(running, sizeof running)) {
		char errors[512];
		ASSERT_INT_EQ(invokeCapturingErrors(commands_probe, bare, errors, sizeof errors), 1);
		ASSERT_INT_EQ(
			strcmp(
				errors,
				"cannot find Football Manager 2024: it is not running or in your Steam libraries; pass "
				"the path to its fm.app\n"
			),
			0
		);
	}
	if (home) {
		ASSERT_INT_EQ(setenv("HOME", previous, 1), 0);
	} else {
		ASSERT_INT_EQ(unsetenv("HOME"), 0);
	}
	for (size_t i = 3; i-- > 0;) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, folders[i]);
		ASSERT_INT_EQ(rmdir(path), 0);
	}
	ASSERT_INT_EQ(rmdir(directory), 0);
}
#endif

#define TEST_COMMIT "e74df0b3c1a2d4e5f60718293a4b5c6d7e8f9012"

// The report of the example in docs/TECHNICAL.md: every seed played or skipped.
static const char exampleReport[] =
	"ok: 1000/1000 runs, 3 skipped (rebuild failed), each seed rebuilt by the game; report "
	"protocol=1.4.0 commit=" TEST_COMMIT " dirty=0 build=2081827 slice=arm64 translated=0 "
	"pid=81234 loaded=1761400012 jobs=17 threads=4 edits=3 elapsed_ms=553210 written=1000 "
	"skipped=3 stopped=0 failed_index=- failed_stage=- output_failed=0";

TEST(parses_the_run_report) {
	RunReport report;
	ASSERT_BOOL_EQ(runReport_parse(exampleReport, &report), true);
	ASSERT_INT_EQ(strcmp(report.protocol, "1.4.0"), 0);
	ASSERT_INT_EQ(strcmp(report.commit, TEST_COMMIT), 0);
	ASSERT_INT_EQ(report.dirty, 0);
	ASSERT_BOOL_EQ(report.build == 2081827 && strcmp(report.slice, "arm64") == 0, true);
	ASSERT_BOOL_EQ(!report.translated && report.pid == 81234 && report.loaded == 1761400012, true);
	ASSERT_BOOL_EQ(
		report.jobs == 17 && report.threads == 4 && report.hasEdits && report.edits == 3, true
	);
	ASSERT_BOOL_EQ(report.elapsedMs == 553210 && report.written == 1000 && report.skipped == 3, true);
	ASSERT_BOOL_EQ(!report.stopped && !report.hasFailure && !report.failedIndex, true);
	ASSERT_BOOL_EQ(!report.failedStage[0] && !report.outputFailed, true);
	// A failure, after "see harness.log", up to a line ending.
	static const char failed[] =
		"error: 412/1000 runs, stopped by the stop file, each seed rebuilt by the game; see "
		"harness.log; report protocol=1.4.0 commit=" TEST_COMMIT " dirty=1 build=0 slice=x86_64 "
		"translated=1 pid=0 loaded=-5 jobs=0 threads=0 edits=- elapsed_ms=0 written=412 skipped=0 "
		"stopped=1 failed_index=37412 failed_stage=deserialize output_failed=1\r\n";
	ASSERT_BOOL_EQ(runReport_parse(failed, &report), true);
	ASSERT_BOOL_EQ(
		report.dirty == 1 && report.translated && strcmp(report.slice, "x86_64") == 0, true
	);
	ASSERT_BOOL_EQ(report.loaded == -5 && !report.hasEdits && report.written == 412, true);
	ASSERT_BOOL_EQ(report.stopped && report.hasFailure && report.failedIndex == 37412, true);
	ASSERT_BOOL_EQ(strcmp(report.failedStage, "deserialize") == 0 && report.outputFailed, true);
	// The lines job_run.c writes: every field at its longest, and a build of unknown commit.
	char line[2 * HARNESS_RESULT_LENGTH];
	jobRunTest_reportLine(true, line, sizeof line);
	ASSERT_BOOL_EQ(runReport_parse(line, &report), true);
	ASSERT_BOOL_EQ(report.build == UINT32_MAX && report.pid == UINT32_MAX, true);
	ASSERT_BOOL_EQ(report.loaded == 99999999999 && report.jobs == 9999999999999U, true);
	ASSERT_BOOL_EQ(report.threads == HARNESS_MAX_THREADS && report.edits == 9999999999999U, true);
	ASSERT_BOOL_EQ(report.written == 99999999999U && report.skipped == 99999999999U, true);
	ASSERT_BOOL_EQ(report.hasFailure && report.failedIndex == UINT64_MAX, true);
	ASSERT_BOOL_EQ(report.dirty == 1 && report.stopped && report.outputFailed, true);
	jobRunTest_reportLine(false, line, sizeof line);
	ASSERT_BOOL_EQ(runReport_parse(line, &report), true);
	ASSERT_BOOL_EQ(strcmp(report.commit, "unknown") == 0 && report.dirty == -1, true);
	ASSERT_BOOL_EQ(!report.hasEdits && !report.hasFailure && !report.written && !report.loaded, true);
	// Every key once, in order, each with a value it can have; anything else is no report.
	static const struct {
		const char *from;
		const char *to;
	} broken[] = {
		{"; report ", "; report  "},
		{"; report ", ";report "},
		{"; report ", " "},
		{"protocol=1.4.0", "protocol=1.4.0x"},
		{"protocol=1.4.0 ", "protocol=1.4.0 protocol=1.4.0 "},
		{"commit=e", "commit=E"},
		{"commit=e", "commit="},
		{"commit=" TEST_COMMIT " dirty=0", "commit=unknown dirty=0"},
		{"dirty=0", "dirty=-"},
		{"dirty=0", "dirty=2"},
		{"build=2081827", "build=4294967296"},
		{"build=2081827", "build=+2081827"},
		{"slice=arm64", "slice=ARM64"},
		{"translated=0", "translated=2"},
		{"pid=81234", "pid=-1"},
		{"loaded=1761400012", "loaded=-"},
		{"jobs=17", "jobs="},
		{"threads=4 edits=3", "edits=3 threads=4"},
		{"edits=3", "edits=three"},
		{"elapsed_ms=553210", "elapsed_ms=18446744073709551616"},
		{"skipped=3", "skipped=1001"},
		{"written=1000 skipped=3", "written=3 skipped=5"},
		{"stopped=0", "stopped=no"},
		{"failed_index=-", "failed_index=7"},
		{"failed_stage=-", "failed_stage=rebuild"},
		{"failed_index=- failed_stage=-", "failed_index=7 failed_stage=crash"},
		{" written=1000", ""},
		{" output_failed=0", ""},
		{"output_failed=0", "output_failed=0 "},
		{"output_failed=0", "output_failed=0 extra=1"},
		{"output_failed=0", "output_failed=0\n\n"},
	};
	for (size_t i = 0; i < sizeof broken / sizeof broken[0]; ++i) {
		char text[1024];
		const char *at = strstr(exampleReport, broken[i].from);
		ASSERT_BOOL_EQ(at != NULL, true);
		if (!at) {
			continue;
		}
		(void)snprintf(
			text,
			sizeof text,
			"%.*s%s%s",
			(int)(at - exampleReport),
			exampleReport,
			broken[i].to,
			at + strlen(broken[i].from)
		);
		bool read = runReport_parse(text, &report);
		ASSERT_BOOL_EQ(read, false);
		ASSERT_BOOL_EQ(!report.protocol[0] && !report.written && !report.build, true);
		if (read) {
			fprintf(stderr, "read as a report: %s\n", text);
		}
	}
	ASSERT_BOOL_EQ(runReport_parse("ok: 8/8 runs, each seed rebuilt by the game", &report), false);
}

// As the harness reads seed lists (job.c's parseSeeds).
TEST(counts_the_seeds_a_list_plays) {
	static const struct {
		const char *seeds;
		bool listed;
		uint64_t count;
		uint32_t first;
		uint32_t last;
	} cases[] = {
		{"42", true, 1, 42, 42},
		{"1-1000", true, 1000, 1, 1000},
		{"1,5,10-20", true, 13, 1, 20},
		{"42,42,42", true, 3, 42, 42},
		{"300-400,7", true, 102, 300, 7},
		{"0-10000000", true, 10000001, 0, 10000000},
		{"4294967295", true, 1, 4294967295U, 4294967295U},
		{"007-009", true, 3, 7, 9},
		{"0-10000001", false, 0, 0, 0},
		{"4294967296", false, 0, 0, 0},
		{"5-4", false, 0, 0, 0},
		{"1,", false, 0, 0, 0},
		{",1", false, 0, 0, 0},
		{"1,,2", false, 0, 0, 0},
		{"1-", false, 0, 0, 0},
		{"1-2-3", false, 0, 0, 0},
		{"1 ", false, 0, 0, 0},
		{"", false, 0, 0, 0},
		{"keep", false, 0, 0, 0},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		uint64_t count = 9;
		uint32_t first = 9;
		uint32_t last = 9;
		ASSERT_BOOL_EQ(runJob_countSeeds(cases[i].seeds, &count, &first, &last), cases[i].listed);
		ASSERT_BOOL_EQ(
			count == cases[i].count && first == cases[i].first && last == cases[i].last, true
		);
	}
}

TEST(reads_the_header_of_a_config) {
	static const char full[] =
		"{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"engine_version\":\"24.2.0.0\","
		"\"source\":\"Liverpool vs Arsenal, 2024 day 123 15:00, seed 42\",\"save\":\"LIV_COPY\","
		"\"game_date\":\"2024 day 123 15:00\",\"fields\":[{\"u8\":1,\"home_team\":5}],"
		"\"away_team\":677}";
	ConfigHeader header;
	ASSERT_BOOL_EQ(matchConfig_readHeader(full, strlen(full), &header), true);
	ASSERT_INT_EQ(strcmp(header.format, "fm24-match-config/1"), 0);
	ASSERT_BOOL_EQ(
		header.gameBuild == 2081827 && strcmp(header.engineVersion, "24.2.0.0") == 0, true
	);
	ASSERT_INT_EQ(strcmp(header.source, "Liverpool vs Arsenal, 2024 day 123 15:00, seed 42"), 0);
	ASSERT_BOOL_EQ(header.origin.hasSave && strcmp(header.origin.save, "LIV_COPY") == 0, true);
	ASSERT_INT_EQ(strcmp(header.origin.gameDate, "2024 day 123 15:00"), 0);
	ASSERT_BOOL_EQ(header.teams[0] == 0 && header.teams[1] == 677, true);
	ConfigOrigin origin;
	ASSERT_BOOL_EQ(matchConfig_readOrigin(full, strlen(full), &origin), true);
	ASSERT_BOOL_EQ(origin.hasSave && strcmp(origin.save, "LIV_COPY") == 0, true);
	ASSERT_INT_EQ(strcmp(origin.gameDate, "2024 day 123 15:00"), 0);
	// Values of another kind, or too long, are none; the last of a key counts.
	static const char odd[] =
		"{\"fields\":[],\"format\":5,\"game_build\":\"2081827\",\"engine_version\":"
		"\"0123456789012345678901234567890123\",\"source\":\"a\\nb\",\"home_team\":-1,"
		"\"away_team\":1.5,\"save\":null,\"extra\":{\"home_team\":9},\"home_team\":4294967297,"
		"\"game_build\":7}";
	ASSERT_BOOL_EQ(matchConfig_readHeader(odd, strlen(odd), &header), true);
	ASSERT_BOOL_EQ(!header.format[0] && !header.engineVersion[0] && !header.source[0], true);
	ASSERT_BOOL_EQ(header.gameBuild == 7 && !header.teams[0] && !header.teams[1], true);
	ASSERT_BOOL_EQ(header.origin.hasSave && !header.origin.save[0], true);
	// Not an object: nothing.
	static const char *const bad[] = {"[]", "{\"format\":", "{\"format\":\"x\" \"y\"}", "", "{,}"};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		memset(&header, 'x', sizeof header);
		ASSERT_BOOL_EQ(matchConfig_readHeader(bad[i], strlen(bad[i]), &header), false);
		ASSERT_BOOL_EQ(!header.format[0] && !header.gameBuild && !header.origin.hasSave, true);
		ASSERT_BOOL_EQ(!header.source[0] && !header.teams[1], true);
	}
}

#define TEST_HASH(c) c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c
#define TEST_RESULT_LINE                                                                           \
	"ok: 1000/1000 runs, each seed rebuilt by the game; report protocol=1.4.0 commit=" TEST_COMMIT   \
	" dirty=0 build=2081827 slice=arm64 translated=0 pid=81234 loaded=1761382682 jobs=17 "           \
	"threads=4 edits=3 elapsed_ms=553210 written=1000 skipped=0 stopped=0 failed_index=- "           \
	"failed_stage=- output_failed=0"

// A thousand matches played on this Mac, with the clock and host the test gives.
static const char runManifest[] =
	"{\"format\":\"fm24-run-manifest/1\",\"outcome\":\"ok\",\"exit_code\":0,"
	"\"client_outcome\":\"replied\",\"result_line\":\"" TEST_RESULT_LINE "\","
	"\"created_at\":\"2025-10-25T09:23:18Z\",\"command\":[\"run\",\"/Users/you/fm24/next.json\","
	"\"1-1000\",\"/Users/you/fm24/out/r.csv\",\"--threads\",\"4\",\"--per-match\",\"--manifest\"],"
	"\"fm24ctl\":{\"protocol\":\"1.4.0\",\"commit\":\"" TEST_COMMIT "\",\"dirty\":false,"
	"\"slice\":\"arm64\",\"translated\":false},"
	"\"harness\":{\"protocol\":\"1.4.0\",\"commit\":\"" TEST_COMMIT "\",\"dirty\":false,"
	"\"build\":2081827,\"slice\":\"arm64\",\"translated\":false,\"pid\":81234,"
	"\"loaded\":\"2025-10-25T08:58:02Z\",\"jobs\":17,\"threads\":4,\"edits\":3,"
	"\"elapsed_ms\":553210,\"written\":1000,\"skipped\":0,\"stopped\":false,\"failed_index\":null,"
	"\"failed_stage\":null,\"output_failed\":false},"
	"\"harness_matches_fm24ctl\":true,"
	"\"game\":{\"pid\":81234,\"under_rosetta\":false,\"save\":\"LIV_COPY\","
	"\"date\":\"2024 day 123 15:00\",\"rss_bytes\":6123456789},"
	"\"host\":{\"model\":\"MacBookAir10,1\",\"cpu\":\"Apple M1\",\"logical_cpus\":8,"
	"\"performance_cores\":4,\"efficiency_cores\":4,\"memory_bytes\":17179869184,"
	"\"os\":\"macOS 27.0\",\"os_build\":\"26A123\"},"
	"\"fixture\":{\"path\":\"/Users/you/fm24/next.json\",\"bytes\":1532214,"
	"\"sha256\":\"" TEST_HASH("a1") "\",\"input_bytes\":412345,\"input_sha256\":\"" TEST_HASH(
		"b2"
	) "\",\"header\":{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,"
		"\"engine_version\":\"24.2.0.0\",\"source\":\"Liverpool vs Arsenal\",\"save\":\"LIV_COPY\","
		"\"game_date\":\"2024 day 123 15:00\",\"home_team\":null,\"away_team\":677}},"
		"\"seeds\":{\"spec\":\"1-1000\",\"count\":1000,\"first\":1,\"last\":1000,\"index_base\":0,"
		"\"sha256\":\"f1c55335a39338e3fda63f2d9555fef7dc7f9ab7eab1edea0a3969d6f904f138\"},"
		"\"flags\":{\"threads\":4,\"per_seed_rebuild\":true,\"keep_build\":false,"
		"\"human_control\":false,\"detailed\":false,\"per_match\":true,\"seed_errors\":\"stop\","
		"\"profile\":null,\"no_launch\":false,\"any_save\":false},"
		"\"timing\":{\"started_at\":\"2025-10-25T09:14:03Z\",\"accepted_at\":\"2025-10-25T09:14:04Z\","
		"\"finished_at\":\"2025-10-25T09:23:18Z\",\"queue_ms\":512,\"run_ms\":555012,\"slept_ms\":0,"
		"\"matches_per_s\":1.80,\"match_ms_mean\":2201.4},"
		"\"outputs\":[{\"role\":\"results\",\"path\":\"r.csv\",\"bytes\":46890,\"rows\":1000,"
		"\"sha256\":\"" TEST_HASH("c3") "\",\"content_sha256\":\"" TEST_HASH(
			"d4"
		) "\"},"
			"{\"role\":\"match_players\",\"path\":\"r-match-players.csv\",\"bytes\":6284112,"
			"\"rows\":40000,\"sha256\":\"" TEST_HASH(
				"e5"
			) "\"},"
				"{\"role\":\"match_teams\",\"path\":\"r-match-teams.csv\",\"bytes\":null,\"rows\":null,"
				"\"sha256\":null}],"
				"\"summary\":{\"matches\":1000,\"home\":559,\"draw\":287,\"away\":154,\"home_goals\":1688,"
				"\"away_goals\":1041,\"unfinished\":0,\"skipped\":0}}\n";

// A run that never went to the harness, from a build outside a Git checkout, on Windows.
static const char unsubmittedManifest[] =
	"{\"format\":\"fm24-run-manifest/1\",\"outcome\":\"not_submitted\",\"exit_code\":3,"
	"\"client_outcome\":null,\"result_line\":null,\"created_at\":\"2025-10-25T09:23:18Z\","
	"\"command\":[\"run\",\"next.bin\",null],"
	"\"fm24ctl\":{\"protocol\":\"1.4.0\",\"commit\":\"unknown\",\"dirty\":null,"
	"\"slice\":\"x86_64\",\"translated\":true},"
	"\"harness\":null,\"harness_matches_fm24ctl\":null,\"game\":null,"
	"\"host\":{\"model\":null,\"cpu\":null,\"logical_cpus\":16,\"performance_cores\":null,"
	"\"efficiency_cores\":null,\"memory_bytes\":34359738368,\"os\":\"Windows\",\"os_build\":null},"
	"\"fixture\":{\"path\":\"C:\\\\fm24\\\\next.bin\",\"bytes\":null,\"sha256\":null,"
	"\"input_bytes\":null,\"input_sha256\":null,\"header\":null},"
	"\"seeds\":{\"spec\":\"keep\",\"count\":1,\"first\":null,\"last\":null,\"index_base\":0,"
	"\"sha256\":\"6ca7ea2feefc88ecb5ed6356ed963f47dc9137f82526fdd25d618ea626d0803f\"},"
	"\"flags\":{\"threads\":15,\"per_seed_rebuild\":false,\"keep_build\":false,"
	"\"human_control\":true,\"detailed\":true,\"per_match\":false,\"seed_errors\":\"skip\","
	"\"profile\":\"C:\\\\fm24\\\\samples.txt\",\"no_launch\":true,\"any_save\":true},"
	"\"timing\":{\"started_at\":null,\"accepted_at\":null,\"finished_at\":null,\"queue_ms\":null,"
	"\"run_ms\":null,\"slept_ms\":null,\"matches_per_s\":null,\"match_ms_mean\":null},"
	"\"outputs\":[],\"summary\":null}\n";

// The manifest's text, or "" if it cannot be formatted, in `out` (freed by the caller).
static const char *formatManifest(const RunManifest *manifest, ByteBuffer *out) {
	buffer_clear(out);
	return manifest_formatRun(manifest, out) ? buffer_string(out) : "";
}

static void checkManifestText(const char *text, const char *expected) {
	bool same = strcmp(text, expected) == 0;
	ASSERT_BOOL_EQ(same, true);
	if (!same) {
		size_t at = 0;
		while (text[at] && text[at] == expected[at]) {
			++at;
		}
		fprintf(stderr, "differs at %zu:\n%s\nexpected:\n%s\n", at, text + at, expected + at);
	}
}

TEST(formats_the_run_manifest) {
	static const char *const command[] = {"run",
		"/Users/you/fm24/next.json",
		"1-1000",
		"/Users/you/fm24/out/r.csv",
		"--threads",
		"4",
		"--per-match",
		"--manifest"};
	HostInfo host = {.model = "MacBookAir10,1",
		.cpu = "Apple M1",
		.logicalCpus = 8,
		.performanceCores = 4,
		.efficiencyCores = 4,
		.memoryBytes = 17179869184U,
		.os = "macOS 27.0",
		.osBuild = "26A123"};
	RunReport report = {.protocol = "1.4.0",
		.commit = TEST_COMMIT,
		.dirty = 0,
		.build = 2081827,
		.slice = "arm64",
		.pid = 81234,
		.loaded = 1761382682,
		.jobs = 17,
		.threads = 4,
		.hasEdits = true,
		.edits = 3,
		.elapsedMs = 553210,
		.written = 1000};
	GameFacts game = {
		.pid = 81234, .residentBytes = 6123456789U, .save = "LIV_COPY", .date = "2024 day 123 15:00"};
	FixtureFacts fixture = {.read = true,
		.bytes = 1532214,
		.sha256 = TEST_HASH("a1"),
		.encoded = true,
		.inputBytes = 412345,
		.inputSha256 = TEST_HASH("b2"),
		.hasHeader = true,
		.header = {.format = "fm24-match-config/1",
			.gameBuild = 2081827,
			.engineVersion = "24.2.0.0",
			.source = "Liverpool vs Arsenal",
			.origin = {.hasSave = true, .save = "LIV_COPY", .gameDate = "2024 day 123 15:00"},
			.teams = {0, 677}}};
	RunJob job = {.input = "/tmp/_job_input-1-1.bin",
		.seeds = "1-1000",
		.output = "/Users/you/fm24/out/r.csv",
		.threads = 4,
		.profile = "",
		.perMatch = true,
		.report = true};
	ClientWatch watch = {.accepted = true, .queuedMs = 512, .runMs = 555012};
	ManifestOutput outputs[] = {
		{.role = "results",
			.name = "r.csv",
			.read = true,
			.bytes = 46890,
			.rows = 1000,
			.sha256 = TEST_HASH("c3"),
			.contentSha256 = TEST_HASH("d4")},
		{.role = "match_players",
			.name = "r-match-players.csv",
			.read = true,
			.bytes = 6284112,
			.rows = 40000,
			.sha256 = TEST_HASH("e5")},
		{.role = "match_teams", .name = "r-match-teams.csv"},
	};
	ResultsSummary summary = {.matches = 1000,
		.home = 559,
		.draw = 287,
		.away = 154,
		.homeGoals = 1688,
		.awayGoals = 1041,
		.totalMs = 2201400.0};
	RunManifest manifest = {.createdAt = 1761384198,
		.outcome = RUN_OK,
		.submitted = true,
		.clientOutcome = CLIENT_REPLIED,
		.result = TEST_RESULT_LINE,
		.command = command,
		.commandCount = sizeof command / sizeof command[0],
		.commit = TEST_COMMIT,
		.dirty = 0,
		.slice = "arm64",
		.host = &host,
		.report = &report,
		.game = &game,
		.fixturePath = "/Users/you/fm24/next.json",
		.fixture = &fixture,
		.job = &job,
		.startedAt = 1761383643,
		.finishedAt = 1761384198,
		.watch = &watch,
		.outputs = outputs,
		.outputCount = sizeof outputs / sizeof outputs[0],
		.summary = &summary};
	ByteBuffer text = {0};
	checkManifestText(formatManifest(&manifest, &text), runManifest);
	// A harness built from another commit, or with changes, is not this build's.
	report.dirty = 1;
	ASSERT_BOOL_EQ(
		strstr(formatManifest(&manifest, &text), "\"harness_matches_fm24ctl\":false,") != NULL, true
	);
	report.dirty = 0;
	manifest.commit = "0000000000000000000000000000000000000000";
	ASSERT_BOOL_EQ(
		strstr(formatManifest(&manifest, &text), "\"harness_matches_fm24ctl\":false,") != NULL, true
	);
	// A run that never went to the harness has nothing it would have told.
	static const char *const unconverted[] = {"run", "next.bin", NULL};
	HostInfo windows = {.logicalCpus = 16, .memoryBytes = 34359738368U, .os = "Windows"};
	windows.translated = true;
	FixtureFacts unread = {0};
	RunJob plain = {.input = "C:\\fm24\\next.bin",
		.seeds = "keep",
		.output = "C:\\fm24\\next.csv",
		.threads = 15,
		.profile = "C:\\fm24\\samples.txt",
		.detailed = true,
		.humanControl = true,
		.skipSeedErrors = true};
	RunManifest unsubmitted = {.createdAt = 1761384198,
		.outcome = RUN_NOT_SUBMITTED,
		.exitCode = 3,
		.clientOutcome = CLIENT_REPLIED,
		.result = "",
		.command = unconverted,
		.commandCount = 3,
		.commit = "unknown",
		.dirty = -1,
		.slice = "x86_64",
		.host = &windows,
		.fixturePath = "C:\\fm24\\next.bin",
		.fixture = &unread,
		.job = &plain,
		.noLaunch = true,
		.anySave = true,
		.watch = &watch};
	checkManifestText(formatManifest(&unsubmitted, &text), unsubmittedManifest);
	buffer_free(&text);
}

TEST(decides_a_runs_outcome) {
	ClientWatch taken = {.accepted = true};
	ClientWatch queued = {.accepted = false};
	RunReport stopped = {.stopped = true};
	RunReport failed = {.stopped = true, .hasFailure = true};
	RunReport unwritten = {.stopped = true, .outputFailed = true};
	static const char stopLine[] = "error: 412/1000 runs, stopped by the stop file";
	ASSERT_INT_EQ(manifest_runOutcome(NULL, CLIENT_REPLIED, "ok", NULL), RUN_NOT_SUBMITTED);
	ASSERT_INT_EQ(manifest_runOutcome(&queued, CLIENT_QUEUE_TIMEOUT, "", NULL), RUN_NOT_SUBMITTED);
	ASSERT_INT_EQ(manifest_runOutcome(&queued, CLIENT_NO_HARNESS, "", NULL), RUN_NOT_SUBMITTED);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_LOST, "", NULL), RUN_ERROR);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_REPLIED, "ok: 8/8 runs", NULL), RUN_OK);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_REPLIED, stopLine, &stopped), RUN_STOPPED);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_REPLIED, stopLine, &failed), RUN_ERROR);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_REPLIED, stopLine, &unwritten), RUN_ERROR);
	ASSERT_INT_EQ(manifest_runOutcome(&taken, CLIENT_REPLIED, stopLine, NULL), RUN_ERROR);
	static const char *const names[] = {
		"replied", "no_harness", "queue_timeout", "run_timeout", "lost", "abandoned", "failed"};
	for (int i = CLIENT_REPLIED; i <= CLIENT_FAILED; ++i) {
		ASSERT_INT_EQ(strcmp(manifest_clientOutcomeName((ClientOutcome)i), names[i]), 0);
	}
}

// A file in the temporary directory named for this process and `name`.
static bool temporaryPath(const wchar_t *name, wchar_t *out, size_t size) {
#ifdef _WIN32
	DWORD length = GetTempPathW((DWORD)size, out);
	if (!length || length >= size) {
		return false;
	}
#else
	size_t length = wcslen(wcscpy(out, L"/tmp/"));
#endif
	int written = swprintf(out + length, size - length, L"fm24-%lu-%ls", platform_processId(), name);
	return written > 0 && (size_t)written < size - length;
}

TEST(reads_the_files_a_run_wrote) {
	wchar_t path[1100];
	ASSERT_BOOL_EQ(temporaryPath(L"results.csv", path, 1100), true);
	// The header, three results, a row that is no result, and a last line cut short.
	static const char rows[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
														 "5,2,1,21600,1,0123456789abcdef,2000.5\n"
														 "6,0,0,21600,0,fedcba9876543210,1999.5\n"
														 "7,1,3,21600,1,00000000000000ff,2100\n"
														 "9,1,0,21600,2,0000000000000000,10\n"
														 "8,4,4,21";
	static const char content[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final\n"
																"5,2,1,21600,1,0123456789abcdef\n"
																"6,0,0,21600,0,fedcba9876543210\n"
																"7,1,3,21600,1,00000000000000ff\n"
																"9,1,0,21600,2,0000000000000000\n"
																"8,4,4";
	ASSERT_BOOL_EQ(files_write(path, rows, sizeof rows - 1), true);
	ManifestOutput output = {.role = "results"};
	ResultsSummary summary;
	ASSERT_BOOL_EQ(manifest_readOutput(path, &output, &summary), true);
	char hash[SHA256_HEX_LENGTH];
	sha256_hexOf(rows, sizeof rows - 1, hash);
	ASSERT_BOOL_EQ(output.read && output.bytes == sizeof rows - 1 && output.rows == 4, true);
	ASSERT_INT_EQ(strcmp(output.sha256, hash), 0);
	sha256_hexOf(content, sizeof content - 1, hash);
	ASSERT_INT_EQ(strcmp(output.contentSha256, hash), 0);
	ASSERT_BOOL_EQ(summary.matches == 3 && summary.home == 1 && summary.draw == 1, true);
	ASSERT_BOOL_EQ(summary.away == 1 && summary.homeGoals == 3 && summary.awayGoals == 4, true);
	ASSERT_BOOL_EQ(summary.unfinished == 1 && summary.totalMs == 6100.0, true);
	// Lines across the reads' blocks, and a file that is not results: only its size, rows and hash.
	ByteBuffer many = {0};
	ByteBuffer kept = {0};
	bool built = buffer_append(&many, rows, strcspn(rows, "\n") + 1) &&
							 buffer_append(&kept, content, strcspn(content, "\n") + 1);
	for (unsigned seed = 0; built && seed < 5000; ++seed) {
		char line[96];
		int length = snprintf(line, sizeof line, "%u,%u,2,21600,1,%016x", seed, seed % 4, seed);
		built = buffer_append(&kept, line, (size_t)length) && buffer_append(&kept, "\n", 1) &&
						buffer_append(&many, line, (size_t)length) && buffer_append(&many, ",2000.25\n", 9);
	}
	ASSERT_BOOL_EQ(built && many.length > 3 * 65536, true);
	ASSERT_BOOL_EQ(files_write(path, many.data, many.length), true);
	ASSERT_BOOL_EQ(manifest_readOutput(path, &output, &summary), true);
	sha256_hexOf(kept.data, kept.length, hash);
	ASSERT_BOOL_EQ(output.rows == 5000 && strcmp(output.contentSha256, hash) == 0, true);
	ASSERT_BOOL_EQ(summary.matches == 5000 && summary.home == 1250 && summary.draw == 1250, true);
	ASSERT_BOOL_EQ(summary.away == 2500 && summary.totalMs == 5000 * 2000.25, true);
	ASSERT_BOOL_EQ(manifest_readOutput(path, &output, NULL), true);
	sha256_hexOf(many.data, many.length, hash);
	ASSERT_BOOL_EQ(output.rows == 5000 && output.bytes == many.length, true);
	ASSERT_BOOL_EQ(strcmp(output.sha256, hash) == 0 && !output.contentSha256[0], true);
	buffer_free(&many);
	buffer_free(&kept);
	// An empty file, and none at all.
	ASSERT_BOOL_EQ(files_write(path, "", 0), true);
	ASSERT_BOOL_EQ(manifest_readOutput(path, &output, &summary), true);
	ASSERT_BOOL_EQ(output.read && !output.bytes && !output.rows && !summary.matches, true);
	ASSERT_BOOL_EQ(platform_removeFile(path), true);
	ASSERT_BOOL_EQ(manifest_readOutput(path, &output, &summary), false);
	ASSERT_BOOL_EQ(!output.read && !output.sha256[0], true);
}

TEST(describes_the_computer) {
	HostInfo host;
	hostInfo_read(&host);
	ASSERT_BOOL_EQ(host.logicalCpus == platform_processorCount(), true);
	ASSERT_BOOL_EQ(host.memoryBytes >= (1ULL << 30), true);
#ifdef _WIN32
	ASSERT_INT_EQ(strcmp(host.os, "Windows"), 0);
	ASSERT_BOOL_EQ(!host.model[0] && !host.translated, true);
#else
	ASSERT_BOOL_EQ(host.model[0] && host.cpu[0] && host.osBuild[0], true);
	ASSERT_BOOL_EQ(
		strncmp(host.os, "macOS ", 6) == 0 && host.os[6] >= '1' && host.os[6] <= '9', true
	);
	int translated = 0;
	size_t size = sizeof translated;
	if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0) {
		translated = 0;
	}
	ASSERT_BOOL_EQ(host.translated, translated == 1);
	// Apple silicon has performance and efficiency cores, Intel neither.
	int arm64 = 0;
	size = sizeof arm64;
	bool appleSilicon = sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64;
	ASSERT_BOOL_EQ(
		appleSilicon ? host.performanceCores > 0 &&
										 host.performanceCores + host.efficiencyCores <= host.logicalCpus
								 : true,
		true
	);
#endif
}

#ifndef _WIN32
// The manifest `run` wrote at `path`, in `out` ("" if there is none); removes it.
static const char *takeManifest(const char *path, ByteBuffer *out) {
	buffer_clear(out);
	FILE *file = fopen(path, "rb");
	char chunk[4096];
	size_t got = 0;
	while (
		file && (got = fread(chunk, 1, sizeof chunk, file)) > 0 && buffer_append(out, chunk, got)) {
	}
	if (file) {
		(void)fclose(file);
		(void)unlink(path);
	}
	return buffer_string(out);
}

// As runPrinting, also writing what `run` printed to stderr to `errors`.
static int runPrintingErrors(
	int argc,
	wchar_t **arguments,
	char *out,
	size_t size,
	char *errors,
	size_t errorsSize
) {
	char printed[] = "/tmp/fm24-run-errors-XXXXXX";
	int file = mkstemp(printed);
	int saved = dup(STDERR_FILENO);
	int status = -1;
	errors[0] = 0;
	if (file >= 0 && saved >= 0 && dup2(file, STDERR_FILENO) >= 0) {
		status = runPrinting(argc, arguments, out, size);
		(void)dup2(saved, STDERR_FILENO);
	}
	if (saved >= 0) {
		close(saved);
	}
	if (file >= 0) {
		ssize_t length = pread(file, errors, errorsSize - 1, 0);
		errors[length > 0 ? length : 0] = 0;
		close(file);
		(void)unlink(printed);
	}
	return status;
}

// True if `text` has each of `parts`, which end with NULL; says which one it lacks if not.
static bool hasAll(const char *text, const char *const *parts) {
	for (size_t i = 0; parts[i]; ++i) {
		if (!strstr(text, parts[i])) {
			fprintf(stderr, "no %s in:\n%s\n", parts[i], text);
			return false;
		}
	}
	return true;
}

// `run --manifest` writes <stem>-manifest.json whatever happens once its paths are known: after a
// reply, after no reply, and when the job never went to the harness.
TEST(writes_a_manifest_for_every_run) {
	char folder[] = "/tmp/fm24-manifest-XXXXXX";
	char csv[96];
	char players[96];
	char teams[96];
	char manifestPath[96];
	char bin[96]; // the input, which run checks is there
	wchar_t output[96];
	wchar_t input[96];
	bool ok = mkdtemp(folder) && snprintf(csv, sizeof csv, "%s/out.csv", folder) > 0 &&
						snprintf(players, sizeof players, "%s/out-match-players.csv", folder) > 0 &&
						snprintf(teams, sizeof teams, "%s/out-match-teams.csv", folder) > 0 &&
						snprintf(manifestPath, sizeof manifestPath, "%s/out-manifest.json", folder) > 0 &&
						snprintf(bin, sizeof bin, "%s/next.bin", folder) > 0 &&
						platform_fromUtf8(csv, output, 96) && platform_fromUtf8(bin, input, 96);
	static const char rows[] = "seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n"
														 "5,2,1,21600,1,0123456789abcdef,2000.0\n"
														 "6,0,0,21600,1,0123456789abcdef,3000.0\n";
	static const char playerRows[] = "index,seed\n0,5\n0,5\n1,6\n";
	ok = ok && files_write(output, rows, sizeof rows - 1);
	wchar_t wide[96];
	ok = ok && platform_fromUtf8(players, wide, 96) &&
			 files_write(wide, playerRows, sizeof playerRows - 1) && platform_fromUtf8(teams, wide, 96) &&
			 files_write(wide, "index,seed\n", 11) && files_write(input, "", 0);
	ASSERT_BOOL_EQ(ok, true);
	if (!ok) {
		return;
	}
	char rowsHash[SHA256_HEX_LENGTH];
	sha256_hexOf(rows, sizeof rows - 1, rowsHash);
	char rowsPart[128];
	(void)snprintf(
		rowsPart,
		sizeof rowsPart,
		"{\"role\":\"results\",\"path\":\"out.csv\",\"bytes\":%zu,\"rows\":2,\"sha256\":\"%s\"",
		sizeof rows - 1,
		rowsHash
	);
	char inputHash[SHA256_HEX_LENGTH];
	sha256_hexOf(plainConfig, strlen(plainConfig), inputHash);
	char fixturePart[320];
	(void)snprintf(
		fixturePart,
		sizeof fixturePart,
		"\"fixture\":{\"path\":\"%s\",\"bytes\":%zu,\"sha256\":\"%s\",\"input_bytes\":%zu,"
		"\"input_sha256\":\"%s\",\"header\":null}",
		bin,
		strlen(plainConfig),
		inputHash,
		strlen(plainConfig),
		inputHash
	);
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	testGame = 4242;
	char commandPart[320];
	(void)snprintf(
		commandPart,
		sizeof commandPart,
		"\"command\":[\"run\",\"%s\",\"5-12\",\"%s\",\"--per-match\",\"--manifest\",\"--threads\","
		"\"2\"],",
		bin,
		csv
	);
	wchar_t *arguments[] = {L"fm24ctl",
		L"run",
		input,
		L"5-12",
		output,
		L"--per-match",
		L"--manifest",
		L"--threads",
		L"2",
		NULL};
	char printed[4096];
	ByteBuffer manifest = {0};
	// A run that played every seed.
	static const char okReply[] =
		"ok: 8/8 runs, each seed rebuilt by the game; report protocol=1.4.0 commit=" TEST_COMMIT
		" dirty=0 build=2081827 slice=arm64 translated=0 pid=4242 loaded=1761382682 jobs=3 threads=2 "
		"edits=0 elapsed_ms=4000 written=8 skipped=0 stopped=0 failed_index=- failed_stage=- "
		"output_failed=0";
	jobReply = okReply;
	char errors[2048];
	ASSERT_INT_EQ(
		runPrintingErrors(9, arguments, printed, sizeof printed, errors, sizeof errors), EXIT_OK
	);
	ASSERT_BOOL_EQ(strstr(submittedJob, "\nper_match=1\nreport=1\n") != NULL, true);
	// The game's harness is from another commit than fm24ctl: a warning to restart the game, unless
	// fm24ctl's own commit is not known.
	ASSERT_BOOL_EQ(
		strstr(errors, "warning: the game's harness was built from e74df0b3c1a2, fm24ctl from ") !=
			NULL,
		buildInfo_dirty >= 0
	);
	char line[160];
	(void)snprintf(line, sizeof line, "\nmanifest: %s\n", manifestPath);
	ASSERT_BOOL_EQ(strstr(printed, line) != NULL, true);
	const char *const played[] = {
		"{\"format\":\"fm24-run-manifest/1\",\"outcome\":\"ok\",\"exit_code\":0,"
		"\"client_outcome\":\"replied\",\"result_line\":\"ok: 8/8 runs,",
		commandPart,
		"\"harness\":{\"protocol\":\"1.4.0\",\"commit\":\"" TEST_COMMIT "\",\"dirty\":false,",
		"\"pid\":4242,\"loaded\":\"2025-10-25T08:58:02Z\",\"jobs\":3,\"threads\":2,\"edits\":0,",
		"\"game\":{\"pid\":4242,\"under_rosetta\":true,\"save\":null,\"date\":null,"
		"\"rss_bytes\":6123456789},",
		fixturePart,
		"\"seeds\":{\"spec\":\"5-12\",\"count\":8,\"first\":5,\"last\":12,\"index_base\":0,"
		"\"sha256\":\"a8b5679e6e92cae6efef3dd182d72e85e5e2e85cecee745a0dde32ff6a1054f2\"}",
		"\"flags\":{\"threads\":2,\"per_seed_rebuild\":true,\"keep_build\":false,"
		"\"human_control\":false,\"detailed\":false,\"per_match\":true,\"seed_errors\":\"stop\","
		"\"profile\":null,\"no_launch\":false,\"any_save\":false}",
		"\"queue_ms\":12,\"run_ms\":4000,\"slept_ms\":0,\"matches_per_s\":2.00,"
		"\"match_ms_mean\":2500.0}",
		rowsPart,
		"{\"role\":\"match_players\",\"path\":\"out-match-players.csv\",\"bytes\":23,\"rows\":3,",
		"{\"role\":\"match_teams\",\"path\":\"out-match-teams.csv\",\"bytes\":11,\"rows\":0,",
		"\"summary\":{\"matches\":2,\"home\":1,\"draw\":1,\"away\":0,\"home_goals\":2,"
		"\"away_goals\":1,\"unfinished\":0,\"skipped\":0}}\n",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), played), true);
	ASSERT_BOOL_EQ(strstr(buffer_string(&manifest), "\"harness_matches_fm24ctl\":") != NULL, true);
	// A harness from fm24ctl's own commit: no warning, and the manifest says they match.
	char sameReply[512];
	(void)snprintf(
		sameReply,
		sizeof sameReply,
		"ok: 8/8 runs, each seed rebuilt by the game; report protocol=1.4.0 commit=%s dirty=%s "
		"build=2081827 slice=arm64 translated=0 pid=4242 loaded=1761382682 jobs=3 threads=2 edits=0 "
		"elapsed_ms=4000 written=8 skipped=0 stopped=0 failed_index=- failed_stage=- output_failed=0",
		buildInfo_commit,
		buildInfo_dirty < 0 ? "-"
		: buildInfo_dirty   ? "1"
												: "0"
	);
	jobReply = sameReply;
	ASSERT_INT_EQ(
		runPrintingErrors(9, arguments, printed, sizeof printed, errors, sizeof errors), EXIT_OK
	);
	ASSERT_BOOL_EQ(strstr(errors, "warning: the game's harness") == NULL, true);
	const char *const same[] = {
		buildInfo_dirty < 0 ? "\"harness_matches_fm24ctl\":null," : "\"harness_matches_fm24ctl\":true,",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), same), true);
	// One stopped by its stop file, and one whose seed failed: the files the job wrote are listed.
	static const struct {
		const char *reply;
		const char *outcome;
	} ended[] = {
		{"error: 3/8 runs, stopped by the stop file, each seed rebuilt by the game; report "
		 "protocol=1.4.0 commit=unknown dirty=- build=2081827 slice=arm64 translated=0 pid=4242 "
		 "loaded=0 jobs=3 threads=2 edits=0 elapsed_ms=4000 written=3 skipped=0 stopped=1 "
		 "failed_index=- failed_stage=- output_failed=0",
			"\"outcome\":\"stopped\",\"exit_code\":1,"},
		{"error: 3/8 runs, each seed rebuilt by the game; see harness.log; report protocol=1.4.0 "
		 "commit=unknown dirty=- build=2081827 slice=arm64 translated=0 pid=4242 loaded=0 jobs=3 "
		 "threads=2 edits=0 elapsed_ms=4000 written=3 skipped=0 stopped=0 failed_index=3 "
		 "failed_stage=deserialize output_failed=0",
			"\"outcome\":\"error\",\"exit_code\":1,"},
	};
	for (size_t i = 0; i < sizeof ended / sizeof ended[0]; ++i) {
		jobReply = ended[i].reply;
		ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
		const char *const parts[] = {ended[i].outcome,
			"\"harness_matches_fm24ctl\":null,",
			"\"loaded\":null,",
			"\"matches_per_s\":0.75,",
			rowsPart,
			"\"role\":\"match_teams\"",
			NULL};
		ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), parts), true);
	}
	// No reply, or none that has a report: the files there may be an earlier run's.
	jobReply = "error: cannot start the run; see harness.log";
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
	const char *const unreported[] = {"\"outcome\":\"error\",\"exit_code\":1,\"client_outcome\":"
																		"\"replied\",\"result_line\":\"error: cannot start the run",
		"\"harness\":null,\"harness_matches_fm24ctl\":null,",
		"\"run_ms\":4000,\"slept_ms\":0,\"matches_per_s\":null,\"match_ms_mean\":null},"
		"\"outputs\":[],\"summary\":null}",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), unreported), true);
	jobReply = okReply; // a reply that is lost does not count
	watchedOutcome = CLIENT_LOST;
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
	const char *const lost[] = {"\"outcome\":\"error\",\"exit_code\":1,\"client_outcome\":\"lost\","
															"\"result_line\":null,",
		"\"harness\":null,",
		"\"outputs\":[],\"summary\":null}",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), lost), true);
	// The harness did not take the job.
	watchedOutcome = CLIENT_QUEUE_TIMEOUT;
	testGame = 0;
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
	const char *const queued[] = {"\"outcome\":\"not_submitted\",\"exit_code\":1,"
																"\"client_outcome\":\"queue_timeout\",\"result_line\":null,",
		"\"game\":{\"pid\":null,\"under_rosetta\":null,\"save\":null,\"date\":null,"
		"\"rss_bytes\":null},",
		"\"queue_ms\":12,\"run_ms\":null,",
		"\"outputs\":[]",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), queued), true);
	watchedOutcome = CLIENT_REPLIED;
	// The game has another save loaded: the job is never submitted.
	arguments[2] = L"/fm24/next.json";
	configText = "{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,\"save\":\"LIV\","
							 "\"game_date\":\"2024 day 1 09:00\",\"fields\":[{\"u8\":42}],\"away_team\":677}";
	ensureResult = EXIT_WRONG_STATE;
	ensureDate = "";
	submittedJob[0] = 0;
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_WRONG_STATE);
	ASSERT_INT_EQ(submittedJob[0], 0);
	char encodedHash[SHA256_HEX_LENGTH];
	sha256_hexOf("\x2a", 1, encodedHash);
	char encodedPart[160];
	(void)snprintf(
		encodedPart, sizeof encodedPart, "\"input_bytes\":1,\"input_sha256\":\"%s\",", encodedHash
	);
	const char *const refused[] = {"\"outcome\":\"not_submitted\",\"exit_code\":3,"
																 "\"client_outcome\":null,\"result_line\":null,",
		"\"save\":\"LIV\",\"date\":null,",
		"\"fixture\":{\"path\":\"/fm24/next.json\",",
		encodedPart,
		"\"header\":{\"format\":\"fm24-match-config/1\",\"game_build\":2081827,"
		"\"engine_version\":null,\"source\":null,\"save\":\"LIV\",\"game_date\":\"2024 day 1 "
		"09:00\",\"home_team\":null,\"away_team\":677}}",
		"\"timing\":{\"started_at\":null,\"accepted_at\":null,\"finished_at\":null,\"queue_ms\":null,",
		"\"outputs\":[],\"summary\":null}",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), refused), true);
	ensureResult = EXIT_OK;
	// A fixture that does not encode.
	configText = "{\"format\":\"fm24-match-config/1\",\"fields\":[{\"u8\":256}]}";
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
	const char *const unencoded[] = {"\"outcome\":\"not_submitted\",\"exit_code\":1,",
		"\"input_bytes\":null,\"input_sha256\":null,\"header\":{\"format\":\"fm24-match-config/1\",",
		NULL};
	ASSERT_BOOL_EQ(hasAll(takeManifest(manifestPath, &manifest), unencoded), true);
	configText = plainConfig;
	arguments[2] = input;
	// Seeds too long for the harness stop run with its usage before anything else (see
	// checkSeedPattern), so no manifest either.
	wchar_t seeds[301];
	for (size_t i = 0; i < 300; ++i) {
		seeds[i] = i % 2 ? L',' : L'7';
	}
	seeds[299] = L'7';
	seeds[300] = 0;
	arguments[3] = seeds;
	submittedJob[0] = 0;
	ASSERT_INT_EQ(
		runPrintingErrors(9, arguments, printed, sizeof printed, errors, sizeof errors), EXIT_USAGE
	);
	ASSERT_BOOL_EQ(!submittedJob[0] && access(manifestPath, F_OK) != 0, true);
	arguments[3] = L"5-12";
	// A manifest that cannot be written fails the run; without --manifest there is none.
	jobReply = okReply;
	ASSERT_BOOL_EQ(mkdir(manifestPath, 0700) == 0, true);
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_FAILED);
	ASSERT_BOOL_EQ(strstr(printed, "manifest:") == NULL, true);
	ASSERT_BOOL_EQ(rmdir(manifestPath) == 0, true);
	jobReply = "ok: 8/8 runs, each seed rebuilt by the game";
	arguments[6] = L"--detailed";
	ASSERT_INT_EQ(runPrinting(9, arguments, printed, sizeof printed), EXIT_OK);
	ASSERT_BOOL_EQ(strstr(submittedJob, "report=") == NULL, true);
	ASSERT_BOOL_EQ(access(manifestPath, F_OK) != 0 && strstr(printed, "manifest:") == NULL, true);
	buffer_free(&manifest);
	buffer_free(&writtenConfig);
	jobReply = "ok";
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	testGame = 0;
	ensureDate = "";
	(void)unlink(csv);
	(void)unlink(players);
	(void)unlink(teams);
	(void)unlink(bin);
	(void)rmdir(folder);
}
#endif

// ---------------------------------------------------------------------------------------------
// lineup, and run's warnings for an edited line-up

#ifdef _WIN32
enum { PRINTS_CAPTURED = 0 }; // the suite does not capture what commands print on Windows
#else
enum { PRINTS_CAPTURED = 1 };
#endif

static ByteBuffer lineupText; // what file reads return once makeLineupSample has made it

// Makes the lineup sample, with `header` (keys, each with its comma) after its format, what every
// file read returns.
static bool makeLineupSample(const char *header) {
	static const char format[] = "\"format\": \"fm24-match-config/1\",";
	ByteBuffer sample = {0};
	buffer_clear(&lineupText);
	bool ok = fixtureSamples_lineup(&sample);
	const char *text = buffer_string(&sample);
	const char *after = ok ? strstr(text, format) : NULL;
	ok = after &&
			 buffer_append(&lineupText, text, (size_t)(after - text) + sizeof format - 1) &&
			 buffer_append(&lineupText, header, strlen(header)) &&
			 buffer_append(
				 &lineupText,
				 after + sizeof format - 1,
				 sample.length - (size_t)(after - text) - (sizeof format - 1)
			 );
	buffer_free(&sample);
	configText = ok ? buffer_string(&lineupText) : plainConfig;
	ASSERT_BOOL_EQ(ok, true);
	return ok;
}

// Replaces every `old` in `text`; how many it replaced.
static unsigned replaceAll(ByteBuffer *text, const char *old, const char *new) {
	ByteBuffer out = {0};
	const char *at = buffer_string(text);
	const char *end = at + text->length;
	unsigned replaced = 0;
	for (const char *found = strstr(at, old); found; found = strstr(at, old)) {
		(void)buffer_append(&out, at, (size_t)(found - at));
		(void)buffer_append(&out, new, strlen(new));
		at = found + strlen(old);
		++replaced;
	}
	(void)buffer_append(&out, at, (size_t)(end - at));
	buffer_free(text);
	*text = out;
	return replaced;
}

// As invoke, with what the command printed to stdout and stderr in `out` and `errors` (both "" on
// Windows).
static int invokePrinting(
	Command *command,
	wchar_t **arguments,
	char *out,
	size_t size,
	char *errors,
	size_t errorsSize
) {
	out[0] = 0;
	errors[0] = 0;
#ifdef _WIN32
	(void)size;
	(void)errorsSize;
	return invoke(command, arguments);
#else
	char printed[] = "/tmp/fm24-lineup-out-XXXXXX";
	char failed[] = "/tmp/fm24-lineup-errors-XXXXXX";
	int file = mkstemp(printed);
	int errorFile = mkstemp(failed);
	(void)fflush(stdout);
	int savedOut = dup(STDOUT_FILENO);
	int savedErrors = dup(STDERR_FILENO);
	int status = -1;
	if (file >= 0 && errorFile >= 0 && savedOut >= 0 && savedErrors >= 0 &&
			dup2(file, STDOUT_FILENO) >= 0 && dup2(errorFile, STDERR_FILENO) >= 0) {
		status = invoke(command, arguments);
		(void)fflush(stdout);
	}
	(void)dup2(savedOut, STDOUT_FILENO);
	(void)dup2(savedErrors, STDERR_FILENO);
	const int files[2] = {file, errorFile};
	char *const texts[2] = {out, errors};
	const size_t sizes[2] = {size, errorsSize};
	for (int i = 0; i < 2; ++i) {
		if (files[i] >= 0) {
			ssize_t length = pread(files[i], texts[i], sizes[i] - 1, 0);
			texts[i][length > 0 ? length : 0] = 0;
			close(files[i]);
		}
	}
	(void)unlink(printed);
	(void)unlink(failed);
	if (savedOut >= 0) {
		close(savedOut);
	}
	if (savedErrors >= 0) {
		close(savedErrors);
	}
	return status;
#endif
}

// True if the command printed `expected`, or the suite cannot tell; says what it printed if not.
static bool printedAs(const char *printed, const char *expected) {
	bool same = !PRINTS_CAPTURED || strcmp(printed, expected) == 0;
	if (!same) {
		fprintf(stderr, "printed:\n%s\nexpected:\n%s\n", printed, expected);
	}
	return same;
}

// The fixture lineup wrote, read back (NULL if it does not read).
static FixtureData *readWritten(ConfigOrigin *origin) {
	FixtureData *data = malloc(sizeof *data);
	char error[256] = "";
	const char *text = buffer_string(&writtenConfig);
	bool ok = data && fixtureData_read(text, writtenConfig.length, data, error, sizeof error) &&
						matchConfig_readOrigin(text, writtenConfig.length, origin);
	ASSERT_BOOL_EQ(ok, true);
	if (!ok) {
		fprintf(stderr, "%s\n", error);
		free(data);
		data = NULL;
	}
	return data;
}

static const char lineupNote[] =
	"note: play it with keep or --keep-build; rebuilding each seed puts FM24's own line-up back\n";
static const char homeXi[] = "XI: Alisson Becker, Conor Bradley, Virgil van Dijk, Ibrahima Konate, "
														 "Andrew Robertson, Wataru Endo, Alexis Mac Allister, Dominik "
														 "Szoboszlai, Mohamed Salah, Cody Gakpo, ";

// Its usage errors stop lineup with exit code 2 before it reads the input: a wrong number of
// arguments, a side that is not home or away, an input or output that is not JSON, a malformed
// place or one of the other side, an empty player or list item, a list of one to ten players, and
// an unknown or repeated option.
TEST(lineup_refuses_malformed_arguments_before_reading) {
	static const struct {
		wchar_t *arguments[10];
		const char *reason; // what comes before the usage
	} cases[] = {
		{{L"fm24ctl", L"lineup"}, ""},
		{{L"fm24ctl", L"lineup", L"next.json"}, ""},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"Wirtz"}, ""},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a", L"b", L"out.json", L"more.json"}, ""},
		{{L"fm24ctl", L"lineup", L"next.json", L"Home"}, "the side is home or away, not Home\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"both", L"a", L"b", L"out.json"},
			"the side is home or away, not both\n"},
		{{L"fm24ctl", L"lineup", L"next.bin", L"home"},
			"lineup reads fixture JSON; the input must end in .json, not next.bin\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a", L"b", L"out.bin"},
			"lineup writes fixture JSON; the output must end in .json, not out.bin\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[x]", L"b", L"out.json"},
			"'player[x]' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a", L"player[]", L"out.json"},
			"'player[]' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L" player[10 ", L"b", L"out.json"},
			"'player[10' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"home.player[1]]", L"b", L"out.json"},
			"'home.player[1]]' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"away", L"player[1234567890]", L"b", L"out.json"},
			"'player[1234567890]' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[3]", L"away.player[2]", L"out.json"},
			"away.player[2] is not on the home side\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"", L"b", L"out.json"},
			"a player is empty: give a name, an ID or player[N]\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a", L" \t", L"out.json"},
			"a player is empty: give a name, an ID or player[N]\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"Wirtz", L"out.json"},
			"give two players to exchange, or the XI as a list\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a,b,c,d,e,f,g,h,i,j", L"out.json"},
			"the list names 10 players: give the XI in slot order, then any substitutes\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a,b,c,d,e,f,g,h,i,j,k,", L"out.json"},
			"the list has an empty item: name a player between each two commas\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a,b,c,d, ,f,g,h,i,j,k", L"out.json"},
			"the list has an empty item: name a player between each two commas\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a,b,c,d,e,f,g,h,i,j,player[k]", L"out.json"},
			"'player[k]' is not a place: use player[N], counting from 0\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", L"a", L"b", L"out.json", L"--full"},
			"unknown option: --full\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"--allow-duplicate", L"home", L"a", L"b", L"out.json"},
			"unknown option: --allow-duplicate\n"},
		{{L"fm24ctl",
			 L"lineup",
			 L"next.json",
			 L"away",
			 L"a",
			 L"b",
			 L"out.json",
			 L"--allow-ai-side",
			 L"--allow-ai-side"},
			"--allow-ai-side is given twice\n"},
		{{L"fm24ctl", L"lineup", L"next.json", L"away", L"--allow-ai-side"},
			"--allow-ai-side applies only to a new line-up\n"},
	};
	static const char usage[] = "usage: fm24ctl lineup <input.json> <home|away>\n"
															"       fm24ctl lineup <input.json> <home|away> <player> <player> "
															"<output.json> [--allow-ai-side]\n";
	char printed[1024];
	char errors[2048];
	buffer_clear(&writtenConfig);
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		configReads = 0;
		int status =
			invokePrinting(commands_lineup, (wchar_t **)cases[i].arguments, printed, sizeof printed, errors, sizeof errors);
		ASSERT_INT_EQ(status, EXIT_USAGE);
		ASSERT_INT_EQ((int)configReads, 0);
		size_t reason = strlen(cases[i].reason);
		bool explained = !PRINTS_CAPTURED || (strncmp(errors, cases[i].reason, reason) == 0 &&
																					 strncmp(errors + reason, usage, sizeof usage - 1) == 0);
		ASSERT_BOOL_EQ(explained, true);
		if (!explained) {
			fprintf(stderr, "case %zu printed:\n%s\n", i, errors);
		}
		ASSERT_BOOL_EQ(printedAs(printed, ""), true);
	}
	ASSERT_INT_EQ((int)writtenConfig.length, 0);
}

// The first form prints the side in place order: its slots, substitutes and reserves, and says when
// FM24's AI picks the side's XI.
TEST(lineup_shows_a_side) {
	if (!makeLineupSample("")) {
		return;
	}
	static const char home[] = "place\tslot\tposition\tplayer\tperson_uid\n"
														 "0\t0\tGK\tAlisson Becker\t19058734\n"
														 "1\t1\tDR\tConor Bradley\t28127166\n"
														 "2\t2\tDCR\tVirgil van Dijk\t37036372\n"
														 "3\t3\tDCL\tIbrahima Konate\t85028014\n"
														 "4\t4\tDL\tAndrew Robertson\t28044813\n"
														 "5\t5\tDMCR\tWataru Endo\t29056345\n"
														 "6\t6\tDMCL\tAlexis Mac Allister\t43219372\n"
														 "7\t7\tAMR\tDominik Szoboszlai\t67236018\n"
														 "8\t8\tAMC\tMohamed Salah\t98028753\n"
														 "9\t9\tAML\tCody Gakpo\t37060815\n"
														 "10\t10\tST\tRyan Gravenberch\t37071197\n"
														 "11\tsub\t\tCaoimhin Kelleher\t28104130\n"
														 "12\tsub\t\tJoe Gomez\t28076016\n"
														 "13\tsub\t\tTrent Alexander-Arnold\t28104124\n"
														 "14\tsub\t\tLewis Jones\t28100917\n"
														 "15\tsub\t\tHarvey Elliott\t29233143\n"
														 "16\tsub\t\tJarell Quansah\t2000049283\n"
														 "17\tsub\t\tCurtis Jones\t28115788\n"
														 "18\tsub\t\tLuis D\xC3\xAD" "az\t76049803\n"
														 "19\tsub\t\tFlorian Wirtz\t91193048\n"
														 "20\treserve\t\tBen Doak\t2000164850\n"
														 "21\treserve\t\tErling Haaland\t29179241\n";
	char printed[4096];
	char errors[1024];
	wchar_t *arguments[] = {L"fm24ctl", L"lineup", L"next.json", L"home", NULL};
	buffer_clear(&writtenConfig);
	ASSERT_INT_EQ(
		invokePrinting(commands_lineup, arguments, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(printedAs(printed, home) && printedAs(errors, ""), true);
	arguments[3] = L"away";
	ASSERT_INT_EQ(
		invokePrinting(commands_lineup, arguments, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(
		printedAs(errors, "note: away is managed by FM24's AI, which picks its own XI at kick-off\n"),
		true
	);
	ASSERT_BOOL_EQ(
		!PRINTS_CAPTURED ||
			(strstr(printed, "\n10\t10\tST\tHarvey Barnes\t28100207\n11\tsub\t\tMartin Dubravka\t3020153\n"
											 "12\tsub\t\tLewis Miley\t2000189871\n") != NULL &&
			 strstr(printed, "\n13\t") == NULL),
		true
	);
	ASSERT_INT_EQ((int)writtenConfig.length, 0);
	configText = plainConfig;
}

// The second form exchanges two players, named by place, ID or name: the fixture fixtureLineup_apply
// writes, whose blocks read back in their new places with the marker, and the opponent's marking
// that followed the player. Exchanging them again gives the input back, with the marker.
TEST(lineup_exchanges_two_players) {
	if (!makeLineupSample("")) {
		return;
	}
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	ASSERT_BOOL_EQ(fixtureLineup_exchange(&lineup, 0, 22, 10, 19), true);
	ByteBuffer expected = {0};
	FixtureLineupResult *result = malloc(sizeof *result);
	char error[256] = "";
	bool applied = result && fixtureLineup_apply(
														 configText, strlen(configText), &lineup, &expected, result, error, sizeof error
													 );
	free(result);
	Path output;
	ASSERT_BOOL_EQ(applied && absolutePath(L"out.json", &output), true);
	if (!applied) {
		buffer_free(&expected);
		return;
	}
	char shown[4096];
	(void)snprintf(
		shown,
		sizeof shown,
		"ok: home line-up: Florian Wirtz to player[10] (slot 10, ST), Ryan Gravenberch to player[19] "
		"(sub); wrote %s\n%sFlorian Wirtz\nbench: Caoimhin Kelleher, Joe Gomez, Trent "
		"Alexander-Arnold, Lewis Jones, Harvey Elliott, Jarell Quansah, Curtis Jones, Luis D\xC3\xAD" "az, "
		"Ryan Gravenberch\nalso re-pointed away's marking of Florian Wirtz\n%s",
		output.utf8,
		homeXi,
		lineupNote
	);
	static const wchar_t *const players[][2] = {
		{L"player[10]", L"player[19]"},
		{L"Wirtz", L"home.player[10]"},
		{L" 37071197 ", L"florian WIRTZ"},
		{L"gravenberch", L"91193048"},
	};
	char printed[4096];
	char errors[1024];
	for (size_t i = 0; i < sizeof players / sizeof players[0]; ++i) {
		wchar_t *arguments[] = {L"fm24ctl",
			L"lineup",
			L"next.json",
			L"home",
			(wchar_t *)players[i][0],
			(wchar_t *)players[i][1],
			L"out.json",
			NULL};
		buffer_clear(&writtenConfig);
		ASSERT_INT_EQ(
			invokePrinting(commands_lineup, arguments, printed, sizeof printed, errors, sizeof errors),
			EXIT_OK
		);
		ASSERT_BOOL_EQ(
			writtenConfig.length == expected.length &&
				memcmp(writtenConfig.data, expected.data, expected.length) == 0,
			true
		);
		ASSERT_BOOL_EQ(printedAs(printed, shown) && printedAs(errors, ""), true);
	}
	ConfigOrigin origin;
	FixtureData *data = readWritten(&origin);
	if (data) {
		const FixtureTeam *team = &data->teams[0];
		ASSERT_INT_EQ(strcmp(team->players[10].name, "Florian Wirtz"), 0);
		ASSERT_INT_EQ(strcmp(team->players[19].name, "Ryan Gravenberch"), 0);
		ASSERT_BOOL_EQ(team->blocksInOrder && team->players[10].personUid.value == 91193048, true);
		ASSERT_BOOL_EQ(origin.lineup[0] && !origin.lineup[1], true);
	}
	free(data);
	// Back again: the input, with the marker before "fields".
	buffer_clear(&lineupText);
	ASSERT_BOOL_EQ(buffer_append(&lineupText, writtenConfig.data, writtenConfig.length), true);
	configText = buffer_string(&lineupText);
	wchar_t *back[] = {
		L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json", NULL};
	buffer_clear(&writtenConfig);
	ASSERT_INT_EQ(invoke(commands_lineup, back), EXIT_OK);
	ByteBuffer input = {0};
	ASSERT_BOOL_EQ(fixtureSamples_lineup(&input), true);
	ASSERT_INT_EQ(
		(int)replaceAll(&input, "\n  \"fields\": [", "\n  \"home_lineup\": true,\n  \"fields\": ["), 1
	);
	ASSERT_BOOL_EQ(strcmp(buffer_string(&writtenConfig), buffer_string(&input)) == 0, true);
	// Away's opposition instructions, which stay as they were, are warned of when they differ.
	static const char away[] = "\"away.manager.tactic.part_4240\"";
	char *records = makeLineupSample("") ? strstr((char *)lineupText.data, away) : NULL;
	char *first = records ? strstr(records, "{\"u8\": 1}") : NULL; // the first record's last u8
	ASSERT_BOOL_EQ(first != NULL, true);
	if (first) {
		first[strlen("{\"u8\": ")] = '2';
		ASSERT_INT_EQ(
			invokePrinting(commands_lineup, back, printed, sizeof printed, errors, sizeof errors), EXIT_OK
		);
		ASSERT_BOOL_EQ(
			printedAs(
				errors,
				"warning: away's opposition instructions (part_4240) are not all alike; lineup leaves "
				"them as they were, so they may now apply to other home players\n"
			),
			true
		);
	}
	buffer_free(&input);
	buffer_free(&expected);
	buffer_clear(&writtenConfig);
	configText = plainConfig;
}

// The third form puts a list's players first, in slot order then on the bench, and the players they
// displace in the places they left: one leaving the squad is left out, a bench without its
// goalkeeper is warned of, and a reordered AI side only with --allow-ai-side, anywhere after lineup.
TEST(lineup_puts_a_list_first) {
	if (!makeLineupSample("")) {
		return;
	}
	Path output;
	ASSERT_BOOL_EQ(absolutePath(L"out.json", &output), true);
	static const char bench[] = "Joe Gomez, Trent Alexander-Arnold, Lewis Jones, Harvey Elliott, "
															"Jarell Quansah, Curtis Jones, Luis D\xC3\xAD" "az, ";
	char diaz[2048];
	char haaland[2048];
	char twelve[2048];
	char away[2048];
	(void)snprintf(
		diaz,
		sizeof diaz,
		"ok: home line-up: Luis D\xC3\xAD" "az to player[10] (slot 10, ST), Ryan Gravenberch to "
		"player[18] (sub); wrote %s\n%sLuis D\xC3\xAD" "az\nbench: Caoimhin Kelleher, Joe Gomez, Trent "
		"Alexander-Arnold, Lewis Jones, Harvey Elliott, Jarell Quansah, Curtis Jones, Ryan "
		"Gravenberch, Florian Wirtz\n%s",
		output.utf8,
		homeXi,
		lineupNote
	);
	(void)snprintf(
		haaland,
		sizeof haaland,
		"ok: home line-up: Erling Haaland to player[10] (slot 10, ST), Ryan Gravenberch to "
		"player[21] (reserve); wrote %s\n%sErling Haaland\nbench: Caoimhin Kelleher, %sFlorian "
		"Wirtz\nleft out: Ryan Gravenberch\n%s",
		output.utf8,
		homeXi,
		bench,
		lineupNote
	);
	(void)snprintf(
		twelve,
		sizeof twelve,
		"ok: home line-up: 4 players moved; wrote %s\n%sErling Haaland\nbench: Florian Wirtz, "
		"%sRyan Gravenberch\nleft out: Caoimhin Kelleher\nalso re-pointed away's marking of Florian "
		"Wirtz\n%s",
		output.utf8,
		homeXi,
		bench,
		lineupNote
	);
	(void)snprintf(
		away,
		sizeof away,
		"ok: away line-up: Martin Dubravka to player[0] (slot 0, GK), Nick Pope to player[11] (sub); "
		"wrote %s\nXI: Martin Dubravka, Kieran Trippier, Sven Botman, Dan Burn, Tino Livramento, "
		"Bruno Guimaraes, Sandro Tonali, Joelinton, Anthony Gordon, Alexander Isak, Harvey "
		"Barnes\nbench: Nick Pope, Lewis Miley\n%s",
		output.utf8,
		lineupNote
	);
	static const wchar_t first[] =
		L"player[0],player[1],player[2],player[3],player[4],player[5],player[6],player[7],player[8],"
		L"player[9],";
	wchar_t diazList[256];
	wchar_t haalandList[256];
	(void)swprintf(diazList, 256, L"%lsLuis Díaz", first);
	(void)swprintf(haalandList, 256, L"%ls Haaland ", first);
	const struct {
		wchar_t *arguments[9];
		const char *printed;
		const char *errors;
		const char *places[4]; // who is at home places 10, 11, 19 and 21 now (away 0 and 11)
	} cases[] = {
		{{L"fm24ctl", L"lineup", L"next.json", L"home", diazList, L"out.json"},
			diaz,
			"",
			{"Luis D\xC3\xAD" "az", "Caoimhin Kelleher", "Florian Wirtz", "Erling Haaland"}},
		{{L"fm24ctl", L"lineup", L"next.json", L"home", haalandList, L"out.json"},
			haaland,
			"",
			{"Erling Haaland", "Caoimhin Kelleher", "Florian Wirtz", "Ryan Gravenberch"}},
		{{L"fm24ctl",
			 L"lineup",
			 L"next.json",
			 L"home",
			 L"19058734,28127166,37036372,85028014,28044813,29056345,43219372,67236018,98028753,"
			 L"37060815,29179241,91193048",
			 L"out.json"},
			twelve,
			"warning: no goalkeeper among the substitutes\n",
			{"Erling Haaland", "Florian Wirtz", "Ryan Gravenberch", "Caoimhin Kelleher"}},
		{{L"fm24ctl",
			 L"lineup",
			 L"--allow-ai-side",
			 L"next.json",
			 L"away",
			 L"Dubravka",
			 L"Nick Pope",
			 L"out.json"},
			away,
			"warning: away is managed by FM24's AI, which picks its own XI at kick-off\n",
			{"Martin Dubravka", "Nick Pope", NULL, NULL}},
	};
	char printed[4096];
	char errors[1024];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		buffer_clear(&writtenConfig);
		ASSERT_INT_EQ(
			invokePrinting(
				commands_lineup, (wchar_t **)cases[i].arguments, printed, sizeof printed, errors, sizeof errors
			),
			EXIT_OK
		);
		ASSERT_BOOL_EQ(printedAs(printed, cases[i].printed) && printedAs(errors, cases[i].errors), true);
		ConfigOrigin origin;
		FixtureData *data = readWritten(&origin);
		if (!data) {
			continue;
		}
		bool home = cases[i].places[2] != NULL;
		const FixtureTeam *team = &data->teams[home ? 0 : 1];
		static const unsigned homePlaces[4] = {10, 11, 19, 21};
		static const unsigned awayPlaces[2] = {0, 11};
		for (unsigned k = 0; k < (home ? 4U : 2U); ++k) {
			unsigned place = home ? homePlaces[k] : awayPlaces[k];
			ASSERT_INT_EQ(strcmp(team->players[place].name, cases[i].places[k]), 0);
		}
		ASSERT_BOOL_EQ(origin.lineup[0] == home && origin.lineup[1] == !home, true);
		free(data);
	}
	buffer_clear(&writtenConfig);
	configText = plainConfig;
}

// What lineup refuses, with exit code 1 and nothing written: an AI side without --allow-ai-side, a
// place the side lacks, a player it cannot find or that matches several, the same player twice,
// blocks out of order, blocks that start differently or hold another block's name, an input that
// is not a fixture or cannot be read, and an output it cannot write.
TEST(lineup_refuses_what_it_cannot_change) {
	Path input;
	Path output;
	ASSERT_BOOL_EQ(absolutePath(L"next.json", &input) && absolutePath(L"out.json", &output), true);
	char unread[1100];
	char notFixture[1100];
	char unwritten[1100];
	(void)snprintf(unread, sizeof unread, "cannot read %s\n", input.utf8);
	(void)snprintf(
		notFixture, sizeof notFixture, "%s is not a fixture (fm24-match-config/1)\n", input.utf8
	);
	(void)snprintf(unwritten, sizeof unwritten, "cannot write %s\n", output.utf8);
	static const char outOfOrder[] =
		"error: home.player[19] is in place 10: the blocks were moved without renaming them\n";
	enum { PLAIN, MOVED, HEADER, NAME, PLAYER, UNREAD, UNWRITTEN };
	const struct {
		int sample;
		wchar_t *arguments[9];
		const char *errors;
	} cases[] = {
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"away", L"Pope", L"Dubravka", L"out.json"},
			"error: away is managed by FM24's AI, which picks its own XI at kick-off; use "
			"--allow-ai-side to reorder it anyway\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[22]", L"player[1]", L"out.json"},
			"error: the fixture has no home.player[22]\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[1]", L"Nobody", L"out.json"},
			"error: no home player matches 'Nobody'\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"Diaz", L"player[1]", L"out.json"},
			"error: no home player matches 'Diaz'\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"999", L"player[1]", L"out.json"},
			"error: no home player has the unique ID 999\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"Jones", L"player[1]", L"out.json"},
			"error: 'Jones' matches more than one home player: player[14] Lewis Jones (28100917), "
			"player[17] Curtis Jones (28115788); use more of the name, the ID or player[N]\n"},
		{PLAIN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"Wirtz", L"player[19]", L"out.json"},
			"error: player[19] Florian Wirtz is given twice\n"},
		{PLAIN,
			{L"fm24ctl",
				L"lineup",
				L"next.json",
				L"home",
				L"player[0],1,2,3,4,5,6,7,8,9,Alisson",
				L"out.json"},
			"error: no home player has the unique ID 1\n"},
		{PLAIN,
			{L"fm24ctl",
				L"lineup",
				L"next.json",
				L"home",
				L"player[0],player[1],player[2],player[3],player[4],player[5],player[6],player[7],"
				L"player[8],player[9],Alisson",
				L"out.json"},
			"error: player[0] Alisson Becker is given twice\n"},
		{MOVED,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json"},
			outOfOrder},
		{MOVED, {L"fm24ctl", L"lineup", L"next.json", L"home"}, outOfOrder},
		{HEADER,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json"},
			"error: home.player[10] and player[19] start with different values; lineup cannot move "
			"them\n"},
		{NAME,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json"},
			"error: unexpected home.player[17].morale in the block of home.player[18]\n"},
		{PLAYER, {L"fm24ctl", L"lineup", L"next.json", L"home"}, notFixture},
		{UNREAD,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json"},
			unread},
		{UNWRITTEN,
			{L"fm24ctl", L"lineup", L"next.json", L"home", L"player[10]", L"player[19]", L"out.json"},
			unwritten},
	};
	char printed[2048];
	char errors[2048];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		if (!makeLineupSample("")) {
			return;
		}
		if (cases[i].sample == MOVED) {
			// As blocks.py swap leaves them: the blocks at places 10 and 19 exchanged, not renamed.
			ASSERT_BOOL_EQ(replaceAll(&lineupText, "\"home.player[10]", "\"home.player[x]") > 5, true);
			ASSERT_BOOL_EQ(replaceAll(&lineupText, "\"home.player[19]", "\"home.player[10]") > 5, true);
			ASSERT_BOOL_EQ(replaceAll(&lineupText, "\"home.player[x]", "\"home.player[19]") > 5, true);
		} else if (cases[i].sample == HEADER) {
			ASSERT_INT_EQ(
				(int)replaceAll(&lineupText, "\"Florian Wirtz\", \"u8\": 255}", "\"Florian Wirtz\", \"u8\": 254}"),
				1
			);
		} else if (cases[i].sample == NAME) {
			ASSERT_INT_EQ(
				(int)replaceAll(&lineupText, "\"home.player[18].morale\"", "\"home.player[17].morale\""), 1
			);
		}
		configText = cases[i].sample == PLAYER ? "{\"format\":\"fm24-player/1\",\"fields\":[]}"
																					 : buffer_string(&lineupText);
		readFails = cases[i].sample == UNREAD;
		writeFails = cases[i].sample == UNWRITTEN;
		buffer_clear(&writtenConfig);
		int status = invokePrinting(
			commands_lineup, (wchar_t **)cases[i].arguments, printed, sizeof printed, errors, sizeof errors
		);
		ASSERT_INT_EQ(status, EXIT_FAILED);
		ASSERT_BOOL_EQ(printedAs(errors, cases[i].errors) && printedAs(printed, ""), true);
		ASSERT_INT_EQ((int)writtenConfig.length, 0);
	}
	readFails = false;
	writeFails = false;
	configText = plainConfig;
}

// The lineup sample with `side`'s places `a` and `b` exchanged by lineup, marker and all, in `out`.
static bool exchangedSample(unsigned side, unsigned a, unsigned b, ByteBuffer *out) {
	ByteBuffer sample = {0};
	FixtureLineup lineup;
	fixtureLineup_identity(&lineup);
	FixtureLineupResult *result = malloc(sizeof *result);
	char error[256] = "";
	buffer_clear(out);
	bool ok = result && fixtureSamples_lineup(&sample) &&
						fixtureLineup_exchange(&lineup, side, side ? 13 : 22, a, b) &&
						fixtureLineup_apply(
							buffer_string(&sample), sample.length, &lineup, out, result, error, sizeof error
						);
	ASSERT_BOOL_EQ(ok, true);
	free(result);
	buffer_free(&sample);
	return ok;
}

// replace-home and replace-away, whose output lists the players as the game's build does, put the
// other side's players back in the input's places, by person, when the input marks that side's
// line-up as changed, and mark it again. A player the rebuild lacks, or whose unique ID it wrote
// back, is missing: the player no person names takes that place. A line-up it cannot keep is
// warned of, and the game's output left as it is; a file it cannot read or write fails.
TEST(replace_keeps_the_other_sides_edited_lineup) {
	ByteBuffer sample = {0};
	ByteBuffer homeEdited = {0};
	ByteBuffer awayEdited = {0};
	ByteBuffer input = {0};
	ByteBuffer rebuilt = {0};
	if (!fixtureSamples_lineup(&sample) || !exchangedSample(0, 10, 19, &homeEdited) ||
			!exchangedSample(1, 9, 12, &awayEdited)) {
		ASSERT_BOOL_EQ(false, true);
		return;
	}
	Path output;
	ASSERT_BOOL_EQ(absolutePath(L"out.json", &output), true);
	char kept[4096];
	(void)snprintf(
		kept,
		sizeof kept,
		"ok\nok: kept the input's home line-up: Florian Wirtz to player[10] (slot 10, ST), Ryan "
		"Gravenberch to player[19] (sub); wrote %s\n%sFlorian Wirtz\nbench: Caoimhin Kelleher, Joe "
		"Gomez, Trent Alexander-Arnold, Lewis Jones, Harvey Elliott, Jarell Quansah, Curtis Jones, Luis "
		"D\xC3\xAD" "az, Ryan Gravenberch\nalso re-pointed away's marking of Florian Wirtz\n%s",
		output.utf8,
		homeXi,
		lineupNote
	);
	wchar_t *replaceAway[] = {L"fm24ctl", L"replace-away", L"next.json", L"677", L"out.json", NULL};
	wchar_t *replaceHome[] = {L"fm24ctl", L"replace-home", L"next.json", L"677", L"out.json", NULL};
	char printed[4096];
	char errors[2048];
	jobOutput = buffer_string(&sample);
	// The home order exchanged before the away team's replacement comes back, marker and all.
	configText = buffer_string(&homeEdited);
	configReads = 0;
	ASSERT_INT_EQ(
		invokePrinting(commands_replaceTeam, replaceAway, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(strstr(submittedJob, "command=replace_away\n") != NULL, true);
	ASSERT_INT_EQ((int)configReads, 2);
	ASSERT_BOOL_EQ(strcmp(buffer_string(&writtenConfig), buffer_string(&homeEdited)) == 0, true);
	ASSERT_BOOL_EQ(printedAs(printed, kept) && printedAs(errors, ""), true);
	// Replacing the home team instead drops its order, and leaves the game's output as it is.
	configText = buffer_string(&homeEdited);
	configReads = 0;
	ASSERT_INT_EQ(
		invokePrinting(commands_replaceTeam, replaceHome, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(configReads == 1 && writtenConfig.length == 0, true);
	ASSERT_BOOL_EQ(printedAs(printed, "ok\n") && printedAs(errors, ""), true);
	// So does an input with no marker, whatever its order.
	ASSERT_BOOL_EQ(buffer_append(&input, homeEdited.data, homeEdited.length), true);
	ASSERT_INT_EQ((int)replaceAll(&input, "\n  \"home_lineup\": true,", ""), 1);
	configText = buffer_string(&input);
	configReads = 0;
	ASSERT_INT_EQ(invoke(commands_replaceTeam, replaceAway), EXIT_OK);
	ASSERT_BOOL_EQ(configReads == 1 && writtenConfig.length == 0, true);
	// A marked away side, reordered with --allow-ai-side, is kept when the home team is replaced.
	configText = buffer_string(&awayEdited);
	ASSERT_INT_EQ(
		invokePrinting(commands_replaceTeam, replaceHome, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(strcmp(buffer_string(&writtenConfig), buffer_string(&awayEdited)) == 0, true);
	ASSERT_BOOL_EQ(
		!PRINTS_CAPTURED || strncmp(printed, "ok\nok: kept the input's away line-up: ", 38) == 0, true
	);
	// A marked order the rebuild already has is marked again.
	buffer_clear(&input);
	ASSERT_BOOL_EQ(buffer_append(&input, sample.data, sample.length), true);
	ASSERT_INT_EQ(
		(int)replaceAll(&input, "\n  \"fields\": [", "\n  \"home_lineup\": true,\n  \"fields\": ["), 1
	);
	configText = buffer_string(&input);
	ASSERT_INT_EQ(
		invokePrinting(commands_replaceTeam, replaceAway, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(strcmp(buffer_string(&writtenConfig), buffer_string(&input)) == 0, true);
	char unmoved[1100];
	(void)snprintf(
		unmoved, sizeof unmoved, "ok\nok: kept the input's home line-up: no player moved; wrote %s\n",
		output.utf8
	);
	ASSERT_BOOL_EQ(
		!PRINTS_CAPTURED ||
			(strncmp(printed, unmoved, strlen(unmoved)) == 0 && !strstr(printed, "note:")),
		true
	);
	// A player the input has by an ID the rebuild wrote back (Gravenberch's, after inject-player):
	// missing, and that player's block takes the place.
	buffer_clear(&input);
	ASSERT_BOOL_EQ(buffer_append(&input, homeEdited.data, homeEdited.length), true);
	ASSERT_INT_EQ((int)replaceAll(&input, "\"u32\": 37071197}", "\"u32\": 2000040347}"), 2);
	configText = buffer_string(&input);
	ASSERT_INT_EQ(
		invokePrinting(commands_replaceTeam, replaceAway, printed, sizeof printed, errors, sizeof errors),
		EXIT_OK
	);
	ASSERT_BOOL_EQ(strcmp(buffer_string(&writtenConfig), buffer_string(&homeEdited)) == 0, true);
	ASSERT_BOOL_EQ(
		printedAs(
			errors,
			"warning: Ryan Gravenberch (2000040347) of the input's home line-up is not in the rebuilt "
			"side; player[19] (sub) has Ryan Gravenberch (37071197) instead\n"
		) && printedAs(printed, kept),
		true
	);
	// A side it cannot reorder, in the output (blocks named for other places) or the input (a
	// player_count short of its blocks), is warned of and left as the game wrote it; the job is
	// played all the same.
	static const char *const notKept[2] = {
		"warning: cannot keep the input's home line-up (home_lineup): home.player[19] is in place 10: "
		"the blocks were moved without renaming them; the output lists the players as the game's "
		"build does, without the marker\n",
		"warning: cannot keep the input's home line-up (home_lineup): home has 22 player blocks, but "
		"its player_count is 21; the output lists the players as the game's build does, without the "
		"marker\n",
	};
	buffer_clear(&rebuilt);
	ASSERT_BOOL_EQ(buffer_append(&rebuilt, sample.data, sample.length), true);
	ASSERT_BOOL_EQ(
		replaceAll(&rebuilt, "\"home.player[10]", "\"home.player[moved]") > 5 &&
			replaceAll(&rebuilt, "\"home.player[19]", "\"home.player[10]") > 5 &&
			replaceAll(&rebuilt, "\"home.player[moved]", "\"home.player[19]") > 5,
		true
	);
	for (int broken = 0; broken < 2; ++broken) {
		buffer_clear(&input);
		ASSERT_BOOL_EQ(buffer_append(&input, homeEdited.data, homeEdited.length), true);
		if (broken) {
			ASSERT_INT_EQ(
				(int)replaceAll(&input, "\"home.player_count\", \"u8\": 22", "\"home.player_count\", \"u8\": 21"),
				1
			);
		}
		configText = buffer_string(&input);
		jobOutput = buffer_string(broken ? &sample : &rebuilt);
		ASSERT_INT_EQ(
			invokePrinting(commands_replaceTeam, replaceAway, printed, sizeof printed, errors, sizeof errors),
			EXIT_OK
		);
		ASSERT_BOOL_EQ(strstr(submittedJob, "command=replace_away\n") != NULL, true);
		ASSERT_INT_EQ((int)writtenConfig.length, 0);
		ASSERT_BOOL_EQ(printedAs(printed, "ok\n") && printedAs(errors, notKept[broken]), true);
	}
	// An output it cannot read or write again fails, with the reason in the result; so does the
	// game's job, whose reason stays.
	jobOutput = buffer_string(&sample);
	char unread[1100];
	char unwritten[1100];
	(void)snprintf(unread, sizeof unread, "cannot read %s\n", output.utf8);
	(void)snprintf(unwritten, sizeof unwritten, "cannot write %s\n", output.utf8);
	const char *failures[2] = {unread, unwritten};
	for (int i = 0; i < 2; ++i) {
		configText = buffer_string(&homeEdited);
		readsFailAfterJob = i == 0;
		writesFailAfterJob = i == 1;
		char result[CLIENT_RESULT_LENGTH] = "";
		configReads = 0;
		ASSERT_INT_EQ(commands_replaceTeamWithResult(5, replaceAway, result, sizeof result), EXIT_FAILED);
		ASSERT_INT_EQ(
			strcmp(result, "Cannot keep the home line-up in the replacement; see the console for details."),
			0
		);
		readFails = false;
		writeFails = false;
		configText = buffer_string(&homeEdited);
		ASSERT_INT_EQ(
			invokePrinting(commands_replaceTeam, replaceAway, printed, sizeof printed, errors, sizeof errors),
			EXIT_FAILED
		);
		ASSERT_BOOL_EQ(printedAs(printed, "ok\n") && printedAs(errors, failures[i]), true);
		readFails = false;
		writeFails = false;
	}
	readsFailAfterJob = false;
	writesFailAfterJob = false;
	jobReply = "error: the input's edits cannot be placed in the new build; see harness.log";
	configText = buffer_string(&homeEdited);
	configReads = 0;
	char result[CLIENT_RESULT_LENGTH] = "";
	ASSERT_INT_EQ(commands_replaceTeamWithResult(5, replaceAway, result, sizeof result), EXIT_FAILED);
	ASSERT_BOOL_EQ(configReads == 1 && strcmp(result, jobReply) == 0, true);
	jobReply = "ok";
	jobOutput = NULL;
	configText = plainConfig;
	buffer_free(&writtenConfig);
	buffer_free(&sample);
	buffer_free(&homeEdited);
	buffer_free(&awayEdited);
	buffer_free(&input);
	buffer_free(&rebuilt);
}

#ifndef _WIN32
// A replacement that keeps the other side's changed line-up reads and writes its output itself
// after the job: a folder macOS's privacy protection closes to fm24ctl stops it before the game is
// contacted, as it stops lineup's. One that keeps no line-up leaves that folder to the game.
TEST(replace_checks_its_output_as_its_own_when_it_keeps_a_lineup) {
	static const char privacy[] =
		"macOS does not let fm24ctl into its folder (operation not permitted): choose another folder "
		"or, for a folder macOS protects, allow the app fm24ctl runs in (such as Terminal) under "
		"System Settings > Privacy & Security\n";
	ByteBuffer homeEdited = {0};
	if (!exchangedSample(0, 10, 19, &homeEdited)) {
		return;
	}
	// Named in the header, the save is checked before a job.
	ASSERT_INT_EQ(
		(int)replaceAll(
			&homeEdited,
			"\"format\": \"fm24-match-config/1\",",
			"\"format\": \"fm24-match-config/1\", \"save\": \"" FIXTURE_SAMPLE_SAVE "\","
		),
		1
	);
	wchar_t *replaceAway[] = {
		L"fm24ctl", L"replace-away", L"next.json", L"677", L"fm24-closed-folder/out.json", NULL};
	wchar_t *replaceHome[] = {
		L"fm24ctl", L"replace-home", L"next.json", L"677", L"fm24-closed-folder/out.json", NULL};
	Path output;
	ASSERT_BOOL_EQ(absolutePath(L"fm24-closed-folder/out.json", &output), true);
	char errors[2048];
	char expected[2048];
	(void)snprintf(expected, sizeof expected, "cannot write %s: %s", output.utf8, privacy);
	foldersClosed = true;
	configText = buffer_string(&homeEdited);
	unsigned removed = removedJobInputs;
	ASSERT_INT_EQ(
		invokeCapturingErrors(commands_replaceTeam, replaceAway, errors, sizeof errors), EXIT_FAILED
	);
	ASSERT_INT_EQ(strcmp(errors, expected), 0);
	ASSERT_BOOL_EQ(!submittedJob[0] && ensures == 0, true);
	ASSERT_INT_EQ((int)(removedJobInputs - removed), 1);
	char result[CLIENT_RESULT_LENGTH] = "";
	ASSERT_INT_EQ(commands_replaceTeamWithResult(5, replaceAway, result, sizeof result), EXIT_FAILED);
	ASSERT_INT_EQ(strcmp(result, "Cannot prepare replacement; see the console for details."), 0);
	// Replacing the side that was changed drops its line-up: the game alone writes the output.
	removed = removedJobInputs;
	ASSERT_INT_EQ(
		invokeCapturingErrors(commands_replaceTeam, replaceHome, errors, sizeof errors), EXIT_OK
	);
	ASSERT_INT_EQ(strcmp(errors, ""), 0);
	ASSERT_BOOL_EQ(strncmp(submittedJob, "command=replace_home\n", 21) == 0, true);
	ASSERT_BOOL_EQ(ensures == 1 && strcmp(ensured.save, FIXTURE_SAMPLE_SAVE) == 0, true);
	ASSERT_INT_EQ((int)(removedJobInputs - removed), 1);
	foldersClosed = false;
	configText = plainConfig;
	buffer_free(&homeEdited);
}
#endif

#ifndef _WIN32
// run warns, before it checks the save, when it may not play an edited line-up: when it rebuilds
// each seed (numeric seeds without --keep-build), and with --human-control. It plays the file all
// the same.
TEST(run_warns_when_it_may_not_play_an_edited_lineup) {
	static const char homeRebuilt[] =
		"warning: home line-up was edited (home_lineup); rebuilding each seed puts FM24's own line-up "
		"back; add --keep-build to play it\n";
	static const char homeManaged[] =
		"warning: home line-up was edited (home_lineup); with --human-control FM24 may play the "
		"manager's own selection instead\n";
	static const char awayRebuilt[] =
		"warning: away line-up was edited (away_lineup); rebuilding each seed puts FM24's own line-up "
		"back; add --keep-build to play it\n";
	static const char awayManaged[] =
		"warning: away line-up was edited (away_lineup); with --human-control FM24 may play the "
		"manager's own selection instead\n";
	char bothManaged[512];
	(void)snprintf(bothManaged, sizeof bothManaged, "%s%s", homeManaged, awayManaged);
	char rebuiltAndManaged[512];
	(void)snprintf(rebuiltAndManaged, sizeof rebuiltAndManaged, "%s%s", homeRebuilt, homeManaged);
	const struct {
		const char *header;
		wchar_t *arguments[8];
		const char *warnings;
	} cases[] = {
		{"\"home_lineup\":true,", {L"fm24ctl", L"run", L"next.json", L"1-10"}, homeRebuilt},
		{"\"home_lineup\":true,", {L"fm24ctl", L"run", L"next.json", L"42", L"out.csv"}, homeRebuilt},
		{"\"home_lineup\":true,", {L"fm24ctl", L"run", L"next.json", L"1-10", L"--keep-build"}, ""},
		{"\"home_lineup\":true,", {L"fm24ctl", L"run", L"next.json"}, ""},
		{"\"home_lineup\":true,", {L"fm24ctl", L"run", L"next.json", L"keep"}, ""},
		{"\"home_lineup\":true,",
			{L"fm24ctl", L"run", L"next.json", L"--human-control"},
			homeManaged},
		{"\"home_lineup\":true,",
			{L"fm24ctl", L"run", L"next.json", L"7", L"--human-control"},
			rebuiltAndManaged},
		{"\"home_lineup\":false,", {L"fm24ctl", L"run", L"next.json", L"1-10"}, ""},
		{"\"home_lineup\":1,", {L"fm24ctl", L"run", L"next.json", L"1-10"}, ""},
		{"", {L"fm24ctl", L"run", L"next.json", L"1-10", L"--human-control"}, ""},
		{"\"away_lineup\":true,", {L"fm24ctl", L"run", L"next.json", L"1-10"}, awayRebuilt},
		{"\"home_lineup\":true,\"away_lineup\":true,",
			{L"fm24ctl", L"run", L"next.json", L"1-10", L"--keep-build", L"--human-control"},
			bothManaged},
	};
	expectedTimeout = CLIENT_LONG_TIMEOUT_MS;
	char config[256];
	char errors[2048];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		(void)snprintf(
			config,
			sizeof config,
			"{\"format\":\"fm24-match-config/1\",%s\"fields\":[{\"u8\":42}]}",
			cases[i].header
		);
		configText = config;
		(void)invokeCapturingErrors(commands_run, (wchar_t **)cases[i].arguments, errors, sizeof errors);
		size_t length = strlen(cases[i].warnings);
		bool warned = strncmp(errors, cases[i].warnings, length) == 0 &&
									!strstr(errors + length, "line-up was edited");
		ASSERT_BOOL_EQ(warned, true);
		if (!warned) {
			fprintf(stderr, "case %zu printed:\n%s\n", i, errors);
		}
		ASSERT_BOOL_EQ(strncmp(submittedJob, "command=run\n", 12) == 0, true);
	}
	buffer_free(&writtenConfig);
	expectedTimeout = CLIENT_SHORT_TIMEOUT_MS;
	configText = plainConfig;
}
#endif

void registerRunnerTests(void) {
#ifdef _WIN32
	test_run(
		"runner extracts the DLL only when it changed", test_extracts_the_dll_only_when_it_changed
	);
#else
	test_run(
		"runner replaces an installed dylib atomically", test_replaces_an_installed_dylib_atomically
	);
#endif
	test_run(
		"runner replaces either team and preserves both overrides",
		test_replaces_either_team_and_preserves_both_overrides
	);
	test_run(
		"runner encodes input and metadata in one read", test_encodes_input_and_metadata_in_one_read
	);
	test_run(
		"runner decodes binary fixtures through the inspect job",
		test_decodes_binary_fixtures_through_the_inspect_job
	);
	test_run(
		"runner encodes configs alike with or without the save and game date",
		test_encodes_configs_alike_with_or_without_the_save_and_date
	);
	test_run(
		"runner status succeeds only when the game is ready", test_reports_whether_the_game_is_ready
	);
	test_run("runner rejects malformed CLI numbers", test_rejects_malformed_cli_numbers);
	test_run("runner parses player IDs and blocks", test_parses_player_ids_and_blocks);
	test_run(
		"runner rejects missing or incomplete results", test_rejects_missing_or_incomplete_results
	);
	test_run("runner parses the harness status line", test_parses_the_harness_status_line);
	test_run("runner finds a save by its exact name", test_finds_a_save_by_its_exact_name);
	test_run(
		"runner writes only LOAD_GAME into the auto-play file",
		test_writes_only_load_game_into_the_autoplay_file
	);
	test_run(
		"runner waits until the save is loaded and stays idle",
		test_waits_until_the_save_is_loaded_and_stays_idle
	);
	test_run(
		"runner reads the save and date a fixture was exported from",
		test_reads_the_save_and_date_a_fixture_was_exported_from
	);
	test_run(
		"runner decides what happens before a job for a save",
		test_decides_what_happens_before_a_job_for_a_save
	);
	test_run(
		"runner keeps every argument form and takes the save options",
		test_keeps_every_argument_form_and_takes_the_save_options
	);
	test_run(
		"runner stops bad seeds and missing output folders before the game",
		test_stops_bad_seeds_and_missing_output_folders_before_the_game
	);
#ifndef _WIN32
	test_run("runner explains refused seeds", test_explains_refused_seeds);
	test_run(
		"runner explains output folders it cannot write in",
		test_explains_output_folders_it_cannot_write_in
	);
	test_run(
		"runner leaves folders closed to fm24ctl alone for outputs the game writes",
		test_leaves_folders_closed_to_fm24ctl_alone_for_outputs_the_game_writes
	);
	test_run(
		"runner explains outputs beside the results it cannot write",
		test_explains_outputs_beside_the_results_it_cannot_write
	);
	test_run(
		"runner checks the fixture's save before its job", test_checks_the_fixtures_save_before_its_job
	);
#endif
	test_run(
		"runner submits the run job as it always has", test_submits_the_run_job_as_it_always_has
	);
	test_run(
		"runner formats run jobs and refuses ones too long",
		test_formats_run_jobs_and_refuses_ones_too_long
	);
	test_run(
		"runner adds the per-match keys only when they are set",
		test_adds_the_per_match_keys_only_when_set
	);
#ifndef _WIN32
	test_run(
		"runner runs with per-match rows and keeps going past rebuild failures",
		test_runs_with_per_match_rows_and_keeps_going
	);
#endif
	test_run(
		"runner lineup refuses malformed arguments before reading",
		test_lineup_refuses_malformed_arguments_before_reading
	);
	test_run("runner lineup shows a side", test_lineup_shows_a_side);
	test_run("runner lineup exchanges two players", test_lineup_exchanges_two_players);
	test_run("runner lineup puts a list first", test_lineup_puts_a_list_first);
	test_run(
		"runner lineup refuses what it cannot change", test_lineup_refuses_what_it_cannot_change
	);
	test_run(
		"runner replace keeps the other side's edited line-up",
		test_replace_keeps_the_other_sides_edited_lineup
	);
#ifndef _WIN32
	test_run(
		"runner replace checks its output as its own when it keeps a line-up",
		test_replace_checks_its_output_as_its_own_when_it_keeps_a_lineup
	);
	test_run(
		"runner run warns when it may not play an edited line-up",
		test_run_warns_when_it_may_not_play_an_edited_lineup
	);
#endif
	test_run("runner writes JSON with escapes and commas", test_writes_json_with_escapes_and_commas);
	test_run("runner prints the build it is", test_prints_the_build_it_is);
	test_run("runner parses the run report", test_parses_the_run_report);
	test_run("runner counts the seeds a list plays", test_counts_the_seeds_a_list_plays);
	test_run("runner reads the header of a config", test_reads_the_header_of_a_config);
	test_run("runner formats the run manifest", test_formats_the_run_manifest);
	test_run("runner decides a run's outcome", test_decides_a_runs_outcome);
	test_run("runner reads the files a run wrote", test_reads_the_files_a_run_wrote);
	test_run("runner describes the computer", test_describes_the_computer);
#ifndef _WIN32
	test_run("runner writes a manifest for every run", test_writes_a_manifest_for_every_run);
#endif
#ifdef _WIN32
	test_run(
		"probe validates PE header and section bounds", test_validates_pe_header_and_section_bounds
	);
	test_run(
		"probe scans selected anchors and ranks shifts",
		test_scans_only_selected_anchors_and_ranks_shifts
	);
	test_run("probe preserves jump candidate overflow", test_preserves_jump_candidate_overflow);
#else
	test_run("probe reads the UUID of each slice", test_probe_reads_the_uuid_of_each_slice);
#endif
}
