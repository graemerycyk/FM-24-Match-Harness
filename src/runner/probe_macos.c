// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// `fmNNctl probe` on macOS: which architecture slices of the game's executable the
// harness supports, by the slices' LC_UUIDs (macos_builds.h). It reads the file only.
// Where the game keeps its match engine in a plugin (EDITION_ENGINE_PLUGIN), it reads the plugin
// in the app's Contents/PlugIns instead, and prints the app's version, which a report of a build
// the harness does not support needs too. Every build macos_builds.h lists has build tables in
// the harness (the macos tests check it), so a supported slice is one the harness has tables for.
//
// Unlike the Windows probe it does not search another build for the harness's functions: the
// macOS tables come from comparing this build with the Windows one (docs/MACOS.md).

#include "buffer.h"
#include "commands.h"
#include "edition.h"
#include "files.h"
#if EDITION_ENGINE_PLUGIN
#include "info_plist.h"
#endif
#include "macos_builds.h"
#include "macos_game.h"
#include "platform.h"
#include "protocol.h"

#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdio.h>
#include <string.h>


enum { PATH_LENGTH = 4096 };

static bool executablePath(int argc, wchar_t **argv, char *out, size_t size);
#if EDITION_ENGINE_PLUGIN
static bool enginePath(const char *given, char *out, size_t size, char *app, size_t appSize);
static bool pluginOfApp(const char *app, char *out, size_t size);
static void reportApp(const char *app);
static char probedApp[PATH_LENGTH]; // the app executablePath found the engine in, or ""
#endif
static uint32_t bigEndian32(const uint8_t *bytes);
static bool readSlice(const ByteBuffer *file, size_t offset, size_t size, int *supported);
static bool reportSlice(const char *architecture, const uint8_t uuid[16]);
static const char *architectureName(cpu_type_t type);

int commands_probe(int argc, wchar_t **argv) {
	char path[PATH_LENGTH];
#if EDITION_ENGINE_PLUGIN
	probedApp[0] = 0;
	if (argc > 3 || (argc == 3 && !executablePath(argc, argv, path, sizeof path))) {
		fprintf(
			stderr, "usage: " EDITION_CLI " probe [path to fm.app, or its engine plugin or bundle]\n"
		);
		return 2;
	}
#else
	if (argc > 3 || (argc == 3 && !executablePath(argc, argv, path, sizeof path))) {
		fprintf(stderr, "usage: " EDITION_CLI " probe [path to fm.app or its executable]\n");
		return 2;
	}
#endif
	if (argc < 3 && !executablePath(argc, argv, path, sizeof path)) {
		fprintf(
			stderr,
			"cannot find " EDITION_GAME ": it is not running or in your Steam libraries; pass the "
			"path to its fm.app\n"
		);
		return 1;
	}
	wchar_t wide[HARNESS_PATH_LENGTH];
	ByteBuffer file = {0};
	if (!platform_fromUtf8(path, wide, HARNESS_PATH_LENGTH) || !files_read(wide, &file) ||
			file.length < sizeof(struct mach_header_64)) {
		fprintf(stderr, "cannot read %s\n", path);
		buffer_free(&file);
		return 1;
	}
#if EDITION_ENGINE_PLUGIN
	reportApp(probedApp);
#endif
	printf("%s\n", path);
	int supported = 0;
	bool ok = true;
	uint32_t magic = bigEndian32(file.data);
	if (magic == FAT_MAGIC) {
		uint32_t count = bigEndian32(file.data + 4);
		for (uint32_t i = 0; i < count && ok; ++i) {
			size_t entry = sizeof(struct fat_header) + i * sizeof(struct fat_arch);
			ok =
				entry + sizeof(struct fat_arch) <= file.length &&
				readSlice(
					&file, bigEndian32(file.data + entry + 8), bigEndian32(file.data + entry + 12), &supported
				);
		}
	} else {
		ok = readSlice(&file, 0, file.length, &supported);
	}
	buffer_free(&file);
	if (!ok) {
		fprintf(stderr, "not a Mach-O executable\n");
		return 1;
	}
#if EDITION_ENGINE_PLUGIN
	printf(
		supported
			? "harness %s supports %d slice(s) of this build\n"
			: "harness %s does not support this build: please report its UUIDs and the game's version\n",
		HARNESS_PROTOCOL_TEXT,
		supported
	);
#else
	printf(
		supported ? "harness %s supports %d slice(s) of this build\n"
							: "harness %s does not support this build: please report its UUIDs\n",
		HARNESS_PROTOCOL_TEXT,
		supported
	);
#endif
	return supported ? 0 : 1;
}

#if EDITION_ENGINE_PLUGIN
// The engine plugin of the app named on the command line (or of the app whose executable or
// plugin bundle is named), or the file named, else the running or installed game's. Notes the app
// in probedApp.
static bool executablePath(int argc, wchar_t **argv, char *out, size_t size) {
	char given[PATH_LENGTH];
	if (argc == 3) {
		if (!platform_toUtf8(argv[2], given, sizeof given)) {
			return false;
		}
		size_t length = strlen(given);
		while (length > 1 && given[length - 1] == '/') {
			given[--length] = 0;
		}
		return enginePath(given, out, size, probedApp, sizeof probedApp);
	}
	char running[PATH_LENGTH];
	if (macosGame_findRunning(running, sizeof running) &&
			macosGame_bundle(running, probedApp, sizeof probedApp)) {
		return pluginOfApp(probedApp, out, size);
	}
	return macosGame_findInstalled(probedApp, sizeof probedApp) && pluginOfApp(probedApp, out, size);
}

// The engine plugin `given` stands for, and the app it is in (or "").
static bool enginePath(const char *given, char *out, size_t size, char *app, size_t appSize) {
	static const char plugins[] = "/Contents/PlugIns/";
	const char *inBundle = strchr(EDITION_ENGINE_PATH, '/'); // after the plugin bundle's name
	size_t bundleName = (size_t)(inBundle - EDITION_ENGINE_PATH);
	size_t length = strlen(given);
	app[0] = 0;
	if (length > 4 && strcmp(given + length - 4, ".app") == 0) {
		int written = snprintf(app, appSize, "%s", given);
		return written > 0 && (size_t)written < appSize && pluginOfApp(app, out, size);
	}
	if (macosGame_bundle(given, app, appSize)) { // the app's executable
		return pluginOfApp(app, out, size);
	}
	app[0] = 0;
	bool bundle = length > bundleName && given[length - bundleName - 1] == '/' &&
								strncmp(given + length - bundleName, EDITION_ENGINE_PATH, bundleName) == 0;
	int written = snprintf(out, size, "%s%s", given, bundle ? inBundle : "");
	if (written <= 0 || (size_t)written >= size) {
		return false;
	}
	// In an app, the plugin is <app>/Contents/PlugIns/<EDITION_ENGINE_PATH>.
	size_t suffix = sizeof plugins - 1 + strlen(EDITION_ENGINE_PATH);
	size_t total = (size_t)written;
	if (total > suffix && strncmp(out + total - suffix, plugins, sizeof plugins - 1) == 0 &&
			strcmp(out + total - suffix + sizeof plugins - 1, EDITION_ENGINE_PATH) == 0) {
		written = snprintf(app, appSize, "%.*s", (int)(total - suffix), out);
		if (written <= 0 || (size_t)written >= appSize) {
			app[0] = 0;
		}
	}
	return true;
}

static bool pluginOfApp(const char *app, char *out, size_t size) {
	int written = snprintf(out, size, "%s/Contents/PlugIns/" EDITION_ENGINE_PATH, app);
	return written > 0 && (size_t)written < size;
}

// The app the engine is in, with its version, if it is known.
static void reportApp(const char *app) {
	char version[128];
	if (!app[0]) {
		return;
	}
	if (!infoPlist_read(app, "CFBundleShortVersionString", version, sizeof version)) {
		(void)snprintf(version, sizeof version, "(unknown version)");
	}
	printf(EDITION_GAME " %s: %s\n", version, app);
}
#else
// The executable named on the command line (an fm.app or the file itself), else the running or
// installed game's.
static bool executablePath(int argc, wchar_t **argv, char *out, size_t size) {
	char given[PATH_LENGTH];
	if (argc == 3) {
		if (!platform_toUtf8(argv[2], given, sizeof given)) {
			return false;
		}
		size_t length = strlen(given);
		while (length > 1 && given[length - 1] == '/') {
			given[--length] = 0;
		}
		bool bundle = length > 4 && strcmp(given + length - 4, ".app") == 0;
		int written =
			snprintf(out, size, bundle ? "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE : "%s", given);
		return written > 0 && (size_t)written < size;
	}
	if (macosGame_findRunning(out, size)) {
		return true;
	}
	char app[PATH_LENGTH];
	int written = macosGame_findInstalled(app, sizeof app)
									? snprintf(out, size, "%s/Contents/MacOS/" EDITION_GAME_EXECUTABLE, app)
									: -1;
	return written > 0 && (size_t)written < size;
}
#endif

static uint32_t bigEndian32(const uint8_t *bytes) {
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

// Prints one slice's architecture, UUID and whether it is supported.
static bool readSlice(const ByteBuffer *file, size_t offset, size_t size, int *supported) {
	if (offset > file->length || size > file->length - offset ||
			size < sizeof(struct mach_header_64)) {
		return false;
	}
	struct mach_header_64 header;
	memcpy(&header, file->data + offset, sizeof header);
	if (header.magic != MH_MAGIC_64) {
		return false;
	}
	const char *architecture = architectureName(header.cputype);
	size_t command = offset + sizeof header;
	size_t end = command + header.sizeofcmds;
	if (end > offset + size) {
		return false;
	}
	for (uint32_t i = 0; i < header.ncmds && command + sizeof(struct load_command) <= end; ++i) {
		struct load_command load;
		memcpy(&load, file->data + command, sizeof load);
		if (load.cmdsize < sizeof load || command + load.cmdsize > end) {
			return false;
		}
		if (load.cmd == LC_UUID && load.cmdsize >= sizeof(struct uuid_command)) {
			struct uuid_command uuid;
			memcpy(&uuid, file->data + command, sizeof uuid);
			*supported += reportSlice(architecture, uuid.uuid);
			return true;
		}
		command += load.cmdsize;
	}
	return false;
}

// Prints a slice's architecture and UUID and whether the harness supports it; true if it does.
static bool reportSlice(const char *architecture, const uint8_t uuid[16]) {
	const MacosBuild *build = macosBuilds_find(architecture, uuid);
	printf("  %-7s ", architecture);
	for (int b = 0; b < 16; ++b) {
		printf(b == 4 || b == 6 || b == 8 || b == 10 ? "-%02X" : "%02X", uuid[b]);
	}
	printf(build ? "  supported: %s\n" : "  not supported\n", build ? build->version : "");
	return build != NULL;
}

static const char *architectureName(cpu_type_t type) {
	switch (type) {
		case CPU_TYPE_ARM64: return "arm64";
		case CPU_TYPE_X86_64: return "x86_64";
		default: return "other";
	}
}
