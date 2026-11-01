// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The tables that tie the macOS build of the game to the Windows Steam build the harness was
// written against (generated into game_macos_tables.c by tools/macos/gen_tables.py; see
// docs/MACOS.md).
//
// Everything else in the harness names game code and data by Windows Steam RVAs (game.h, sites.h,
// site_members.c). On macOS, game_address() looks each such RVA up in `addresses`, and the write
// trace translates the return addresses of each macOS stack into the frames the Windows build would
// have had for the same write (`frames`), so labels, members and per-seed alignment work unchanged.
// The two compilers inline and tail-call differently, so one macOS frame may stand for several
// Windows frames (a call the macOS build inlined), or for none (a call Windows inlined or turned
// into a jump). A frame without an entry keeps its own identity: MACOS_FRAME_TAG | its macOS
// offset.
#pragma once

#include "edition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


enum {
	MACOS_FRAME_MAX = 4,      // Windows frames one macOS frame can stand for
	MACOS_SIGNATURE_MAX = 32, // bytes checked at the start of each function
};

#define MACOS_FRAME_TAG 0x80000000U // marks a macOS offset that has no Windows counterpart

// A game.h RVA_* constant (a Windows RVA) and its counterpart: an offset from the macOS image's
// Mach-O header.
typedef struct MacosAddress {
	uint32_t windows;
	uint32_t mac;
} MacosAddress;

// One macOS return address (an offset from the header) and the Windows frames it stands for, inner
// first; `count` 0 drops it. A nonzero `parent` restricts the entry to stacks whose next outer
// frame returns to that offset, and in a plugin edition a nonzero `grandparent` further to those
// whose frame after the parent returns to that one.
typedef struct MacosFrame {
	uint32_t mac;
	uint32_t parent;
	uint32_t count;
	uint32_t windows[MACOS_FRAME_MAX];
#if EDITION_ENGINE_PLUGIN
	uint32_t grandparent;
#endif
} MacosFrame;

// Bytes that start a game function the harness calls, checked before it is used.
typedef struct MacosSignature {
	uint32_t mac;
	uint32_t length;
	uint8_t bytes[MACOS_SIGNATURE_MAX];
} MacosSignature;

typedef struct MacosTables {
	const char *architecture;      // the slice these tables are for: "arm64" or "x86_64"
	const MacosAddress *addresses; // sorted by `windows`
	size_t addressCount;
	const MacosFrame *frames; // sorted by `mac`, then `parent` (then `grandparent`)
	size_t frameCount;
	const MacosSignature *signatures;
	size_t signatureCount;
} MacosTables;

// The tables for the architecture this file was compiled for, where an edition supports one build
// (game_macos_tables.c).
extern const MacosTables macosTables;

// The tables of one build, for the slice of the architecture this file was compiled for: an edition
// whose engine is a plugin carries the tables of every build of it that is mapped, and uses those
// of the build it finds (game_macos_tables_fmNN.c).
typedef struct MacosBuildTables {
	uint8_t uuid[16]; // the slice's LC_UUID
	const MacosTables *tables;
	uint16_t formatVersion; // the version its serialised matches carry (match_seedOffset)
} MacosBuildTables;

extern const MacosBuildTables macosBuildTables[];
extern const size_t macosBuildTableCount;

// The tables of the build of this architecture's slice with this UUID, or NULL.
const MacosTables *macosTables_forBuild(const uint8_t uuid[16]);

// The format version of that build's serialised matches, or 0.
uint16_t macosTables_formatVersion(const uint8_t uuid[16]);

// A game.h ID the harness resolves on macOS; `function` if it is code the harness calls or
// compares.
typedef struct MacosRequirement {
	uint32_t rva;
	bool function;
} MacosRequirement;

// Every ID the harness resolves on macOS. game_init refuses tables that lack one, or that lack a
// signature for a function among them.
extern const MacosRequirement macosRequirements[];
extern const size_t macosRequirementCount;

// The entry for a macOS return address (with its next outer frame's offset, or 0, and in a plugin
// edition the one after that), or NULL. The most specific entry that matches wins.
#if EDITION_ENGINE_PLUGIN
const MacosFrame *macosTables_findFrame(
	const MacosTables *tables,
	uint32_t mac,
	uint32_t parent,
	uint32_t grandparent
);
#else
const MacosFrame *macosTables_findFrame(const MacosTables *tables, uint32_t mac, uint32_t parent);
#endif

// The macOS offset for a game.h RVA, or 0 if the tables have none.
uint32_t macosTables_findAddress(const MacosTables *tables, uint32_t windows);

// Translates macOS frame offsets (0 for frames outside the game) into Windows-equivalent RVAs:
// at most `capacity` of them, inner first, as game_rva() gives them on Windows. Returns the count.
size_t macosTables_translate(
	const MacosTables *tables,
	const uint32_t *frames,
	size_t count,
	uint32_t *out,
	size_t capacity
);
