// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fixture_export.h"
#include "test.h"
#include "tests.h"

#include <stdio.h>
#include <string.h>


TEST(builds_export_filenames) {
	FixtureData data = {0};
	wchar_t filename[FIXTURE_EXPORT_FILENAME_LENGTH] = {0};
	(void)snprintf(data.teams[0].name, FIXTURE_TEXT, "Aston Villa");
	(void)snprintf(data.teams[1].name, FIXTURE_TEXT, "Bristol City");
	ASSERT_BOOL_EQ(
		fixtureExport_filename(&data, 1792074287, filename, FIXTURE_EXPORT_FILENAME_LENGTH), true
	);
	ASSERT_INT_EQ(wcscmp(filename, L"aston-villa-v-bristol-city-1792074287.json"), 0);
}

TEST(sanitises_export_filenames) {
	FixtureData data = {0};
	wchar_t filename[FIXTURE_EXPORT_FILENAME_LENGTH] = {0};
	(void)snprintf(data.teams[0].name, FIXTURE_TEXT, "M\xc3\xbcnchen\t\r\n\x01\x7f/\\:*?\"<>|");
	(void)snprintf(data.teams[1].name, FIXTURE_TEXT, "FC  CITY-1.2&'()");
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 42, filename, FIXTURE_EXPORT_FILENAME_LENGTH), true);
	ASSERT_INT_EQ(wcscmp(filename, L"mnchen-v-fc--city-1.2&'()-42.json"), 0);
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 42, filename, 8), false);
}

TEST(handles_empty_team_names) {
	FixtureData data = {0};
	wchar_t filename[FIXTURE_EXPORT_FILENAME_LENGTH] = {0};
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 0, filename, FIXTURE_EXPORT_FILENAME_LENGTH), true);
	ASSERT_INT_EQ(wcscmp(filename, L"-v--0.json"), 0);
}

TEST(enforces_export_filename_length) {
	FixtureData data = {0};
	wchar_t filename[FIXTURE_EXPORT_FILENAME_LENGTH] = {0};
	memset(data.teams[0].name, 'A', FIXTURE_TEXT - 1);
	ASSERT_BOOL_EQ(
		fixtureExport_filename(&data, 42, filename, FIXTURE_EXPORT_FILENAME_LENGTH), false
	);
	size_t homeLength = 255 - wcslen(L"-v--42.json");
	data.teams[0].name[homeLength] = 0;
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 42, filename, FIXTURE_EXPORT_FILENAME_LENGTH), true);
	ASSERT_BOOL_EQ(wcslen(filename) == 255, true);
	data.teams[0].name[homeLength] = 'A';
	data.teams[0].name[homeLength + 1] = 0;
	wchar_t longer[512];
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 42, longer, 512), false);
}

TEST(builds_utf8_export_filenames) {
	FixtureData data = {0};
	char filename[FIXTURE_EXPORT_FILENAME_LENGTH] = "";
	(void)snprintf(data.teams[0].name, FIXTURE_TEXT, "M\xc3\xbcnchen\t\r\n\x01\x7f/\\:*?\"<>|");
	(void)snprintf(data.teams[1].name, FIXTURE_TEXT, "FC  CITY-1.2&'()");
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 42, ".json", filename, sizeof filename), true);
	ASSERT_INT_EQ(strcmp(filename, "mnchen-v-fc--city-1.2&'()-42.json"), 0);
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 42, ".csv", filename, sizeof filename), true);
	ASSERT_INT_EQ(strcmp(filename, "mnchen-v-fc--city-1.2&'()-42.csv"), 0);
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 42, ".json", filename, 8), false);
	ASSERT_INT_EQ(filename[0], 0);
	memset(&data, 0, sizeof data);
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 0, ".json", filename, sizeof filename), true);
	ASSERT_INT_EQ(strcmp(filename, "-v--0.json"), 0);
	// The same names, and the same 255-character limit, as the wide version.
	size_t homeLength = 255 - strlen("-v--42.json");
	memset(data.teams[0].name, 'A', homeLength);
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 42, ".json", filename, sizeof filename), true);
	ASSERT_BOOL_EQ(strlen(filename) == 255, true);
	wchar_t wide[FIXTURE_EXPORT_FILENAME_LENGTH];
	ASSERT_BOOL_EQ(fixtureExport_filename(&data, 42, wide, FIXTURE_EXPORT_FILENAME_LENGTH), true);
	for (size_t i = 0; i <= 255; ++i) {
		ASSERT_BOOL_EQ(wide[i] == (wchar_t)filename[i], true);
	}
	data.teams[0].name[homeLength] = 'A';
	char longer[512];
	ASSERT_BOOL_EQ(fixtureExport_filenameUtf8(&data, 42, ".json", longer, sizeof longer), false);
}

void registerFixtureExportTests(void) {
	test_run("fixture export builds team-based filenames", test_builds_export_filenames);
	test_run("fixture export sanitises team names", test_sanitises_export_filenames);
	test_run("fixture export handles empty team names", test_handles_empty_team_names);
	test_run(
		"fixture export enforces the filename length limit", test_enforces_export_filename_length
	);
	test_run("fixture export builds UTF-8 filenames", test_builds_utf8_export_filenames);
}
