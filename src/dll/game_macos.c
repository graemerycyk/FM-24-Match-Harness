// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// game.h on macOS: the game is the main executable, identified by the LC_UUID of the running slice
// (macos_builds.h), and the harness's Windows RVAs are translated through game_macos.h's tables.

#include "game_macos.h"

#include "game.h"
#include "log.h"
#include "macos_builds.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdio.h>
#include <string.h>


const MacosRequirement macosRequirements[] = {
	{RVA_GAME_MATCH_CREATE, true},
	{RVA_GAME_MATCH_DESERIALIZE, true},
	{RVA_GAME_MATCH_PREPARE, true},
	{RVA_GAME_MATCH_RUN, true},
	{RVA_GAME_MATCH_SERIALIZE, true},
	{RVA_DATA_BUFFER_CTOR, true},
	{RVA_ALLOC_FROM_POOL, true},
	{RVA_MATCH_CTOR, true},
	{RVA_FIXTURE_PREPARE_TEAMS, true},
	{RVA_INIT_FROM_FIXTURE, true},
	{RVA_GAME_MATCH_DTOR, true},
	{RVA_GAME_MATCH_POOL_FREE, true},
	{RVA_MACOS_THREAD_RNG, true},
	{RVA_MACOS_PERSON_FORENAME, true},
	{RVA_GAME_MATCH_VTABLE, false},
	{RVA_HUMAN_NON_PLAYER_VTABLE, false},
	{RVA_FIXTURE_VTABLE, false},
	{RVA_TEAM_VTABLE, false},
	{RVA_PEOPLE_LIST, false},
	{RVA_CURRENT_DATE, false},
	{RVA_HUMAN_MATCHES_HOLDER, false},
	{RVA_MACOS_GAME, false},
	{RVA_MACOS_SAVE_SYSTEM, false},
	{RVA_MACOS_GAME_OPTIONS, false},
	{RVA_MACOS_START_SCREEN, false},
};
const size_t macosRequirementCount = sizeof macosRequirements / sizeof macosRequirements[0];

static uintptr_t base;      // the game's Mach-O header
static uintptr_t imageSpan; // bytes from the header to the end of its last segment
static const MacosBuild *build;

static const struct mach_header_64 *findGame(void);
static bool checkImage(const struct mach_header_64 *header);
static bool readIdentity(const struct mach_header_64 *header, uint8_t uuid[16]);
static const char *architectureOf(const struct mach_header_64 *header);
static bool checkTables(void);
static bool hasSignature(uint32_t mac);
static uint32_t macOffset(const void *address);

bool game_init(void) {
	const struct mach_header_64 *header = findGame();
	if (!header) {
		log_message("refusing: cannot find the game's executable image");
		return false;
	}
	return checkImage(header);
}

// Accepts the game image at `header` if it is a supported build and the tables match it.
static bool checkImage(const struct mach_header_64 *header) {
	uint8_t uuid[16] = {0};
	const char *architecture = architectureOf(header);
	if (!readIdentity(header, uuid)) {
		log_message("refusing: the game's image has no LC_UUID");
		return false;
	}
	char text[40];
	(void)snprintf(
		text,
		sizeof text,
		"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		uuid[0],
		uuid[1],
		uuid[2],
		uuid[3],
		uuid[4],
		uuid[5],
		uuid[6],
		uuid[7],
		uuid[8],
		uuid[9],
		uuid[10],
		uuid[11],
		uuid[12],
		uuid[13],
		uuid[14],
		uuid[15]
	);
	build = macosBuilds_find(architecture, uuid);
	if (!build || strcmp(macosTables.architecture, architecture) != 0) {
		log_message("refusing: unsupported game build (%s, UUID %s)", architecture, text);
		build = NULL;
		return false;
	}
	base = (uintptr_t)header;
	if (!checkTables()) {
		return false;
	}
	log_message(
		"game build: Football Manager 2024 %s, %s (UUID %s)", build->version, architecture, text
	);
	return true;
}

uintptr_t game_base(void) {
	return base;
}

uint32_t game_timestamp(void) {
	return build ? build->bundleVersion : 0;
}

void *game_address(uint32_t rva) {
	uint32_t offset = macosTables_findAddress(&macosTables, rva);
	if (!offset) {
		log_message("no macOS address for the Windows RVA %x", rva);
		return NULL;
	}
	return (void *)(base + offset); // NOLINT(performance-no-int-to-ptr)
}

VoidFn game_functionAt(const void *address) {
	VoidFn function = NULL;
	memcpy(&function, &address, sizeof function);
	return function;
}

VoidFn game_function(uint32_t rva) {
	return game_functionAt(game_address(rva));
}

uint32_t game_rva(const void *address) {
	uint32_t offset = macOffset(address);
	uint32_t rva = 0;
	return offset && macosTables_translate(&macosTables, &offset, 1, &rva, 1) ? rva : 0;
}

size_t game_translateFrames(void *const *frames, size_t count, uint32_t *out, size_t capacity) {
	uint32_t offsets[64];
	if (count > sizeof offsets / sizeof offsets[0]) {
		count = sizeof offsets / sizeof offsets[0];
	}
	for (size_t i = 0; i < count; ++i) {
		offsets[i] = macOffset(frames[i]);
	}
	return macosTables_translate(&macosTables, offsets, count, out, capacity);
}

uint8_t *game_initThread(void) {
	// Mach-O thread-locals start one by one on first use, so there is nothing to run for the whole
	// image. The game reaches its PCG32 through an accessor that seeds it on a thread's first call;
	// calling it first means the harness's seed is not overwritten by that seeding later.
	typedef uint8_t *(*ThreadRngFn)(void);
	return ((ThreadRngFn)game_function(RVA_MACOS_THREAD_RNG))();
}

const MacosFrame *macosTables_findFrame(const MacosTables *tables, uint32_t mac, uint32_t parent) {
	size_t low = 0;
	size_t high = tables->frameCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (tables->frames[middle].mac < mac) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	const MacosFrame *general = NULL;
	for (size_t i = low; i < tables->frameCount && tables->frames[i].mac == mac; ++i) {
		const MacosFrame *frame = &tables->frames[i];
		if (frame->parent == 0) {
			general = frame;
		} else if (frame->parent == parent) {
			return frame;
		}
	}
	return general;
}

uint32_t macosTables_findAddress(const MacosTables *tables, uint32_t windows) {
	size_t low = 0;
	size_t high = tables->addressCount;
	while (low < high) {
		size_t middle = low + (high - low) / 2;
		if (tables->addresses[middle].windows < windows) {
			low = middle + 1;
		} else {
			high = middle;
		}
	}
	return low < tables->addressCount && tables->addresses[low].windows == windows
					 ? tables->addresses[low].mac
					 : 0;
}

size_t macosTables_translate(
	const MacosTables *tables,
	const uint32_t *frames,
	size_t count,
	uint32_t *out,
	size_t capacity
) {
	size_t written = 0;
	for (size_t i = 0; i < count && written < capacity; ++i) {
		if (!frames[i]) {
			out[written++] = 0; // outside the game, as game_rva gives it on Windows
			continue;
		}
		const MacosFrame *frame =
			macosTables_findFrame(tables, frames[i], i + 1 < count ? frames[i + 1] : 0);
		if (!frame) {
			out[written++] = MACOS_FRAME_TAG | frames[i];
			continue;
		}
		for (uint32_t k = 0; k < frame->count && written < capacity; ++k) {
			out[written++] = frame->windows[k];
		}
	}
	return written;
}

// The main executable, if it is a Football Manager app bundle's Contents/MacOS/fm.
static const struct mach_header_64 *findGame(void) {
	static const char suffix[] = "/Contents/MacOS/fm";
	for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
		const struct mach_header *header = _dyld_get_image_header(i);
		const char *name = _dyld_get_image_name(i);
		size_t length = name ? strlen(name) : 0;
		if (header && header->filetype == MH_EXECUTE && header->magic == MH_MAGIC_64 &&
				length >= sizeof suffix - 1 && strcmp(name + length - (sizeof suffix - 1), suffix) == 0) {
			return (const struct mach_header_64 *)header;
		}
	}
	return NULL;
}

// Reads the slice's UUID and the extent of its segments.
static bool readIdentity(const struct mach_header_64 *header, uint8_t uuid[16]) {
	const uint8_t *command = (const uint8_t *)(header + 1);
	uint64_t textAddress = 0;
	uint64_t end = 0;
	bool found = false;
	for (uint32_t i = 0; i < header->ncmds; ++i) {
		const struct load_command *load = (const struct load_command *)command;
		if (load->cmd == LC_UUID) {
			memcpy(uuid, ((const struct uuid_command *)command)->uuid, 16);
			found = true;
		} else if (load->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
			if (strcmp(segment->segname, SEG_TEXT) == 0) {
				textAddress = segment->vmaddr;
			}
			if (segment->vmsize && segment->vmaddr + segment->vmsize > end) {
				end = segment->vmaddr + segment->vmsize;
			}
		}
		command += load->cmdsize;
	}
	imageSpan = end > textAddress ? (uintptr_t)(end - textAddress) : 0;
	return found;
}

static const char *architectureOf(const struct mach_header_64 *header) {
	switch (header->cputype) {
		case CPU_TYPE_ARM64: return "arm64";
		case CPU_TYPE_X86_64: return "x86_64";
		default: return "other";
	}
}

// The tables must name everything the harness uses, every address must lie inside the image, and
// every function the harness calls must start with the bytes it had when the tables were made (a
// software breakpoint changes them too).
static bool checkTables(void) {
	for (size_t i = 0; i < macosRequirementCount; ++i) {
		uint32_t mac = macosTables_findAddress(&macosTables, macosRequirements[i].rva);
		if (!mac || (macosRequirements[i].function && !hasSignature(mac))) {
			log_message(
				"refusing: the tables have no %s for %x",
				mac ? "signature" : "address",
				macosRequirements[i].rva
			);
			return false;
		}
	}
	for (size_t i = 0; i < macosTables.addressCount; ++i) {
		if (macosTables.addresses[i].mac >= imageSpan) {
			log_message("refusing: table address %x is outside the image", macosTables.addresses[i].mac);
			return false;
		}
	}
	for (size_t i = 0; i < macosTables.signatureCount; ++i) {
		const MacosSignature *signature = &macosTables.signatures[i];
		const void *code = (const void *)(base + signature->mac); // NOLINT(performance-no-int-to-ptr)
		if (signature->mac >= imageSpan || memcmp(code, signature->bytes, signature->length) != 0) {
			log_message(
				"refusing: unexpected code at +%x (a software breakpoint, or another build)", signature->mac
			);
			return false;
		}
	}
	return true;
}

static bool hasSignature(uint32_t mac) {
	for (size_t i = 0; i < macosTables.signatureCount; ++i) {
		if (macosTables.signatures[i].mac == mac) {
			return true;
		}
	}
	return false;
}

// The offset of an address from the game's header, or 0 if it is outside the image.
static uint32_t macOffset(const void *address) {
	uintptr_t value = (uintptr_t)address;
	return value > base && value - base < imageSpan ? (uint32_t)(value - base) : 0;
}
