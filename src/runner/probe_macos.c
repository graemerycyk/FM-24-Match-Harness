// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// `fm24ctl probe` on macOS: which architecture slices of a Football Manager 2024 executable the
// harness supports, by the slices' LC_UUIDs (macos_builds.h). It reads the file only.
//
// Unlike the Windows probe it does not search another build for the harness's functions: the
// macOS tables come from comparing this build with the Windows one (docs/MACOS.md).

#include "buffer.h"
#include "commands.h"
#include "files.h"
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
static uint32_t bigEndian32(const uint8_t *bytes);
static bool readSlice(const ByteBuffer *file, size_t offset, size_t size, int *supported);
static bool reportSlice(const char *architecture, const uint8_t uuid[16]);
static const char *architectureName(cpu_type_t type);

int commands_probe(int argc, wchar_t **argv) {
	char path[PATH_LENGTH];
	if (argc > 3 || (argc == 3 && !executablePath(argc, argv, path, sizeof path))) {
		fprintf(stderr, "usage: fm24ctl probe [path to fm.app or its executable]\n");
		return 2;
	}
	if (argc < 3 && !executablePath(argc, argv, path, sizeof path)) {
		fprintf(
			stderr,
			"cannot find Football Manager 2024: it is not running or in your Steam libraries; pass the "
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
	printf(
		supported ? "harness %s supports %d slice(s) of this build\n"
							: "harness %s does not support this build: please report its UUIDs\n",
		HARNESS_PROTOCOL_TEXT,
		supported
	);
	return supported ? 0 : 1;
}

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
		int written = snprintf(out, size, bundle ? "%s/Contents/MacOS/fm" : "%s", given);
		return written > 0 && (size_t)written < size;
	}
	if (macosGame_findRunning(out, size)) {
		return true;
	}
	char app[PATH_LENGTH];
	int written = macosGame_findInstalled(app, sizeof app)
									? snprintf(out, size, "%s/Contents/MacOS/fm", app)
									: -1;
	return written > 0 && (size_t)written < size;
}

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
