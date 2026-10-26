// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "client.h"
#include "embedded.h"
#include "files.h"
#include "paths.h"
#include "platform.h"
#include "saves.h"
#include "session.h"
#include "test.h"
#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <membership.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


static bool readConfig(const wchar_t *path, ByteBuffer *out);
static bool writeConfig(const wchar_t *path, const void *bytes, size_t length);
static bool submitJob(const char *job, unsigned timeout, char *result, size_t resultSize);
#ifndef _WIN32
static int ensureTestSave(const SessionNeed *need, char *date, size_t size);
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
#define commands_status runnerTest_status
#define commands_probe runnerTest_probe
#define files_read readConfig
#define files_write writeConfig
#define client_submit submitJob
#define macosGame_ensureSave ensureTestSave
#define paths_parentFolder testParentFolder
#define platform_removeFile removeTestFile
#include "../src/runner/commands.c" // NOLINT(bugprone-suspicious-include): exercise private parsers.
#undef files_read
#undef files_write
#undef client_submit
#undef macosGame_ensureSave
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
// Only while folderChecked is set does paths_parentFolder look at the folder: otherwise every
// folder is open, so that the outputs of tests in the working directory pass wherever the suite
// runs. While foldersClosed is set, a folder named "fm24-closed-folder", which no folder the suite
// runs in is called, is one macOS's privacy protection keeps fm24ctl out of.
static bool folderChecked;
static bool foldersClosed;
static unsigned removedJobInputs; // the encoded inputs commands have removed

static bool submitJob(const char *job, unsigned timeout, char *result, size_t resultSize) {
	ASSERT_BOOL_EQ(timeout == expectedTimeout, true);
	(void)snprintf(submittedJob, sizeof submittedJob, "%s", job);
	(void)snprintf(result, resultSize, "%s", jobReply);
	return !submitFails;
}

static bool readConfig(const wchar_t *path, ByteBuffer *out) {
	(void)path;
	++configReads;
	return buffer_append(out, configText, strlen(configText));
}

static bool writeConfig(const wchar_t *path, const void *bytes, size_t length) {
	(void)path;
	return buffer_append(&writtenConfig, bytes, length);
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
	configReads = 0;
	ASSERT_BOOL_EQ(encodeToFile(&input, &output, &size, teams, &check), true);
	ASSERT_INT_EQ(configReads, 1);
	ASSERT_BOOL_EQ(size == 1 && writtenConfig.length == 1 && writtenConfig.data[0] == 42, true);
	ASSERT_BOOL_EQ(teams[0] == 678 && teams[1] == 677, true);
	ASSERT_BOOL_EQ(!check.save[0] && !check.date[0] && !check.badSave, true);
	buffer_free(&writtenConfig);
	// The save and date come from the same read.
	configText = "{\"format\":\"fm24-match-config/1\",\"save\":\"LIV\",\"game_date\":\"2024 day 1 "
							 "09:00\",\"fields\":[{\"u8\":42}]}";
	configReads = 0;
	ASSERT_BOOL_EQ(encodeToFile(&input, &output, &size, teams, &check), true);
	ASSERT_INT_EQ(configReads, 1);
	ASSERT_BOOL_EQ(strcmp(check.save, "LIV") == 0 && strcmp(check.date, "2024 day 1 09:00") == 0, true);
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
	};
	folderChecked = true;
	for (size_t i = 0; i < sizeof missingFolder / sizeof missingFolder[0]; ++i) {
		checkArguments(&missingFolder[i], 1, false);
#ifndef _WIN32
		ASSERT_INT_EQ((int)ensures, 0);
#endif
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

// macOS's privacy protection keeps out the app fm24ctl runs in, not the game: a folder it closes to
// fm24ctl (EPERM) stops only run's results, given or by default, which fm24ctl reads back, and
// inject-player's output, which it writes. The outputs only the game writes go on to the job.
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
		"runner checks the fixture's save before its job", test_checks_the_fixtures_save_before_its_job
	);
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
