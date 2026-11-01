// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// game.h on macOS: the game's match engine is the main executable, or a plugin bundle the game
// loads once started (EDITION_ENGINE_PLUGIN), identified by the LC_UUID of the running slice
// (macos_builds.h), and the harness's Windows RVAs are translated through game_macos.h's tables.

#include "game_macos.h"

#include "edition.h"
#include "game.h"
#if EDITION_ENGINE_PLUGIN
#include "info_plist.h"
#endif
#include "log.h"
#include "macos_builds.h"
#include "platform.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


const MacosRequirement macosRequirements[] = {
	{RVA_GAME_MATCH_CREATE, true},
	{RVA_GAME_MATCH_DESERIALIZE, true},
	{RVA_GAME_MATCH_PREPARE, true},
	{RVA_GAME_MATCH_RUN, true},
	{RVA_GAME_MATCH_SERIALIZE, true},
	{RVA_DATA_BUFFER_CTOR, true},
#if EDITION_ENGINE_PLUGIN
	{RVA_MACOS_MATCH_FACTORY, true},
#else
	{RVA_ALLOC_FROM_POOL, true},
#endif
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
#if EDITION_ENGINE_PLUGIN
	{RVA_MACOS_MATCH_CONTEXT, false},
#endif
};
const size_t macosRequirementCount = sizeof macosRequirements / sizeof macosRequirements[0];

static uintptr_t base;      // the game's Mach-O header
static uintptr_t imageSpan; // bytes from the header to the end of its last segment
static const MacosBuild *build;
#if EDITION_ENGINE_PLUGIN
// The tables of the build found (macosBuildTables), or none.
#if defined(__aarch64__)
static const MacosTables noTables = {.architecture = "arm64"};
#else
static const MacosTables noTables = {.architecture = "x86_64"};
#endif
static const MacosTables *buildTables = &noTables;
static uint16_t formatVersion; // that build's (macosBuildTables), once it is accepted
#define TABLES (*buildTables)
#else
#define TABLES macosTables
#endif

#if !EDITION_ENGINE_PLUGIN
static const struct mach_header_64 *findGame(void);
#endif
static bool checkImage(const struct mach_header_64 *header);
static bool readIdentity(const struct mach_header_64 *header, uint8_t uuid[16]);
static const char *architectureOf(const struct mach_header_64 *header);
static bool checkTables(void);
static bool hasSignature(uint32_t mac);
static uint32_t macOffset(const void *address);
#if EDITION_ENGINE_PLUGIN
static const MacosBuildTables *findBuildTables(const uint8_t uuid[16]);
static void bundleAdded(const struct mach_header *header, intptr_t slide);
static void imageRemoved(const struct mach_header *header, intptr_t slide);
static void wake(void);
static void findPlugin(void);
static bool holdImage(const struct mach_header *header, Dl_info *info);
static bool isPluginPath(const char *path);
static void examinePlugin(const struct mach_header_64 *header, const char *path);
static void noteUnloaded(void);
static void gameVersion(char *out, size_t size);
#endif

#if !EDITION_ENGINE_PLUGIN
bool game_init(void) {
	const struct mach_header_64 *header = findGame();
	if (!header) {
		log_message("refusing: cannot find the game's executable image");
		return false;
	}
	return checkImage(header);
}
#endif

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
#if EDITION_ENGINE_PLUGIN
	buildTables = build ? macosTables_forBuild(uuid) : NULL;
	if (build && !buildTables) {
		log_message(
			"refusing: no tables for " EDITION_GAME " %s, %s (UUID %s)",
			build->version,
			architecture,
			text
		);
		build = NULL;
		buildTables = &noTables;
		return false;
	}
	buildTables = buildTables ? buildTables : &noTables;
#endif
	if (!build || strcmp(TABLES.architecture, architecture) != 0) {
		log_message("refusing: unsupported game build (%s, UUID %s)", architecture, text);
		build = NULL;
		return false;
	}
	base = (uintptr_t)header;
	if (!checkTables()) {
		return false;
	}
#if EDITION_ENGINE_PLUGIN
	formatVersion = macosTables_formatVersion(uuid);
#endif
	log_message("game build: " EDITION_GAME " %s, %s (UUID %s)", build->version, architecture, text);
	return true;
}

uintptr_t game_base(void) {
	return base;
}

#if EDITION_ENGINE_PLUGIN
static const MacosBuildTables *findBuildTables(const uint8_t uuid[16]) {
	for (size_t i = 0; i < macosBuildTableCount; ++i) {
		if (memcmp(macosBuildTables[i].uuid, uuid, sizeof macosBuildTables[i].uuid) == 0) {
			return &macosBuildTables[i];
		}
	}
	return NULL;
}

const MacosTables *macosTables_forBuild(const uint8_t uuid[16]) {
	const MacosBuildTables *found = findBuildTables(uuid);
	return found ? found->tables : NULL;
}

uint16_t macosTables_formatVersion(const uint8_t uuid[16]) {
	const MacosBuildTables *found = findBuildTables(uuid);
	return found ? found->formatVersion : 0;
}

uint16_t game_formatVersion(void) {
	return formatVersion;
}
#endif

uint32_t game_timestamp(void) {
	return build ? build->bundleVersion : 0;
}

void *game_address(uint32_t rva) {
	uint32_t offset = macosTables_findAddress(&TABLES, rva);
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
	return offset && macosTables_translate(&TABLES, &offset, 1, &rva, 1) ? rva : 0;
}

size_t game_translateFrames(void *const *frames, size_t count, uint32_t *out, size_t capacity) {
	uint32_t offsets[64];
	if (count > sizeof offsets / sizeof offsets[0]) {
		count = sizeof offsets / sizeof offsets[0];
	}
	for (size_t i = 0; i < count; ++i) {
		offsets[i] = macOffset(frames[i]);
	}
	return macosTables_translate(&TABLES, offsets, count, out, capacity);
}

uint8_t *game_initThread(void) {
	// Mach-O thread-locals start one by one on first use, so there is nothing to run for the whole
	// image. The game reaches its PCG32 through an accessor that seeds it on a thread's first call;
	// calling it first means the harness's seed is not overwritten by that seeding later.
	typedef uint8_t *(*ThreadRngFn)(void);
	return ((ThreadRngFn)game_function(RVA_MACOS_THREAD_RNG))();
}

#if EDITION_ENGINE_PLUGIN
const MacosFrame *macosTables_findFrame(
	const MacosTables *tables,
	uint32_t mac,
	uint32_t parent,
	uint32_t grandparent
) {
#else
const MacosFrame *macosTables_findFrame(const MacosTables *tables, uint32_t mac, uint32_t parent) {
#endif
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
#if EDITION_ENGINE_PLUGIN
	const MacosFrame *withParent = NULL;
	for (size_t i = low; i < tables->frameCount && tables->frames[i].mac == mac; ++i) {
		const MacosFrame *frame = &tables->frames[i];
		if (frame->parent == 0) {
			general = frame;
		} else if (frame->parent == parent && frame->grandparent == 0) {
			withParent = frame;
		} else if (frame->parent == parent && frame->grandparent == grandparent) {
			return frame;
		}
	}
	return withParent ? withParent : general;
#else
	for (size_t i = low; i < tables->frameCount && tables->frames[i].mac == mac; ++i) {
		const MacosFrame *frame = &tables->frames[i];
		if (frame->parent == 0) {
			general = frame;
		} else if (frame->parent == parent) {
			return frame;
		}
	}
	return general;
#endif
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
#if EDITION_ENGINE_PLUGIN
		const MacosFrame *frame = macosTables_findFrame(
			tables, frames[i], i + 1 < count ? frames[i + 1] : 0, i + 2 < count ? frames[i + 2] : 0
		);
#else
		const MacosFrame *frame =
			macosTables_findFrame(tables, frames[i], i + 1 < count ? frames[i + 1] : 0);
#endif
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

#if !EDITION_ENGINE_PLUGIN
// The main executable, if it is a Football Manager app bundle's Contents/MacOS/fm.
static const struct mach_header_64 *findGame(void) {
	static const char suffix[] = "/Contents/MacOS/" EDITION_GAME_EXECUTABLE;
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
#endif

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
		uint32_t mac = macosTables_findAddress(&TABLES, macosRequirements[i].rva);
		if (!mac || (macosRequirements[i].function && !hasSignature(mac))) {
			log_message(
				"refusing: the tables have no %s for %x",
				mac ? "signature" : "address",
				macosRequirements[i].rva
			);
			return false;
		}
	}
	for (size_t i = 0; i < TABLES.addressCount; ++i) {
		if (TABLES.addresses[i].mac >= imageSpan) {
			log_message("refusing: table address %x is outside the image", TABLES.addresses[i].mac);
			return false;
		}
	}
	for (size_t i = 0; i < TABLES.signatureCount; ++i) {
		const MacosSignature *signature = &TABLES.signatures[i];
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
	for (size_t i = 0; i < TABLES.signatureCount; ++i) {
		if (TABLES.signatures[i].mac == mac) {
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

#if EDITION_ENGINE_PLUGIN
// The engine plugin. dyld calls the add-image and remove-image callbacks with its lock held, so
// they only count what happened and wake the job thread through a pipe: the job thread finds and
// checks the plugin itself, with dladdr, which needs that lock. The game unloads the plugin as it
// quits, perhaps while a job runs: the remove-image callback runs before the image is unmapped, so
// it waits (for a while) until the image is no longer held by a job, or by the job thread's own
// look at it, which a job stops at its next boundary for (game_loaded).
enum {
	UNLOAD_WAIT_MS = 10000, // the longest the game's unloading of the plugin waits for a job
	UNLOAD_POLL_MS = 5,
	HOLD_ATTEMPTS = 100, // 1 ms apart, while another image's unloading is under way
};

static int watchPipe[2] = {-1, -1}; // the callbacks write to [1], the job thread reads [0]
static atomic_uint bundlesLoaded;   // bundles dyld has added
static unsigned bundlesSeen;        // bundlesLoaded when the job thread last looked
static atomic_uint removals;        // twice the images removed, odd during a removal
static _Atomic(const struct mach_header *) plugin; // the plugin found (accepted or not) until gone
static _Atomic(const struct mach_header *) held;   // an image the job thread is reading
static atomic_bool unloading;                      // the game has begun to unload `plugin`
static atomic_bool unloaded;                       // ... and its remove-image callback is over
static atomic_uint unloadWaitedMs;                 // how long that callback waited for `held`
static unsigned unloadWaitMs = UNLOAD_WAIT_MS;     // (tests shorten it)
static GameEngineState engineState = GAME_ENGINE_STARTING;

bool game_watch(void) {
	if (pipe(watchPipe) != 0) {
		return false;
	}
	for (int i = 0; i < 2; ++i) {
		(void)fcntl(watchPipe[i], F_SETFD, FD_CLOEXEC);
		(void)fcntl(watchPipe[i], F_SETFL, O_NONBLOCK);
	}
	// Removals first, so that any plugin the job thread can find is already watched for unloading.
	_dyld_register_func_for_remove_image(imageRemoved);
	_dyld_register_func_for_add_image(bundleAdded);
	return true;
}

int game_watchDescriptor(void) {
	return watchPipe[0];
}

GameEngineState game_update(void) {
	char drained[64];
	while (read(watchPipe[0], drained, sizeof drained) > 0) {
	}
	if (atomic_exchange(&unloaded, false)) {
		noteUnloaded();
	}
	unsigned loaded = atomic_load(&bundlesLoaded);
	if (loaded != bundlesSeen && !atomic_load(&plugin)) {
		bundlesSeen = loaded;
		findPlugin();
	}
	return engineState;
}

GameEngineState game_engineState(void) {
	return engineState;
}

bool game_hold(void) {
	atomic_store(&held, atomic_load(&plugin));
	if (atomic_load(&unloading)) {
		atomic_store(&held, NULL);
		return false;
	}
	return true;
}

void game_release(void) {
	atomic_store(&held, NULL);
}

bool game_loaded(void) {
	return !atomic_load(&unloading);
}

static void bundleAdded(const struct mach_header *header, intptr_t slide) {
	(void)slide;
	if (header->filetype == MH_BUNDLE) {
		atomic_fetch_add(&bundlesLoaded, 1);
		wake();
	}
}

// Before dyld unmaps an image. For the plugin, jobs are told to stop (game_loaded), and the image
// stays mapped until none holds it, for at most unloadWaitMs. The job thread may find the plugin
// while this waits for its look at it, so whether it is the plugin is asked again until the image
// is no longer held.
static void imageRemoved(const struct mach_header *header, intptr_t slide) {
	(void)slide;
	atomic_fetch_add(&removals, 1);
	bool ours = false;
	unsigned waited = 0;
	for (;;) {
		if (!ours && atomic_load(&plugin) == header) {
			ours = true;
			atomic_store(&unloading, true);
		}
		if (atomic_load(&held) == header && waited < unloadWaitMs) {
			platform_sleep(UNLOAD_POLL_MS);
			waited += UNLOAD_POLL_MS;
		} else if (ours || atomic_load(&plugin) != header) {
			break;
		}
	}
	atomic_fetch_add(&removals, 1);
	if (ours) {
		atomic_store(&unloadWaitedMs, waited);
		atomic_store(&unloaded, true);
		wake();
	}
}

static void wake(void) {
	char byte = 1;
	(void)write(watchPipe[1], &byte, 1); // a full pipe wakes the job thread already
}

// Looks through the loaded images for the plugin: the callbacks cannot tell which bundle dyld
// loaded.
static void findPlugin(void) {
	uint32_t count = _dyld_image_count();
	for (uint32_t i = 0; i < count && !atomic_load(&plugin); ++i) {
		const struct mach_header *header = _dyld_get_image_header(i);
		Dl_info info;
		if (!header || !holdImage(header, &info)) {
			continue;
		}
		if (header->magic == MH_MAGIC_64 && header->filetype == MH_BUNDLE &&
				isPluginPath(info.dli_fname)) {
			examinePlugin((const struct mach_header_64 *)header, info.dli_fname);
		}
		atomic_store(&held, NULL);
	}
}

// Holds `header` if it is a loaded image, with dladdr's information on it, which stays valid while
// it is held. dladdr waits for a removal dyld is making, but an image whose removal has begun is
// not held: imageRemoved may have looked at `held` already.
static bool holdImage(const struct mach_header *header, Dl_info *info) {
	for (unsigned attempt = 0; attempt < HOLD_ATTEMPTS; ++attempt) {
		unsigned before = atomic_load(&removals);
		if (before % 2 == 0) {
			if (!dladdr(header, info) || info->dli_fbase != header || !info->dli_fname) {
				return false;
			}
			atomic_store(&held, header);
			if (atomic_load(&removals) == before) {
				return true;
			}
			atomic_store(&held, NULL);
		}
		platform_sleep(1);
	}
	return false;
}

static bool isPluginPath(const char *path) {
	static const char suffix[] = "/" EDITION_ENGINE_PATH;
	size_t length = strlen(path);
	return length >= sizeof suffix - 1 && strcmp(path + length - (sizeof suffix - 1), suffix) == 0;
}

// Identifies the plugin and checks the tables against it, as game_init does for an engine in the
// executable.
static void examinePlugin(const struct mach_header_64 *header, const char *path) {
	char version[64];
	gameVersion(version, sizeof version);
	log_message("found " EDITION_GAME "'s engine (the game's version %s): %s", version, path);
	bool accepted = checkImage(header);
	if (!accepted) {
		if (build) {
			log_message(
				"refusing: %s, %s, is a supported build, but the checks above failed",
				build->version,
				build->architecture
			);
		}
		base = 0;
		imageSpan = 0;
		build = NULL;
		buildTables = &noTables;
		formatVersion = 0;
	}
	engineState = accepted ? GAME_ENGINE_ACCEPTED : GAME_ENGINE_REFUSED;
	atomic_store(&plugin, (const struct mach_header *)header);
}

static void noteUnloaded(void) {
	unsigned waited = atomic_load(&unloadWaitedMs);
	if (waited >= unloadWaitMs) {
		log_message(
			EDITION_GAME " unloaded its engine while the harness was still using it (after %u ms)", waited
		);
	} else if (waited) {
		log_message(
			EDITION_GAME " unloaded its engine once the harness had stopped using it (after %u ms)",
			waited
		);
	} else {
		log_message(EDITION_GAME " unloaded its engine");
	}
	atomic_store(&plugin, NULL);
	atomic_store(&unloading, false);
	base = 0;
	imageSpan = 0;
	build = NULL;
	buildTables = &noTables;
	formatVersion = 0;
	engineState = GAME_ENGINE_GONE;
}

// The game app's version, its Info.plist's CFBundleShortVersionString, else "unknown". It names a
// build the tables do not know.
static void gameVersion(char *out, size_t size) {
	char path[4096];
	uint32_t pathSize = sizeof path;
	bool found = _NSGetExecutablePath(path, &pathSize) == 0;
	for (int up = 0; up < 3 && found; ++up) { // from Contents/MacOS/<executable> up to the app
		char *slash = strrchr(path, '/');
		found = slash != NULL;
		if (slash) {
			*slash = 0;
		}
	}
	if (!found || !infoPlist_read(path, "CFBundleShortVersionString", out, size)) {
		(void)snprintf(out, size, "unknown");
	}
}
#endif
