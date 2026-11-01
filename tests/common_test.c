// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files.h"
#include "platform.h"
#include "sha256.h"
#include "test.h"
#include "tests.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif


enum { TEST_PATH_LENGTH = 1024 };

// The hex digest of `size` bytes added in pieces of `piece` bytes.
static void hashInPieces(const uint8_t *bytes, size_t size, size_t piece, char *hex) {
	Sha256 hash;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_start(&hash);
	for (size_t at = 0; at < size; at += piece) {
		sha256_add(&hash, bytes + at, size - at < piece ? size - at : piece);
	}
	sha256_finish(&hash, digest);
	sha256_hex(digest, hex);
}

TEST(hashes_the_nist_vectors) {
	static const struct {
		const char *text;
		const char *digest;
	} vectors[] = {
		{"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
		{"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
		{"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
			"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
		{"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqr"
		 "lmnopqrsmnopqrstnopqrstu",
			"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
	};
	char hex[SHA256_HEX_LENGTH];
	for (size_t i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
		sha256_hexOf(vectors[i].text, strlen(vectors[i].text), hex);
		ASSERT_INT_EQ(strcmp(hex, vectors[i].digest), 0);
		for (size_t piece = 1; piece < 4; ++piece) {
			hashInPieces((const uint8_t *)vectors[i].text, strlen(vectors[i].text), piece, hex);
			ASSERT_INT_EQ(strcmp(hex, vectors[i].digest), 0);
		}
	}
	// One million "a", in one go and in pieces that cross every block boundary differently.
	enum { MILLION = 1000000 };
	uint8_t *many = malloc(MILLION);
	ASSERT_BOOL_EQ(many != NULL, true);
	if (!many) {
		return;
	}
	memset(many, 'a', MILLION);
	static const char millionA[] = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
	static const size_t pieces[] = {MILLION, 997, 64, 63, 65};
	for (size_t i = 0; i < sizeof pieces / sizeof pieces[0]; ++i) {
		hashInPieces(many, MILLION, pieces[i], hex);
		ASSERT_INT_EQ(strcmp(hex, millionA), 0);
	}
	free(many);
}

// The lengths around the padding's limits (55 and 56 bytes in a block), from Python's hashlib.
TEST(hashes_around_the_padding_boundaries) {
	static const struct {
		size_t length;
		const char *digest;
	} cases[] = {
		{55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
		{56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
		{57, "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
		{63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
		{64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
		{65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
		{119, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e"},
		{120, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5"},
	};
	uint8_t bytes[128];
	for (size_t i = 0; i < sizeof bytes; ++i) {
		bytes[i] = (uint8_t)(i * 7 + 3);
	}
	char hex[SHA256_HEX_LENGTH];
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
		sha256_hexOf(bytes, cases[i].length, hex);
		ASSERT_INT_EQ(strcmp(hex, cases[i].digest), 0);
		hashInPieces(bytes, cases[i].length, 9, hex);
		ASSERT_INT_EQ(strcmp(hex, cases[i].digest), 0);
	}
}

// A new, empty folder for one test, in the temporary directory.
static bool makeTestFolder(wchar_t *out) {
#ifdef _WIN32
	wchar_t directory[MAX_PATH + 1];
	DWORD length = GetTempPathW(MAX_PATH + 1, directory);
	if (!length || length > MAX_PATH) {
		return false;
	}
	unsigned long long now = platform_ticks();
	int written = swprintf(
		out, TEST_PATH_LENGTH, L"%lsfm24-common-%lu-%llu", directory, platform_processId(), now
	);
	return written > 0 && written < TEST_PATH_LENGTH && platform_makeDirectory(out);
#else
	char made[] = "/tmp/fm24-common-XXXXXX";
	return mkdtemp(made) && platform_fromUtf8(made, out, TEST_PATH_LENGTH);
#endif
}

// Not with swprintf, which fails on characters the C locale cannot encode on macOS.
static void joinPath(wchar_t *out, const wchar_t *folder, const wchar_t *name) {
	size_t length = wcslen(folder);
	wmemcpy(out, folder, length);
	out[length] = PLATFORM_PATH_SEPARATOR;
	wcscpy(out + length + 1, name);
}

static bool writeText(const wchar_t *path, const char *text) {
	FILE *file = platform_openFile(path, "wb");
	if (!file) {
		return false;
	}
	bool ok = fputs(text, file) >= 0 && platform_syncFile(file);
	return fclose(file) == 0 && ok;
}

static bool readsAs(const wchar_t *path, const char *text) {
	char read[64] = "";
	FILE *file = platform_openFile(path, "rb");
	if (!file) {
		return false;
	}
	size_t length = fread(read, 1, sizeof read - 1, file);
	(void)fclose(file);
	return length == strlen(text) && memcmp(read, text, length) == 0;
}

typedef struct Listing {
	unsigned entries;
	unsigned stopAfter; // 0: never stop
	unsigned files;     // f.txt and the one with a non-ASCII name
	unsigned folders;   // sub
	unsigned other;
} Listing;

static bool listEntry(void *context, const wchar_t *name, bool isDirectory) {
	Listing *listing = context;
	++listing->entries;
	if (!isDirectory &&
			(wcscmp(name, L"f.txt") == 0 || wcscmp(name, L"\u00D8degaard \u20AC.txt") == 0)) {
		++listing->files;
	} else if (isDirectory && wcscmp(name, L"sub") == 0) {
		++listing->folders;
	} else {
		++listing->other;
	}
	return listing->entries != listing->stopAfter;
}

TEST(makes_lists_syncs_and_removes_folders) {
	wchar_t root[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(root), true);
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t sub[TEST_PATH_LENGTH];
	wchar_t file[TEST_PATH_LENGTH];
	wchar_t named[TEST_PATH_LENGTH];
	wchar_t missing[TEST_PATH_LENGTH];
	joinPath(folder, root, L"a");
	joinPath(sub, folder, L"sub");
	joinPath(file, folder, L"f.txt");
	joinPath(named, folder, L"\u00D8degaard \u20AC.txt");
	joinPath(missing, root, L"missing");
	ASSERT_BOOL_EQ(platform_makeDirectory(folder), true);
	ASSERT_BOOL_EQ(platform_makeDirectory(folder), true); // already there
	ASSERT_BOOL_EQ(platform_makeDirectory(sub), true);
	bool written = writeText(file, "first") && writeText(named, "other");
	ASSERT_BOOL_EQ(written, true);
	if (!written) {
		return;
	}
	ASSERT_BOOL_EQ(platform_makeDirectory(file), false); // a file is in the way
	Listing listing = {0};
	ASSERT_BOOL_EQ(platform_listDirectory(folder, listEntry, &listing), true);
	ASSERT_BOOL_EQ(listing.entries == 3 && listing.files == 2 && listing.folders == 1, true);
	listing = (Listing){.stopAfter = 2};
	ASSERT_BOOL_EQ(platform_listDirectory(folder, listEntry, &listing), false);
	ASSERT_INT_EQ((int)listing.entries, 2);
	listing = (Listing){0};
	ASSERT_BOOL_EQ(platform_listDirectory(missing, listEntry, &listing), false);
	ASSERT_INT_EQ((int)listing.entries, 0);
	ASSERT_BOOL_EQ(platform_syncDirectory(folder), true);
#ifndef _WIN32
	ASSERT_BOOL_EQ(platform_syncDirectory(missing), false); // Windows has nothing to sync
#endif
	uint64_t space = 0;
	ASSERT_BOOL_EQ(platform_freeBytes(root, &space) && space > 0, true);
	ASSERT_BOOL_EQ(platform_freeBytes(missing, &space), false);

	// A rename replaces the file in its way, and a missing file renames to nothing.
	ASSERT_BOOL_EQ(platform_replaceFile(named, file), true);
	ASSERT_BOOL_EQ(readsAs(file, "other"), true);
	ASSERT_BOOL_EQ(platform_replaceFile(named, file), false);
	ASSERT_BOOL_EQ(readsAs(file, "other"), true);
	// Folders rename too, to a new name.
	wchar_t renamed[TEST_PATH_LENGTH];
	joinPath(renamed, folder, L"sub2");
	ASSERT_BOOL_EQ(platform_replaceFile(sub, renamed), true);
	listing = (Listing){0};
	ASSERT_BOOL_EQ(platform_listDirectory(folder, listEntry, &listing), true);
	ASSERT_BOOL_EQ(listing.entries == 2 && listing.files == 1 && listing.other == 1, true);

	// Only empty folders go, and a missing one is gone already.
	ASSERT_BOOL_EQ(platform_removeDirectory(folder), false);
	ASSERT_BOOL_EQ(platform_removeDirectory(renamed), true);
	ASSERT_BOOL_EQ(platform_removeDirectory(renamed), true);
	ASSERT_BOOL_EQ(platform_removeFile(file), true);
	ASSERT_BOOL_EQ(platform_removeDirectory(folder), true);
	ASSERT_BOOL_EQ(platform_removeDirectory(root), true);
}

TEST(locks_a_file_for_one_holder_at_a_time) {
	wchar_t root[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(root), true);
	wchar_t path[TEST_PATH_LENGTH];
	joinPath(path, root, L"batch.lock");
	PlatformFileLock lock;
	PlatformFileLock other;
	ASSERT_INT_EQ(platform_lockFile(path, &lock), PLATFORM_LOCK_TAKEN);
	ASSERT_INT_EQ(platform_lockFile(path, &other), PLATFORM_LOCK_HELD);
	platform_unlockFile(lock);
	ASSERT_INT_EQ(platform_lockFile(path, &other), PLATFORM_LOCK_TAKEN);
	platform_unlockFile(other);
	wchar_t missing[TEST_PATH_LENGTH];
	joinPath(missing, root, L"missing");
	joinPath(path, missing, L"batch.lock");
	ASSERT_INT_EQ(platform_lockFile(path, &lock), PLATFORM_LOCK_FAILED);
	joinPath(path, root, L"batch.lock");
	ASSERT_BOOL_EQ(platform_removeFile(path) && platform_removeDirectory(root), true);
}

// A manifest is written whole or not at all, with no temporary file left beside it either way.
TEST(replaces_a_file_in_one_step) {
	wchar_t root[TEST_PATH_LENGTH];
	ASSERT_BOOL_EQ(makeTestFolder(root), true);
	wchar_t path[TEST_PATH_LENGTH];
	wchar_t named[TEST_PATH_LENGTH];
	wchar_t folder[TEST_PATH_LENGTH];
	wchar_t missing[TEST_PATH_LENGTH];
	joinPath(path, root, L"f.txt");
	joinPath(named, root, L"\u00D8degaard \u20AC.txt");
	joinPath(folder, root, L"sub");
	joinPath(missing, folder, L"f.txt");
	ASSERT_BOOL_EQ(files_replace(path, "first", 5), true);
	ASSERT_BOOL_EQ(readsAs(path, "first"), true);
	ASSERT_BOOL_EQ(files_replace(path, "other", 5), true);
	ASSERT_BOOL_EQ(readsAs(path, "other"), true);
	ASSERT_BOOL_EQ(files_replace(named, "", 0), true);
	ASSERT_BOOL_EQ(readsAs(named, ""), true);
	// A missing folder, and a folder where the file would go.
	ASSERT_BOOL_EQ(files_replace(missing, "first", 5), false);
	ASSERT_BOOL_EQ(platform_makeDirectory(folder), true);
	ASSERT_BOOL_EQ(files_replace(folder, "first", 5), false);
	Listing listing = {0};
	ASSERT_BOOL_EQ(platform_listDirectory(root, listEntry, &listing), true);
	ASSERT_BOOL_EQ(listing.entries == 3 && listing.files == 2 && listing.folders == 1, true);
	ASSERT_BOOL_EQ(readsAs(path, "other"), true);
	bool removed = platform_removeFile(path) && platform_removeFile(named) &&
								 platform_removeDirectory(folder) && platform_removeDirectory(root);
	ASSERT_BOOL_EQ(removed, true);
}

TEST(tells_the_time_in_utc_and_without_sleep) {
	struct tm utc;
	ASSERT_BOOL_EQ(platform_utcTime(1761384198, &utc), true);
	ASSERT_BOOL_EQ(
		utc.tm_year == 125 && utc.tm_mon == 9 && utc.tm_mday == 25 && utc.tm_hour == 9 &&
			utc.tm_min == 23 && utc.tm_sec == 18,
		true
	);
	ASSERT_BOOL_EQ(platform_utcTime(951782400, &utc), true); // 2000-02-29
	ASSERT_BOOL_EQ(utc.tm_year == 100 && utc.tm_mon == 1 && utc.tm_mday == 29 && !utc.tm_hour, true);
	// The uptime clock runs at platform_tickFrequency, as the monotonic one does while awake. On
	// Windows it moves only at each clock interrupt, every 15.6 ms by default, so either reading can
	// be that far behind.
	uint64_t frequency = platform_tickFrequency();
#ifdef _WIN32
	uint64_t slack = frequency / 40;
#else
	uint64_t slack = frequency / 100;
#endif
	uint64_t ticks = platform_ticks();
	uint64_t uptime = platform_uptimeTicks();
	platform_sleep(50);
	uint64_t ranUptime = platform_uptimeTicks() - uptime;
	uint64_t ranTicks = platform_ticks() - ticks;
	ASSERT_BOOL_EQ(ranUptime + slack >= frequency / 20 && ranUptime <= ranTicks + slack, true);
}

void registerCommonTests(void) {
	test_run("common hashes the NIST vectors with SHA-256", test_hashes_the_nist_vectors);
	test_run(
		"common hashes around the padding boundaries", test_hashes_around_the_padding_boundaries
	);
	test_run(
		"common makes, lists, syncs and removes folders", test_makes_lists_syncs_and_removes_folders
	);
	test_run(
		"common locks a file for one holder at a time", test_locks_a_file_for_one_holder_at_a_time
	);
	test_run(
		"common tells the time in UTC and without sleep", test_tells_the_time_in_utc_and_without_sleep
	);
	test_run("common replaces a file in one step", test_replaces_a_file_in_one_step);
}
