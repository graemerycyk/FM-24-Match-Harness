// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// macOS: text and path conversions, the frame translation tables, the memory scan, the job socket
// between the runner and the dylib, recognising the game's build, and finding and starting
// Football Manager 2024 from Steam.

#include "../src/dll/memory.h"
#include "../src/dll/sites.h"
#include "channel.h"
#include "edition.h"
#if EDITION_ENGINE_PLUGIN
#include "../src/dll/sites_fm27.h"
#endif
#include "paths.h"
#include "save_list.h"
#include "save_samples.h"
#include "test.h"
#include "tests.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>


static char testSocket[104];
static bool testChannel(char *out, size_t size);
static int recordSpawn(
	pid_t *pid,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attributes,
	char *const *arguments,
	char *const *environment
);
static bool submitRecorded(const char *job, unsigned timeoutMs, char *result, size_t size);
bool macosTest_executableDirectory(wchar_t *out, size_t size);
static const char *extractFrom; // a stand-in harness that installing puts in place of fmNNctl's

// log_message, job_writeResult and job_takeResult come from job_test.c and job.c, and
// inject_harness from runner_test.c; dll_test.c's game API stubs and inject_macos.c's and
// session.c's own functions are renamed away from those.
void log_open(const wchar_t *directory);
void job_execute(char *text);
bool macosTest_injectHarness(void);
unsigned long macosTest_findGame(void);

#define game_function macosTest_function
#define game_functionAt macosTest_functionAt
#define game_address macosTest_address
#define game_rva macosTest_rva
#define game_timestamp macosTest_timestamp
#define game_translateFrames macosTest_translateFrames
#define game_engineState macosTest_engineState // also for main_macos.c
#include "../src/dll/game_macos.c" // NOLINT(bugprone-suspicious-include): frames and images.
#undef game_function
#undef game_functionAt
#undef game_address
#undef game_rva
#undef game_timestamp
#undef game_translateFrames

#define channel_socketPath testChannel
#define sendAll serverSendAll
#include "../src/dll/main_macos.c" // NOLINT(bugprone-suspicious-include): serve over a test socket.
#undef sendAll
#undef game_engineState
#define client_submit macosTest_submit
#define client_succeeded macosTest_succeeded
#include "../src/runner/client_posix.c" // NOLINT(bugprone-suspicious-include): talk to it.
#undef client_submit
#undef client_succeeded

#define client_submit submitRecorded
#define client_succeeded macosTest_succeeded
#define session_query macosTest_query
#define session_plan macosTest_plan
#define session_submits macosTest_submits
#define session_start macosTest_start
#define session_step macosTest_step
#define session_waitReady macosTest_waitReady
#include "../src/runner/session.c" // NOLINT(bugprone-suspicious-include): ask the test harness.
#undef client_submit
#undef client_succeeded

#define posix_spawn recordSpawn
#define embedded_extract macosTest_extract
#define paths_executableDirectory macosTest_executableDirectory
#include "../src/runner/macos_game.c" // NOLINT(bugprone-suspicious-include): fake Steam and open.
#undef paths_executableDirectory
#undef embedded_extract
#undef posix_spawn
#undef session_query
#undef session_plan
#undef session_submits
#undef session_start
#undef session_step
#undef session_waitReady
#define PATH_LENGTH INJECT_PATH_LENGTH
#define inject_harness macosTest_injectHarness
#define inject_findGame macosTest_findGame
#include "../src/runner/inject_macos.c" // NOLINT(bugprone-suspicious-include): other versions.
#undef inject_findGame
#undef PATH_LENGTH

// fmNNctl's commands, talking to the test socket and starting the game through macos_game.c as it
// is here (runner_test.c and runner_main_test.c have their own).
#define commands_defaultThreads macosTest_defaultThreads
#define commands_run macosTest_run
#define commands_runWithProgress macosTest_runWithProgress
#define commands_getNextFixture macosTest_getNextFixture
#define commands_replaceTeam macosTest_replaceTeam
#define commands_replaceTeamWithResult macosTest_replaceTeamWithResult
#define commands_listTeams macosTest_listTeams
#define commands_listTeamsWithResult macosTest_listTeamsWithResult
#define commands_listPlayers macosTest_listPlayers
#define commands_listPlayersWithResult macosTest_listPlayersWithResult
#define commands_encode macosTest_encode
#define commands_decode macosTest_decode
#define commands_getPlayer macosTest_getPlayer
#define commands_injectPlayer macosTest_injectPlayer
#define commands_injectPlayerWithResult macosTest_injectPlayerWithResult
#define commands_status macosTest_status
#define client_submit macosTest_submit
#define client_succeeded macosTest_succeeded
#define ensureSave commandsEnsureSave
#define EXIT_WRONG_STATE COMMANDS_EXIT_WRONG_STATE
#include "../src/runner/commands.c" // NOLINT(bugprone-suspicious-include): end to end.
#undef EXIT_WRONG_STATE
#undef ensureSave
#undef client_submit
#undef client_succeeded
#undef inject_harness
#undef channel_socketPath


static char executedJob[HARNESS_JOB_LENGTH + 1];
static const char *const *statusReplies; // what status jobs answer, in turn; the last one repeats
static size_t statusReplyCount;
static atomic_size_t statusRepliesGiven;

void log_open(const wchar_t *path) {
	(void)path;
}

void job_execute(char *text) {
	(void)snprintf(executedJob, sizeof executedJob, "%s", text);
	if (statusReplyCount && strcmp(text, "command=status\n") == 0) {
		size_t given = atomic_fetch_add(&statusRepliesGiven, 1);
		job_writeResult("%s", statusReplies[given < statusReplyCount ? given : statusReplyCount - 1]);
		return;
	}
	job_writeResult("ok: %zu bytes", strlen(text));
}

static bool testChannel(char *out, size_t size) {
	return snprintf(out, size, "%s", testSocket) < (int)size;
}

// client_submit for session.c: the test socket, waiting at most five seconds. The timeout the
// status query gave is kept.
static unsigned queryTimeoutMs;

static bool submitRecorded(const char *job, unsigned timeoutMs, char *result, size_t size) {
	queryTimeoutMs = timeoutMs;
	return macosTest_submit(job, 5000, result, size);
}

// embedded_extract for macos_game.c, as fm24_tests carries no harness dylib: a stand-in for it, or
// the stand-in harness extractFrom names.
bool macosTest_extract(const wchar_t *path) {
	static const char standIn[] = "stand-in harness";
	if (!extractFrom) {
		return embedded_write(path, standIn, sizeof standIn - 1);
	}
	ByteBuffer bytes = {0};
	bool ok = readFile(extractFrom, &bytes) && embedded_write(path, bytes.data, bytes.length);
	buffer_free(&bytes);
	return ok;
}

// paths_executableDirectory for macos_game.c: none while a test installs a stand-in harness, which
// a development build's dylib beside the tests (in build/) would otherwise replace.
bool macosTest_executableDirectory(wchar_t *out, size_t size) {
	return !extractFrom && paths_executableDirectory(out, size);
}

static char spawned[4096];                   // what macos_game.c started, each part followed by '|'
static char *spawnInstead = "/usr/bin/true"; // what recordSpawn runs in its place
static int spawnError;                       // recordSpawn fails with this instead, if set
static void (*steamStarts)(void); // what a start through Steam also does (it starts nothing)

// posix_spawn for macos_game.c: records the program and its arguments, and runs another program.
static int recordSpawn(
	pid_t *pid,
	const char *path,
	const posix_spawn_file_actions_t *actions,
	const posix_spawnattr_t *attributes,
	char *const *arguments,
	char *const *environment
) {
	int written = snprintf(spawned, sizeof spawned, "%s|", path);
	for (size_t i = 0; arguments[i] && written > 0 && (size_t)written < sizeof spawned; ++i) {
		int added = snprintf(spawned + written, sizeof spawned - (size_t)written, "%s|", arguments[i]);
		written = added < 0 ? -1 : written + added;
	}
	if (spawnError) {
		return spawnError;
	}
	if (steamStarts && strcmp(path, "/usr/bin/open") == 0 && arguments[1] &&
			strncmp(arguments[1], "steam://rungameid/", 18) == 0) {
		steamStarts();
	}
	char *const instead[] = {spawnInstead, NULL};
	return posix_spawn(pid, spawnInstead, actions, attributes, instead, environment);
}

static bool writeText(const char *path, const char *text) {
	FILE *file = fopen(path, "wb");
	bool ok = file && fputs(text, file) >= 0;
	return file && fclose(file) == 0 && ok;
}

// The bundle `app`'s Info.plist, with the identifier `identifier` and, if given, the executable's
// name, `executable`.
static bool writeInfoPlist(const char *app, const char *identifier, const char *executable) {
	char path[1200];
	char text[1024];
	(void)snprintf(path, sizeof path, "%s/Contents/Info.plist", app);
	(void)snprintf(
		text,
		sizeof text,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
		"\t<key>CFBundleIdentifier</key>\n\t<string>%s</string>\n%s%s%s</dict>\n</plist>\n",
		identifier,
		executable ? "\t<key>CFBundleExecutable</key>\n\t<string>" : "",
		executable ? executable : "",
		executable ? "</string>\n" : ""
	);
	return writeText(path, text);
}

static bool makeDirectories(const char *path) {
	char partial[1024];
	for (size_t i = 1; path[i]; ++i) {
		if (path[i] == '/') {
			(void)snprintf(partial, sizeof partial, "%.*s", (int)i, path);
			(void)mkdir(partial, 0755);
		}
	}
	return mkdir(path, 0755) == 0 || errno == EEXIST;
}

static bool removeTree(const char *path) {
	char command[1200];
	(void)snprintf(command, sizeof command, "/bin/rm -rf '%s'", path);
	return system(command) == 0;
}

// What a command prints to standard output and error while it is captured.
typedef struct Capture {
	char path[64];
	int file;
	int output;
	int errors;
} Capture;

static bool startCapture(Capture *capture) {
	(void)snprintf(capture->path, sizeof capture->path, "/tmp/" EDITION_LOWER "-printed-XXXXXX");
	(void)fflush(stdout);
	(void)fflush(stderr);
	capture->file = mkstemp(capture->path);
	capture->output = dup(STDOUT_FILENO);
	capture->errors = dup(STDERR_FILENO);
	return capture->file >= 0 && capture->output >= 0 && capture->errors >= 0 &&
				 dup2(capture->file, STDOUT_FILENO) >= 0 && dup2(capture->file, STDERR_FILENO) >= 0;
}

// Ends the capture, with what was printed in `out`.
static void endCapture(Capture *capture, char *out, size_t size) {
	(void)fflush(stdout);
	(void)fflush(stderr);
	(void)dup2(capture->output, STDOUT_FILENO);
	(void)dup2(capture->errors, STDERR_FILENO);
	close(capture->output);
	close(capture->errors);
	ssize_t length = capture->file >= 0 ? pread(capture->file, out, size - 1, 0) : 0;
	out[length > 0 ? length : 0] = 0;
	if (capture->file >= 0) {
		close(capture->file);
		(void)unlink(capture->path);
	}
}

TEST(converts_utf8_paths) {
	static const char text[] = "/Users/\xC3\x98\x64\x65gaard/\xE2\x82\xAC/\xF0\x9F\x8F\x86.json";
	wchar_t wide[64];
	char back[64];
	ASSERT_BOOL_EQ(platform_fromUtf8(text, wide, 64), true);
	ASSERT_BOOL_EQ(wide[7] == 0xD8 && wcslen(wide) == 24, true);
	ASSERT_BOOL_EQ(platform_toUtf8(wide, back, sizeof back), true);
	ASSERT_INT_EQ(strcmp(back, text), 0);
	static const char *const invalid[] = {
		"\xC0\xAF",         // overlong
		"\xED\xA0\x80",     // surrogate
		"\xF4\x90\x80\x80", // beyond U+10FFFF
		"\xE2\x82",         // truncated
		"\x80",             // continuation byte
	};
	for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
		ASSERT_BOOL_EQ(platform_fromUtf8(invalid[i], wide, 64), false);
	}
	ASSERT_BOOL_EQ(platform_fromUtf8("abc", wide, 3), false);
	ASSERT_BOOL_EQ(platform_fromUtf8("abc", wide, 4), true);
	ASSERT_BOOL_EQ(platform_toUtf8(L"\xD800", back, sizeof back), false);
	ASSERT_BOOL_EQ(platform_toUtf8(L"\x20AC", back, 3), false);
	ASSERT_BOOL_EQ(platform_toUtf8(L"\x20AC", back, 4), true);
}

TEST(normalises_absolute_paths) {
	wchar_t out[HARNESS_PATH_LENGTH];
	ASSERT_BOOL_EQ(paths_absolute(L"/a/./b//c/../d", out, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(wcscmp(out, L"/a/b/d") == 0, true);
	ASSERT_BOOL_EQ(paths_absolute(L"/..", out, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(wcscmp(out, L"/") == 0, true);
	ASSERT_BOOL_EQ(paths_absolute(L"", out, HARNESS_PATH_LENGTH), false);
	char cwd[1024];
	wchar_t expected[HARNESS_PATH_LENGTH];
	ASSERT_BOOL_EQ(getcwd(cwd, sizeof cwd) != NULL, true);
	ASSERT_BOOL_EQ(platform_fromUtf8(cwd, expected, HARNESS_PATH_LENGTH), true);
	wcscat(expected, L"/next.json");
	ASSERT_BOOL_EQ(paths_absolute(L"sub/../next.json", out, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(wcscmp(out, expected) == 0, true);
	wchar_t directory[HARNESS_PATH_LENGTH];
	ASSERT_BOOL_EQ(paths_jobDirectory(directory, HARNESS_PATH_LENGTH), true);
	ASSERT_BOOL_EQ(directory[0] == L'/' && directory[wcslen(directory) - 1] != L'/', true);
}

TEST(translates_frames_into_windows_frames) {
	static const MacosFrame frames[] = {
		{.mac = 0x1000, .count = 1, .windows = {0xA}},
		{.mac = 0x2000, .count = 2, .windows = {0xB, 0xC}}, // a call the macOS build inlined
		{.mac = 0x3000, .count = 0},                        // a frame Windows does not have
		{.mac = 0x4000, .count = 1, .windows = {0xE}},
		{.mac = 0x4000, .parent = 0x5000, .count = 1, .windows = {0xD}},
	};
	MacosTables tables = {.frames = frames, .frameCount = sizeof frames / sizeof frames[0]};
	uint32_t stack[] = {0x1000, 0x2000, 0x3000, 0x4000, 0x5000, 0, 0x9999};
	uint32_t out[16];
	size_t count = macosTables_translate(&tables, stack, 7, out, 16);
	ASSERT_INT_EQ((int)count, 7);
	ASSERT_BOOL_EQ(out[0] == 0xA && out[1] == 0xB && out[2] == 0xC && out[3] == 0xD, true);
	ASSERT_BOOL_EQ(out[4] == (MACOS_FRAME_TAG | 0x5000) && out[5] == 0, true);
	ASSERT_BOOL_EQ(out[6] == (MACOS_FRAME_TAG | 0x9999), true);
	uint32_t other[] = {0x4000, 0x6000};
	count = macosTables_translate(&tables, other, 2, out, 16);
	ASSERT_BOOL_EQ(count == 2 && out[0] == 0xE && out[1] == (MACOS_FRAME_TAG | 0x6000), true);
	count = macosTables_translate(&tables, stack, 7, out, 2);
	ASSERT_BOOL_EQ(count == 2 && out[0] == 0xA && out[1] == 0xB, true);
#if EDITION_ENGINE_PLUGIN
	// The plugin's entries may also depend on the frame after the parent: the most specific wins.
	static const MacosFrame contextual[] = {
		{.mac = 0x7000, .count = 1, .windows = {0x70}},
		{.mac = 0x7000, .parent = 0x8000, .count = 1, .windows = {0x78}},
		{.mac = 0x7000, .parent = 0x8000, .count = 1, .windows = {0x79}, .grandparent = 0x9000},
		{.mac = 0x7000, .parent = 0x8000, .count = 0, .grandparent = 0xA000},
	};
	MacosTables context = {.frames = contextual, .frameCount = 4};
	uint32_t deep[][3] = {
		{0x7000, 0x8000, 0x9000}, {0x7000, 0x8000, 0xB000}, {0x7000, 0xC000, 0x9000}};
	static const uint32_t expected[] = {0x79, 0x78, 0x70};
	for (size_t i = 0; i < 3; ++i) {
		count = macosTables_translate(&context, deep[i], 3, out, 16);
		ASSERT_BOOL_EQ(count == 3 && out[0] == expected[i], true);
	}
	uint32_t dropped[] = {0x7000, 0x8000, 0xA000};
	count = macosTables_translate(&context, dropped, 3, out, 16);
	ASSERT_BOOL_EQ(count == 2 && out[0] == (MACOS_FRAME_TAG | 0x8000), true);
#endif
	static const MacosAddress addresses[] = {{0x10, 0x100}, {0x20, 0x200}, {0x30, 0x300}};
	tables.addresses = addresses;
	tables.addressCount = 3;
	ASSERT_BOOL_EQ(macosTables_findAddress(&tables, 0x20) == 0x200, true);
	ASSERT_BOOL_EQ(macosTables_findAddress(&tables, 0x25) == 0, true);
}

// The architecture of the slice this test runs as, which the harness's tables are for.
static const char *sliceArchitecture(void) {
#if defined(__aarch64__)
	return "arm64";
#else
	return "x86_64";
#endif
}

// The ith set of tables the harness carries for this slice, or NULL past the last: one per mapped
// build where the engine is a plugin, else the one build's.
static const MacosTables *carriedTables(size_t i) {
#if EDITION_ENGINE_PLUGIN
	return i < macosBuildTableCount ? macosBuildTables[i].tables : NULL;
#else
	return i == 0 ? &macosTables : NULL;
#endif
}

static void checkSortedAndComplete(const MacosTables *tables);

TEST(builds_sorted_tables_for_every_harness_address) {
	size_t count = 0;
	for (const MacosTables *tables; (tables = carriedTables(count)) != NULL; ++count) {
		ASSERT_INT_EQ(strcmp(tables->architecture, sliceArchitecture()), 0);
		checkSortedAndComplete(tables);
	}
	ASSERT_BOOL_EQ(count > 0, true);
#if EDITION_ENGINE_PLUGIN
	// Every supported build of this slice has tables, and every set of tables is a supported build's.
	size_t builds = 0;
	for (size_t i = 0; i < macosBuildCount; ++i) {
		if (strcmp(macosBuilds[i].architecture, sliceArchitecture()) == 0) {
			ASSERT_BOOL_EQ(macosTables_forBuild(macosBuilds[i].uuid) != NULL, true);
			++builds;
		}
	}
	ASSERT_INT_EQ((int)builds, (int)macosBuildTableCount);
	for (size_t i = 0; i < macosBuildTableCount; ++i) {
		ASSERT_BOOL_EQ(macosBuilds_find(sliceArchitecture(), macosBuildTables[i].uuid) != NULL, true);
		const MacosTables *found = macosTables_forBuild(macosBuildTables[i].uuid);
		ASSERT_BOOL_EQ(found == macosBuildTables[i].tables, true);
	}
	uint8_t unknown[16] = {0};
	ASSERT_BOOL_EQ(macosTables_forBuild(unknown) == NULL, true);
#endif
}

static void checkSortedAndComplete(const MacosTables *tables) {
	bool sorted = true;
	for (size_t i = 1; i < tables->frameCount; ++i) {
		const MacosFrame *a = &tables->frames[i - 1];
		const MacosFrame *b = &tables->frames[i];
#if EDITION_ENGINE_PLUGIN
		sorted =
			sorted && (a->mac < b->mac || (a->mac == b->mac && a->parent < b->parent) ||
									(a->mac == b->mac && a->parent == b->parent && a->grandparent < b->grandparent));
		sorted = sorted && (!b->grandparent || b->parent);
#else
		sorted = sorted && (a->mac < b->mac || (a->mac == b->mac && a->parent < b->parent));
#endif
	}
	for (size_t i = 0; i < tables->frameCount; ++i) {
		const MacosFrame *frame = &tables->frames[i];
		sorted = sorted && frame->count <= MACOS_FRAME_MAX;
		for (uint32_t k = 0; k < frame->count; ++k) {
			sorted = sorted && frame->windows[k] != 0;
		}
	}
	for (size_t i = 1; i < tables->addressCount; ++i) {
		sorted = sorted && tables->addresses[i - 1].windows < tables->addresses[i].windows;
	}
	ASSERT_BOOL_EQ(sorted, true);
	// Everything the harness resolves has an address, and every function among them a signature.
	for (size_t i = 0; i < macosRequirementCount; ++i) {
		uint32_t mac = macosTables_findAddress(tables, macosRequirements[i].rva);
		bool signatureFound = false;
		for (size_t k = 0; k < tables->signatureCount; ++k) {
			signatureFound = signatureFound || tables->signatures[k].mac == mac;
		}
		ASSERT_BOOL_EQ(mac != 0 && (!macosRequirements[i].function || signatureFound), true);
	}
	// Every write site in sites.h (but the tactic serialiser's bounds, and where the engine is a
	// plugin the doctor's, which it has not) must be reachable from some macOS frame, and so must the
	// plugin's own named parts.
	ASSERT_BOOL_EQ(tables->frameCount > 0, true);
	static const uint32_t parts[] = {
		SITE_PART_MATCH,
		SITE_PART_OFFICIAL_0,
		SITE_PART_OFFICIAL_1,
		SITE_PART_OFFICIAL_2,
		SITE_PART_OFFICIAL_3,
		SITE_PART_REFEREE,
		SITE_PART_VENUE,
		SITE_PART_FIXTURE,
		SITE_PART_FIXTURE_EXTRA,
		SITE_PART_HOME,
		SITE_PART_AWAY,
		SITE_PART_EXTRA_53952,
		SITE_PART_EXTRA_1872,
		SITE_TEAM_BASE,
		SITE_TEAM_INFO,
		SITE_TEAM_MANAGER,
		SITE_TEAM_STAFF_0,
#if !EDITION_ENGINE_PLUGIN
		SITE_TEAM_STAFF_1,
		SITE_TEAM_HAS_STAFF_1,
#endif
		SITE_TEAM_STAFF_2,
		SITE_TEAM_PLAYER,
		SITE_TEAM_EXTRA,
		SITE_TEAM_BACKROOM,
		SITE_TEAM_HAS_STAFF_2,
		SITE_TEAM_FLAG_961,
		SITE_TEAM_PLAYER_COUNT,
		SITE_TEAM_HAS_EXTRA,
		SITE_TEAM_BACKROOM_COUNT,
		SITE_PLAYER_FIRST,
		SITE_PLAYER_UID,
		SITE_PLAYER_RATINGS,
		SITE_BACKROOM_FIRST,
		SITE_STAFF_ATTRIBUTES,
		SITE_PLAYER_ABILITY,
		SITE_PLAYER_LEADERSHIP,
		SITE_ROLE_RATING,
		SITE_ROLE_ID,
		SITE_ROLE_POSITION,
		SITE_FAMILIARITY_VERSION,
		SITE_FAMILIARITY_ROLE_COUNT,
		SITE_FAMILIARITY_ROLE_KEY,
		SITE_FAMILIARITY_ROLE_POSITIONS,
		SITE_FAMILIARITY_ROLE_AMOUNT,
		SITE_FAMILIARITY_COUNT,
		SITE_FAMILIARITY_SETTING,
		SITE_FAMILIARITY_AMOUNT,
		SITE_PERSON_HEIGHT,
		SITE_PERSON_WEIGHT,
		SITE_OFFICIAL_ALLOWING_FLOW,
		SITE_OFFICIAL_DISCIPLINE,
		SITE_OFFICIAL_IMPORTANT_MATCHES,
		SITE_OFFICIAL_REFEREEING,
		SITE_OFFICIAL_RUNNING_LINE,
		SITE_OFFICIAL_TIME_KEEPING,
		SITE_STAFF_FOCUS_COUNT,
		SITE_STAFF_FOCUS_CODE,
		SITE_WEATHER_CHANGE_TIME,
		SITE_PITCH_WIDTH,
		SITE_PITCH_HALF_WIDTH,
		SITE_PITCH_HALF_LENGTH,
		SITE_PITCH_QUARTER_WIDTH,
		SITE_PITCH_QUARTER_LENGTH,
		SITE_STADIUM_INDEX,
		SITE_STADIUM_UID,
		SITE_TEXT,
		SITE_STADIUM_NAME_CALLER,
		SITE_VENUE_UID,
		SITE_SEAT_COLOUR_0,
		SITE_SEAT_COLOUR_1,
		SITE_STAND_0_CAPACITY,
		SITE_OCCUPANCY_CALLER,
		SITE_OCCUPANCY_FIRST,
		SITE_OCCUPANCY_LAST,
		SITE_SECTION_SEATS_FIRST,
		SITE_SECTION_SEATS_LAST,
		SITE_FIXTURE_ATTENDANCE,
		SITE_FIXTURE_AWAY_FANS,
		SITE_FIXTURE_EXTRA_HOME_FANS,
		SITE_FIXTURE_EXTRA_AWAY_FANS,
		SITE_TACTIC_TAKERS,
		SITE_TACTIC_PART_4240,
		SITE_TACTIC_PLAN_0,
		SITE_TACTIC_PLAN_1,
		SITE_TACTIC_SET_PIECES,
		SITE_TACTIC_SLOTS_6920_FIRST,
		SITE_TACTIC_SLOTS_6920_LAST,
		SITE_TACTIC_FORMATION_FIRST,
		SITE_TACTIC_FORMATION_LAST,
		SITE_TACTIC_MENTALITY,
		SITE_TACTIC_TEAM_INSTRUCTIONS,
		SITE_TACTIC_SLOT_POSITION,
		SITE_TACTIC_SLOT_ROLE,
		SITE_TACTIC_INSTRUCTIONS_ROLE,
		SITE_TACTIC_INSTRUCTIONS_BITS,
#if EDITION_ENGINE_PLUGIN
		SITE27_PART_EXTRA_1776,
		SITE27_TACTIC_PLAN_2,
		SITE27_TACTIC_EXTRA,
		SITE27_TACTIC_SLOT_IP_FIRST,
		SITE27_TACTIC_SLOT_IP_LAST,
		SITE27_TACTIC_SLOT_OOP_FIRST,
		SITE27_TACTIC_SLOT_OOP_LAST,
		SITE27_TACTIC_SLOT_BYTE_FIRST,
		SITE27_TACTIC_SLOT_BYTE_LAST,
#endif
	};
	for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
		bool found = false;
		for (size_t f = 0; f < tables->frameCount && !found; ++f) {
			for (uint32_t k = 0; k < tables->frames[f].count; ++k) {
				found = found || tables->frames[f].windows[k] == parts[i];
			}
		}
		ASSERT_BOOL_EQ(found, true);
	}
}

// The nth supported build of this test's slice, or with `other`, of the other slice; NULL past the
// last.
static const MacosBuild *sliceBuild(bool other, size_t n) {
	for (size_t i = 0; i < macosBuildCount; ++i) {
		if ((strcmp(macosBuilds[i].architecture, sliceArchitecture()) != 0) == other && n-- == 0) {
			return &macosBuilds[i];
		}
	}
	return NULL;
}

// The tables the harness carries for a build of this slice.
static const MacosTables *tablesOf(const MacosBuild *game) {
#if EDITION_ENGINE_PLUGIN
	return macosTables_forBuild(game->uuid);
#else
	(void)game;
	return &macosTables;
#endif
}

static cpu_type_t cpuType(const char *architecture) {
	return strcmp(architecture, "arm64") == 0 ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64;
}

// The game's slice as dyld maps it, as far as game_init reads it: the Mach-O header, a __TEXT
// segment spanning the image and an LC_UUID, and the first bytes of the functions the tables check.
typedef struct GameImage {
	uint8_t *bytes;
	size_t size;
	struct mach_header_64 *header;
	struct segment_command_64 *text;
	struct uuid_command *uuid;
} GameImage;

static bool mapGameImage(GameImage *image, const MacosBuild *game, const MacosTables *tables) {
	size_t end = sizeof *image->header + sizeof *image->text + sizeof *image->uuid;
	for (size_t i = 0; i < tables->addressCount; ++i) {
		end = tables->addresses[i].mac >= end ? tables->addresses[i].mac + 1 : end;
	}
	for (size_t i = 0; i < tables->signatureCount; ++i) {
		const MacosSignature *signature = &tables->signatures[i];
		end = signature->mac + signature->length > end ? signature->mac + signature->length : end;
	}
	size_t page = (size_t)getpagesize();
	image->size = (end + page - 1) / page * page;
	image->bytes = mmap(NULL, image->size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (image->bytes == MAP_FAILED) {
		image->bytes = NULL;
		return false;
	}
	for (size_t i = 0; i < tables->signatureCount; ++i) {
		const MacosSignature *signature = &tables->signatures[i];
		memcpy(image->bytes + signature->mac, signature->bytes, signature->length);
	}
	image->header = (struct mach_header_64 *)image->bytes;
	image->text = (struct segment_command_64 *)(image->header + 1);
	image->uuid = (struct uuid_command *)(image->text + 1);
	*image->header = (struct mach_header_64){
		.magic = MH_MAGIC_64,
		.cputype = cpuType(game->architecture),
		.filetype = MH_EXECUTE,
		.ncmds = 2,
		.sizeofcmds = sizeof *image->text + sizeof *image->uuid,
	};
	*image->text = (struct segment_command_64){
		.cmd = LC_SEGMENT_64,
		.cmdsize = sizeof *image->text,
		.segname = SEG_TEXT,
		.vmaddr = 0x100000000,
		.vmsize = image->size,
	};
	*image->uuid = (struct uuid_command){.cmd = LC_UUID, .cmdsize = sizeof *image->uuid};
	memcpy(image->uuid->uuid, game->uuid, sizeof game->uuid);
	return true;
}

TEST(accepts_only_a_supported_build_its_tables_match) {
	const MacosBuild *other = sliceBuild(true, 0);
	ASSERT_BOOL_EQ(other != NULL, true);
	size_t count = 0;
	for (const MacosBuild *game; other && (game = sliceBuild(false, count)) != NULL; ++count) {
		const MacosTables *tables = tablesOf(game);
		GameImage image = {0};
		bool mapped = tables && mapGameImage(&image, game, tables);
		ASSERT_BOOL_EQ(mapped, true);
		if (!mapped) {
			return;
		}
		ASSERT_BOOL_EQ(checkImage(image.header), true);
		ASSERT_BOOL_EQ(game_base() == (uintptr_t)image.bytes, true);
		ASSERT_BOOL_EQ(macosTest_timestamp() == game->bundleVersion, true);
		ASSERT_BOOL_EQ(macosTables_findAddress(&TABLES, RVA_GAME_MATCH_VTABLE) != 0, true);
		// Another build of this slice.
		image.uuid->uuid[15] ^= 1;
		ASSERT_BOOL_EQ(checkImage(image.header), false);
		image.uuid->uuid[15] ^= 1;
		// The other slice's supported build, which these tables are not for.
		image.header->cputype = cpuType(other->architecture);
		memcpy(image.uuid->uuid, other->uuid, sizeof other->uuid);
		ASSERT_BOOL_EQ(checkImage(image.header), false);
		image.header->cputype = cpuType(game->architecture);
		memcpy(image.uuid->uuid, game->uuid, sizeof game->uuid);
		// No UUID.
		image.uuid->cmd = LC_SOURCE_VERSION;
		ASSERT_BOOL_EQ(checkImage(image.header), false);
		image.uuid->cmd = LC_UUID;
		// Segments that end before the tables' addresses.
		image.text->vmsize = (uint64_t)getpagesize();
		ASSERT_BOOL_EQ(checkImage(image.header), false);
		image.text->vmsize = image.size;
		// Other code at the start of a function the harness calls, as a software breakpoint leaves.
		uint8_t *code = image.bytes + tables->signatures[0].mac;
		*code ^= 0xFF;
		ASSERT_BOOL_EQ(checkImage(image.header), false);
		*code ^= 0xFF;
		ASSERT_BOOL_EQ(checkImage(image.header), true);
		(void)munmap(image.bytes, image.size);
		base = 0;
		imageSpan = 0;
		build = NULL;
#if EDITION_ENGINE_PLUGIN
		buildTables = &noTables;
#endif
	}
	ASSERT_BOOL_EQ(count > 0, true);
}

// Serves one connection, or gives up after a few seconds (so a client failure cannot hang).
static void *serveOnce(void *parameter) {
	int listener = *(int *)parameter;
	struct pollfd entry = {.fd = listener, .events = POLLIN};
	int connection = poll(&entry, 1, 5000) == 1 ? accept(listener, NULL, NULL) : -1;
	if (connection >= 0) {
		serve(connection);
		close(connection);
	}
	return NULL;
}

// A socket file nothing listens on, as a game that crashed leaves it.
static bool leaveStaleSocket(const char *path) {
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	(void)snprintf(address.sun_path, sizeof address.sun_path, "%s", path);
	(void)unlink(path); // left by a run that was killed
	int stale = socket(AF_UNIX, SOCK_STREAM, 0);
	bool bound = stale >= 0 && bind(stale, (const struct sockaddr *)&address, sizeof address) == 0;
	if (stale >= 0) {
		close(stale);
	}
	return bound;
}

TEST(serves_one_job_per_connection) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-socket-test-%d.sock", (int)getpid());
	supported = true;
	int listener = openSocket();
	ASSERT_BOOL_EQ(listener >= 0, true);
	if (listener < 0) {
		return;
	}
	pthread_t server;
	ASSERT_INT_EQ(pthread_create(&server, NULL, serveOnce, &listener), 0);
	char result[HARNESS_RESULT_LENGTH];
	ASSERT_BOOL_EQ(macosTest_submit("command=run\ninput=/x\n", 5000, result, sizeof result), true);
	pthread_join(server, NULL);
	ASSERT_INT_EQ(strcmp(executedJob, "command=run\ninput=/x\n"), 0);
	ASSERT_INT_EQ(strcmp(result, "ok: 21 bytes"), 0);
	supported = false;
#if EDITION_ENGINE_PLUGIN
	engineState = GAME_ENGINE_REFUSED;
#endif
	ASSERT_INT_EQ(pthread_create(&server, NULL, serveOnce, &listener), 0);
	ASSERT_BOOL_EQ(macosTest_submit("command=run\n", 5000, result, sizeof result), true);
	pthread_join(server, NULL);
	ASSERT_BOOL_EQ(strstr(result, "does not support this game build") != NULL, true);
	ASSERT_BOOL_EQ(macosTest_succeeded(result), false);
#if EDITION_ENGINE_PLUGIN
	engineState = GAME_ENGINE_STARTING;
#endif
	ASSERT_BOOL_EQ(channel_served(testSocket), true);
	close(listener);
	stopServing();
	struct stat status;
	ASSERT_BOOL_EQ(lstat(testSocket, &status) != 0 && errno == ENOENT, true);
	(void)unlink(testSocket);
	ASSERT_BOOL_EQ(channel_served(testSocket), false);
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	ASSERT_INT_EQ(unlink(lockPath), 0);
}

typedef struct ScanCheck {
	uint8_t *wanted[2];
	bool seen[2];
} ScanCheck;

static void noteFound(void *context, uint8_t *address) {
	ScanCheck *check = context;
	for (int i = 0; i < 2; ++i) {
		check->seen[i] = check->seen[i] || address == check->wanted[i];
	}
}

TEST(scans_malloc_memory_whatever_its_share_mode) {
	// A malloc region that something else also references, as a buffer the GPU uses does, is
	// reported as shared: the game's objects in it must still be found. Graphics memory is not
	// scanned.
	enum { SIZE = 4 << 20, AT = (2 << 20) + 64 };
	const uintptr_t marker = (uintptr_t)0x5CA1AB1E0DDBA11ULL ^ (uintptr_t)getpid();
	uint8_t *heap = mmap(
		NULL,
		SIZE,
		PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE,
		VM_MAKE_TAG(VM_MEMORY_MALLOC_MEDIUM),
		0
	);
	uint8_t *graphics = mmap(
		NULL,
		SIZE,
		PROT_READ | PROT_WRITE,
		MAP_ANON | MAP_PRIVATE,
		VM_MAKE_TAG(VM_MEMORY_IOACCELERATOR),
		0
	);
	ASSERT_BOOL_EQ(heap != MAP_FAILED && graphics != MAP_FAILED, true);
	if (heap == MAP_FAILED || graphics == MAP_FAILED) {
		return;
	}
	memset(heap, 1, SIZE);
	memset(graphics, 1, SIZE);
	memcpy(heap + AT, &marker, sizeof marker);
	memcpy(graphics + AT, &marker, sizeof marker);
	memory_object_size_t length = 1 << 20;
	mach_port_t entry = MACH_PORT_NULL;
	ASSERT_INT_EQ(
		mach_make_memory_entry_64(
			mach_task_self(),
			&length,
			(memory_object_offset_t)(uintptr_t)(heap + (2 << 20)),
			VM_PROT_READ | VM_PROT_WRITE,
			&entry,
			MACH_PORT_NULL
		),
		KERN_SUCCESS
	);
	mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)heap;
	mach_vm_size_t size = 0;
	natural_t depth = 0;
	vm_region_submap_info_data_64_t info;
	mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
	ASSERT_INT_EQ(
		mach_vm_region_recurse(
			mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count
		),
		KERN_SUCCESS
	);
	ASSERT_BOOL_EQ(info.share_mode != SM_PRIVATE && info.share_mode != SM_COW, true);
	ScanCheck check = {.wanted = {heap + AT, graphics + AT}};
	memory_scanForQword(marker, noteFound, &check);
	ASSERT_BOOL_EQ(check.seen[0], true);
	ASSERT_BOOL_EQ(check.seen[1], false);
	(void)mach_port_deallocate(mach_task_self(), entry);
	(void)munmap(heap, SIZE);
	(void)munmap(graphics, SIZE);
}

static volatile sig_atomic_t brokenPipes;

static void countBrokenPipe(int number) {
	(void)number;
	brokenPipes = brokenPipes + 1;
}

TEST(drops_a_runner_that_gave_up) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-gave-up-test-%d.sock", (int)getpid());
	supported = true;
	int listener = openSocket();
	ASSERT_BOOL_EQ(listener >= 0, true);
	if (listener < 0) {
		return;
	}
	// A runner that gave up while an earlier job ran: it connected, then closed the connection
	// before the harness accepted it. Answering it must not raise SIGPIPE, which ends the game.
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	(void)snprintf(address.sun_path, sizeof address.sun_path, "%s", testSocket);
	int runner = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_BOOL_EQ(
		runner >= 0 && connect(runner, (const struct sockaddr *)&address, sizeof address) == 0, true
	);
	if (runner >= 0) {
		close(runner);
	}
	brokenPipes = 0;
	void (*previous)(int) = signal(SIGPIPE, countBrokenPipe);
	pthread_t server;
	ASSERT_INT_EQ(pthread_create(&server, NULL, serveOnce, &listener), 0);
	pthread_join(server, NULL);
	ASSERT_INT_EQ(brokenPipes, 0);
	// The next runner is served as usual.
	ASSERT_INT_EQ(pthread_create(&server, NULL, serveOnce, &listener), 0);
	char result[HARNESS_RESULT_LENGTH];
	ASSERT_BOOL_EQ(macosTest_submit("command=run\n", 5000, result, sizeof result), true);
	pthread_join(server, NULL);
	ASSERT_INT_EQ(strcmp(result, "ok: 12 bytes"), 0);
	ASSERT_INT_EQ(brokenPipes, 0);
	(void)signal(SIGPIPE, previous);
	close(listener);
	stopServing();
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	ASSERT_INT_EQ(unlink(lockPath), 0);
}

TEST(tells_a_stale_socket_from_a_busy_harness) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stale-test-%d.sock", (int)getpid());
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	ASSERT_BOOL_EQ(leaveStaleSocket(testSocket), true);
	// No harness holds the lock: the runner gives up at once.
	char result[HARNESS_RESULT_LENGTH];
	uint64_t started = nowMs();
	ASSERT_BOOL_EQ(macosTest_submit("command=run\n", 3000, result, sizeof result), false);
	ASSERT_BOOL_EQ(nowMs() - started < 1000, true);
	ASSERT_BOOL_EQ(harnessListening(), false);
	// A harness holds it but refuses connections (its backlog is full): the runner waits it out.
	int lock = open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	ASSERT_BOOL_EQ(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0, true);
	ASSERT_BOOL_EQ(channel_served(testSocket), true);
	ASSERT_BOOL_EQ(harnessListening(), true);
	started = nowMs();
	ASSERT_BOOL_EQ(macosTest_submit("command=run\n", 600, result, sizeof result), false);
	ASSERT_BOOL_EQ(nowMs() - started >= 500, true);
	// It holds the lock but has no socket: the runner allows it a moment to start, then gives up.
	ASSERT_INT_EQ(unlink(testSocket), 0);
	ASSERT_BOOL_EQ(harnessListening(), false);
	started = nowMs();
	ASSERT_BOOL_EQ(macosTest_submit("command=run\n", 10000, result, sizeof result), false);
	uint64_t waited = nowMs() - started;
	ASSERT_BOOL_EQ(waited >= SOCKET_GRACE_MS && waited < SOCKET_GRACE_MS + 2000, true);
	if (lock >= 0) {
		close(lock);
	}
	(void)unlink(lockPath);
}

// Has the harness serve `listener` once, for `job`, with the result in `result`.
static void serveJob(int *listener, const char *job, char *result, size_t size) {
	pthread_t server;
	ASSERT_INT_EQ(pthread_create(&server, NULL, serveOnce, listener), 0);
	ASSERT_BOOL_EQ(macosTest_submit(job, 5000, result, size), true);
	pthread_join(server, NULL);
}

TEST(refuses_jobs_outside_a_supported_game) {
	// fm24_tests is no game: the harness it starts serves, but runs no job.
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-refusal-test-%d.sock", (int)getpid());
	supported = true;
	int listener = startServing();
	ASSERT_BOOL_EQ(listener >= 0, true);
	ASSERT_BOOL_EQ(supported, false);
	if (listener < 0) {
		return;
	}
	executedJob[0] = 0;
	char result[HARNESS_RESULT_LENGTH];
#if EDITION_ENGINE_PLUGIN
	// Its engine is never loaded: the game is starting, so job_execute answers status and refuses
	// the rest (as the dll suite checks).
	ASSERT_INT_EQ((int)engineState, GAME_ENGINE_STARTING);
	serveJob(&listener, "command=run\n", result, sizeof result);
	ASSERT_INT_EQ(strcmp(executedJob, "command=run\n"), 0);
	// A game whose engine is another build, or has been unloaded, runs no job.
	static const struct {
		GameEngineState state;
		const char *result;
	} refused[] = {
		{GAME_ENGINE_REFUSED,
			"error: harness " HARNESS_PROTOCOL_TEXT " does not support this game build"},
		{GAME_ENGINE_GONE, "error: " EDITION_PRODUCT " is quitting: it has unloaded its match engine"},
	};
	for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i) {
		engineState = refused[i].state;
		executedJob[0] = 0;
		serveJob(&listener, "command=status\n", result, sizeof result);
		ASSERT_INT_EQ(strncmp(result, refused[i].result, strlen(refused[i].result)), 0);
		ASSERT_INT_EQ(executedJob[0], 0);
	}
	engineState = GAME_ENGINE_STARTING;
#else
	serveJob(&listener, "command=run\n", result, sizeof result);
	ASSERT_BOOL_EQ(strstr(result, "does not support this game build") != NULL, true);
	ASSERT_INT_EQ(executedJob[0], 0);
#endif
	close(listener);
	stopServing();
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	ASSERT_INT_EQ(unlink(lockPath), 0);
}

#if EDITION_ENGINE_PLUGIN
// dyld's removal of an image, on a thread of its own, as the game's unloading of its engine.
typedef struct Removal {
	const struct mach_header *header;
	pthread_t thread;
	atomic_bool done;
} Removal;

static void *removeImage(void *parameter) {
	Removal *removal = parameter;
	imageRemoved(removal->header, 0);
	atomic_store(&removal->done, true);
	return NULL;
}

static bool startRemoval(Removal *removal, const struct mach_header *header) {
	removal->header = header;
	atomic_store(&removal->done, false);
	return pthread_create(&removal->thread, NULL, removeImage, removal) == 0;
}

// The engine as the job thread has found it, standing for an image dyld has loaded.
static void acceptEngine(const struct mach_header *header) {
	atomic_store(&plugin, header);
	engineState = GAME_ENGINE_ACCEPTED;
}

TEST(holds_the_engine_while_a_job_uses_it) {
	static const struct mach_header engine = {.magic = MH_MAGIC_64, .filetype = MH_BUNDLE};
	static const struct mach_header other = {.magic = MH_MAGIC_64, .filetype = MH_BUNDLE};
	// A job holds the engine: the game's unloading of it waits for the job, which is told to stop.
	acceptEngine(&engine);
	ASSERT_BOOL_EQ(game_hold() && game_loaded(), true);
	Removal removal;
	bool started = startRemoval(&removal, &engine);
	ASSERT_BOOL_EQ(started, true);
	if (!started) {
		return;
	}
	platform_sleep(100);
	ASSERT_BOOL_EQ(atomic_load(&removal.done), false);
	ASSERT_BOOL_EQ(game_loaded(), false);
	game_release();
	pthread_join(removal.thread, NULL);
	ASSERT_BOOL_EQ(atomic_load(&unloadWaitedMs) > 0, true);
	// No job is let in once it has begun, and the job thread then notes the engine gone.
	ASSERT_BOOL_EQ(game_hold(), false);
	ASSERT_INT_EQ((int)game_update(), GAME_ENGINE_GONE);
	ASSERT_BOOL_EQ(atomic_load(&plugin) == NULL && game_base() == 0, true);
	// A job that does not stop holds the unloading up only so long.
	acceptEngine(&engine);
	unloadWaitMs = 50;
	ASSERT_BOOL_EQ(game_hold(), true);
	uint64_t start = platform_ticks();
	imageRemoved(&engine, 0);
	uint64_t waitedMs = (platform_ticks() - start) * 1000 / platform_tickFrequency();
	ASSERT_BOOL_EQ(waitedMs >= 50 && waitedMs < 5000, true);
	game_release();
	ASSERT_INT_EQ((int)game_update(), GAME_ENGINE_GONE);
	unloadWaitMs = UNLOAD_WAIT_MS;
	// The job thread may find the engine while dyld removes it: the removal waits for that look, and
	// then tells jobs to stop as for an engine found before.
	engineState = GAME_ENGINE_STARTING;
	atomic_store(&held, &engine);
	started = startRemoval(&removal, &engine);
	ASSERT_BOOL_EQ(started, true);
	if (started) {
		platform_sleep(50);
		ASSERT_BOOL_EQ(atomic_load(&removal.done), false);
		acceptEngine(&engine);
		atomic_store(&held, NULL);
		pthread_join(removal.thread, NULL);
		ASSERT_BOOL_EQ(game_loaded() || game_hold(), false);
		ASSERT_INT_EQ((int)game_update(), GAME_ENGINE_GONE);
	}
	// The removal of another image changes nothing.
	acceptEngine(&engine);
	imageRemoved(&other, 0);
	ASSERT_BOOL_EQ(game_loaded() && game_hold(), true);
	game_release();
	ASSERT_INT_EQ((int)game_update(), GAME_ENGINE_ACCEPTED);
	atomic_store(&plugin, NULL);
	engineState = GAME_ENGINE_STARTING;
}
#endif

// Takes the lock of the socket at `path`, as a harness serving it does. The lock's descriptor, or
// -1.
static int holdSocketLock(const char *path) {
	char lockPath[1100];
	int lock = channel_lockPath(path, lockPath, sizeof lockPath)
							 ? open(lockPath, O_RDWR | O_CREAT | O_CLOEXEC, 0600)
							 : -1;
	if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
		close(lock);
		lock = -1;
	}
	return lock;
}

TEST(finds_another_harness_version_serving) {
	char directory[] = "/tmp/" EDITION_LOWER "-versions-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(testSocket, sizeof testSocket, "%s/%s", directory, HARNESS_SOCKET_NAME);
	char name[64] = "";
	ASSERT_BOOL_EQ(otherVersion(name, sizeof name), false);
	// This version's socket, others' named like it, and a socket an older version left when its
	// game crashed do not count.
	static const char *const others[] = {EDITION_HARNESS "-1.1.0.sock.new", "other-1.1.0.sock"};
	int locks[4];
	char path[1100];
	ASSERT_BOOL_EQ(leaveStaleSocket(testSocket), true);
	locks[0] = holdSocketLock(testSocket);
	for (size_t i = 0; i < 2; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, others[i]);
		ASSERT_BOOL_EQ(writeText(path, ""), true);
		locks[1 + i] = holdSocketLock(path);
	}
	(void)snprintf(path, sizeof path, "%s/" EDITION_HARNESS "-1.1.0.sock", directory);
	ASSERT_BOOL_EQ(leaveStaleSocket(path), true);
	ASSERT_BOOL_EQ(otherVersion(name, sizeof name), false);
	// A game serves the older version's socket: its name is reported, if it fits.
	locks[3] = holdSocketLock(path);
	ASSERT_BOOL_EQ(otherVersion(name, sizeof EDITION_HARNESS "-1.1.0.sock" - 1), false);
	ASSERT_BOOL_EQ(otherVersion(name, sizeof EDITION_HARNESS "-1.1.0.sock"), true);
	ASSERT_INT_EQ(strcmp(name, EDITION_HARNESS "-1.1.0.sock"), 0);
	for (size_t i = 0; i < 4; ++i) {
		ASSERT_BOOL_EQ(locks[i] >= 0, true);
		if (locks[i] >= 0) {
			close(locks[i]);
		}
	}
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

TEST(finds_football_manager_in_steam_libraries) {
	char home[] = "/tmp/" EDITION_LOWER "-home-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char path[1024];
	char other[1024];
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/steamapps", home);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(other, sizeof other, "%s/Games \"SSD\"", home);
	char vdf[4096];
	(void)snprintf(
		vdf,
		sizeof vdf,
		"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%s/Library/Application Support/Steam\"\n"
		"\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"%s/Games \\\"SSD\\\"\"\n\t}\n}\n",
		home,
		home
	);
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/steamapps/libraryfolders.vdf", home);
	ASSERT_BOOL_EQ(writeText(path, vdf), true);
	char app[1024];
	ASSERT_BOOL_EQ(macosGame_findInstalled(app, sizeof app), false);
	// The game is in the second library.
	(void)snprintf(path, sizeof path, "%s/steamapps/common/" EDITION_GAME "/fm.app/Contents/MacOS", other);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/steamapps/common/" EDITION_GAME "/fm.app/Contents/MacOS/" EDITION_GAME_EXECUTABLE, other);
	ASSERT_BOOL_EQ(writeText(path, "#!/bin/sh\n"), true);
	(void)snprintf(path, sizeof path, "%s/steamapps/common/" EDITION_GAME "/fm.app", other);
	ASSERT_BOOL_EQ(writeInfoPlist(path, EDITION_GAME_BUNDLE_ID, EDITION_GAME_EXECUTABLE), true);
	(void)snprintf(path, sizeof path, "%s/steamapps/appmanifest_" EDITION_STEAM_APP_ID ".acf", other);
	ASSERT_BOOL_EQ(writeText(path, "\"AppState\"\n{\n\t\"installdir\"\t\t\"" EDITION_GAME "\"\n}\n"), true);
	ASSERT_BOOL_EQ(macosGame_findInstalled(app, sizeof app), true);
	char expected[1024];
	(void)snprintf(expected, sizeof expected, "%s/steamapps/common/" EDITION_GAME "/fm.app", other);
	ASSERT_INT_EQ(strcmp(app, expected), 0);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(path), true);
	ASSERT_BOOL_EQ(writeInfoPlist(app, "com.SportsInteractive.FootballManager26", NULL), true);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(path), false);
#if EDITION_ENGINE_PLUGIN
	// The executable is the one the Info.plist names, in a bundle with exactly the game's identifier;
	// where it names none, it is the edition's.
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID ".Beta", NULL), true);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(path), false);
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, NULL), true);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(path), true);
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, "Renamed &amp; Game"), true);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(path), false);
	char renamed[1100];
	(void)snprintf(renamed, sizeof renamed, "%s/Contents/MacOS/Renamed & Game", app);
	ASSERT_BOOL_EQ(macosGame_isGameExecutable(renamed), true);
	char found[1024];
	ASSERT_BOOL_EQ(macosGame_bundle(renamed, found, sizeof found) && strcmp(found, app) == 0, true);
	ASSERT_BOOL_EQ(macosGame_bundle(path, found, sizeof found), false);
	ASSERT_BOOL_EQ(macosGame_findInstalled(found, sizeof found) && strcmp(found, app) == 0, true);
	char executable[1100];
	ASSERT_BOOL_EQ(macosGame_appExecutable(app, executable, sizeof executable), true);
	ASSERT_INT_EQ(strcmp(executable, renamed), 0);
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, "../" EDITION_GAME_EXECUTABLE), true);
	ASSERT_BOOL_EQ(macosGame_appExecutable(app, executable, sizeof executable), false);
	ASSERT_BOOL_EQ(macosGame_findInstalled(found, sizeof found), false);
#endif
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, EDITION_GAME_EXECUTABLE), true);
	// `launch` takes the bundle or its executable, and nothing else.
	char bundle[1024];
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ASSERT_BOOL_EQ(macosGame_bundle(path, bundle, sizeof bundle) && strcmp(bundle, app) == 0, true);
	(void)snprintf(path, sizeof path, "%s//", app);
	ASSERT_BOOL_EQ(macosGame_bundle(path, bundle, sizeof bundle) && strcmp(bundle, app) == 0, true);
	ASSERT_BOOL_EQ(macosGame_bundle(home, bundle, sizeof bundle), false);
	// The current Steam account's launch option for app 2252570 naming the launcher.
	static const char option[] =
		"\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n"
		"\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"%s\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LaunchOptions\"\t\t"
		"\"/bin/sh \\\"/x/" EDITION_LAUNCHER "\\\" %%command%%\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n";
	char config[2048];
	ASSERT_BOOL_EQ(launchOptionSet("/x/" EDITION_LAUNCHER), false);
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/userdata/42/config", home);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/userdata/42/config/localconfig.vdf", home);
	(void)snprintf(config, sizeof config, option, "2252590"); // another app's
	ASSERT_BOOL_EQ(writeText(path, config), true);
	ASSERT_BOOL_EQ(launchOptionSet("/x/" EDITION_LAUNCHER), false);
	(void)snprintf(config, sizeof config, option, EDITION_STEAM_APP_ID);
	ASSERT_BOOL_EQ(writeText(path, config), true);
	ASSERT_BOOL_EQ(launchOptionSet("/x/" EDITION_LAUNCHER), true);
	ASSERT_BOOL_EQ(launchOptionSet("/y/" EDITION_LAUNCHER), false);
	// With two accounts, the one that signed in last counts (42 is SteamID64 76561197960265770).
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/userdata/43/config", home);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/config", home);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/config/loginusers.vdf", home);
	ASSERT_BOOL_EQ(writeText(path, "\"users\"\n{\n\t\"76561197960265770\"\n\t{\n\t\t\"Timestamp\"\t\t\"200\"\n\t}\n"
		"\t\"76561197960265771\"\n\t{\n\t\t\"Timestamp\"\t\t\"100\"\n\t}\n}\n"), true);
	ASSERT_BOOL_EQ(launchOptionSet("/x/" EDITION_LAUNCHER), true);
	ASSERT_BOOL_EQ(writeText(path, "\"users\"\n{\n\t\"76561197960265770\"\n\t{\n\t\t\"Timestamp\"\t\t\"200\"\n\t}\n"
		"\t\"76561197960265771\"\n\t{\n\t\t\"MostRecent\"\t\t\"1\"\n\t}\n}\n"), true);
	ASSERT_BOOL_EQ(launchOptionSet("/x/" EDITION_LAUNCHER), false);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

// Copies a stand-in built next to fm24_tests (see CMakeLists.txt) to `to`.
static bool copyStandIn(const char *name, const char *to) {
	wchar_t wide[1024];
	char directory[1024];
	if (!paths_executableDirectory(wide, 1024) ||
			!platform_toUtf8(wide, directory, sizeof directory)) {
		return false;
	}
	char command[3200];
	int written = snprintf(command, sizeof command, "/bin/cp '%s/%s' '%s'", directory, name, to);
	return written > 0 && (size_t)written < sizeof command && system(command) == 0;
}

static bool readText(const char *path, char *out, size_t size) {
	FILE *file = fopen(path, "rb");
	size_t length = file ? fread(out, 1, size - 1, file) : 0;
	bool whole = file && !ferror(file) && feof(file);
	out[length] = '\0';
	return file && fclose(file) == 0 && whole;
}

// Starts the game bundle `app` through `launcher` as Steam does: as an Intel process where the Mac
// can run one (`intel`), with a stand-in for Steam's overlay or none. The game must keep the
// process, as Steam tracks it, get its arguments and the libraries in order, and run under Rosetta
// exactly when `translated`.
static void checkLaunch(
	const char *directory,
	const char *launcher,
	const char *app,
	bool overlay,
	bool intel,
	int translated
) {
	char real[PATH_MAX];
	ASSERT_BOOL_EQ(realpath(directory, real) != NULL, true);
	char inserted[1100] = "";
	char loaded[PATH_MAX + 16] = "";
	if (overlay) {
		(void)snprintf(inserted, sizeof inserted, "%s/overlay.dylib", directory);
		(void)snprintf(loaded, sizeof loaded, "%s/overlay.dylib:", real);
	}
	char pidPath[1100];
	char outputPath[1100];
	(void)snprintf(pidPath, sizeof pidPath, "%s/pid.txt", directory);
	(void)snprintf(outputPath, sizeof outputPath, "%s/started.txt", directory);
	(void)unlink(pidPath);
	(void)unlink(outputPath);
	char command[4096];
	(void)snprintf(
		command,
		sizeof command,
		"echo $$ > '%s' && export " EDITION_ENV_PREFIX "TEST_OUTPUT='%s' STEAM_DYLD_INSERT_LIBRARIES='%s' && exec "
		"%s/bin/sh '%s' '%s' -windowed 'two words'",
		pidPath,
		outputPath,
		inserted,
		intel ? "/usr/bin/arch -x86_64 " : "",
		launcher,
		app
	);
	ASSERT_INT_EQ(system(command), 0);
	char text[64];
	char started[4096];
	ASSERT_BOOL_EQ(
		readText(pidPath, text, sizeof text) && readText(outputPath, started, sizeof started), true
	);
	int pid = 0;
	int ranTranslated = -1;
	int consumed = 0;
	ASSERT_INT_EQ(sscanf(started, "%d|%d|%n", &pid, &ranTranslated, &consumed), 2);
	ASSERT_INT_EQ(pid, atoi(text));
	ASSERT_INT_EQ(ranTranslated, translated);
	// The harness is inserted after Steam's libraries.
	char expected[4096];
	(void)snprintf(
		expected,
		sizeof expected,
		"%s%s/" EDITION_HARNESS "-test.dylib|%s%s%s/" EDITION_HARNESS "-test.dylib|%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"
		"two words|",
		loaded,
		real,
		inserted,
		overlay ? ":" : "",
		directory,
		app
	);
	ASSERT_INT_EQ(strcmp(started + consumed, expected), 0);
}

// Copies a stand-in game into the bundle `app`, all of it or (with `slice`) one architecture.
static bool installStandInGame(const char *app, const char *slice) {
	char path[1100];
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS", app);
	if (!makeDirectories(path)) {
		return false;
	}
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	if (!slice) {
		return copyStandIn(STAND_IN_GAME, path);
	}
	char universal[1200];
	(void)snprintf(universal, sizeof universal, "%s.universal", path);
	char command[3000];
	(void)snprintf(
		command, sizeof command, "/usr/bin/lipo '%s' -thin %s -output '%s'", universal, slice, path
	);
	bool thinned = copyStandIn(STAND_IN_GAME, universal) && system(command) == 0;
	(void)unlink(universal);
	return thinned;
}

static bool hasSlice(const char *app, const char *slice) {
	char command[1200];
	(void)snprintf(
		command, sizeof command, "/usr/bin/lipo '%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE "' -verify_arch %s", app, slice
	);
	return system(command) == 0;
}

// Whether this Mac runs x86_64 programs: natively, or on Apple silicon under Rosetta. Not tried if
// FM24_ROSETTA_TESTS is OFF, as ctest sets it when Rosetta did not work when configuring (a hung
// Rosetta would otherwise hang the suite).
static bool runsIntelCode(void) {
	const char *setting = getenv(EDITION_ENV_PREFIX "ROSETTA_TESTS");
	if (setting && strcmp(setting, "OFF") == 0) {
		return false;
	}
	return system("/usr/bin/arch -x86_64 /usr/bin/true 2>/dev/null") == 0;
}

TEST(starts_the_game_through_the_launcher) {
	// In a folder with a space, as Application Support has.
	char directory[] = "/tmp/" EDITION_LOWER " launch-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char launcher[1024];
	(void)snprintf(launcher, sizeof launcher, "%s/" EDITION_LAUNCHER, directory);
	ASSERT_BOOL_EQ(writeLauncher(launcher, EDITION_HARNESS "-test.dylib"), true);
	char command[4096];
	(void)snprintf(command, sizeof command, "/bin/sh -n '%s'", launcher);
	ASSERT_INT_EQ(system(command), 0);
	// Stand-ins for the game, the harness and Steam's overlay record how the game was started.
	char app[1100];
	(void)snprintf(app, sizeof app, "%s/Fake Game.app", directory);
	ASSERT_BOOL_EQ(installStandInGame(app, NULL), true);
	char path[1100];
	(void)snprintf(path, sizeof path, "%s/" EDITION_HARNESS "-test.dylib", directory);
	ASSERT_BOOL_EQ(copyStandIn(STAND_IN_LIBRARY, path), true);
	(void)snprintf(path, sizeof path, "%s/overlay.dylib", directory);
	ASSERT_BOOL_EQ(copyStandIn(STAND_IN_LIBRARY, path), true);
	// Steam starts the launch option as an Intel process, and so does this test where the Mac can
	// run one (with Rosetta on Apple silicon). The game still runs natively where it can.
	int appleSilicon = 0;
	size_t size = sizeof appleSilicon;
	if (sysctlbyname("hw.optional.arm64", &appleSilicon, &size, NULL, 0) != 0) {
		appleSilicon = 0; // an Intel Mac
	}
	bool intel = runsIntelCode();
	int translated = appleSilicon && !hasSlice(app, "arm64");
	checkLaunch(directory, launcher, app, false, intel, translated);
	checkLaunch(directory, launcher, app, true, intel, translated);
	if (intel && hasSlice(app, "x86_64")) {
		// A game without an arm64 slice (the fallback Intel Macs rely on).
		char intelApp[1100];
		(void)snprintf(intelApp, sizeof intelApp, "%s/Intel Game.app", directory);
		ASSERT_BOOL_EQ(installStandInGame(intelApp, "x86_64"), true);
		checkLaunch(directory, launcher, intelApp, true, intel, appleSilicon);
		// The line an Intel Mac runs, where sysctl reports no arm64.
		static const char probe[] = "/usr/sbin/sysctl -n hw.optional.arm64";
		char script[4096];
		ASSERT_BOOL_EQ(readText(launcher, script, sizeof script), true);
		const char *at = strstr(script, probe);
		ASSERT_BOOL_EQ(at != NULL, true);
		if (at) {
			char intelScript[4096];
			(void)snprintf(
				intelScript,
				sizeof intelScript,
				"%.*s/bin/echo 0%s",
				(int)(at - script),
				script,
				at + strlen(probe)
			);
			(void)snprintf(path, sizeof path, "%s/launch-intel.sh", directory);
			ASSERT_BOOL_EQ(writeText(path, intelScript), true);
			checkLaunch(directory, path, app, true, intel, appleSilicon);
		}
	}
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

// Football Manager 2024 in the Steam library `library`: its manifest and an fm.app with the
// game's bundle identifier. Writes the bundle's path to `app`.
static bool installFakeGame(const char *library, char *app, size_t size) {
	char path[1200];
	(void)snprintf(app, size, "%s/steamapps/common/" EDITION_GAME "/fm.app", library);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS", app);
	bool ok = makeDirectories(path);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ok = ok && writeText(path, "#!/bin/sh\n");
	ok = ok && writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, EDITION_GAME_EXECUTABLE);
	(void)snprintf(path, sizeof path, "%s/steamapps/appmanifest_" EDITION_STEAM_APP_ID ".acf", library);
	return ok &&
				 writeText(path, "\"AppState\"\n{\n\t\"installdir\"\t\t\"" EDITION_GAME "\"\n}\n");
}

TEST(opens_the_game_with_the_harness_inserted) {
	char home[] = "/tmp/" EDITION_LOWER "-open-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	static const char launcher[] = "/x/Application Support/" EDITION_HARNESS "/" EDITION_LAUNCHER;
	char steam[1100];
	char app[1100];
	char path[1200];
	wchar_t given[1200];
	(void)snprintf(steam, sizeof steam, "%s/Library/Application Support/Steam", home);
	// Nothing starts without the game.
	spawned[0] = 0;
	ASSERT_BOOL_EQ(startDirectly(NULL, launcher, NULL, false), false);
	ASSERT_BOOL_EQ(
		platform_fromUtf8(home, given, 1200) && !startDirectly(given, launcher, NULL, false), true
	);
	ASSERT_INT_EQ(spawned[0], 0);
	// The installed game, its bundle or its executable: a new instance, with the dylib next to the
	// launcher inserted and Steam's app ID.
	ASSERT_BOOL_EQ(installFakeGame(steam, app, sizeof app), true);
	char expected[4096];
	(void)snprintf(
		expected,
		sizeof expected,
		"/usr/bin/open|/usr/bin/open|-n|--env|DYLD_INSERT_LIBRARIES=/x/Application "
		"Support/" EDITION_HARNESS "/" EDITION_HARNESS "-%s.dylib|--env|SteamAppId=" EDITION_STEAM_APP_ID "|--env|"
		"SteamGameId=" EDITION_STEAM_APP_ID "|%s|",
		HARNESS_PROTOCOL_TEXT,
		app
	);
	// Each says first that this start has not been verified, and why it starts the game this way.
	Capture capture;
	char printed[4096];
	char notice[4096];
	ASSERT_BOOL_EQ(startCapture(&capture), true);
	bool opened = startDirectly(NULL, launcher, NULL, false);
	endCapture(&capture, printed, sizeof printed);
	ASSERT_BOOL_EQ(opened, true);
	ASSERT_INT_EQ(strcmp(spawned, expected), 0);
	(void)snprintf(
		notice,
		sizeof notice,
		"Steam's launch option for " EDITION_GAME " is not set, or Steam has not saved it yet: "
		"opening fm.app directly, which has not been verified: the game crashed in its one attempt "
		"(`" EDITION_CLI " install` says how to set Steam's launch option)\nstarting %s with the harness\n",
		app
	);
	ASSERT_INT_EQ(strcmp(printed, notice), 0);
	spawned[0] = 0;
	ASSERT_BOOL_EQ(platform_fromUtf8(app, given, 1200) && startCapture(&capture), true);
	opened = startDirectly(given, launcher, NULL, false);
	endCapture(&capture, printed, sizeof printed);
	ASSERT_BOOL_EQ(opened, true);
	ASSERT_INT_EQ(strcmp(spawned, expected), 0);
	(void)snprintf(
		notice,
		sizeof notice,
		"Opening the given fm.app directly, without Steam's launch option, which has not been "
		"verified: the game crashed in its one attempt (`" EDITION_CLI " install` says how to set Steam's "
		"launch option)\nstarting %s with the harness\n",
		app
	);
	ASSERT_INT_EQ(strcmp(printed, notice), 0);
	spawned[0] = 0;
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ASSERT_BOOL_EQ(
		platform_fromUtf8(path, given, 1200) && startDirectly(given, launcher, NULL, false), true
	);
	ASSERT_INT_EQ(strcmp(spawned, expected), 0);
	// With an auto-play file, the game gets it as its only argument.
	static const char autoplay[] = "/x/Application Support/" EDITION_HARNESS "/autoplay-42.json";
	size_t length = strlen(expected);
	(void)snprintf(
		expected + length, sizeof expected - length, "--args|--auto_play_game_file=%s|", autoplay
	);
	spawned[0] = 0;
	ASSERT_BOOL_EQ(startDirectly(NULL, launcher, autoplay, false), true);
	ASSERT_INT_EQ(strcmp(spawned, expected), 0);
	// --rosetta: open starts the x86_64 slice, with or without the file.
	char rosetta[4096];
	char *at = strstr(expected, app);
	ASSERT_BOOL_EQ(at != NULL, true);
	if (at) {
		(void)snprintf(
			rosetta, sizeof rosetta, "%.*s--arch|x86_64|%s", (int)(at - expected), expected, at
		);
		spawned[0] = 0;
		ASSERT_BOOL_EQ(startDirectly(NULL, launcher, autoplay, true), true);
		ASSERT_INT_EQ(strcmp(spawned, rosetta), 0);
		rosetta[strlen(rosetta) - strlen(expected + length)] = 0;
		spawned[0] = 0;
		ASSERT_BOOL_EQ(startDirectly(NULL, launcher, NULL, true), true);
		ASSERT_INT_EQ(strcmp(spawned, rosetta), 0);
	}
	// open fails or cannot start.
	spawnInstead = "/usr/bin/false";
	ASSERT_BOOL_EQ(startDirectly(NULL, launcher, NULL, false), false);
	spawnInstead = "/usr/bin/true";
	spawnError = ENOENT;
	ASSERT_BOOL_EQ(startDirectly(NULL, launcher, NULL, false), false);
	spawnError = 0;
	// The inserted path does not fit.
	char longLauncher[4200];
	memset(longLauncher, 'a', sizeof longLauncher);
	(void)snprintf(longLauncher + 4090, sizeof longLauncher - 4090, "/" EDITION_LAUNCHER);
	longLauncher[0] = '/';
	spawned[0] = 0;
	ASSERT_BOOL_EQ(startDirectly(NULL, longLauncher, NULL, false), false);
	ASSERT_INT_EQ(spawned[0], 0);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(waits_until_the_started_game_keeps_serving) {
	enum { START = 1000 }; // a clock time: never 0, like a real one
	// No game process.
	LaunchWatch watch = {.started = START, .reported = START};
	ASSERT_INT_EQ(launchState(&watch, 0, false, START + START_TIMEOUT_S - 1), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 0, false, START + START_TIMEOUT_S), LAUNCH_NEVER_STARTED);
	// Ready once one game process has served for SETTLE_S.
	watch = (LaunchWatch){.started = START};
	ASSERT_INT_EQ(launchState(&watch, 7, false, START + 5), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + 6), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + 5 + SETTLE_S), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + 6 + SETTLE_S), LAUNCH_READY);
	ASSERT_INT_EQ(watch.readyPid, 7);
	// A new process, or one that stopped serving for a moment, must settle again.
	watch = (LaunchWatch){.started = START};
	ASSERT_INT_EQ(launchState(&watch, 7, true, START), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, true, START + 9), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, true, START + SETTLE_S), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, false, START + SETTLE_S + 1), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, true, START + SETTLE_S + 2), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, true, START + 2 * SETTLE_S + 1), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 8, true, START + 2 * SETTLE_S + 2), LAUNCH_READY);
	ASSERT_INT_EQ(watch.readyPid, 8);
	// The game quits after loading the harness, or before.
	for (int loaded = 0; loaded < 2; ++loaded) {
		watch = (LaunchWatch){.started = START};
		ASSERT_INT_EQ(launchState(&watch, 7, loaded, START), LAUNCH_WAITING);
		ASSERT_INT_EQ(launchState(&watch, 0, false, START + EXIT_GRACE_S), LAUNCH_WAITING);
		ASSERT_INT_EQ(launchState(&watch, 0, false, START + EXIT_GRACE_S + 1), LAUNCH_EXITED);
		ASSERT_BOOL_EQ(watch.wasReady, loaded);
	}
	// It runs without the harness: Steam started it some other way.
	watch = (LaunchWatch){.started = START};
	ASSERT_INT_EQ(launchState(&watch, 7, false, START), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, false, START + NO_HARNESS_GRACE_S), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + NO_HARNESS_GRACE_S + 1), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, false, START + NO_HARNESS_GRACE_S + 2), LAUNCH_NO_HARNESS);
	// Under Rosetta, which may first translate the game, it has longer.
	ASSERT_BOOL_EQ(ROSETTA_GRACE_S > NO_HARNESS_GRACE_S, true);
	watch = (LaunchWatch){.started = START, .rosetta = true};
	ASSERT_INT_EQ(launchState(&watch, 7, false, START), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, false, START + ROSETTA_GRACE_S), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, false, START + ROSETTA_GRACE_S + 1), LAUNCH_NO_HARNESS);
	watch = (LaunchWatch){.started = START, .rosetta = true};
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + ROSETTA_GRACE_S), LAUNCH_WAITING);
	ASSERT_INT_EQ(launchState(&watch, 7, true, START + ROSETTA_GRACE_S + SETTLE_S), LAUNCH_READY);
}

static bool endsWith(const char *text, const char *suffix) {
	size_t length = strlen(text);
	size_t suffixLength = strlen(suffix);
	return length >= suffixLength && strcmp(text + length - suffixLength, suffix) == 0;
}

// Starts the game bundle `app` through `launcher` with the argument -windowed, as Steam does but
// natively. Writes what the stand-in game recorded to `started`; returns its process ID, or 0.
static int startThroughLauncher(
	const char *directory,
	const char *launcher,
	const char *app,
	char *started,
	size_t size
) {
	char pidPath[1100];
	char outputPath[1100];
	char command[4096];
	char pid[64];
	(void)snprintf(pidPath, sizeof pidPath, "%s/pid.txt", directory);
	(void)snprintf(outputPath, sizeof outputPath, "%s/started.txt", directory);
	(void)unlink(pidPath);
	(void)unlink(outputPath);
	(void)snprintf(
		command,
		sizeof command,
		"echo $$ > '%s' && export " EDITION_ENV_PREFIX "TEST_OUTPUT='%s' STEAM_DYLD_INSERT_LIBRARIES= && exec /bin/sh "
		"'%s' '%s' -windowed",
		pidPath,
		outputPath,
		launcher,
		app
	);
	bool ran = system(command) == 0 && readText(pidPath, pid, sizeof pid) &&
						 readText(outputPath, started, size);
	return ran ? atoi(pid) : 0;
}

// The ID of a process that has exited, or 0.
static pid_t exitedProcess(void) {
	pid_t pid = 0;
	char *const program[] = {"/usr/bin/true", NULL};
	int status = 0;
	bool ran = posix_spawn(&pid, program[0], NULL, NULL, program, environ) == 0 &&
						 waitpid(pid, &status, 0) == pid;
	return ran ? pid : 0;
}

TEST(hands_a_fresh_save_request_to_the_game_once) {
	// Quotes and spaces in the folder, as a home folder's name may have.
	char directory[] = "/tmp/" EDITION_LOWER " \"auto\" play-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char launcher[1100];
	char app[1100];
	char path[1200];
	(void)snprintf(launcher, sizeof launcher, "%s/" EDITION_LAUNCHER, directory);
	(void)snprintf(app, sizeof app, "%s/Fake Game.app", directory);
	(void)snprintf(path, sizeof path, "%s/" EDITION_HARNESS "-test.dylib", directory);
	ASSERT_BOOL_EQ(writeLauncher(launcher, EDITION_HARNESS "-test.dylib"), true);
	ASSERT_BOOL_EQ(installStandInGame(app, NULL) && copyStandIn(STAND_IN_LIBRARY, path), true);
	char command[1300];
	(void)snprintf(command, sizeof command, "/bin/sh -n '%s'", launcher);
	ASSERT_INT_EQ(system(command), 0);
	char request[1100];
	char taken[1100];
	char started[4096];
	char tail[2400];
	char text[256];
	// A request of this process, which waits for it, as launch --save does.
	(void)snprintf(request, sizeof request, "%s/autoplay-request-%d.json", directory, (int)getpid());
	// Without a request the game gets only its own arguments.
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	// A fresh request is renamed after the game's process and added after its arguments, once.
	static const char json[] = "{\"LOAD_GAME\": \"/x/LIV_MAIN_FM24.fm\"}";
	ASSERT_BOOL_EQ(writeText(request, json), true);
	int pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	(void)snprintf(
		tail, sizeof tail, "|-windowed|--auto_play_game_file=%s/autoplay-%d.json|", directory, pid
	);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, tail), true);
	(void)snprintf(taken, sizeof taken, "%s/autoplay-%d.json", directory, pid);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && readText(taken, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, json), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	// One four and a half minutes old is still taken; one older than five minutes is ignored.
	ASSERT_BOOL_EQ(writeText(request, json), true);
	struct timeval old[2] = {{.tv_sec = time(NULL) - 270}, {.tv_sec = time(NULL) - 270}};
	ASSERT_INT_EQ(utimes(request, old), 0);
	pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	(void)snprintf(
		tail, sizeof tail, "|-windowed|--auto_play_game_file=%s/autoplay-%d.json|", directory, pid
	);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, tail) && access(request, F_OK) != 0, true);
	ASSERT_BOOL_EQ(writeText(request, json), true);
	old[0].tv_sec = old[1].tv_sec = time(NULL) - 6 * 60;
	ASSERT_INT_EQ(utimes(request, old), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	ASSERT_INT_EQ(access(request, F_OK), 0);
	// A request whose fm24ctl has exited (one interrupted while Steam started the game), or whose
	// name has no process ID, is ignored; a live one beside it is taken.
	char orphan[1100];
	char odd[1100];
	pid_t exited = exitedProcess();
	(void)snprintf(orphan, sizeof orphan, "%s/autoplay-request-%d.json", directory, (int)exited);
	(void)snprintf(odd, sizeof odd, "%s/autoplay-request-12x.json", directory);
	ASSERT_BOOL_EQ(exited > 0 && writeText(orphan, json) && writeText(odd, json), true);
	ASSERT_INT_EQ(unlink(request), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	ASSERT_BOOL_EQ(access(orphan, F_OK) == 0 && access(odd, F_OK) == 0, true);
	static const char other[] = "{\"LOAD_GAME\": \"/x/Other.fm\"}";
	ASSERT_BOOL_EQ(writeText(request, other), true);
	pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	(void)snprintf(
		tail, sizeof tail, "|-windowed|--auto_play_game_file=%s/autoplay-%d.json|", directory, pid
	);
	(void)snprintf(taken, sizeof taken, "%s/autoplay-%d.json", directory, pid);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, tail) && readText(taken, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, other), 0);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && access(orphan, F_OK) == 0, true);
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

// Whether the stand-in game ran under Rosetta, from what it recorded: 1 or 0, or -1 if unreadable.
static int ranTranslated(const char *started) {
	int pid = 0;
	int translated = -1;
	return sscanf(started, "%d|%d|", &pid, &translated) == 2 ? translated : -1;
}

static int appleSiliconMac(void) {
	int appleSilicon = 0;
	size_t size = sizeof appleSilicon;
	if (sysctlbyname("hw.optional.arm64", &appleSilicon, &size, NULL, 0) != 0) {
		appleSilicon = 0; // an Intel Mac
	}
	return appleSilicon;
}

TEST(starts_the_x86_64_slice_for_a_fresh_rosetta_request_once) {
	char directory[] = "/tmp/" EDITION_LOWER " \"rosetta\" launch-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char launcher[1100];
	char app[1100];
	char path[1200];
	(void)snprintf(launcher, sizeof launcher, "%s/" EDITION_LAUNCHER, directory);
	(void)snprintf(app, sizeof app, "%s/Fake Game.app", directory);
	(void)snprintf(path, sizeof path, "%s/" EDITION_HARNESS "-test.dylib", directory);
	ASSERT_BOOL_EQ(writeLauncher(launcher, EDITION_HARNESS "-test.dylib"), true);
	ASSERT_BOOL_EQ(installStandInGame(app, NULL) && copyStandIn(STAND_IN_LIBRARY, path), true);
	char command[1300];
	(void)snprintf(command, sizeof command, "/bin/sh -n '%s'", launcher);
	ASSERT_INT_EQ(system(command), 0);
	// The x86_64 slice runs under Rosetta on Apple silicon, and natively on an Intel Mac. Without
	// Rosetta, Apple silicon cannot run it at all.
	int appleSilicon = appleSiliconMac();
	if (!runsIntelCode()) {
		ASSERT_BOOL_EQ(removeTree(directory), true);
		return;
	}
	char request[1100];
	char save[1100];
	char taken[1100];
	char started[4096];
	char tail[2400];
	char text[256];
	// Requests of this process, which waits for them, as launch --rosetta and --save do.
	(void)snprintf(request, sizeof request, "%s/rosetta-request-%d", directory, (int)getpid());
	(void)snprintf(save, sizeof save, "%s/autoplay-request-%d.json", directory, (int)getpid());
	// Without a request the game runs natively where it can, as before.
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_INT_EQ(ranTranslated(started), 0);
	ASSERT_BOOL_EQ(endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	// A fresh request starts the x86_64 slice, with the same arguments and libraries, once, and no
	// file is left.
	ASSERT_BOOL_EQ(writeText(request, "x86_64\n"), true);
	int pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, "/Contents/MacOS/" EDITION_GAME_EXECUTABLE "|-windowed|"), true);
	ASSERT_INT_EQ(ranTranslated(started), appleSilicon);
	(void)snprintf(taken, sizeof taken, "%s/rosetta-%d", directory, pid);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && access(taken, F_OK) != 0, true);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_INT_EQ(ranTranslated(started), 0);
	// With a save request too, the game gets both, and the save's file as fm24ctl wrote it.
	static const char json[] = "{\"LOAD_GAME\": \"/x/LIV_MAIN_FM24.fm\"}";
	ASSERT_BOOL_EQ(writeText(request, "x86_64\n") && writeText(save, json), true);
	pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	(void)snprintf(
		tail, sizeof tail, "|-windowed|--auto_play_game_file=%s/autoplay-%d.json|", directory, pid
	);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, tail), true);
	ASSERT_INT_EQ(ranTranslated(started), appleSilicon);
	(void)snprintf(taken, sizeof taken, "%s/autoplay-%d.json", directory, pid);
	ASSERT_BOOL_EQ(readText(taken, text, sizeof text) && strcmp(text, json) == 0, true);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && access(save, F_OK) != 0, true);
	// A save request alone leaves the slice as it was.
	ASSERT_BOOL_EQ(writeText(save, json), true);
	pid = startThroughLauncher(directory, launcher, app, started, sizeof started);
	(void)snprintf(
		tail, sizeof tail, "|-windowed|--auto_play_game_file=%s/autoplay-%d.json|", directory, pid
	);
	ASSERT_BOOL_EQ(pid > 0 && endsWith(started, tail), true);
	ASSERT_INT_EQ(ranTranslated(started), 0);
	// One four and a half minutes old is still taken; one older than five minutes is ignored.
	ASSERT_BOOL_EQ(writeText(request, "x86_64\n"), true);
	struct timeval old[2] = {{.tv_sec = time(NULL) - 270}, {.tv_sec = time(NULL) - 270}};
	ASSERT_INT_EQ(utimes(request, old), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(ranTranslated(started) == appleSilicon && access(request, F_OK) != 0, true);
	ASSERT_BOOL_EQ(writeText(request, "x86_64\n"), true);
	old[0].tv_sec = old[1].tv_sec = time(NULL) - 6 * 60;
	ASSERT_INT_EQ(utimes(request, old), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(ranTranslated(started) == 0 && access(request, F_OK) == 0, true);
	// A request whose fm24ctl has exited, or whose name has no process ID, is ignored; a live one
	// beside them is taken.
	char orphan[1100];
	char odd[1100];
	pid_t exited = exitedProcess();
	(void)snprintf(orphan, sizeof orphan, "%s/rosetta-request-%d", directory, (int)exited);
	(void)snprintf(odd, sizeof odd, "%s/rosetta-request-12x", directory);
	ASSERT_BOOL_EQ(exited > 0 && writeText(orphan, "x86_64\n") && writeText(odd, "x86_64\n"), true);
	ASSERT_INT_EQ(unlink(request), 0);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_INT_EQ(ranTranslated(started), 0);
	ASSERT_BOOL_EQ(access(orphan, F_OK) == 0 && access(odd, F_OK) == 0, true);
	ASSERT_BOOL_EQ(writeText(request, "x86_64\n"), true);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_INT_EQ(ranTranslated(started), appleSilicon);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && access(orphan, F_OK) == 0, true);
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

#if EDITION_ENGINE_PLUGIN
TEST(starts_the_executable_the_apps_info_plist_names) {
	char directory[] = "/tmp/" EDITION_LOWER " renamed-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char launcher[1100];
	char app[1100];
	char path[1300];
	char started[4096];
	char expected[1400];
	(void)snprintf(launcher, sizeof launcher, "%s/" EDITION_LAUNCHER, directory);
	(void)snprintf(app, sizeof app, "%s/Renamed Game.app", directory);
	(void)snprintf(path, sizeof path, "%s/" EDITION_HARNESS "-test.dylib", directory);
	ASSERT_BOOL_EQ(writeLauncher(launcher, EDITION_HARNESS "-test.dylib"), true);
	ASSERT_BOOL_EQ(copyStandIn(STAND_IN_LIBRARY, path), true);
	// A build that names its executable otherwise, as Steam's %command% may give the app.
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS", app);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/Contents/MacOS/Renamed & \"Game\"", app);
	ASSERT_BOOL_EQ(copyStandIn(STAND_IN_GAME, path), true);
	ASSERT_BOOL_EQ(
		writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, "Renamed &amp; &quot;Game&quot;"), true
	);
	(void)snprintf(expected, sizeof expected, "|%s|-windowed|", path);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(strstr(started, expected) != NULL, true);
	char slashed[1200];
	(void)snprintf(slashed, sizeof slashed, "%s/", app);
	ASSERT_BOOL_EQ(
		startThroughLauncher(directory, launcher, slashed, started, sizeof started) > 0, true
	);
	ASSERT_BOOL_EQ(strstr(started, expected) != NULL, true);
	// An Info.plist that names none: the edition's executable.
	char edition[1300];
	(void)snprintf(edition, sizeof edition, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	ASSERT_INT_EQ(rename(path, edition), 0);
	ASSERT_BOOL_EQ(writeInfoPlist(app, EDITION_GAME_BUNDLE_ID, NULL), true);
	(void)snprintf(expected, sizeof expected, "|%s|-windowed|", edition);
	ASSERT_BOOL_EQ(startThroughLauncher(directory, launcher, app, started, sizeof started) > 0, true);
	ASSERT_BOOL_EQ(strstr(started, expected) != NULL, true);
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

// The stand-in game of the plugin test, in a folder with spaces, as the game's are: an app with
// the game's executable and engine plugin, and another bundle, beside another build of the plugin
// and the stand-in harness (CMakeLists.txt), which the game loads.
#define STAND_IN_APP "FM Stand-in.app"
static const char *const standInFiles[][2] = {
	{STAND_IN_APP "/Contents/PlugIns/" EDITION_ENGINE_PATH, STAND_IN_PLUGIN},
	{STAND_IN_APP "/Contents/PlugIns/BacktraceMacUnity.bundle/Contents/MacOS/BacktraceMacUnity",
		STAND_IN_PLUGIN},
	{"Other Build/" EDITION_ENGINE_PATH, STAND_IN_OTHER_PLUGIN},
	{"Harness Folder/" EDITION_HARNESS "-test.dylib", STAND_IN_HARNESS},
};
enum { STAND_IN_PLUGIN_FILE, STAND_IN_UNRELATED_FILE, STAND_IN_OTHER_FILE, STAND_IN_HARNESS_FILE };

// The stand-in game, running, with pipes to its standard input and output.
typedef struct StandInGame {
	pid_t pid;
	int commands;
	int answers;
} StandInGame;

static bool installStandInPluginGame(const char *directory) {
	char path[1200];
	(void)snprintf(path, sizeof path, "%s/" STAND_IN_APP, directory);
	if (!installStandInGame(path, NULL)) {
		return false;
	}
	(void)snprintf(path, sizeof path, "%s/" STAND_IN_APP "/Contents/Info.plist", directory);
	if (!writeText(
				path,
				"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
				"\t<key>CFBundleExecutable</key>\n\t<string>" EDITION_GAME_EXECUTABLE "</string>\n"
				"\t<key>CFBundleShortVersionString</key>\n\t<string>27.0.0.1</string>\n"
				"</dict>\n</plist>\n"
			)) {
		return false;
	}
	for (size_t i = 0; i < sizeof standInFiles / sizeof standInFiles[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, standInFiles[i][0]);
		char *slash = strrchr(path, '/');
		*slash = 0;
		bool made = makeDirectories(path);
		*slash = '/';
		if (!made || !copyStandIn(standInFiles[i][1], path)) {
			return false;
		}
	}
	return true;
}

static bool startStandInGameAt(StandInGame *game, const char *directory, const char *executable);

// Starts the stand-in game in `directory` with the stand-in harness inserted, in this slice,
// serving testSocket. The stand-in engine notes its calls in `directory`/engine.log.
static bool startStandInGame(StandInGame *game, const char *directory) {
	char executable[1200];
	(void)snprintf(
		executable,
		sizeof executable,
		"%s/" STAND_IN_APP "/Contents/MacOS/" EDITION_GAME_EXECUTABLE,
		directory
	);
	return startStandInGameAt(game, directory, executable);
}

// startStandInGame with the stand-in game's executable at `executable`.
static bool startStandInGameAt(StandInGame *game, const char *directory, const char *executable) {
	char inserted[1300];
	char socket[160];
	char engineLog[1300];
	(void)snprintf(
		inserted,
		sizeof inserted,
		"DYLD_INSERT_LIBRARIES=%s/%s",
		directory,
		standInFiles[STAND_IN_HARNESS_FILE][0]
	);
	(void)snprintf(socket, sizeof socket, EDITION_ENV_PREFIX "TEST_SOCKET=%s", testSocket);
	(void)snprintf(
		engineLog, sizeof engineLog, EDITION_ENV_PREFIX "TEST_ENGINE_LOG=%s/engine.log", directory
	);
	size_t count = 0;
	while (environ[count]) {
		++count;
	}
	char **environment = calloc(count + 5, sizeof *environment);
	int input[2] = {-1, -1};
	int output[2] = {-1, -1};
	if (!environment || pipe(input) != 0 || pipe(output) != 0) {
		free(environment);
		return false;
	}
	size_t used = 0;
	static const char testPrefix[] = EDITION_ENV_PREFIX "TEST_";
	for (size_t i = 0; i < count; ++i) {
		if (strncmp(environ[i], "DYLD_", 5) != 0 &&
				strncmp(environ[i], testPrefix, sizeof testPrefix - 1) != 0) {
			environment[used++] = environ[i];
		}
	}
	char bundles[] = EDITION_ENV_PREFIX "TEST_BUNDLES=1";
	environment[used++] = inserted;
	environment[used++] = socket;
	environment[used++] = engineLog;
	environment[used++] = bundles;
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	cpu_type_t slice = cpuType(sliceArchitecture());
	size_t set = 0;
	(void)posix_spawn_file_actions_init(&actions);
	(void)posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
	(void)posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
	(void)posix_spawnattr_init(&attributes);
	(void)posix_spawnattr_setbinpref_np(&attributes, 1, &slice, &set);
	char *arguments[] = {(char *)executable, NULL};
	int error = posix_spawn(&game->pid, executable, &actions, &attributes, arguments, environment);
	(void)posix_spawn_file_actions_destroy(&actions);
	(void)posix_spawnattr_destroy(&attributes);
	free(environment);
	close(input[0]);
	close(output[1]);
	game->commands = input[1];
	game->answers = output[0];
	(void)fcntl(game->commands, F_SETNOSIGPIPE, 1);
	if (error != 0) {
		close(game->commands);
		close(game->answers);
	}
	return error == 0 && set == 1;
}

static bool hears(StandInGame *game, const char *answer);

// Gives the stand-in game a command; true if it answers `answer` within ten seconds.
static bool tell(StandInGame *game, const char *command, const char *answer) {
	char line[1400];
	int length = snprintf(line, sizeof line, "%s\n", command);
	return length > 0 && (size_t)length < sizeof line &&
				 write(game->commands, line, (size_t)length) == length && hears(game, answer);
}

// True if the stand-in game's next line, within ten seconds, is `answer`.
static bool hears(StandInGame *game, const char *answer) {
	char line[1400];
	size_t got = 0;
	for (;;) {
		struct pollfd entry = {.fd = game->answers, .events = POLLIN};
		if (poll(&entry, 1, 10000) != 1 || read(game->answers, line + got, 1) != 1) {
			return false;
		}
		if (line[got] == '\n' || ++got >= sizeof line - 1) {
			break;
		}
	}
	line[got] = 0;
	if (strcmp(line, answer) != 0) {
		fprintf(stderr, "the stand-in game answered: %s\n", line);
		return false;
	}
	return true;
}

// Has the stand-in game exit; its exit status, or -1 if it did not exit within ten seconds (it is
// then killed).
static int stopStandInGame(StandInGame *game) {
	(void)write(game->commands, "exit\n", 5);
	close(game->commands);
	int status = 0;
	pid_t done = 0;
	for (int i = 0; i < 1000 && (done = waitpid(game->pid, &status, WNOHANG)) == 0; ++i) {
		platform_sleep(10);
	}
	if (done == 0) {
		(void)kill(game->pid, SIGKILL);
		(void)waitpid(game->pid, &status, 0);
	}
	close(game->answers);
	return done == game->pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The stand-in harness's log, or "".
static const char *standInLog(const char *directory) {
	static char text[65536];
	char path[1200];
	(void)snprintf(path, sizeof path, "%s/Harness Folder/harness.log", directory);
	return readText(path, text, sizeof text) ? text : "";
}

// True once the stand-in harness's log has each of `lines` in that order (within ten seconds).
static bool logShows(const char *directory, const char *const *lines, size_t count) {
	for (int attempt = 0; attempt < 500; ++attempt) {
		const char *at = standInLog(directory);
		for (size_t i = 0; i < count && at; ++i) {
			at = strstr(at, lines[i]);
			at = at ? at + strlen(lines[i]) : NULL;
		}
		if (at) {
			return true;
		}
		platform_sleep(20);
	}
	fprintf(stderr, "harness.log:\n%s\n", standInLog(directory));
	return false;
}

static bool servedWithin(const char *socket, unsigned milliseconds) {
	for (unsigned waited = 0; waited < milliseconds; waited += 20) {
		if (channel_served(socket)) {
			return true;
		}
		platform_sleep(20);
	}
	return false;
}

static void answers(const char *job, const char *expected) {
	char result[HARNESS_RESULT_LENGTH];
	ASSERT_BOOL_EQ(macosTest_submit(job, 10000, result, sizeof result), true);
	if (strcmp(result, expected) != 0) {
		fprintf(stderr, "%s -> %s\n", job, result);
	}
	ASSERT_INT_EQ(strcmp(result, expected), 0);
}

static void removeStandInGame(const char *directory) {
	char lockPath[sizeof testSocket + 8];
	if (channel_lockPath(testSocket, lockPath, sizeof lockPath)) {
		(void)unlink(lockPath);
	}
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

TEST(finds_the_engine_as_the_game_loads_and_unloads_it) {
	static const char starting[] =
		"ok: starting in_progress=0 processing=0 kind=-1 people=0 human_team=0 menu_seen=0 autoplay=0 "
		"autoplay_read=0";
	static const char refused[] =
		"error: harness " HARNESS_PROTOCOL_TEXT " does not support this game build; see harness.log";
	static const char quitting[] =
		"error: " EDITION_PRODUCT " is quitting: it has unloaded its match engine";
	char directory[] = "/tmp/" EDITION_LOWER " stand-in game-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stand-in-%d.sock", (int)getpid()
	);
	char load[3][1300];
	for (int i = 0; i < 3; ++i) {
		(void)snprintf(load[i], sizeof load[i], "load %s/%s", directory, standInFiles[i][0]);
	}
	char run[1300];
	(void)snprintf(
		run,
		sizeof run,
		"command=run\ninput=%s/none.bin\noutput=%s/none.csv\nseeds=1\n",
		directory,
		directory
	);
	StandInGame game;
	bool started = installStandInPluginGame(directory) && startStandInGame(&game, directory);
	ASSERT_BOOL_EQ(started, true);
	if (!started) {
		removeStandInGame(directory);
		return;
	}
	// The harness serves at once: the game is starting until it loads its engine, and other jobs
	// wait for that.
	ASSERT_BOOL_EQ(servedWithin(testSocket, 10000), true);
	answers("command=status\n", starting);
	answers(run, "error: " EDITION_PRODUCT " is busy (starting); wait until it is idle");
	// Another bundle is not the engine, even with the engine's code in it.
	ASSERT_BOOL_EQ(tell(&game, load[STAND_IN_UNRELATED_FILE], "loaded"), true);
	answers("command=status\n", starting);
	// Another build of the engine is refused, as every job is until the game unloads it.
	ASSERT_BOOL_EQ(tell(&game, load[STAND_IN_OTHER_FILE], "loaded"), true);
	answers("command=status\n", refused);
	ASSERT_BOOL_EQ(tell(&game, "unload", "unloaded"), true);
	answers("command=status\n", quitting);
	// The supported build is accepted, and the state of a game at its main menu read from it; jobs
	// reach the job code.
	ASSERT_BOOL_EQ(tell(&game, load[STAND_IN_PLUGIN_FILE], "loaded"), true);
	answers(
		"command=status\n",
		"ok: menu in_progress=0 processing=0 kind=-1 people=0 human_team=0 menu_seen=1 autoplay=0 "
		"autoplay_read=0"
	);
	answers(run, "error: cannot start the run; see harness.log");
	ASSERT_BOOL_EQ(tell(&game, "unload", "unloaded"), true);
	answers("command=status\n", quitting);
	ASSERT_INT_EQ(stopStandInGame(&game), 0);
	char loaded[64];
	(void)snprintf(
		loaded, sizeof loaded, "harness " HARNESS_PROTOCOL_TEXT " loaded (pid %d)", (int)game.pid
	);
	const char *const lines[] = {
		loaded,
		"starting: waiting for " EDITION_GAME " to load its engine, " EDITION_ENGINE_PATH "\n",
		"serving jobs: status reports the game starting until then\n",
		"refusing a job: " EDITION_PRODUCT " is busy (starting)\n",
		"found " EDITION_GAME "'s engine (the game's version 27.0.0.1): /",
		"/Other Build/" EDITION_ENGINE_PATH "\n",
		"refusing: unsupported game build (",
		"refusing jobs: unsupported game build\n",
		EDITION_GAME " unloaded its engine\n",
		"gone: refusing jobs until the game loads its engine again\n",
		"found " EDITION_GAME "'s engine (the game's version 27.0.0.1): /",
		"/" STAND_IN_APP "/Contents/PlugIns/" EDITION_ENGINE_PATH "\n",
		"game build: " EDITION_GAME " stand-in, ",
		"accepted: ready; waiting for jobs\n",
		"cannot start the run\n",
		EDITION_GAME " unloaded its engine\n",
		"gone: refusing jobs until the game loads its engine again\n",
	};
	ASSERT_BOOL_EQ(logShows(directory, lines, sizeof lines / sizeof lines[0]), true);
	ASSERT_BOOL_EQ(strstr(standInLog(directory), "BacktraceMacUnity") == NULL, true);
	// The harness removed its socket as the game exited.
	struct stat status;
	ASSERT_BOOL_EQ(lstat(testSocket, &status) != 0 && errno == ENOENT, true);
	removeStandInGame(directory);
}

// A serialised match for the stand-in engine (macos_stand_in_plugin.c): the edition's header with
// the format version, the label, the seed after it, then the home and away managers' flags.
static bool writeStandInMatch(const char *path, uint8_t format, uint32_t seed) {
	uint8_t bytes[64] = {27, 0, 0, 1, format, 0, 0, 0x02};
	static const char label[] = "stand-in";
	uint32_t length = sizeof label - 1;
	uint32_t after[] = {0, seed, MANAGER_HUMAN_IN_CONTROL | 0x21306, MANAGER_AI_MANAGED | 1};
	memcpy(bytes + 12, &length, sizeof length);
	memcpy(bytes + 16, label, length);
	memcpy(bytes + 16 + length, after, sizeof after);
	FILE *file = fopen(path, "wb");
	size_t size = 16 + length + sizeof after;
	bool ok = file && fwrite(bytes, 1, size, file) == size;
	return file && fclose(file) == 0 && ok;
}

// The run's results file, without its timings: true if each line starts with its `rows` entry.
static bool resultsAre(const char *path, const char *const *rows, size_t count) {
	char text[4096];
	if (!readText(path, text, sizeof text)) {
		return false;
	}
	const char *line = text;
	for (size_t i = 0; i < count; ++i) {
		if (!line || strncmp(line, rows[i], strlen(rows[i])) != 0) {
			fprintf(stderr, "%s:\n%s\n", path, text);
			return false;
		}
		line = strchr(line, '\n');
		line = line ? line + 1 : NULL;
	}
	return line && *line == 0;
}

// True if `path` has each of `lines`, in any order.
static bool hasLines(const char *path, const char *const *lines, size_t count) {
	static char text[65536];
	if (!readText(path, text, sizeof text)) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		if (!strstr(text, lines[i])) {
			fprintf(stderr, "%s has no \"%s\":\n%s\n", path, lines[i], text);
			return false;
		}
	}
	return true;
}

TEST(plays_matches_with_the_engines_calls) {
	char directory[] = "/tmp/" EDITION_LOWER " stand-in engine-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stand-in-%d.sock", (int)getpid()
	);
	char load[1300];
	char input[1200];
	char other[1200];
	char results[1200];
	char engineLog[1200];
	(void)snprintf(load, sizeof load, "load %s/%s", directory, standInFiles[STAND_IN_PLUGIN_FILE][0]);
	(void)snprintf(input, sizeof input, "%s/match.bin", directory);
	(void)snprintf(other, sizeof other, "%s/other.bin", directory);
	(void)snprintf(results, sizeof results, "%s/run.csv", directory);
	(void)snprintf(engineLog, sizeof engineLog, "%s/engine.log", directory);
	StandInGame game;
	bool started = writeStandInMatch(input, 0x4C, 7) && writeStandInMatch(other, 0x4B, 7) &&
								 installStandInPluginGame(directory) && startStandInGame(&game, directory);
	ASSERT_BOOL_EQ(started, true);
	if (!started) {
		removeStandInGame(directory);
		return;
	}
	ASSERT_BOOL_EQ(servedWithin(testSocket, 10000), true);
	ASSERT_BOOL_EQ(tell(&game, load, "loaded"), true);
	// Six seeds on two threads, each patched into the input (the seed after a 12-byte header, an
	// 8-byte label and 4 more bytes), loaded into a GAME_MATCH with the engine's context, handed to
	// the assistant, prepared and run to the end, read and deleted.
	char job[4096];
	(void)snprintf(
		job,
		sizeof job,
		"command=run\ninput=%s\noutput=%s\nseeds=1-6\nkeep_build=1\nthreads=2\ndetailed=1\n",
		input,
		results
	);
	answers(job, "ok: 6/6 runs");
	const char *const rows[] = {
		"seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n",
		"1,1,0,21604,1,5851f42d4c957f2e,",
		"2,2,0,21608,1,b0a3e85a992afe5b,",
		"3,3,0,21612,1,08f5dc87e5c07d88,",
		"4,0,1,21616,1,6147d0b53255fcb5,",
		"5,1,1,21620,1,b999c4e27eeb7be2,",
		"6,2,1,21624,1,11ebb90fcb80fb0f,",
	};
	ASSERT_BOOL_EQ(resultsAre(results, rows, sizeof rows / sizeof rows[0]), true);
	char line[6][3][96];
	const char *played[18];
	for (unsigned seed = 1; seed <= 6; ++seed) {
		char (*own)[96] = line[seed - 1];
		(void)snprintf(
			own[0], sizeof own[0], "deserialize seed=%u context=engine flags=0 ok=1\n", seed
		);
		(void)snprintf(
			own[1],
			sizeof own[1],
			"run seed=%u home=0x21306 away=0x14000 prepared=1 mode=1 until=-1\n",
			seed
		);
		(void)snprintf(own[2], sizeof own[2], "delete seed=%u\n", seed);
		for (int k = 0; k < 3; ++k) {
			played[3 * (seed - 1) + (unsigned)k] = own[k];
		}
	}
	ASSERT_BOOL_EQ(hasLines(engineLog, played, sizeof played / sizeof played[0]), true);
	// The detailed statistics, from the edition's MATCH_TEAM, MATCH_PLAYER and statistics layouts.
	char details[1200];
	(void)snprintf(details, sizeof details, "%s/run-players.csv", directory);
	const char *const players[] = {
		"home,\"?\",\"?\",101,6,6,6,90.0,7.00,1.500,",
		"home,\"?\",\"?\",102,6,6,6,60.0,7.00,0.000,",
		"away,\"?\",\"?\",201,6,6,6,90.0,7.00,0.500,",
	};
	ASSERT_BOOL_EQ(hasLines(details, players, sizeof players / sizeof players[0]), true);
	(void)snprintf(details, sizeof details, "%s/run-teams.csv", directory);
	const char *const teams[] = {
		"home,\"?\",6,1.500,50.000,7.000,", "away,\"?\",6,0.500,50.000,7.000,"};
	ASSERT_BOOL_EQ(hasLines(details, teams, sizeof teams / sizeof teams[0]), true);
	// With human control the managers keep their flags; the input's own seed plays once.
	(void)snprintf(
		job,
		sizeof job,
		"command=run\ninput=%s\noutput=%s\nseeds=keep\nhuman_control=1\n",
		input,
		results
	);
	answers(job, "ok: 1/1 runs");
	const char *const kept[] = {"run seed=7 home=0x21307 away=0x14001 prepared=1 mode=1 until=-1\n"};
	ASSERT_BOOL_EQ(hasLines(engineLog, kept, 1), true);
	// Another format version than the build's is not a match the harness reads.
	(void)snprintf(job, sizeof job, "command=run\ninput=%s\noutput=%s\nseeds=keep\n", other, results);
	answers(job, "error: cannot start the run; see harness.log");
	ASSERT_BOOL_EQ(tell(&game, "unload", "unloaded"), true);
	ASSERT_INT_EQ(stopStandInGame(&game), 0);
	const char *const gone[] = {"unloaded with 0 matches alive\n"};
	ASSERT_BOOL_EQ(hasLines(engineLog, gone, 1), true);
	const char *const logged[] = {
		"accepted: ready; waiting for jobs\n",
		"input 40 bytes, seed at +28 = 7; 6 runs on 2 threads\n",
		"job finished: 6/6 runs",
		"input 40 bytes, seed at +28 = 7; 1 runs on 1 threads\n",
		"the input is not a serialised GAME_MATCH\n",
	};
	ASSERT_BOOL_EQ(logShows(directory, logged, sizeof logged / sizeof logged[0]), true);
	removeStandInGame(directory);
}

// The status line of the stand-in game while it loads a save by hand (`play`), with `people` of its
// two loaded, and once it is ready; and the time between the steps of its load.
enum { STAND_IN_LOAD_STEP_MS = 300 };
#define STAND_IN_LOADING(people)                                                                   \
	"ok: loading in_progress=1 processing=0 kind=-1 people=" #people                                \
	" human_team=0 menu_seen=0 autoplay=0 autoplay_read=0 date=2026 day 200 15:00"
#define STAND_IN_READY                                                                             \
	"ok: ready in_progress=1 processing=0 kind=-1 people=2 human_team=1 menu_seen=1 autoplay=0 "     \
	"autoplay_read=0 date=2026 day 200 15:00"

// A load in the engine sets "in progress" at once and loads the people later, in steps, with the
// GAME object, which the start screen stands for, made long before: the harness reports it loading,
// and refuses jobs, until nothing it samples has changed for GAME_STATE_SETTLE_MS.
TEST(waits_for_a_load_to_settle) {
	char directory[] = "/tmp/" EDITION_LOWER " stand-in load-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stand-in-%d.sock", (int)getpid()
	);
	char load[1300];
	char play[1300];
	char engineLog[1200];
	char run[2600];
	(void)snprintf(load, sizeof load, "load %s/%s", directory, standInFiles[STAND_IN_PLUGIN_FILE][0]);
	(void)snprintf(play, sizeof play, "play %s/Stand-in Save.fm", directory);
	(void)snprintf(engineLog, sizeof engineLog, "%s/engine.log", directory);
	(void)snprintf(
		run,
		sizeof run,
		"command=run\ninput=%s/none.bin\noutput=%s/none.csv\nseeds=1\n",
		directory,
		directory
	);
	StandInGame game;
	bool started = installStandInPluginGame(directory) && startStandInGame(&game, directory);
	ASSERT_BOOL_EQ(started, true);
	if (!started) {
		removeStandInGame(directory);
		return;
	}
	ASSERT_BOOL_EQ(servedWithin(testSocket, 10000), true);
	ASSERT_BOOL_EQ(tell(&game, load, "loaded"), true);
	answers(
		"command=status\n",
		"ok: menu in_progress=0 processing=0 kind=-1 people=0 human_team=0 menu_seen=1 autoplay=0 "
		"autoplay_read=0"
	);
	ASSERT_BOOL_EQ(tell(&game, play, "playing"), true);
	uint64_t played = platform_ticks();
	answers("command=status\n", STAND_IN_LOADING(0));
	answers(run, "error: " EDITION_PRODUCT " is busy (loading); wait until it is idle");
	// Loading, whatever it has loaded, until it is ready, which is no sooner than the load's last
	// step and GAME_STATE_SETTLE_MS after it.
	char result[HARNESS_RESULT_LENGTH] = "";
	unsigned loading = 0;
	uint64_t waitedMs = 0;
	while (waitedMs < 20000 && macosTest_submit("command=status\n", 10000, result, sizeof result) &&
				 strcmp(result, STAND_IN_READY) != 0) {
		bool known = strcmp(result, STAND_IN_LOADING(0)) == 0 ||
								 strcmp(result, STAND_IN_LOADING(1)) == 0 || strcmp(result, STAND_IN_LOADING(2)) == 0;
		if (!known) {
			fprintf(stderr, "while loading: %s\n", result);
		}
		ASSERT_BOOL_EQ(known, true);
		loading += known;
		platform_sleep(100);
		waitedMs = (platform_ticks() - played) * 1000 / platform_tickFrequency();
	}
	waitedMs = (platform_ticks() - played) * 1000 / platform_tickFrequency();
	ASSERT_INT_EQ(strcmp(result, STAND_IN_READY), 0);
	ASSERT_BOOL_EQ(waitedMs >= GAME_STATE_SETTLE_MS && loading > 0, true);
	char loaded[1400];
	(void)snprintf(loaded, sizeof loaded, "loaded %s/Stand-in Save.fm\n", directory);
	const char *const lines[] = {loaded};
	ASSERT_BOOL_EQ(hasLines(engineLog, lines, 1), true);
	// Settled, it stays ready. Another load, which nothing asks about until it has settled: the
	// harness has sampled it meanwhile, so it is ready at once.
	platform_sleep(GAME_STATE_SAMPLE_MS + 200);
	answers("command=status\n", STAND_IN_READY);
	ASSERT_BOOL_EQ(tell(&game, play, "playing"), true);
	answers("command=status\n", STAND_IN_LOADING(0));
	platform_sleep(2 * STAND_IN_LOAD_STEP_MS + GAME_STATE_SETTLE_MS + 3 * GAME_STATE_SAMPLE_MS);
	answers("command=status\n", STAND_IN_READY);
	ASSERT_BOOL_EQ(tell(&game, "unload", "unloaded"), true);
	ASSERT_INT_EQ(stopStandInGame(&game), 0);
	removeStandInGame(directory);
}

TEST(serves_the_executable_the_games_info_plist_names) {
	char directory[] = "/tmp/" EDITION_LOWER " stand-in renamed-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stand-in-%d.sock", (int)getpid()
	);
	char app[1200];
	char edition[1300];
	char renamed[1300];
	(void)snprintf(app, sizeof app, "%s/" STAND_IN_APP, directory);
	(void)snprintf(edition, sizeof edition, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app);
	(void)snprintf(renamed, sizeof renamed, "%s/Contents/MacOS/Renamed Game", app);
	bool ready = installStandInPluginGame(directory) && rename(edition, renamed) == 0;
	ASSERT_BOOL_EQ(ready, true);
	// The executable a build of the game names otherwise serves, as the edition's would; another
	// executable in its bundle, or one in another bundle, does not.
	static const struct {
		const char *identifier;
		const char *executable;
		bool served;
	} cases[] = {
		{EDITION_GAME_BUNDLE_ID, "Renamed Game", true},
		{EDITION_GAME_BUNDLE_ID, EDITION_GAME_EXECUTABLE, false},
		{EDITION_GAME_BUNDLE_ID, NULL, false},
		{"com.SportsInteractive.FootballManager26", "Renamed Game", false},
	};
	for (size_t i = 0; ready && i < sizeof cases / sizeof cases[0]; ++i) {
		StandInGame game;
		ASSERT_BOOL_EQ(writeInfoPlist(app, cases[i].identifier, cases[i].executable), true);
		bool started = startStandInGameAt(&game, directory, renamed);
		ASSERT_BOOL_EQ(started, true);
		if (!started) {
			break;
		}
		ASSERT_BOOL_EQ(servedWithin(testSocket, cases[i].served ? 10000 : 2000), cases[i].served);
		ASSERT_INT_EQ(stopStandInGame(&game), 0);
	}
	removeStandInGame(directory);
}

TEST(only_logs_the_engine_in_log_only_mode) {
	char directory[] = "/tmp/" EDITION_LOWER " stand-in log-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-stand-in-%d.sock", (int)getpid()
	);
	// An earlier test's stand-in game can leave its socket and lock behind; this test checks that
	// none are made.
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	(void)unlink(testSocket);
	(void)unlink(lockPath);
	char marker[1200];
	(void)snprintf(marker, sizeof marker, "%s/Harness Folder/" HARNESS_LOG_ONLY_FILE, directory);
	char load[1300];
	(void)snprintf(load, sizeof load, "load %s/%s", directory, standInFiles[STAND_IN_PLUGIN_FILE][0]);
	StandInGame game;
	bool started = installStandInPluginGame(directory) && writeText(marker, "") &&
								 startStandInGame(&game, directory);
	ASSERT_BOOL_EQ(started, true);
	if (!started) {
		removeStandInGame(directory);
		return;
	}
	const char *const lines[] = {
		"starting: waiting for " EDITION_GAME " to load its engine, " EDITION_ENGINE_PATH "\n",
		"log-only mode (",
		"/Harness Folder/" HARNESS_LOG_ONLY_FILE "): logging the engine's loading and unloading, and "
		"serving no jobs\n",
		"game build: " EDITION_GAME " stand-in, ",
		"accepted (log-only mode: serving no jobs)\n",
		EDITION_GAME " unloaded its engine\n",
		"gone (log-only mode: serving no jobs)\n",
	};
	ASSERT_BOOL_EQ(logShows(directory, lines, 3), true);
	ASSERT_BOOL_EQ(tell(&game, load, "loaded"), true);
	ASSERT_BOOL_EQ(logShows(directory, lines, 5), true);
	ASSERT_BOOL_EQ(tell(&game, "unload", "unloaded"), true);
	ASSERT_BOOL_EQ(logShows(directory, lines, sizeof lines / sizeof lines[0]), true);
	// No socket was opened, or locked.
	struct stat status;
	ASSERT_BOOL_EQ(lstat(testSocket, &status) != 0 && lstat(lockPath, &status) != 0, true);
	ASSERT_BOOL_EQ(strstr(standInLog(directory), "serving jobs:") == NULL, true);
	ASSERT_INT_EQ(stopStandInGame(&game), 0);
	removeStandInGame(directory);
}
#endif

TEST(parses_launch_arguments) {
	LaunchOptions options;
	wchar_t *plain[] = {L"" EDITION_CLI, L"launch", L"/Games/fm.app", NULL};
	ASSERT_BOOL_EQ(parseLaunchArguments(2, plain, true, &options), true);
	ASSERT_BOOL_EQ(!options.app && !options.save && !options.userData && !options.rosetta, true);
	ASSERT_BOOL_EQ(options.timeoutS == SAVE_TIMEOUT_S, true);
	ASSERT_BOOL_EQ(
		parseLaunchArguments(3, plain, true, &options) && wcscmp(options.app, plain[2]) == 0, true
	);
	// Without Apple silicon only --rosetta is refused: the x86_64 slice runs natively there.
	ASSERT_BOOL_EQ(parseLaunchArguments(3, plain, false, &options) && !options.rosetta, true);
	wchar_t *rosetta[] = {L"" EDITION_CLI, L"launch", L"--rosetta", L"/Games/fm.app", NULL};
	ASSERT_BOOL_EQ(parseLaunchArguments(3, rosetta, true, &options) && options.rosetta, true);
	ASSERT_BOOL_EQ(!options.app && !options.save, true);
	ASSERT_BOOL_EQ(parseLaunchArguments(4, rosetta, true, &options) && options.rosetta, true);
	ASSERT_BOOL_EQ(wcscmp(options.app, L"/Games/fm.app") == 0, true);
	ASSERT_BOOL_EQ(parseLaunchArguments(3, rosetta, false, &options), false);
	ASSERT_BOOL_EQ(parseLaunchArguments(4, rosetta, false, &options), false);
	wchar_t *full[] = {
		L"" EDITION_CLI,
		L"launch",
		L"--save",
		L"LIV MAIN (v02)",
		L"/Games/fm.app",
		L"--timeout",
		L"60",
		L"--user-data",
		L"/Data",
		L"--rosetta",
		NULL,
	};
	ASSERT_BOOL_EQ(parseLaunchArguments(9, full, true, &options) && !options.rosetta, true);
	ASSERT_BOOL_EQ(wcscmp(options.save, L"LIV MAIN (v02)") == 0 && options.timeoutS == 60, true);
	ASSERT_BOOL_EQ(wcscmp(options.app, L"/Games/fm.app") == 0, true);
	ASSERT_BOOL_EQ(wcscmp(options.userData, L"/Data") == 0, true);
	ASSERT_BOOL_EQ(parseLaunchArguments(9, full, false, &options), true);
	ASSERT_BOOL_EQ(parseLaunchArguments(10, full, true, &options) && options.rosetta, true);
	ASSERT_BOOL_EQ(wcscmp(options.save, L"LIV MAIN (v02)") == 0 && options.timeoutS == 60, true);
	ASSERT_BOOL_EQ(wcscmp(options.app, L"/Games/fm.app") == 0, true);
	ASSERT_BOOL_EQ(wcscmp(options.userData, L"/Data") == 0, true);
	ASSERT_BOOL_EQ(parseLaunchArguments(10, full, false, &options), false);
	static const wchar_t *const bad[][5] = {
		{L"--save", L"X", L"--timeout", L"0"},
		{L"--save", L"X", L"--timeout", L"86401"},
		{L"--save", L"X", L"--timeout", L"1x"},
		{L"--save", L"X", L"--timeout", L"-5"},
		{L"--save"},
		{L"/A/fm.app", L"/B/fm.app"},
		{L"--load_game", L"X"},
		{L"--timeout", L"60"},
		{L"--user-data", L"/Data"},
		{L"--rosetta", L"--timeout", L"60"},
		{L"--rosetta=1"},
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
		wchar_t *arguments[8] = {L"" EDITION_CLI, L"launch"};
		int count = 2;
		for (size_t k = 0; k < 5 && bad[i][k]; ++k) {
			arguments[count++] = (wchar_t *)bad[i][k];
		}
		ASSERT_BOOL_EQ(parseLaunchArguments(count, arguments, true, &options), false);
	}
}

static bool writeSave(const char *data, const char *folder, const char *name) {
	char path[1300];
	(void)snprintf(path, sizeof path, "%s/%s/%s", data, folder, name);
	return writeText(path, "fmf.");
}

// Looks `save` up as `launch --save` does; false if it is not found.
static bool lookUp(const wchar_t *save, const char *data, SaveTarget *target) {
	wchar_t wideData[1200];
	LaunchOptions options = {.save = save, .userData = wideData, .timeoutS = SAVE_TIMEOUT_S};
	if (!data) {
		options.userData = NULL;
	} else if (!platform_fromUtf8(data, wideData, 1200)) {
		return false;
	}
	*target = (SaveTarget){0};
	return findSave(&options, false, true, target);
}

TEST(finds_a_save_by_name_or_file) {
	char home[] = "/tmp/" EDITION_LOWER " saves-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char data[1200];
	char path[1300];
	(void)snprintf(
		data, sizeof data, "%s/Library/Application Support/" EDITION_DATA_FOLDER, home
	);
	(void)snprintf(path, sizeof path, "%s/cloud/games", data);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	(void)snprintf(path, sizeof path, "%s/games/Folder.fm", data);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	static const char *const cloud[] = {
		"LIV_MAIN_FM24.fm",
		"LIV_MAIN_FM24 (v02).fm",
		"LIV_MAIN_FM24_FINAL.fm",
		"last save overwrite backup.fm",
		"Both.fm",
	};
	for (size_t i = 0; i < sizeof cloud / sizeof cloud[0]; ++i) {
		ASSERT_BOOL_EQ(writeSave(data, "cloud/games", cloud[i]), true);
	}
	ASSERT_BOOL_EQ(writeSave(data, "games", "Local Save.fm") && writeSave(data, "games", "Both.fm"), true);
	ASSERT_BOOL_EQ(writeSave(data, "games", "config.xml"), true);
	// Exact names, in either folder.
	SaveTarget target;
	char expected[1300];
	ASSERT_BOOL_EQ(lookUp(L"LIV_MAIN_FM24", data, &target), true);
	(void)snprintf(expected, sizeof expected, "%s/cloud/games/LIV_MAIN_FM24.fm", data);
	ASSERT_BOOL_EQ(strcmp(target.path, expected) == 0 && strcmp(target.name, "LIV_MAIN_FM24") == 0, true);
	ASSERT_BOOL_EQ(lookUp(L"LIV_MAIN_FM24 (v02)", data, &target), true);
	ASSERT_INT_EQ(strcmp(target.name, "LIV_MAIN_FM24 (v02)"), 0);
	ASSERT_BOOL_EQ(lookUp(L"Local Save", data, &target), true);
	(void)snprintf(expected, sizeof expected, "%s/games/Local Save.fm", data);
	ASSERT_INT_EQ(strcmp(target.path, expected), 0);
	// In both folders, only as a prefix or in another case, a folder or another file: not found.
	static const wchar_t *const missing[] = {L"Both", L"LIV", L"liv_main_fm24", L"Folder", L"config"};
	for (size_t i = 0; i < sizeof missing / sizeof missing[0]; ++i) {
		ASSERT_BOOL_EQ(lookUp(missing[i], data, &target), false);
	}
	// The usual data folder is in the home folder.
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	ASSERT_BOOL_EQ(lookUp(L"LIV_MAIN_FM24_FINAL", NULL, &target), true);
	ASSERT_INT_EQ(strcmp(target.name, "LIV_MAIN_FM24_FINAL"), 0);
	// A .fm file by its path, which must be a file.
	wchar_t given[1300];
	(void)snprintf(path, sizeof path, "%s/games/Local Save.fm", data);
	ASSERT_BOOL_EQ(platform_fromUtf8(path, given, 1300) && lookUp(given, NULL, &target), true);
	ASSERT_BOOL_EQ(strcmp(target.path, path) == 0 && strcmp(target.name, "Local Save") == 0, true);
	static const char *const notSaves[] = {"games/Folder.fm", "games/config.xml", "games/Gone.fm"};
	for (size_t i = 0; i < sizeof notSaves / sizeof notSaves[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", data, notSaves[i]);
		ASSERT_BOOL_EQ(platform_fromUtf8(path, given, 1300) && !lookUp(given, NULL, &target), true);
	}
	// fm24ctl tells saves apart by name, so a copy elsewhere with the name of a save in FM24's
	// folders is refused, unless it is that file. A copy with a name of its own is not.
	char sandbox[1200];
	char original[1300];
	(void)snprintf(sandbox, sizeof sandbox, "%s/sandbox", home);
	(void)snprintf(original, sizeof original, "%s/cloud/games/LIV_MAIN_FM24.fm", data);
	ASSERT_BOOL_EQ(makeDirectories(sandbox), true);
	static const struct {
		const char *name;
		int kind; // 0: a copy, 1: a hard link to the original, 2: a symbolic link to it
		bool found;
	} copies[] = {
		{"LIV_MAIN_FM24.fm", 0, false},
		{"Both.fm", 0, false},
		{"LIV_COPY.fm", 0, true},
		{"liv_main_fm24.fm", 0, true},
		{"Hard link/LIV_MAIN_FM24.fm", 1, true},
		{"Symbolic link/LIV_MAIN_FM24.fm", 2, true},
	};
	for (size_t i = 0; i < sizeof copies / sizeof copies[0]; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", sandbox, copies[i].name);
		char *slash = strrchr(path, '/');
		*slash = 0;
		bool made = makeDirectories(path);
		*slash = '/';
		made = made && (copies[i].kind == 0   ? writeText(path, "fmf.")
										: copies[i].kind == 1 ? link(original, path) == 0
																					: symlink(original, path) == 0);
		ASSERT_BOOL_EQ(made && platform_fromUtf8(path, given, 1300), true);
		ASSERT_BOOL_EQ(lookUp(given, NULL, &target), copies[i].found);
	}
	// FM24's own saves pass by their paths, even with the name in both folders.
	(void)snprintf(path, sizeof path, "%s/games/Both.fm", data);
	ASSERT_BOOL_EQ(platform_fromUtf8(path, given, 1300) && lookUp(given, NULL, &target), true);
	(void)snprintf(path, sizeof path, "%s/sandbox/LIV_MAIN_FM24.fm", home);
	ASSERT_BOOL_EQ(platform_fromUtf8(path, given, 1300) && lookUp(given, home, &target), true);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(writes_autoplay_files_and_removes_those_of_exited_games) {
	char directory[] = "/tmp/" EDITION_LOWER "-autoplay-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char path[1100];
	char text[256];
	(void)snprintf(path, sizeof path, "%s/autoplay.json", directory);
	ASSERT_BOOL_EQ(writeAutoplay(path, "/a b/\"LIV\".fm") && readText(path, text, sizeof text), true);
	ASSERT_INT_EQ(strcmp(text, "{\"LOAD_GAME\": \"/a b/\\\"LIV\\\".fm\"}"), 0);
	ASSERT_BOOL_EQ(writeAutoplay(path, "/a\nb.fm"), false);
	ASSERT_BOOL_EQ(access(path, F_OK) == 0, true); // the earlier file, which it did not touch
	// A file that cannot be written whole, here past the file size limit, is not left behind.
	ASSERT_INT_EQ(unlink(path), 0);
	struct rlimit limit;
	ASSERT_INT_EQ(getrlimit(RLIMIT_FSIZE, &limit), 0);
	struct rlimit small = {.rlim_cur = 8, .rlim_max = limit.rlim_max};
	(void)fflush(stdout);
	(void)fflush(stderr);
	void (*handler)(int) = signal(SIGXFSZ, SIG_IGN);
	bool written = setrlimit(RLIMIT_FSIZE, &small) == 0 && writeAutoplay(path, "/x/LIV.fm");
	ASSERT_INT_EQ(setrlimit(RLIMIT_FSIZE, &limit), 0);
	(void)signal(SIGXFSZ, handler);
	clearerr(stderr); // its message, past the limit, may not have been written to a log file
	ASSERT_BOOL_EQ(written, false);
	ASSERT_BOOL_EQ(access(path, F_OK) != 0, true);
	pid_t exited = exitedProcess();
	ASSERT_BOOL_EQ(exited > 0, true);
	// Files of processes that have exited go: the first four are. Rosetta's are named alike, without
	// the extension.
	enum { NAMES = 15 };
	char names[NAMES][64];
	(void)snprintf(names[0], sizeof names[0], "autoplay-%d.json", (int)exited);
	(void)snprintf(names[1], sizeof names[1], "autoplay-request-%d.json", (int)exited);
	(void)snprintf(names[2], sizeof names[2], "rosetta-%d", (int)exited);
	(void)snprintf(names[3], sizeof names[3], "rosetta-request-%d", (int)exited);
	(void)snprintf(names[4], sizeof names[4], "autoplay-%d.json", (int)getpid());
	(void)snprintf(names[5], sizeof names[5], "autoplay-request-%d.json", (int)getpid());
	(void)snprintf(names[6], sizeof names[6], "autoplay.json");
	(void)snprintf(names[7], sizeof names[7], "autoplay-12x.json");
	(void)snprintf(names[8], sizeof names[8], "autoplay-request-1234567890.json");
	(void)snprintf(names[9], sizeof names[9], "other-%d.json", (int)exited);
	(void)snprintf(names[10], sizeof names[10], "rosetta-%d", (int)getpid());
	(void)snprintf(names[11], sizeof names[11], "rosetta-request-%d", (int)getpid());
	(void)snprintf(names[12], sizeof names[12], "rosetta-request-%d.json", (int)exited);
	(void)snprintf(names[13], sizeof names[13], "rosetta-request-12x");
	(void)snprintf(names[14], sizeof names[14], "rosetta-");
	for (size_t i = 0; i < NAMES; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, names[i]);
		ASSERT_BOOL_EQ(writeText(path, "{}"), true);
	}
	// While a game runs, the auto-play file of an fm24ctl that started it directly and has exited
	// stays, as the game may not have read it yet; the rest go.
	removeStaleAutoplay(directory, true);
	for (size_t i = 0; i < NAMES; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, names[i]);
		ASSERT_BOOL_EQ(access(path, F_OK) == 0, i == 0 || i > 3);
	}
	for (size_t i = 1; i <= 3; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, names[i]);
		ASSERT_BOOL_EQ(writeText(path, "{}"), true);
	}
	removeStaleAutoplay(directory, false);
	for (size_t i = 0; i < NAMES; ++i) {
		(void)snprintf(path, sizeof path, "%s/%s", directory, names[i]);
		ASSERT_BOOL_EQ(access(path, F_OK) == 0, i > 3);
	}
	ASSERT_BOOL_EQ(removeTree(directory), true);
}

static atomic_bool stopServer;

// Serves connections until stopServer is set.
static void *serveUntilStopped(void *parameter) {
	int listener = *(int *)parameter;
	while (!atomic_load(&stopServer)) {
		struct pollfd entry = {.fd = listener, .events = POLLIN};
		int connection = poll(&entry, 1, 50) == 1 ? accept(listener, NULL, NULL) : -1;
		if (connection >= 0) {
			serve(connection);
			close(connection);
		}
	}
	return NULL;
}

#define TEST_IDLE_FLAGS                                                                            \
	"in_progress=1 processing=0 kind=-1 people=9 human_team=1 menu_seen=1 autoplay=0 "              \
	"autoplay_read=0 date=2024 day 123 15:00"

// What a stand-in harness answers status jobs with, in turn.
static const char *const sameSave[] = {"ok: ready " TEST_IDLE_FLAGS " save=LIV_MAIN_FM24"};
static const char *const otherSave[] = {"ok: ready " TEST_IDLE_FLAGS " save=LIV_MAIN_FM24 (v02)"};
static const char *const unnamedSave[] = {"ok: ready " TEST_IDLE_FLAGS};
static const char *const atMenu[] = {
	"ok: menu in_progress=0 processing=0 kind=-1 people=0 human_team=0 menu_seen=1 autoplay=0 "
	"autoplay_read=0",
};
static const char *const busyThenReady[] = {
	"ok: busy in_progress=1 processing=1 kind=4 people=9 human_team=0 menu_seen=1 autoplay=0 "
	"autoplay_read=0 save=LIV_MAIN_FM24",
	"ok: ready " TEST_IDLE_FLAGS " save=LIV_MAIN_FM24",
};
static const char *const refusedStatus[] = {"error: the harness does not support this game build"};

// Has a supported game's harness serve `testSocket` on another thread until stopHarness.
static bool startHarness(pthread_t *server, int *listener) {
	supported = true;
	*listener = openSocket();
	atomic_store(&stopServer, false);
	return *listener >= 0 && pthread_create(server, NULL, serveUntilStopped, listener) == 0;
}

static void stopHarness(pthread_t server, int listener) {
	statusReplyCount = 0;
	atomic_store(&stopServer, true);
	pthread_join(server, NULL);
	close(listener);
	stopServing();
	char lockPath[sizeof testSocket + 8];
	ASSERT_BOOL_EQ(channel_lockPath(testSocket, lockPath, sizeof lockPath), true);
	ASSERT_INT_EQ(unlink(lockPath), 0);
}

// Has the stand-in harness answer status jobs with `count` `replies`, counting them afresh.
static void answerStatus(const char *const *replies, size_t count) {
	statusReplies = replies;
	statusReplyCount = count;
	atomic_store(&statusRepliesGiven, 0);
}

TEST(checks_the_save_a_running_game_has_loaded) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-running-test-%d.sock", (int)getpid());
	(void)unlink(testSocket);
	SaveTarget target = {.name = "LIV_MAIN_FM24", .path = "/x/LIV_MAIN_FM24.fm"};
	LaunchOptions options = {.save = L"LIV_MAIN_FM24", .timeoutS = 30};
	pid_t self = getpid();
	// Without this harness, as launch has always said.
	ASSERT_INT_EQ(useRunningGame(self, &options, &target), 1);
	pthread_t server;
	int listener = -1;
	bool serving = startHarness(&server, &listener);
	ASSERT_BOOL_EQ(serving, true);
	if (!serving) {
		return;
	}
	static const struct {
		const char *const *replies;
		size_t count;
		int status;
		size_t asked;
	} cases[] = {
		{sameSave, 1, 0, 1},
		{otherSave, 1, EXIT_WRONG_STATE, 1},
		{unnamedSave, 1, 0, 1},
		{atMenu, 1, EXIT_WRONG_STATE, 1},
		{busyThenReady, 2, 0, 1 + SESSION_STABLE_SAMPLES}, // idle again, it must stay idle
		{refusedStatus, 1, 1, 1},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		answerStatus(cases[i].replies, cases[i].count);
		ASSERT_INT_EQ(useRunningGame(self, &options, &target), cases[i].status);
		ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), (int)cases[i].asked);
		ASSERT_BOOL_EQ(queryTimeoutMs == CLIENT_STATUS_TIMEOUT_MS, true);
	}
	// Without --save the game is not asked anything.
	options.save = NULL;
	atomic_store(&statusRepliesGiven, 0);
	ASSERT_INT_EQ(useRunningGame(self, &options, &target), 0);
	ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 0);
	stopHarness(server, listener);
}

// A process sleeping in the slice of CPU `type`, as arch starts one, or 0 if it cannot start.
static pid_t startSleeper(cpu_type_t type) {
	posix_spawnattr_t attributes;
	if (posix_spawnattr_init(&attributes) != 0) {
		return 0;
	}
	cpu_type_t types[] = {type};
	size_t set = 0;
	pid_t pid = 0;
	char *const program[] = {"/bin/sleep", "60", NULL};
	bool started = posix_spawnattr_setbinpref_np(&attributes, 1, types, &set) == 0 && set == 1 &&
								 posix_spawn(&pid, program[0], NULL, &attributes, program, environ) == 0;
	(void)posix_spawnattr_destroy(&attributes);
	return started ? pid : 0;
}

static void stopSleeper(pid_t pid) {
	if (pid > 0) {
		(void)kill(pid, SIGKILL);
		(void)waitpid(pid, NULL, 0);
	}
}

TEST(checks_that_the_game_runs_the_slice_rosetta_asks_for) {
	int appleSilicon = appleSiliconMac();
	int translated = 0;
	size_t size = sizeof translated;
	if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) != 0) {
		translated = 0; // An Intel Mac has no Rosetta.
	}
	// This process (under Rosetta as `arch -x86_64 fm24_tests`), one that has exited, and one in
	// each slice (the x86_64 one only with Rosetta on Apple silicon).
	ASSERT_BOOL_EQ(underRosetta(getpid()), translated == 1);
	ASSERT_BOOL_EQ(underRosetta(exitedProcess()), false);
	bool rosettaInstalled = appleSilicon && runsIntelCode();
	pid_t native = startSleeper(appleSilicon ? CPU_TYPE_ARM64 : CPU_TYPE_X86_64);
	pid_t intel = rosettaInstalled ? startSleeper(CPU_TYPE_X86_64) : 0;
	ASSERT_BOOL_EQ(native > 0 && !underRosetta(native), true);
	ASSERT_BOOL_EQ(rosettaInstalled ? intel > 0 && underRosetta(intel) : intel == 0, true);
	// --rosetta takes only a game under Rosetta, running or just started; without it either will do.
	LaunchOptions rosetta = {.rosetta = true, .timeoutS = 30};
	LaunchOptions plain = {.timeoutS = 30};
	ASSERT_BOOL_EQ(runsAskedSlice(native, &plain, false, false), true);
	ASSERT_BOOL_EQ(runsAskedSlice(native, &rosetta, false, false), false);
	ASSERT_BOOL_EQ(runsAskedSlice(native, &rosetta, true, true), false);
	ASSERT_BOOL_EQ(runsAskedSlice(native, &rosetta, true, false), false);
	if (intel) {
		ASSERT_BOOL_EQ(runsAskedSlice(intel, &rosetta, false, false), true);
		ASSERT_BOOL_EQ(runsAskedSlice(intel, &rosetta, true, true), true);
		ASSERT_BOOL_EQ(runsAskedSlice(intel, &plain, false, false), true);
	}
	// A fresh start whose harness is ready fails if the game does not run that slice, and still
	// gives its process ID for the clean-up.
	static const char launcher[] = "/x/" EDITION_LAUNCHER;
	LaunchWatch watch = {.readyPid = native, .wasReady = true};
	pid_t ready = 0;
	ASSERT_INT_EQ(launchExitCode(LAUNCH_READY, &watch, launcher, true, &rosetta, &ready), 1);
	ASSERT_INT_EQ((int)ready, (int)native);
	ready = 0;
	ASSERT_INT_EQ(launchExitCode(LAUNCH_READY, &watch, launcher, false, &rosetta, &ready), 1);
	ASSERT_INT_EQ(launchExitCode(LAUNCH_READY, &watch, launcher, true, &plain, &ready), 0);
	ASSERT_INT_EQ((int)ready, (int)native);
	if (intel) {
		watch.readyPid = intel;
		ASSERT_INT_EQ(launchExitCode(LAUNCH_READY, &watch, launcher, true, &rosetta, &ready), 0);
		ASSERT_INT_EQ((int)ready, (int)intel);
	}
	// A running game with this harness that runs natively is refused before anything is asked, with
	// or without --save; one under Rosetta goes on to the save check.
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-rosetta-test-%d.sock", (int)getpid());
	(void)unlink(testSocket);
	SaveTarget target = {.name = "LIV_MAIN_FM24", .path = "/x/LIV_MAIN_FM24.fm"};
	// Without the harness it fails first, as without --rosetta.
	ASSERT_INT_EQ(useRunningGame(native, &rosetta, &target), 1);
	pthread_t server;
	int listener = -1;
	bool serving = startHarness(&server, &listener);
	ASSERT_BOOL_EQ(serving, true);
	if (serving) {
		answerStatus(sameSave, 1);
		ASSERT_INT_EQ(useRunningGame(native, &rosetta, &target), EXIT_WRONG_STATE);
		ASSERT_INT_EQ(useRunningGame(native, &plain, &target), 0);
		rosetta.save = L"LIV_MAIN_FM24";
		ASSERT_INT_EQ(useRunningGame(native, &rosetta, &target), EXIT_WRONG_STATE);
		ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 0);
		if (intel) {
			ASSERT_INT_EQ(useRunningGame(intel, &rosetta, &target), 0);
			ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 1);
		}
		stopHarness(server, listener);
	}
	stopSleeper(native);
	stopSleeper(intel);
}

TEST(checks_the_running_game_before_a_job) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-job-check-test-%d.sock", (int)getpid());
	(void)unlink(testSocket);
	SessionNeed need = {.save = "LIV_MAIN_FM24", .launch = true, .timeoutMs = 600000};
	char date[SAVES_DATE_LENGTH] = "stale";
	pid_t self = getpid();
	atomic_store(&statusRepliesGiven, 0);
	// A game without this harness gets the job as before, which explains; so does no game with
	// --no-launch.
	ASSERT_INT_EQ(ensureSave(&need, self, date, sizeof date), 0);
	ASSERT_INT_EQ(date[0], 0);
	need.launch = false;
	ASSERT_INT_EQ(ensureSave(&need, 0, date, sizeof date), 0);
	need.launch = true;
	// With no game running, a save that cannot be found starts nothing.
	char home[] = "/tmp/" EDITION_LOWER " job check-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	spawned[0] = 0;
	ASSERT_INT_EQ(ensureSave(&need, 0, date, sizeof date), 1);
	ASSERT_INT_EQ(spawned[0], 0);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
	ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 0);
	// The running game with this harness: the job goes ahead only with the save, or with another
	// one given --any-save, once the game is idle. The check waits for another runner's job as long
	// as the job would.
	pthread_t server;
	int listener = -1;
	bool serving = startHarness(&server, &listener);
	ASSERT_BOOL_EQ(serving, true);
	if (!serving) {
		return;
	}
	static const char loadedDate[] = "2024 day 123 15:00";
	static const struct {
		const char *const *replies;
		size_t count;
		bool anySave;
		int status;
		size_t asked;
		const char *date;
	} cases[] = {
		{sameSave, 1, false, 0, 1, loadedDate},
		{otherSave, 1, false, EXIT_WRONG_STATE, 1, ""},
		{otherSave, 1, true, 0, 1, ""},
		{unnamedSave, 1, false, 0, 1, loadedDate},
		{atMenu, 1, true, EXIT_WRONG_STATE, 1, ""},
		{busyThenReady, 2, false, 0, 1 + SESSION_STABLE_SAMPLES, loadedDate},
		{refusedStatus, 1, false, 0, 1, ""}, // a harness without the status job
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		answerStatus(cases[i].replies, cases[i].count);
		need.anySave = cases[i].anySave;
		(void)snprintf(date, sizeof date, "stale");
		ASSERT_INT_EQ(ensureSave(&need, self, date, sizeof date), cases[i].status);
		ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), (int)cases[i].asked);
		ASSERT_INT_EQ(strcmp(date, cases[i].date), 0);
		ASSERT_BOOL_EQ(queryTimeoutMs == need.timeoutMs, true);
	}
	// A job without a save asks nothing.
	need.save = NULL;
	answerStatus(sameSave, 1);
	ASSERT_INT_EQ(ensureSave(&need, self, NULL, 0), 0);
	ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 0);
	stopHarness(server, listener);
}

#if EDITION_ENGINE_PLUGIN
// What fmNNctl says when the game is not running for a fixture that does not record its save.
#define UNRECORDED_NOT_RUNNING                                                                     \
	EDITION_GAME " is not running: start it with the harness, load the fixture's save and leave "   \
							 "the game idle, then run this again. " EDITION_PRODUCT                                \
							 " exports do not record their save, so " EDITION_CLI " cannot start the game with "  \
							 "it.\n"

static const char *const loadingThenReady[] = {
	"ok: loading in_progress=1 processing=0 kind=-1 people=9 human_team=0 menu_seen=0 autoplay=0 "
	"autoplay_read=0 date=2024 day 123 15:00",
	"ok: ready " TEST_IDLE_FLAGS,
};

// What ensureSave printed and returned for `need` with game `pid` (0: none running).
static int ensureCapturing(const SessionNeed *need, pid_t pid, char *date, size_t size, char *out) {
	Capture capture;
	bool capturing = startCapture(&capture);
	int status = capturing ? ensureSave(need, pid, date, size) : -1;
	endCapture(&capture, out, 4096);
	return status;
}

// The edition's exports record no save: a job for one needs the running game to have a save loaded
// and idle, whichever it is, which fmNNctl waits for as it does for a save, and it never starts the
// game for one.
TEST(checks_the_running_game_before_a_job_for_an_export) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-export-check-test-%d.sock", (int)getpid());
	(void)unlink(testSocket);
	SessionNeed need = {.anySave = true, .timeoutMs = 600000, .unrecorded = true};
	char date[SAVES_DATE_LENGTH] = "stale";
	char printed[4096];
	pid_t self = getpid();
	atomic_store(&statusRepliesGiven, 0);
	// No game running: it says what to do, and starts nothing.
	spawned[0] = 0;
	ASSERT_INT_EQ(ensureCapturing(&need, 0, date, sizeof date, printed), 1);
	ASSERT_INT_EQ(strcmp(printed, UNRECORDED_NOT_RUNNING), 0);
	ASSERT_BOOL_EQ(!spawned[0] && !date[0], true);
	// A game without this harness gets the job as before, which explains.
	ASSERT_INT_EQ(ensureSave(&need, self, date, sizeof date), 0);
	ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), 0);
	pthread_t server;
	int listener = -1;
	bool serving = startHarness(&server, &listener);
	ASSERT_BOOL_EQ(serving, true);
	if (!serving) {
		return;
	}
	// The game with this harness: the job goes ahead once it is idle with a save, which fmNNctl
	// takes to be the fixture's, giving its date. The check waits for another runner's job as long
	// as the job would.
	static const char loadedDate[] = "2024 day 123 15:00";
	static const struct {
		const char *const *replies;
		size_t count;
		int status;
		size_t asked;
		const char *date;
	} cases[] = {
		{loadingThenReady, 2, 0, 1 + SESSION_STABLE_SAMPLES, loadedDate}, // loading, then ready
		{unnamedSave, 1, 0, 1, loadedDate},
		{sameSave, 1, 0, 1, ""},   // a harness that names its save: any save will do
		{otherSave, 1, 0, 1, ""},  // ...
		{atMenu, 1, EXIT_WRONG_STATE, 1, ""},
		{refusedStatus, 1, 0, 1, ""}, // a harness without the status job
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		answerStatus(cases[i].replies, cases[i].count);
		(void)snprintf(date, sizeof date, "stale");
		ASSERT_INT_EQ(ensureCapturing(&need, self, date, sizeof date, printed), cases[i].status);
		ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), (int)cases[i].asked);
		ASSERT_INT_EQ(strcmp(date, cases[i].date), 0);
		ASSERT_BOOL_EQ(queryTimeoutMs == need.timeoutMs, true);
		ASSERT_BOOL_EQ(strstr(printed, "--any-save") == NULL && strstr(printed, "start") == NULL, true);
		if (i == 0) {
			char ready[256];
			(void)snprintf(
				ready, sizeof ready, "ready: the fixture's save is already loaded (pid %d, %s)\n", (int)self, loadedDate
			);
			ASSERT_BOOL_EQ(strstr(printed, EDITION_PRODUCT " state: loading\n") != NULL, true);
			ASSERT_BOOL_EQ(strstr(printed, ready) != NULL, true);
		}
		if (cases[i].date[0]) {
			ASSERT_BOOL_EQ(
				strstr(
					printed,
					"the harness cannot read which save " EDITION_PRODUCT
					" has loaded; taking it to be the fixture's save\n"
				) != NULL,
				true
			);
		}
	}
	char atMenuText[256];
	(void)snprintf(
		atMenuText,
		sizeof atMenuText,
		EDITION_PRODUCT " (pid %d) has no save loaded: load the fixture's save in " EDITION_PRODUCT
		", leave the game idle, then run this again.\n",
		(int)self
	);
	answerStatus(atMenu, 1);
	ASSERT_INT_EQ(ensureCapturing(&need, self, date, sizeof date, printed), EXIT_WRONG_STATE);
	ASSERT_INT_EQ(strcmp(printed, atMenuText), 0);
	stopHarness(server, listener);
	// Through the commands, with no game running: each stops before its job, and starts nothing.
	char running[1100];
	char fixture[] = "/tmp/" EDITION_LOWER "-export-XXXXXX.json";
	int made = mkstemps(fixture, 5);
	ASSERT_BOOL_EQ(made >= 0 && close(made) == 0, true);
	ASSERT_BOOL_EQ(
		writeText(
			fixture,
			"{\"format\":\"" EDITION_MATCH_CONFIG_FORMAT "\",\"game_date\":\"2026 day 200 15:00\","
			"\"fields\":[{\"u8\":42}]}\n"
		),
		true
	);
	wchar_t input[64];
	ASSERT_BOOL_EQ(platform_fromUtf8(fixture, input, 64), true);
	if (!macosGame_findRunning(running, sizeof running)) {
		wchar_t *run[] = {L"" EDITION_CLI, L"run", input, L"1-2", NULL};
		wchar_t *replace[] = {
			L"" EDITION_CLI, L"replace-away", input, L"677", L"/tmp/" EDITION_LOWER "-export-out.json", NULL
		};
		static int (*const commands[])(int, wchar_t **) = {macosTest_run, macosTest_replaceTeam};
		wchar_t **arguments[] = {run, replace};
		for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
			int count = 0;
			while (arguments[i][count]) {
				++count;
			}
			spawned[0] = 0;
			Capture capture;
			bool capturing = startCapture(&capture);
			int status = capturing ? commands[i](count, arguments[i]) : -1;
			endCapture(&capture, printed, sizeof printed);
			ASSERT_INT_EQ(status, 1);
			ASSERT_INT_EQ(strcmp(printed, UNRECORDED_NOT_RUNNING), 0);
			ASSERT_BOOL_EQ(!spawned[0], true);
		}
	} else {
		fprintf(stderr, "not run: the game is running (%s), and would be the one checked\n", running);
	}
	ASSERT_INT_EQ(unlink(fixture), 0);
}
#endif

// Gives app 2252570 the launch option `option` for the only Steam account in the home folder
// `home`.
static bool setLaunchOption(const char *home, const char *option) {
	char escaped[2400];
	size_t length = 0;
	for (const char *c = option; *c && length + 2 < sizeof escaped; ++c) {
		if (*c == '"' || *c == '\\') {
			escaped[length++] = '\\';
		}
		escaped[length++] = *c;
	}
	escaped[length] = 0;
	char config[3000];
	(void)snprintf(
		config,
		sizeof config,
		"\"UserLocalConfigStore\" { \"Software\" { \"Valve\" { \"Steam\" { \"apps\" { \"" EDITION_STEAM_APP_ID "\" {\n"
		"\t\"LaunchOptions\"\t\t\"%s\"\n} } } } } }\n",
		escaped
	);
	char path[1300];
	(void)snprintf(
		path, sizeof path, "%s/Library/Application Support/Steam/userdata/42/config", home
	);
	bool made = makeDirectories(path);
	size_t used = strlen(path);
	(void)snprintf(path + used, sizeof path - used, "/localconfig.vdf");
	return made && writeText(path, config);
}

// FM24's usual data folder under the home folder `home`, with its save folders.
static bool makeDataFolder(const char *home, char *data, size_t size) {
	char path[1300];
	(void)snprintf(
		data, size, "%s/Library/Application Support/" EDITION_DATA_FOLDER, home
	);
	(void)snprintf(path, sizeof path, "%s/cloud/games", data);
	bool made = makeDirectories(path);
	(void)snprintf(path, sizeof path, "%s/games", data);
	return made && makeDirectories(path);
}

TEST(starts_the_game_for_a_job_and_removes_its_auto_play_files) {
	char home[] = "/tmp/" EDITION_LOWER " job launch-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char data[1200];
	char harness[1200];
	char steam[1200];
	char app[1300];
	char path[1400];
	ASSERT_BOOL_EQ(makeDataFolder(home, data, sizeof data), true);
	ASSERT_BOOL_EQ(writeSave(data, "cloud/games", "LIV_MAIN_FM24.fm"), true);
	(void)snprintf(harness, sizeof harness, "%s/Library/Application Support/" EDITION_HARNESS, home);
	(void)snprintf(steam, sizeof steam, "%s/Library/Application Support/Steam", home);
	ASSERT_BOOL_EQ(makeDirectories(harness) && installFakeGame(steam, app, sizeof app), true);
	// Installing the harness, as install and every launch do, deletes the requests of an fm24ctl
	// that has exited (as an interrupted one leaves), and keeps a running one's.
	int self = (int)getpid();
	pid_t exited = exitedProcess();
	char orphan[1300];
	char live[1300];
	(void)snprintf(orphan, sizeof orphan, "%s/autoplay-request-%d.json", harness, (int)exited);
	(void)snprintf(live, sizeof live, "%s/autoplay-request-%d.json", harness, self);
	ASSERT_BOOL_EQ(exited > 0 && writeText(orphan, "{}") && writeText(live, "{}"), true);
	char launcher[1300];
	ASSERT_BOOL_EQ(macosGame_installHarness(false, launcher, sizeof launcher), true);
	ASSERT_BOOL_EQ(access(orphan, F_OK) != 0 && access(live, F_OK) == 0, true);
	ASSERT_INT_EQ(unlink(live), 0);
	// open's stand-in lists the harness folder as the game would start, and fails, so nothing then
	// waits for a game.
	char script[1300];
	char seenPath[1300];
	char seen[4096];
	char name[64];
	(void)snprintf(script, sizeof script, "%s/open.sh", home);
	(void)snprintf(seenPath, sizeof seenPath, "%s/seen.txt", home);
	(void)snprintf(path, sizeof path, "#!/bin/sh\n/bin/ls '%s' > '%s'\nexit 1\n", harness, seenPath);
	ASSERT_BOOL_EQ(writeText(script, path) && chmod(script, 0755) == 0, true);
	spawnInstead = script;
	SessionNeed need = {.save = "LIV_MAIN_FM24", .launch = true, .timeoutMs = 1000};
	char date[SAVES_DATE_LENGTH];
	// Without Steam's launch option the game is opened with the file named after fm24ctl, which goes
	// once the start has failed.
	spawned[0] = 0;
	ASSERT_INT_EQ(ensureSave(&need, 0, date, sizeof date), 1);
	char expected[1600];
	(void)snprintf(
		expected,
		sizeof expected,
		"|%s|--args|--auto_play_game_file=%s/autoplay-%d.json|",
		app,
		harness,
		self
	);
	ASSERT_BOOL_EQ(endsWith(spawned, expected), true);
	(void)snprintf(name, sizeof name, "autoplay-%d.json\n", self);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, name) != NULL, true);
	(void)snprintf(path, sizeof path, "%s/autoplay-%d.json", harness, self);
	ASSERT_BOOL_EQ(access(path, F_OK) != 0, true);
	// Through Steam the request, named after fm24ctl, waits for the launcher, and goes once the start
	// has failed.
	char option[1400];
	(void)snprintf(option, sizeof option, "/bin/sh \"%s\" %%command%%", launcher);
	ASSERT_BOOL_EQ(setLaunchOption(home, option) && unlink(seenPath) == 0, true);
	spawned[0] = 0;
	ASSERT_INT_EQ(ensureSave(&need, 0, date, sizeof date), 1);
	ASSERT_INT_EQ(strcmp(spawned, "/usr/bin/open|/usr/bin/open|steam://rungameid/" EDITION_STEAM_APP_ID "|"), 0);
	(void)snprintf(name, sizeof name, "autoplay-request-%d.json\n", self);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, name) != NULL, true);
	ASSERT_BOOL_EQ(access(live, F_OK) != 0, true);
	spawnInstead = "/usr/bin/true";
	// Once the game has started and the wait has ended: the request the launcher did not take goes,
	// and so does the game's copy, unless the game may still be loading it.
	static const struct {
		bool throughSteam;
		bool started;
		SessionOutcome outcome;
		bool fileLeft; // what fm24ctl wrote
		bool copyLeft; // the launcher's copy for game 4242
	} cases[] = {
		{true, true, SESSION_READY, false, false},
		{true, true, SESSION_OTHER_SAVE, false, false},
		{true, true, SESSION_TIMED_OUT, false, true},
		{true, false, SESSION_EXITED, false, true},
		{false, true, SESSION_READY, false, true},
		{false, true, SESSION_TIMED_OUT, true, true},
		{false, false, SESSION_EXITED, false, true},
	};
	char copy[1300];
	(void)snprintf(copy, sizeof copy, "%s/autoplay-4242.json", harness);
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		char file[1300];
		ASSERT_BOOL_EQ(autoplayPath(harness, cases[i].throughSteam, file, sizeof file), true);
		ASSERT_BOOL_EQ(strcmp(file, cases[i].throughSteam ? live : path) == 0, true);
		ASSERT_BOOL_EQ(writeText(file, "{}") && writeText(copy, "{}"), true);
		pid_t game = cases[i].started ? 4242 : 0;
		removeAutoplay(harness, file, cases[i].throughSteam, game, cases[i].outcome);
		ASSERT_BOOL_EQ(access(file, F_OK) == 0, cases[i].fileLeft);
		ASSERT_BOOL_EQ(access(copy, F_OK) == 0, cases[i].copyLeft);
		(void)unlink(file);
		(void)unlink(copy);
	}
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(leaves_a_rosetta_request_only_while_steam_starts_the_game) {
	char home[] = "/tmp/" EDITION_LOWER " rosetta request-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char data[1200];
	char harness[1200];
	char steam[1200];
	char app[1300];
	char launcher[1300];
	ASSERT_BOOL_EQ(makeDataFolder(home, data, sizeof data), true);
	ASSERT_BOOL_EQ(writeSave(data, "cloud/games", "LIV_MAIN_FM24.fm"), true);
	(void)snprintf(harness, sizeof harness, "%s/Library/Application Support/" EDITION_HARNESS, home);
	(void)snprintf(steam, sizeof steam, "%s/Library/Application Support/Steam", home);
	ASSERT_BOOL_EQ(makeDirectories(harness) && installFakeGame(steam, app, sizeof app), true);
	ASSERT_BOOL_EQ(macosGame_installHarness(false, launcher, sizeof launcher), true);
	char option[1400];
	(void)snprintf(option, sizeof option, "/bin/sh \"%s\" %%command%%", launcher);
	ASSERT_BOOL_EQ(setLaunchOption(home, option), true);
	// open's stand-in lists the harness folder as the game would start, and fails.
	char script[1300];
	char seenPath[1300];
	char seen[4096];
	char body[2800];
	(void)snprintf(script, sizeof script, "%s/open.sh", home);
	(void)snprintf(seenPath, sizeof seenPath, "%s/seen.txt", home);
	(void)snprintf(body, sizeof body, "#!/bin/sh\n/bin/ls '%s' > '%s'\nexit 1\n", harness, seenPath);
	ASSERT_BOOL_EQ(writeText(script, body) && chmod(script, 0755) == 0, true);
	spawnInstead = script;
	int self = (int)getpid();
	char request[1400];
	char saveRequest[1400];
	char name[64];
	char saveName[64];
	(void)snprintf(request, sizeof request, "%s/rosetta-request-%d", harness, self);
	(void)snprintf(saveRequest, sizeof saveRequest, "%s/autoplay-request-%d.json", harness, self);
	(void)snprintf(name, sizeof name, "rosetta-request-%d\n", self);
	(void)snprintf(saveName, sizeof saveName, "autoplay-request-%d.json\n", self);
	// Through Steam the request, named after fm24ctl, waits for the launcher, with or without a
	// save's, and goes once the start has failed.
	LaunchOptions options = {.rosetta = true, .timeoutS = 30};
	spawned[0] = 0;
	ASSERT_INT_EQ(launchGame(&options, launcher, 0), 1);
	ASSERT_INT_EQ(strcmp(spawned, "/usr/bin/open|/usr/bin/open|steam://rungameid/" EDITION_STEAM_APP_ID "|"), 0);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, name) != NULL, true);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0, true);
	options.save = L"LIV_MAIN_FM24";
	ASSERT_INT_EQ(unlink(seenPath), 0);
	ASSERT_INT_EQ(launchGame(&options, launcher, 0), 1);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, name) != NULL, true);
	ASSERT_BOOL_EQ(strstr(seen, saveName) != NULL, true);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0 && access(saveRequest, F_OK) != 0, true);
	// Without --rosetta there is no such request.
	options.rosetta = false;
	ASSERT_INT_EQ(unlink(seenPath), 0);
	ASSERT_INT_EQ(launchGame(&options, launcher, 0), 1);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, name) == NULL, true);
	ASSERT_BOOL_EQ(strstr(seen, saveName) != NULL, true);
	// A direct start opens the x86_64 slice itself and leaves no request.
	wchar_t given[1300];
	ASSERT_BOOL_EQ(platform_fromUtf8(app, given, 1300), true);
	options = (LaunchOptions){.app = given, .rosetta = true, .timeoutS = 30};
	ASSERT_INT_EQ(unlink(seenPath), 0);
	spawned[0] = 0;
	ASSERT_INT_EQ(launchGame(&options, launcher, 0), 1);
	char expected[1400];
	(void)snprintf(expected, sizeof expected, "|--arch|x86_64|%s|", app);
	ASSERT_BOOL_EQ(endsWith(spawned, expected), true);
	ASSERT_BOOL_EQ(readText(seenPath, seen, sizeof seen) && strstr(seen, "rosetta-") == NULL, true);
	// A request that cannot be written starts nothing.
	options.app = NULL;
	ASSERT_INT_EQ(chmod(harness, 0555), 0);
	spawned[0] = 0;
	ASSERT_INT_EQ(launchGame(&options, launcher, 0), 1);
	ASSERT_INT_EQ(spawned[0], 0);
	ASSERT_INT_EQ(chmod(harness, 0755), 0);
	ASSERT_BOOL_EQ(access(request, F_OK) != 0, true);
	spawnInstead = "/usr/bin/true";
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(checks_a_running_game_before_looking_its_save_up) {
	(void)snprintf(testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-launch-test-%d.sock", (int)getpid());
	(void)unlink(testSocket);
	char home[] = "/tmp/" EDITION_LOWER " running launch-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char data[1200];
	char sandbox[1300];
	ASSERT_BOOL_EQ(makeDataFolder(home, data, sizeof data), true);
	(void)snprintf(sandbox, sizeof sandbox, "%s/sandbox", home);
	ASSERT_BOOL_EQ(makeDirectories(sandbox), true);
	ASSERT_BOOL_EQ(writeSave(data, "cloud/games", "LIV_MAIN_FM24.fm"), true);
	ASSERT_BOOL_EQ(writeSave(data, "cloud/games", "Both.fm"), true);
	ASSERT_BOOL_EQ(writeSave(data, "games", "Both.fm"), true);
	ASSERT_BOOL_EQ(writeSave(home, "sandbox", "LIV_MAIN_FM24.fm"), true);
	ASSERT_BOOL_EQ(writeSave(home, "sandbox", "LIV_COPY.fm"), true);
	pthread_t server;
	int listener = -1;
	bool serving = startHarness(&server, &listener);
	ASSERT_BOOL_EQ(serving, true);
	if (!serving) {
		return;
	}
	static const char *const bothLoaded[] = {"ok: ready " TEST_IDLE_FLAGS " save=Both"};
	static const char *const goneLoaded[] = {"ok: ready " TEST_IDLE_FLAGS " save=Gone"};
	char paths[3][1400];
	(void)snprintf(paths[0], sizeof paths[0], "%s/LIV_MAIN_FM24.fm", sandbox);
	(void)snprintf(paths[1], sizeof paths[1], "%s/LIV_COPY.fm", sandbox);
	(void)snprintf(paths[2], sizeof paths[2], "%s/cloud/games/LIV_MAIN_FM24.fm", data);
	wchar_t given[3][1400];
	for (size_t i = 0; i < 3; ++i) {
		ASSERT_BOOL_EQ(platform_fromUtf8(paths[i], given[i], 1400), true);
	}
	// A name is compared with the loaded save's, even one that is in both save folders or in
	// neither. A copy with the name of a save in FM24's folders is refused before the game is
	// asked; other paths count by their names.
	const struct {
		const wchar_t *save;
		const char *const *replies;
		int status;
		size_t asked;
	} cases[] = {
		{L"Both", bothLoaded, 0, 1},
		{L"Gone", goneLoaded, 0, 1},
		{L"LIV_MAIN_FM24", otherSave, EXIT_WRONG_STATE, 1},
		{given[0], sameSave, 1, 0},
		{given[1], sameSave, EXIT_WRONG_STATE, 1},
		{given[2], sameSave, 0, 1},
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		answerStatus(cases[i].replies, 1);
		LaunchOptions options = {.save = cases[i].save, .timeoutS = 30};
		ASSERT_INT_EQ(launchGame(&options, "/x/" EDITION_LAUNCHER, getpid()), cases[i].status);
		ASSERT_INT_EQ((int)atomic_load(&statusRepliesGiven), (int)cases[i].asked);
	}
	stopHarness(server, listener);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(looks_saves_up_in_the_data_folder_steam_gives_the_game) {
	static const struct {
		const char *option;
		OptionData data;
		const char *folder;
	} options[] = {
		{"/bin/sh \"/x/" EDITION_LAUNCHER "\" %command%", OPTION_DATA_NONE, ""},
		{"/bin/sh \"/x/l.sh\" %command% --user_data_location=/V/FM", OPTION_DATA_FOUND, "/V/FM"},
		{"%command% -user_data_location=\"/V/My FM\" -windowed", OPTION_DATA_FOUND, "/V/My FM"},
		{"%command% \"--user_data_location=/V/My \\\"FM\\\"\"", OPTION_DATA_FOUND, "/V/My \"FM\""},
		{"%command% --user_data_location='/V/a \\ b'", OPTION_DATA_FOUND, "/V/a \\ b"},
		{"%command% --user_data_location=/V/a\\ b", OPTION_DATA_FOUND, "/V/a b"},
		{"%command% --user_data_location=/A --user_data_location=/A", OPTION_DATA_FOUND, "/A"},
		{"%command% --user_data_location /Volumes/FM", OPTION_DATA_UNCLEAR, ""},
		{"%command% --user_data_location=~/FM", OPTION_DATA_UNCLEAR, ""},
		{"%command% --user_data_location=$HOME/FM", OPTION_DATA_UNCLEAR, ""},
		{"%command% --user_data_location=", OPTION_DATA_UNCLEAR, ""},
		{"%command% ---user_data_location=/A", OPTION_DATA_UNCLEAR, ""},
		{"%command% --user_data_location=/A --user_data_location=/B", OPTION_DATA_UNCLEAR, ""},
	};
	for (size_t i = 0; i < sizeof options / sizeof options[0]; ++i) {
		char folder[256] = "";
		OptionData found = optionDataFolder(options[i].option, folder, sizeof folder);
		ASSERT_INT_EQ((int)found, (int)options[i].data);
		if (options[i].data == OPTION_DATA_FOUND) {
			ASSERT_INT_EQ(strcmp(folder, options[i].folder), 0);
		}
	}
	// Through Steam a save is looked up in the folder its launch option gives the game, not in a
	// stale copy in the usual one; a direct start, or --user-data, uses the usual folder.
	char home[] = "/tmp/" EDITION_LOWER " data folder-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char data[1200];
	char moved[1300];
	char option[1500];
	char expected[1500];
	ASSERT_BOOL_EQ(makeDataFolder(home, data, sizeof data), true);
	(void)snprintf(moved, sizeof moved, "%s/FM Data/cloud/games", home);
	ASSERT_BOOL_EQ(makeDirectories(moved), true);
	(void)snprintf(moved, sizeof moved, "%s/FM Data", home);
	ASSERT_BOOL_EQ(writeSave(data, "cloud/games", "LIV_MAIN_FM24.fm"), true);
	ASSERT_BOOL_EQ(writeSave(moved, "cloud/games", "LIV_MAIN_FM24.fm"), true);
	static const char launchWith[] = "/bin/sh \"/x/" EDITION_LAUNCHER "\" %command% --user_data_location";
	(void)snprintf(option, sizeof option, "%s=\"%s\"", launchWith, moved);
	ASSERT_BOOL_EQ(setLaunchOption(home, option), true);
	wchar_t usual[1200];
	ASSERT_BOOL_EQ(platform_fromUtf8(data, usual, 1200), true);
	LaunchOptions launch = {.save = L"LIV_MAIN_FM24", .timeoutS = 30};
	SaveTarget target = {0};
	ASSERT_BOOL_EQ(findSave(&launch, true, true, &target), true);
	(void)snprintf(expected, sizeof expected, "%s/cloud/games/LIV_MAIN_FM24.fm", moved);
	ASSERT_INT_EQ(strcmp(target.path, expected), 0);
	(void)snprintf(expected, sizeof expected, "%s/cloud/games/LIV_MAIN_FM24.fm", data);
	ASSERT_BOOL_EQ(findSave(&launch, false, true, &target), true);
	ASSERT_INT_EQ(strcmp(target.path, expected), 0);
	launch.userData = usual;
	ASSERT_BOOL_EQ(findSave(&launch, true, true, &target), true);
	ASSERT_INT_EQ(strcmp(target.path, expected), 0);
	// A launch option that sets the folder in a way fm24ctl does not follow stops a start through
	// Steam, for launch and for a command's job.
	(void)snprintf(option, sizeof option, "%s \"%s\"", launchWith, moved);
	ASSERT_BOOL_EQ(setLaunchOption(home, option), true);
	launch.userData = NULL;
	ASSERT_BOOL_EQ(findSave(&launch, true, true, &target), false);
	SessionNeed need = {.save = "LIV_MAIN_FM24"};
	launch.job = &need;
	ASSERT_BOOL_EQ(findSave(&launch, true, true, &target), false);
	ASSERT_BOOL_EQ(findSave(&launch, false, true, &target), true);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

// Whether the viewer's list has a save named `name` in `folder`.
static bool listed(const SaveList *list, const char *name, unsigned folder) {
	for (size_t i = 0; i < saveList_count(list); ++i) {
		char listedName[256];
		(void)saveList_name(list, i, listedName, sizeof listedName);
		if (strcmp(listedName, name) == 0 && saveList_folder(list, i) == folder) {
			return true;
		}
	}
	return false;
}

// Checks the viewer's list of the saves under `home`, made for `option`, against fm24ctl's search,
// with HOME set to `home`.
static void checkSaveList(const char *home, int option) {
	static const char *const others[] = {"UPPER", "Folder", "Broken", "Stale", "Elsewhere", "config"};
	char launcher[1200];
	(void)snprintf(
		launcher,
		sizeof launcher,
		"%s/Library/Application Support/" EDITION_HARNESS "/%s",
		home,
		MACOS_GAME_LAUNCHER
	);
	bool throughSteam = launchOptionSet(launcher);
	ASSERT_BOOL_EQ(
		throughSteam, option != SAVE_SAMPLES_NO_OPTION && option != SAVE_SAMPLES_OTHER_LAUNCHER
	);
	// The viewer reads Steam's launch option as fm24ctl does, for its advice before a direct start.
	ASSERT_BOOL_EQ(saveList_launchOptionSet(home), throughSteam);
	LaunchOptions launch = {.save = L"LIV_MAIN_FM24", .timeoutS = 30};
	char data[1300] = "";
	char expected[1300];
	bool known = dataFolder(&launch, throughSteam, data, sizeof data);
	ASSERT_BOOL_EQ(known, option != SAVE_SAMPLES_UNCLEAR);
	ASSERT_BOOL_EQ(saveSamples_dataFolder(home, option, expected, sizeof expected), true);
	SaveList *list = saveList_read(home);
	ASSERT_BOOL_EQ(list != NULL, true);
	if (!list) {
		return;
	}
	char folder[1300];
	(void)saveList_dataFolder(list, folder, sizeof folder);
	SaveListData kind = option == SAVE_SAMPLES_STEAM_FOLDER ? SAVE_LIST_STEAM
											: option == SAVE_SAMPLES_UNCLEAR    ? SAVE_LIST_UNCLEAR
																													: SAVE_LIST_USUAL;
	ASSERT_INT_EQ((int)saveList_data(list), (int)kind);
	ASSERT_INT_EQ(strcmp(folder, known ? data : ""), 0);
	ASSERT_INT_EQ(strcmp(folder, expected), 0);
	ASSERT_INT_EQ((int)saveList_count(list), known ? SAVE_SAMPLES_SAVES : 0);
	// Each save, newest first, which `launch --save <its path>` loads under its name.
	for (size_t i = 0; known && i < saveList_count(list) && i < SAVE_SAMPLES_SAVES; ++i) {
		const SaveSample *sample = &saveSamples_saves[i];
		char name[256];
		char path[1300];
		char samplePath[1500];
		(void)saveList_name(list, i, name, sizeof name);
		(void)saveList_path(list, i, path, sizeof path);
		(void)snprintf(samplePath, sizeof samplePath, "%s/%s/%s", data, sample->folder, sample->file);
		ASSERT_INT_EQ(strcmp(name, sample->name), 0);
		ASSERT_INT_EQ(strcmp(path, samplePath), 0);
		ASSERT_INT_EQ(
			strcmp(saves_folderName((SavesFolder)saveList_folder(list, i)), sample->folder), 0
		);
		ASSERT_BOOL_EQ(saveList_modified(list, i) == SAVE_SAMPLES_TIME + (long long)sample->age, true);
		wchar_t given[1300];
		SaveTarget target = {0};
		launch.save = given;
		ASSERT_BOOL_EQ(
			platform_fromUtf8(path, given, 1300) && findSave(&launch, throughSteam, true, &target), true
		);
		ASSERT_BOOL_EQ(strcmp(target.path, path) == 0 && strcmp(target.name, name) == 0, true);
	}
	// fm24ctl's own search of the folders finds a save of each name where the list has one, and
	// none of the other files.
	for (size_t i = 0; known && i < SAVE_SAMPLES_SAVES + sizeof others / sizeof others[0]; ++i) {
		const char *name =
			i < SAVE_SAMPLES_SAVES ? saveSamples_saves[i].name : others[i - SAVE_SAMPLES_SAVES];
		SaveSearch search;
		saves_startSearch(&search, name);
		for (int searched = 0; searched < SAVES_FOLDER_COUNT; ++searched) {
			ASSERT_BOOL_EQ(listSaves(data, (SavesFolder)searched, &search), true);
		}
		for (unsigned searched = 0; searched < SAVES_FOLDER_COUNT; ++searched) {
			ASSERT_BOOL_EQ(search.found[searched], listed(list, name, searched));
		}
	}
	saveList_free(list);
}

TEST(lists_the_saves_launch_finds_for_the_viewer) {
	// The viewer's Launch FM24 sheet (save_list.c) offers the saves `launch --save` finds, in the
	// data folder `launch` uses, whatever Steam's launch option says about it; the viewer's Swift
	// tests check its list of the same folders.
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	for (int option = 0; option < SAVE_SAMPLES_OPTION_COUNT; ++option) {
		char home[] = "/tmp/" EDITION_LOWER " save list-XXXXXX";
		ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
		ASSERT_BOOL_EQ(saveSamples_make(home, option), true);
		setenv("HOME", home, 1);
		checkSaveList(home, option);
		ASSERT_BOOL_EQ(removeTree(home), true);
	}
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	// Without a usable home folder there is nothing to list.
	SaveList *list = saveList_read("relative/home");
	ASSERT_BOOL_EQ(
		list && saveList_data(list) == SAVE_LIST_NO_HOME && saveList_count(list) == 0, true
	);
	saveList_free(list);
}

// Steam's settings for one account that gives FM24 launch option `option`.
static bool writeLaunchOption(const char *home, const char *option) {
	char escaped[2048];
	size_t length = 0;
	for (const char *c = option; *c && length + 2 < sizeof escaped; ++c) {
		if (*c == '"' || *c == '\\') {
			escaped[length++] = '\\';
		}
		escaped[length++] = *c;
	}
	escaped[length] = 0;
	char path[1200];
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/userdata/42/config", home);
	char text[4096];
	(void)snprintf(
		text,
		sizeof text,
		"\"UserLocalConfigStore\"\n{\n\t\"Software\"\n\t{\n\t\t\"Valve\"\n\t\t{\n\t\t\t\"Steam\"\n\t\t\t{\n"
		"\t\t\t\t\"apps\"\n\t\t\t\t{\n\t\t\t\t\t\"" EDITION_STEAM_APP_ID "\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"LaunchOptions\"\t\t"
		"\"%s\"\n\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n",
		escaped
	);
	bool made = makeDirectories(path);
	(void)snprintf(
		path,
		sizeof path,
		"%s/Library/Application Support/Steam/userdata/42/config/localconfig.vdf",
		home
	);
	return made && writeText(path, text);
}

// The launch option `install` prints for the launcher in `home`, with `rest` after %command%.
static void launcherOption(const char *home, const char *rest, char *out, size_t size) {
	(void)snprintf(
		out,
		size,
		"/bin/sh \"%s/Library/Application Support/" EDITION_HARNESS "/%s\" %%command%%%s",
		home,
		MACOS_GAME_LAUNCHER,
		rest
	);
}

// What `fm24ctl install` printed, with its exit code checked.
static void install(char *out, size_t size) {
	wchar_t *arguments[] = {L"" EDITION_CLI, L"install", NULL};
	Capture capture;
	bool capturing = startCapture(&capture);
	int status = capturing ? macosGame_install(2, arguments) : -1;
	endCapture(&capture, out, size);
	ASSERT_BOOL_EQ(capturing, true);
	ASSERT_INT_EQ(status, 0);
}

TEST(install_says_where_it_looked_and_what_steam_has) {
	char home[] = "/tmp/" EDITION_LOWER " install texts-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char printed[16384];
	char expected[4096];
	// No Steam: where fm24ctl looks for it, and how to set the option, keeping one's own arguments.
	install(printed, sizeof printed);
	(void)snprintf(
		expected,
		sizeof expected,
		EDITION_GAME " was not found in your Steam libraries\nSteam is not in %s/Library/"
		"Application Support/Steam, where " EDITION_CLI " looks for it: install Steam and " EDITION_GAME " "
		"(" EDITION_GAME_VERSION "), then run `" EDITION_CLI " install` again.\n",
		home
	);
	ASSERT_BOOL_EQ(strstr(printed, expected) != NULL, true);
	ASSERT_BOOL_EQ(
		strstr(
			printed,
			"Keep any arguments of your own after %command%. Steam may save the option only when it "
			"quits: run `" EDITION_CLI " install` again until it says that Steam's launch option uses this "
			"launcher, quitting Steam if it does not. Until then, `" EDITION_CLI " launch` starts the game "
			"without the launch option, which has not been verified: the game crashed in its one "
			"attempt.\n"
		) != NULL,
		true
	);
	ASSERT_BOOL_EQ(strstr(printed, "is now: ") == NULL, true);
	// Steam without the game: which libraries it looked through.
	char path[1200];
	(void)snprintf(path, sizeof path, "%s/Library/Application Support/Steam/steamapps", home);
	ASSERT_BOOL_EQ(makeDirectories(path), true);
	char vdf[2048];
	(void)snprintf(
		vdf,
		sizeof vdf,
		"\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"%s/Library/Application Support/Steam\"\n"
		"\t}\n}\n",
		home
	);
	(void)snprintf(
		path, sizeof path, "%s/Library/Application Support/Steam/steamapps/libraryfolders.vdf", home
	);
	ASSERT_BOOL_EQ(writeText(path, vdf), true);
	install(printed, sizeof printed);
	(void)snprintf(
		expected,
		sizeof expected,
		EDITION_GAME " was not found in your Steam libraries\nInstall " EDITION_GAME " "
		"(" EDITION_GAME_VERSION ") in Steam, then run `" EDITION_CLI " install` again; " EDITION_CLI " looks in the Steam libraries "
		"that %s/Library/Application Support/Steam/steamapps/libraryfolders.vdf lists.\n",
		home
	);
	ASSERT_BOOL_EQ(strstr(printed, expected) != NULL, true);
	// The account's own option, which the new one replaces.
	ASSERT_BOOL_EQ(writeLaunchOption(home, "-windowed %command% -skip_intro"), true);
	install(printed, sizeof printed);
	ASSERT_BOOL_EQ(
		strstr(
			printed,
			"\nSteam's launch option for " EDITION_GAME " is now: -windowed %command% "
			"-skip_intro\n\nTo start " EDITION_GAME " from Steam with the harness, set its launch "
			"option once:\n"
		) != NULL,
		true
	);
	// The launcher's option: start from Steam; with a data folder fm24ctl does not follow, a
	// warning.
	static const char alreadySet[] =
		"\nSteam's launch option for " EDITION_GAME " already uses this launcher: start the game "
		"from Steam as usual.\n";
	static const char unclear[] =
		"It sets " EDITION_PRODUCT "'s data folder (user_data_location) in a way " EDITION_CLI " does not follow, which "
		"stops `" EDITION_CLI " launch --save` and the commands that start the game for a fixture's save: "
		"write it as --user_data_location=<absolute folder>.\n";
	char option[1500];
	launcherOption(home, "", option, sizeof option);
	ASSERT_BOOL_EQ(writeLaunchOption(home, option), true);
	install(printed, sizeof printed);
	ASSERT_BOOL_EQ(strstr(printed, alreadySet) != NULL, true);
	ASSERT_BOOL_EQ(strstr(printed, unclear) == NULL && strstr(printed, "is now: ") == NULL, true);
	ASSERT_BOOL_EQ(strstr(printed, "Keep any arguments") == NULL, true);
	launcherOption(home, " --user_data_location \"/Volumes/FM Data\"", option, sizeof option);
	ASSERT_BOOL_EQ(writeLaunchOption(home, option), true);
	install(printed, sizeof printed);
	(void)snprintf(expected, sizeof expected, "%s%s", alreadySet, unclear);
	ASSERT_BOOL_EQ(strstr(printed, expected) != NULL, true);
	launcherOption(home, " --user_data_location=\"/Volumes/FM Data\"", option, sizeof option);
	ASSERT_BOOL_EQ(writeLaunchOption(home, option), true);
	install(printed, sizeof printed);
	ASSERT_BOOL_EQ(strstr(printed, alreadySet) != NULL && strstr(printed, unclear) == NULL, true);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

#if EDITION_ENGINE_PLUGIN
// What `fmNNctl install` with `option` printed, and its exit code.
static int installWith(const wchar_t *option, char *out, size_t size) {
	wchar_t *arguments[] = {L"" EDITION_CLI, L"install", (wchar_t *)option, NULL};
	Capture capture;
	bool capturing = startCapture(&capture);
	int status = capturing ? macosGame_install(option ? 3 : 2, arguments) : -1;
	endCapture(&capture, out, size);
	ASSERT_BOOL_EQ(capturing, true);
	return status;
}

TEST(install_log_only_has_the_harness_only_log) {
	char home[] = "/tmp/" EDITION_LOWER " install log-only-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char marker[1200];
	(void)snprintf(
		marker,
		sizeof marker,
		"%s/Library/Application Support/" EDITION_HARNESS "/" HARNESS_LOG_ONLY_FILE,
		home
	);
	char printed[16384];
	char expected[2048];
	struct stat status;
	ASSERT_INT_EQ(installWith(L"--log-only", printed, sizeof printed), 0);
	ASSERT_INT_EQ(stat(marker, &status), 0);
	(void)snprintf(
		expected,
		sizeof expected,
		"log-only mode: the harness only logs to %s/Library/Application Support/" EDITION_HARNESS
		"/harness.log how " EDITION_GAME " loads and unloads its match engine, and serves no jobs. `"
		EDITION_CLI " install` without --log-only turns this off, as does any " EDITION_CLI
		" command that installs the harness.\n",
		home
	);
	ASSERT_BOOL_EQ(strstr(printed, expected) != NULL, true);
	// Another install serves jobs again, as does a command that installs the harness first.
	ASSERT_INT_EQ(installWith(NULL, printed, sizeof printed), 0);
	ASSERT_BOOL_EQ(stat(marker, &status) != 0 && errno == ENOENT, true);
	ASSERT_BOOL_EQ(strstr(printed, "log-only mode") == NULL, true);
	ASSERT_INT_EQ(installWith(L"--log-only", printed, sizeof printed), 0);
	char launcher[1200];
	ASSERT_BOOL_EQ(macosGame_installHarness(false, launcher, sizeof launcher), true);
	ASSERT_BOOL_EQ(stat(marker, &status) != 0 && errno == ENOENT, true);
	ASSERT_INT_EQ(installWith(L"--log", printed, sizeof printed), 2);
	ASSERT_BOOL_EQ(strstr(printed, "usage: " EDITION_CLI " install [--log-only]\n") != NULL, true);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}
#endif

TEST(explains_how_to_start_the_game_with_the_harness) {
	char running[1024];
	if (macosGame_findRunning(running, sizeof running)) {
		return; // a game that runs is described otherwise
	}
	char home[] = "/tmp/" EDITION_LOWER " inject texts-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char printed[4096];
	char expected[4096];
	char option[1500];
	launcherOption(home, "", option, sizeof option);
	// Without Steam's launch option: set it, then start the game from Steam (`launch` would start
	// it directly, which has not been verified).
	Capture capture;
	ASSERT_BOOL_EQ(startCapture(&capture), true);
	bool injected = macosTest_injectHarness();
	endCapture(&capture, printed, sizeof printed);
	ASSERT_BOOL_EQ(injected, false);
	(void)snprintf(
		expected,
		sizeof expected,
		EDITION_GAME " is not running.\nSet Steam's launch option for it once, then start it "
		"from Steam (`" EDITION_CLI " install` says how, and when Steam has saved the option):\n  %s\n",
		option
	);
	ASSERT_INT_EQ(strcmp(printed, expected), 0);
	// With it, Steam or `launch`, which then goes through Steam.
	ASSERT_BOOL_EQ(writeLaunchOption(home, option), true);
	ASSERT_BOOL_EQ(startCapture(&capture), true);
	injected = macosTest_injectHarness();
	endCapture(&capture, printed, sizeof printed);
	ASSERT_BOOL_EQ(injected, false);
	ASSERT_INT_EQ(
		strcmp(
			printed,
			EDITION_GAME " is not running.\nStart it from Steam, whose launch option loads the "
			"harness, or with `" EDITION_CLI " launch`; then load your save and leave the game idle.\n"
		),
		0
	);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

TEST(says_how_to_load_the_save_no_launch_does_not_start) {
	char home[] = "/tmp/" EDITION_LOWER " no-launch texts-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(home) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", home, 1);
	char printed[4096];
	Capture capture;
	// Without Steam's launch option, Steam's route only: `launch` would start the game directly.
	SessionNeed need = {.save = "Rob's Career", .launch = false, .timeoutMs = 1000};
	ASSERT_BOOL_EQ(startCapture(&capture), true);
	int status = ensureSave(&need, 0, NULL, 0);
	endCapture(&capture, printed, sizeof printed);
	ASSERT_INT_EQ(status, 0);
	ASSERT_INT_EQ(
		strcmp(
			printed,
			"The job is for the save Rob's Career, and --no-launch keeps " EDITION_CLI " from starting " EDITION_PRODUCT " "
			"with it: set Steam's launch option for the harness (`" EDITION_CLI " install` says how), start "
			EDITION_PRODUCT " from Steam and load that save, then run this again.\n"
		),
		0
	);
	// With it, also `launch --save`, the name quoted to paste.
	char option[1500];
	launcherOption(home, "", option, sizeof option);
	ASSERT_BOOL_EQ(writeLaunchOption(home, option), true);
	static const struct {
		const char *save;
		const char *word;
	} saves[] = {
		{"Rob's Career", "'Rob'\\''s Career'"},
		{"My Career", "'My Career'"},
		{"LIV_MAIN_FM24", "LIV_MAIN_FM24"},
		{"$HOME", "'$HOME'"},
	};
	char expected[2048];
	for (size_t i = 0; i < sizeof saves / sizeof saves[0]; ++i) {
		need.save = saves[i].save;
		ASSERT_BOOL_EQ(startCapture(&capture), true);
		status = ensureSave(&need, 0, NULL, 0);
		endCapture(&capture, printed, sizeof printed);
		ASSERT_INT_EQ(status, 0);
		(void)snprintf(
			expected,
			sizeof expected,
			"The job is for the save %s, and --no-launch keeps " EDITION_CLI " from starting " EDITION_PRODUCT " with it: "
			"start " EDITION_PRODUCT " from Steam and load that save, or run `" EDITION_CLI " launch --save %s`, then run "
			"this again; or leave out --no-launch.\n",
			saves[i].save,
			saves[i].word
		);
		ASSERT_INT_EQ(strcmp(printed, expected), 0);
	}
	// A job for whatever save is loaded, and one that may start the game, get no note.
	need.save = NULL;
	ASSERT_BOOL_EQ(startCapture(&capture), true);
	status = ensureSave(&need, 0, NULL, 0);
	endCapture(&capture, printed, sizeof printed);
	ASSERT_BOOL_EQ(status == 0 && printed[0] == 0, true);
	// Quoting: an empty word, and one that does not fit.
	char word[8];
	ASSERT_BOOL_EQ(shellWord("", word, sizeof word) && strcmp(word, "''") == 0, true);
	ASSERT_BOOL_EQ(shellWord("abcdefg", word, sizeof word) && strcmp(word, "abcdefg") == 0, true);
	ASSERT_BOOL_EQ(shellWord("abcdefgh", word, sizeof word), false);
	ASSERT_BOOL_EQ(shellWord("a b c d", word, sizeof word), false);
	ASSERT_BOOL_EQ(shellWord("a'b", word, sizeof word), false); // 'a'\''b' and its NUL: 9 bytes
	char longer[9];
	ASSERT_BOOL_EQ(shellWord("a'b", longer, sizeof longer) && strcmp(longer, "'a'\\''b'") == 0, true);
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	ASSERT_BOOL_EQ(removeTree(home), true);
}

#if EDITION_ENGINE_PLUGIN
// The stand-in game as Steam starts the game, which the end-to-end test has a start through Steam
// do: the launcher in Steam's launch option, given the app as Steam's %command%, starts it with the
// stand-in harness `install` put in place, and it loads the stand-in engine as it starts.
typedef struct SteamGame {
	StandInGame game;
	bool started;
	bool commandLine; // the game passes its command line to its engine
	char launcher[1300];
	char app[1300];
	char engine[1400];    // FMNN_TEST_LOAD: the engine plugin
	char engineLog[1400]; // FMNN_TEST_ENGINE_LOG
	char socket[200];     // FMNN_TEST_SOCKET: testSocket
} SteamGame;
static SteamGame steamGame;

static void startThroughSteamLauncher(void) {
	size_t count = 0;
	while (environ[count]) {
		++count;
	}
	char **environment = calloc(count + 6, sizeof *environment);
	int input[2] = {-1, -1};
	int output[2] = {-1, -1};
	if (steamGame.started || !environment || pipe(input) != 0 || pipe(output) != 0) {
		free(environment);
		return;
	}
	for (int i = 0; i < 2; ++i) {
		(void)fcntl(input[i], F_SETFD, FD_CLOEXEC);
		(void)fcntl(output[i], F_SETFD, FD_CLOEXEC);
	}
	size_t used = 0;
	static const char testPrefix[] = EDITION_ENV_PREFIX "TEST_";
	for (size_t i = 0; i < count; ++i) {
		if (strncmp(environ[i], "DYLD_", 5) != 0 &&
				strncmp(environ[i], testPrefix, sizeof testPrefix - 1) != 0) {
			environment[used++] = environ[i];
		}
	}
	static char bundles[] = EDITION_ENV_PREFIX "TEST_BUNDLES=1";
	static char noCommandLine[] = EDITION_ENV_PREFIX "TEST_NO_COMMAND_LINE=1";
	environment[used++] = bundles;
	environment[used++] = steamGame.socket;
	environment[used++] = steamGame.engine;
	environment[used++] = steamGame.engineLog;
	if (!steamGame.commandLine) {
		environment[used++] = noCommandLine;
	}
	posix_spawn_file_actions_t actions;
	(void)posix_spawn_file_actions_init(&actions);
	(void)posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
	(void)posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
	char *arguments[] = {"/bin/sh", steamGame.launcher, steamGame.app, NULL};
	int error =
		posix_spawn(&steamGame.game.pid, arguments[0], &actions, NULL, arguments, environment);
	(void)posix_spawn_file_actions_destroy(&actions);
	free(environment);
	close(input[0]);
	close(output[1]);
	steamGame.game.commands = input[1];
	steamGame.game.answers = output[0];
	(void)fcntl(steamGame.game.commands, F_SETNOSIGPIPE, 1);
	if (error != 0) {
		close(steamGame.game.commands);
		close(steamGame.game.answers);
	}
	steamGame.started = error == 0;
}

// What a load by hand waits for: `launch`'s words, captured in `printed`, saying to load the save.
typedef struct ByHand {
	int printed;
	char command[1500]; // play <save>
	bool played;
} ByHand;

static void *loadByHand(void *parameter) {
	ByHand *hand = parameter;
	char text[16384];
	for (int i = 0; i < 1200; ++i) {
		ssize_t length = pread(hand->printed, text, sizeof text - 1, 0);
		text[length > 0 ? length : 0] = 0;
		if (strstr(text, " yourself (")) {
			hand->played =
				hears(&steamGame.game, "loaded") && tell(&steamGame.game, hand->command, "playing");
			break;
		}
		platform_sleep(100);
	}
	return NULL;
}

// What `fmNNctl launch --save STAND_IN_SAVE --timeout 120` printed (with what the game was told by
// hand, if `hand`), and its exit code.
static int launchCapturing(ByHand *hand, char *out, size_t size) {
	wchar_t *arguments[] = {
		L"" EDITION_CLI, L"launch", L"--save", L"STAND_IN_SAVE", L"--timeout", L"120", NULL};
	Capture capture;
	pthread_t helper;
	bool capturing = startCapture(&capture);
	bool helping = false;
	// A line at a time, as fmNNctl prints (its main), so that the helper sees each as it is printed.
	(void)setvbuf(stdout, NULL, _IOLBF, 0);
	if (capturing && hand) {
		hand->printed = capture.file;
		helping = pthread_create(&helper, NULL, loadByHand, hand) == 0;
	}
	int status = capturing && (!hand || helping) ? macosGame_launch(6, arguments) : -1;
	if (helping) {
		pthread_join(helper, NULL);
	}
	endCapture(&capture, out, size);
	return status;
}

// True if one of fmNNctl's commands exits with `expected`; what it printed is in `out` (and on
// standard error if it did not).
static bool commandExits(
	int (*command)(int, wchar_t **),
	wchar_t **arguments,
	int expected,
	char *out,
	size_t size
) {
	int count = 0;
	while (arguments[count]) {
		++count;
	}
	Capture capture;
	bool capturing = startCapture(&capture);
	int status = capturing ? command(count, arguments) : -1;
	endCapture(&capture, out, size);
	if (status != expected) {
		fprintf(stderr, "%ls exited with %d, printing:\n%s\n", arguments[1], status, out);
	}
	return status == expected;
}

// True if no auto-play file or request is left in `folder`.
static bool noAutoplayFiles(const char *folder) {
	DIR *directory = opendir(folder);
	bool none = directory != NULL;
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = none ? readdir(directory) : NULL; entry; entry = readdir(directory)) {
		if (strncmp(entry->d_name, "autoplay", 8) == 0) {
			fprintf(stderr, "left in %s: %s\n", folder, entry->d_name);
			none = false;
		}
	}
	if (directory) {
		closedir(directory);
	}
	return none;
}

static bool printedHas(const char *printed, const char *text) {
	if (strstr(printed, text)) {
		return true;
	}
	fprintf(stderr, "not printed: \"%s\", in:\n%s\n", text, printed);
	return false;
}

// Ends the end-to-end test: no more stand-ins for Steam and the harness, HOME as it was before
// (`previous`, which is freed), and the test's folder and socket gone.
static void endEndToEnd(char *previous, const char *directory) {
	steamStarts = NULL;
	extractFrom = NULL;
	if (previous) {
		setenv("HOME", previous, 1);
		free(previous);
	}
	removeStandInGame(directory);
}

TEST(drives_the_stand_in_game_steam_starts_through_every_command) {
	char directory[] = "/tmp/" EDITION_LOWER " end to end-XXXXXX";
	ASSERT_BOOL_EQ(mkdtemp(directory) != NULL, true);
	char *previous = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
	setenv("HOME", directory, 1);
	(void)snprintf(
		testSocket, sizeof testSocket, "/tmp/" EDITION_LOWER "-end-to-end-%d.sock", (int)getpid()
	);
	(void)unlink(testSocket);
	// The game in a Steam library, with Steam's launch option set; a save; the stand-in game Steam
	// starts, as an app in a folder with spaces; and the stand-in harness for install to put in
	// place.
	char data[1200];
	char library[1200];
	char installed[1300];
	char harness[1300];
	char save[1400];
	char path[1500];
	char standInHarness[1100];
	wchar_t wide[1100];
	char directoryOfTests[1024];
	ASSERT_BOOL_EQ(
		paths_executableDirectory(wide, 1100) &&
			platform_toUtf8(wide, directoryOfTests, sizeof directoryOfTests),
		true
	);
	(void)snprintf(standInHarness, sizeof standInHarness, "%s/" STAND_IN_HARNESS, directoryOfTests);
	(void)snprintf(library, sizeof library, "%s/Library/Application Support/Steam", directory);
	(void)snprintf(
		harness, sizeof harness, "%s/Library/Application Support/" EDITION_HARNESS, directory
	);
	steamGame = (SteamGame){.commandLine = true};
	(void)snprintf(steamGame.launcher, sizeof steamGame.launcher, "%s/" EDITION_LAUNCHER, harness);
	(void)snprintf(steamGame.app, sizeof steamGame.app, "%s/" STAND_IN_APP, directory);
	(void)snprintf(
		steamGame.engine,
		sizeof steamGame.engine,
		EDITION_ENV_PREFIX "TEST_LOAD=%s/" STAND_IN_APP "/Contents/PlugIns/" EDITION_ENGINE_PATH,
		directory
	);
	(void)snprintf(
		steamGame.engineLog,
		sizeof steamGame.engineLog,
		EDITION_ENV_PREFIX "TEST_ENGINE_LOG=%s/engine.log",
		directory
	);
	(void)snprintf(
		steamGame.socket, sizeof steamGame.socket, EDITION_ENV_PREFIX "TEST_SOCKET=%s", testSocket
	);
	(void)snprintf(path, sizeof path, "/bin/sh \"%s\" %%command%%", steamGame.launcher);
	bool ready = makeDataFolder(directory, data, sizeof data) &&
							 writeSave(data, "cloud/games", "STAND_IN_SAVE.fm") &&
							 installFakeGame(library, installed, sizeof installed) &&
							 installStandInPluginGame(directory) && setLaunchOption(directory, path);
	ASSERT_BOOL_EQ(ready, true);
	(void)snprintf(path, sizeof path, "%s/Contents/Info.plist", steamGame.app);
	ready =
		ready && writeText(
							 path,
							 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
							 "\t<key>CFBundleExecutable</key>\n\t<string>" EDITION_GAME_EXECUTABLE "</string>\n"
							 "\t<key>CFBundleIdentifier</key>\n\t<string>" EDITION_GAME_BUNDLE_ID "</string>\n"
							 "\t<key>CFBundleShortVersionString</key>\n\t<string>27.0.0.1</string>\n"
							 "</dict>\n</plist>\n"
						 );
	ASSERT_BOOL_EQ(ready, true);
	(void)snprintf(save, sizeof save, "%s/cloud/games/STAND_IN_SAVE.fm", data);
	char running[1100];
	if (!ready || macosGame_findRunning(running, sizeof running)) {
		if (ready) {
			fprintf(stderr, "not run: the game is running (%s), and would be the one checked\n", running);
		}
		endEndToEnd(previous, directory);
		return;
	}
	extractFrom = standInHarness;
	steamStarts = startThroughSteamLauncher;
	char printed[32768];
	char expected[2400];
	// install puts the harness and launcher in place, finds the game and Steam's option for it.
	install(printed, sizeof printed);
	(void)snprintf(expected, sizeof expected, EDITION_GAME ": %s\n", installed);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	ASSERT_BOOL_EQ(
		printedHas(printed, "launch option for " EDITION_GAME " already uses this launcher"), true
	);
	// launch --save: Steam starts the game through the launcher, which hands it the auto-play file;
	// its engine loads the save as it starts, and fmNNctl waits for it, then removes the files.
	spawned[0] = 0;
	int status = launchCapturing(NULL, printed, sizeof printed);
	ASSERT_INT_EQ(status, 0);
	ASSERT_BOOL_EQ(steamGame.started, true);
	if (!steamGame.started) {
		endEndToEnd(previous, directory);
		return;
	}
	ASSERT_INT_EQ(
		strcmp(spawned, "/usr/bin/open|/usr/bin/open|steam://rungameid/" EDITION_STEAM_APP_ID "|"), 0
	);
	int game = (int)steamGame.game.pid;
	(void)snprintf(expected, sizeof expected, "loading STAND_IN_SAVE: %s\n", save);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	ASSERT_BOOL_EQ(
		printedHas(
			printed, "starting " EDITION_GAME " through Steam (its launch option loads the harness)\n"
		),
		true
	);
	(void)snprintf(
		expected, sizeof expected, "the harness is ready in " EDITION_GAME " (pid %d)\n", game
	);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	(void)snprintf(
		expected, sizeof expected, "ready: STAND_IN_SAVE loaded (pid %d, 2026 day 200 15:00)\n", game
	);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	ASSERT_BOOL_EQ(
		printedHas(
			printed,
			"the harness cannot read which save " EDITION_PRODUCT
			" has loaded; taking it to be STAND_IN_SAVE\n"
		),
		true
	);
	ASSERT_BOOL_EQ(strstr(printed, "yourself") == NULL, true);
	ASSERT_BOOL_EQ(hears(&steamGame.game, "loaded"), true);
	ASSERT_BOOL_EQ(noAutoplayFiles(harness), true);
	char engineLog[1300];
	(void)snprintf(engineLog, sizeof engineLog, "%s/engine.log", directory);
	char loadedSave[1500];
	(void)snprintf(loadedSave, sizeof loadedSave, "loaded %s\n", save);
	const char *const autoplayLoaded[] = {loadedSave};
	ASSERT_BOOL_EQ(hasLines(engineLog, autoplayLoaded, 1), true);
	// status: loaded and idle.
	wchar_t *statusArguments[] = {L"" EDITION_CLI, L"status", NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_status, statusArguments, 0, printed, sizeof printed), true);
	ASSERT_INT_EQ(
		strcmp(
			printed,
			"ok: ready in_progress=1 processing=0 kind=-1 people=2 human_team=1 menu_seen=1 autoplay=1 "
			"autoplay_read=1 date=2026 day 200 15:00\n"
		),
		0
	);
	// list-teams: the human manager's team, by its unique ID (clubs' names are not located).
	wchar_t output[1300];
	char teams[1300];
	(void)snprintf(teams, sizeof teams, "%s/teams.txt", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(teams, output, 1300), true);
	wchar_t *teamArguments[] = {L"" EDITION_CLI, L"list-teams", output, NULL};
	ASSERT_BOOL_EQ(
		commandExits(macosTest_listTeams, teamArguments, 0, printed, sizeof printed), true
	);
	char text[4096];
	ASSERT_BOOL_EQ(readText(teams, text, sizeof text) && strcmp(text, "8676\t?\n") == 0, true);
	// list-players and get-player: the save's one player, by the RTTI of the person's record.
	char players[1300];
	(void)snprintf(players, sizeof players, "%s/players.txt", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(players, output, 1300), true);
	wchar_t *playerArguments[] = {L"" EDITION_CLI, L"list-players", output, NULL};
	ASSERT_BOOL_EQ(
		commandExits(macosTest_listPlayers, playerArguments, 0, printed, sizeof printed), true
	);
	ASSERT_BOOL_EQ(
		readText(players, text, sizeof text) && strcmp(text, "31250\tAda\tStand-in\t\t\n") == 0, true
	);
	char player[1300];
	(void)snprintf(player, sizeof player, "%s/player.json", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(player, output, 1300), true);
	wchar_t *getArguments[] = {L"" EDITION_CLI, L"get-player", L"31250", output, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_getPlayer, getArguments, 0, printed, sizeof printed), true);
	const char *const values[] = {
		"\"format\": \"" EDITION_PLAYER_FORMAT "\"",
		"\"source\": \"Ada Stand-in\"",
		"{\"name\": \"person_uid\", \"player\": \"Ada Stand-in\", \"u32\": 31250}",
		"{\"name\": \"condition\", \"u16\": 9000}",
		"{\"name\": \"nationality\", \"u32\": 793}",
		"{\"name\": \"age\", \"u8\": 26}",
	};
	ASSERT_BOOL_EQ(hasLines(player, values, sizeof values / sizeof values[0]), true);
	// get-next-fixture: the harness looks for the human's next fixture, which this save has none of.
	char next[1300];
	(void)snprintf(next, sizeof next, "%s/next.json", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(next, output, 1300), true);
	wchar_t *nextArguments[] = {L"" EDITION_CLI, L"get-next-fixture", output, NULL};
	ASSERT_BOOL_EQ(
		commandExits(macosTest_getNextFixture, nextArguments, 1, printed, sizeof printed), true
	);
	ASSERT_BOOL_EQ(printedHas(printed, "no upcoming fixture"), true);
	// run: a fixture for this save, which fmNNctl checks is loaded, encodes and has the game play,
	// keeping its build, with detailed statistics; then encode, and the binary's own seed.
	char fixture[1300];
	char results[1300];
	(void)snprintf(fixture, sizeof fixture, "%s/match.json", directory);
	(void)snprintf(results, sizeof results, "%s/match.csv", directory);
	ASSERT_BOOL_EQ(
		writeText(
			fixture,
			"{\"format\":\"" EDITION_MATCH_CONFIG_FORMAT "\",\"save\":\"STAND_IN_SAVE\","
			"\"game_date\":\"2026 day 200 15:00\",\"fields\":[{\"u8\":27},{\"u8\":0},{\"u8\":0},"
			"{\"u8\":1},{\"u16\":76},{\"u8\":0},{\"u8\":2},{\"u32\":0},{\"str\":\"stand-in\"},"
			"{\"u32\":0},{\"u32\":7},{\"u32\":135943},{\"u32\":81921},"
			"{\"name\":\"home.player[0]\",\"player\":\"Old Player\",\"u8\":0},"
			"{\"name\":\"home.player[0].person_uid\",\"u32\":555},"
			"{\"name\":\"home.player[0].condition\",\"u16\":5000},{\"u32\":555}]}\n"
		),
		true
	);
	wchar_t input[1300];
	wchar_t csv[1300];
	ASSERT_BOOL_EQ(
		platform_fromUtf8(fixture, input, 1300) && platform_fromUtf8(results, csv, 1300), true
	);
	wchar_t *runArguments[] = {L"" EDITION_CLI,
		L"run",
		input,
		L"1-6",
		csv,
		L"--keep-build",
		L"--detailed",
		L"--threads",
		L"2",
		NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_run, runArguments, 0, printed, sizeof printed), true);
	const char *const rows[] = {
		"seed,home_goals,away_goals,clock_ticks,finished,rng_final,ms\n",
		"1,1,0,21604,1,5851f42d4c957f2e,",
		"2,2,0,21608,1,b0a3e85a992afe5b,",
		"3,3,0,21612,1,08f5dc87e5c07d88,",
		"4,0,1,21616,1,6147d0b53255fcb5,",
		"5,1,1,21620,1,b999c4e27eeb7be2,",
		"6,2,1,21624,1,11ebb90fcb80fb0f,",
	};
	ASSERT_BOOL_EQ(resultsAre(results, rows, sizeof rows / sizeof rows[0]), true);
	(void)snprintf(path, sizeof path, "%s/match-players.csv", directory);
	const char *const scorer[] = {"home,\"?\",\"?\",101,6,6,6,90.0,7.00,1.500,"};
	ASSERT_BOOL_EQ(hasLines(path, scorer, 1), true);
	char binary[1300];
	(void)snprintf(binary, sizeof binary, "%s/match.bin", directory);
	wchar_t encoded[1300];
	ASSERT_BOOL_EQ(platform_fromUtf8(binary, encoded, 1300), true);
	wchar_t *encodeArguments[] = {L"" EDITION_CLI, L"encode", input, encoded, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_encode, encodeArguments, 0, printed, sizeof printed), true);
	wchar_t *keepArguments[] = {L"" EDITION_CLI, L"run", encoded, L"keep", csv, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_run, keepArguments, 0, printed, sizeof printed), true);
	const char *const kept[] = {rows[0], "7,3,1,21628,1,"};
	ASSERT_BOOL_EQ(resultsAre(results, kept, 2), true);
	// inject-player: the player's values in the fixture's player block, which still plays.
	char injected[1300];
	(void)snprintf(injected, sizeof injected, "%s/injected.json", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(injected, output, 1300), true);
	wchar_t *injectArguments[] = {
		L"" EDITION_CLI, L"inject-player", input, L"home.player[0]", L"31250", output, NULL};
	ASSERT_BOOL_EQ(
		commandExits(macosTest_injectPlayer, injectArguments, 0, printed, sizeof printed), true
	);
	const char *const replaced[] = {
		"{\"name\":\"home.player[0]\",\"player\":\"Ada Stand-in\",\"u8\":0}",
		"{\"name\":\"home.player[0].person_uid\",\"u32\":31250}",
		"{\"name\":\"home.player[0].condition\",\"u16\":9000},{\"u32\":31250}]}",
	};
	ASSERT_BOOL_EQ(hasLines(injected, replaced, sizeof replaced / sizeof replaced[0]), true);
	wchar_t injectedInput[1300];
	ASSERT_BOOL_EQ(platform_fromUtf8(injected, injectedInput, 1300), true);
	wchar_t *injectedRun[] = {L"" EDITION_CLI, L"run", injectedInput, L"keep", csv, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_run, injectedRun, 0, printed, sizeof printed), true);
	ASSERT_BOOL_EQ(resultsAre(results, kept, 2), true);
	// An export, which records no save: run plays it once the game is idle with a save, which it
	// takes to be the fixture's, and warns that the save is at another date than the export's.
	char exported[1300];
	(void)snprintf(exported, sizeof exported, "%s/export.json", directory);
	ASSERT_BOOL_EQ(
		writeText(
			exported,
			"{\"format\":\"" EDITION_MATCH_CONFIG_FORMAT "\",\"game_date\":\"2026 day 100 15:00\","
			"\"fields\":[{\"u8\":27},{\"u8\":0},{\"u8\":0},{\"u8\":1},{\"u16\":76},{\"u8\":0},"
			"{\"u8\":2},{\"u32\":0},{\"str\":\"stand-in\"},{\"u32\":0},{\"u32\":7},{\"u32\":135943},"
			"{\"u32\":81921},{\"name\":\"home.player[0]\",\"player\":\"Old Player\",\"u8\":0},"
			"{\"name\":\"home.player[0].person_uid\",\"u32\":555},"
			"{\"name\":\"home.player[0].condition\",\"u16\":5000},{\"u32\":555}]}\n"
		),
		true
	);
	wchar_t exportInput[1300];
	ASSERT_BOOL_EQ(platform_fromUtf8(exported, exportInput, 1300), true);
	wchar_t *exportRun[] = {L"" EDITION_CLI, L"run", exportInput, L"1-6", csv, L"--keep-build", NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_run, exportRun, 0, printed, sizeof printed), true);
	ASSERT_BOOL_EQ(
		printedHas(
			printed,
			"warning: the save is at 2026 day 200 15:00, the fixture was exported at 2026 day 100 "
			"15:00; the results reflect the save\n"
		),
		true
	);
	ASSERT_BOOL_EQ(resultsAre(results, rows, sizeof rows / sizeof rows[0]), true);
	// While the game loads a save, run waits until the load has settled, then plays.
	(void)snprintf(path, sizeof path, "play %s", save);
	ASSERT_BOOL_EQ(tell(&steamGame.game, path, "playing"), true);
	ASSERT_BOOL_EQ(commandExits(macosTest_run, exportRun, 0, printed, sizeof printed), true);
	ASSERT_BOOL_EQ(printedHas(printed, EDITION_PRODUCT " state: loading\n"), true);
	(void)snprintf(
		expected,
		sizeof expected,
		"ready: the fixture's save is already loaded (pid %d, 2026 day 200 15:00)\n",
		game
	);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	ASSERT_BOOL_EQ(resultsAre(results, rows, sizeof rows / sizeof rows[0]), true);
	// replace-away: the harness finds a team by its unique ID, and refuses a club's name, as it
	// cannot read clubs' names.
	char replacedPath[1300];
	(void)snprintf(replacedPath, sizeof replacedPath, "%s/replaced.json", directory);
	ASSERT_BOOL_EQ(platform_fromUtf8(replacedPath, output, 1300), true);
	wchar_t *byName[] = {L"" EDITION_CLI, L"replace-away", exportInput, L"Stand-in", output, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_replaceTeam, byName, 1, printed, sizeof printed), true);
	ASSERT_BOOL_EQ(
		printedHas(
			printed,
			"error: the " EDITION_PRODUCT " harness cannot look teams up by club name yet: give the "
			"team's unique ID (list-teams lists them)\n"
		),
		true
	);
	wchar_t *byId[] = {L"" EDITION_CLI, L"replace-away", exportInput, L"999999", output, NULL};
	ASSERT_BOOL_EQ(commandExits(macosTest_replaceTeam, byId, 1, printed, sizeof printed), true);
	ASSERT_BOOL_EQ(printedHas(printed, "error: no team with unique ID 999999\n"), true);
	ASSERT_BOOL_EQ(tell(&steamGame.game, "unload", "unloaded"), true);
	ASSERT_INT_EQ(stopStandInGame(&steamGame.game), 0);
	// The fallback: the game does not pass its command line to its engine, which reaches its menu
	// without the save; fmNNctl says to load it by hand and waits until it is.
	steamGame.started = false;
	steamGame.commandLine = false;
	ByHand hand = {.printed = -1};
	(void)snprintf(hand.command, sizeof hand.command, "play %s", save);
	status = launchCapturing(&hand, printed, sizeof printed);
	ASSERT_INT_EQ(status, 0);
	ASSERT_BOOL_EQ(steamGame.started && hand.played, true);
	(void)snprintf(
		expected,
		sizeof expected,
		EDITION_PRODUCT " reached its menu without loading STAND_IN_SAVE: it did not read the "
										"auto-play file, so its command line may not reach its match engine. Load it "
										"in " EDITION_PRODUCT " yourself (%s), and leave the game idle; " EDITION_CLI
										" waits until it is loaded, for at most ",
		save
	);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	(void)snprintf(
		expected,
		sizeof expected,
		"ready: STAND_IN_SAVE loaded (pid %d, 2026 day 200 15:00)\n",
		(int)steamGame.game.pid
	);
	ASSERT_BOOL_EQ(printedHas(printed, expected), true);
	ASSERT_BOOL_EQ(noAutoplayFiles(harness), true);
	const char *const byHand[] = {"started without the game's command line\n", loadedSave};
	ASSERT_BOOL_EQ(hasLines(engineLog, byHand, 2), true);
	if (steamGame.started) {
		ASSERT_BOOL_EQ(tell(&steamGame.game, "unload", "unloaded"), true);
		ASSERT_INT_EQ(stopStandInGame(&steamGame.game), 0);
	}
	// With no game running, run on the export stops before its job, saying what to do: fmNNctl
	// does not start the game for a fixture that does not record its save.
	steamGame.started = false;
	spawned[0] = 0;
	ASSERT_BOOL_EQ(commandExits(macosTest_run, exportRun, 1, printed, sizeof printed), true);
	ASSERT_INT_EQ(strcmp(printed, UNRECORDED_NOT_RUNNING), 0);
	ASSERT_BOOL_EQ(!spawned[0] && !steamGame.started, true);
	// The harness found the engine in the game Steam started, with the game's version.
	(void)snprintf(path, sizeof path, "%s/harness.log", harness);
	const char *const logged[] = {
		"found " EDITION_GAME "'s engine (the game's version 27.0.0.1): /",
		"accepted: ready; waiting for jobs\n",
		"list_teams: 1 teams\n",
		"get_player: Ada Stand-in (31250)\n",
		"build_next: no upcoming fixture\n",
		"job finished: 6/6 runs",
	};
	ASSERT_BOOL_EQ(hasLines(path, logged, sizeof logged / sizeof logged[0]), true);
	endEndToEnd(previous, directory);
}
#endif

void registerMacosTests(void) {
	test_run("macos converts UTF-8 paths", test_converts_utf8_paths);
	test_run("macos normalises absolute paths", test_normalises_absolute_paths);
	test_run("macos translates frames into Windows frames", test_translates_frames_into_windows_frames);
	test_run(
		"macos builds sorted tables for every harness address",
		test_builds_sorted_tables_for_every_harness_address
	);
	test_run(
		"macos scans malloc memory whatever its share mode",
		test_scans_malloc_memory_whatever_its_share_mode
	);
	test_run("macos serves one job per connection", test_serves_one_job_per_connection);
	test_run("macos drops a runner that gave up", test_drops_a_runner_that_gave_up);
	test_run(
		"macos tells a stale socket from a busy harness", test_tells_a_stale_socket_from_a_busy_harness
	);
	test_run(
		"macos accepts only a supported build its tables match",
		test_accepts_only_a_supported_build_its_tables_match
	);
	test_run(
		"macos refuses jobs outside a supported game", test_refuses_jobs_outside_a_supported_game
	);
#if EDITION_ENGINE_PLUGIN
	test_run("macos holds the engine while a job uses it", test_holds_the_engine_while_a_job_uses_it);
	test_run(
		"macos finds the engine as the game loads and unloads it",
		test_finds_the_engine_as_the_game_loads_and_unloads_it
	);
	test_run(
		"macos plays matches with the engine's calls", test_plays_matches_with_the_engines_calls
	);
	test_run("macos waits for a load to settle", test_waits_for_a_load_to_settle);
	test_run(
		"macos serves the executable the game's Info.plist names",
		test_serves_the_executable_the_games_info_plist_names
	);
	test_run(
		"macos only logs the engine in log-only mode", test_only_logs_the_engine_in_log_only_mode
	);
#endif
	test_run(
		"macos finds another harness version serving", test_finds_another_harness_version_serving
	);
	test_run(
		"macos finds Football Manager in Steam libraries", test_finds_football_manager_in_steam_libraries
	);
	test_run("macos starts the game through the launcher", test_starts_the_game_through_the_launcher);
	test_run(
		"macos opens the game with the harness inserted", test_opens_the_game_with_the_harness_inserted
	);
	test_run(
		"macos waits until the started game keeps serving",
		test_waits_until_the_started_game_keeps_serving
	);
	test_run(
		"macos hands a fresh save request to the game once",
		test_hands_a_fresh_save_request_to_the_game_once
	);
	test_run(
		"macos starts the x86_64 slice for a fresh Rosetta request once",
		test_starts_the_x86_64_slice_for_a_fresh_rosetta_request_once
	);
#if EDITION_ENGINE_PLUGIN
	test_run(
		"macos starts the executable the app's Info.plist names",
		test_starts_the_executable_the_apps_info_plist_names
	);
#endif
	test_run("macos parses launch arguments", test_parses_launch_arguments);
	test_run("macos finds a save by name or file", test_finds_a_save_by_name_or_file);
	test_run(
		"macos writes autoplay files and removes those of exited games",
		test_writes_autoplay_files_and_removes_those_of_exited_games
	);
	test_run(
		"macos checks the save a running game has loaded", test_checks_the_save_a_running_game_has_loaded
	);
	test_run(
		"macos checks that the game runs the slice --rosetta asks for",
		test_checks_that_the_game_runs_the_slice_rosetta_asks_for
	);
	test_run(
		"macos checks the running game before a job", test_checks_the_running_game_before_a_job
	);
#if EDITION_ENGINE_PLUGIN
	test_run(
		"macos checks the running game before a job for an export",
		test_checks_the_running_game_before_a_job_for_an_export
	);
#endif
	test_run(
		"macos starts the game for a job and removes its auto-play files",
		test_starts_the_game_for_a_job_and_removes_its_auto_play_files
	);
	test_run(
		"macos leaves a Rosetta request only while Steam starts the game",
		test_leaves_a_rosetta_request_only_while_steam_starts_the_game
	);
	test_run(
		"macos checks a running game before looking its save up",
		test_checks_a_running_game_before_looking_its_save_up
	);
	test_run(
		"macos looks saves up in the data folder Steam gives the game",
		test_looks_saves_up_in_the_data_folder_steam_gives_the_game
	);
	test_run(
		"macos lists the saves launch finds for the viewer",
		test_lists_the_saves_launch_finds_for_the_viewer
	);
	test_run(
		"macos install says where it looked and what Steam has",
		test_install_says_where_it_looked_and_what_steam_has
	);
#if EDITION_ENGINE_PLUGIN
	test_run(
		"macos install --log-only has the harness only log",
		test_install_log_only_has_the_harness_only_log
	);
#endif
	test_run(
		"macos explains how to start the game with the harness",
		test_explains_how_to_start_the_game_with_the_harness
	);
	test_run(
		"macos says how to load the save --no-launch does not start",
		test_says_how_to_load_the_save_no_launch_does_not_start
	);
#if EDITION_ENGINE_PLUGIN
	test_run(
		"macos drives the stand-in game Steam starts through every command",
		test_drives_the_stand_in_game_steam_starts_through_every_command
	);
#endif
}
