// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// profiler.h on macOS. The game keeps frame pointers (the Apple ABIs require them), so a sample is
// the program counter and the frame-pointer chain; where the chain ends early it goes on with a
// stack scan for plausible return addresses, as on Windows.

#include "game.h"
#include "log.h"
#include "platform.h"
#include "profiler.h"

#include <mach-o/loader.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


enum {
	MAX_FRAMES = 64,
	SCAN_SLOTS = 8192,     // how far up the stack (in 8-byte slots) a scan looks
	INTERVAL_NS = 1000000, // 1 ms
	MAX_CODE_RANGES = 16,
};

typedef struct Range {
	uintptr_t start;
	uintptr_t end;
} Range;

struct Profiler {
	thread_act_t target;
	pthread_t thread;
	bool started;
	FILE *file;
	PlatformLock lock; // the file is written by both threads
	atomic_bool stop;
	uintptr_t stackLimit; // the target's stack, for the frame walk and the return address scan
	uintptr_t stackBase;
	Range code[MAX_CODE_RANGES]; // the game's executable segments
	int codeCount;
};

static bool inCode(const Profiler *profiler, uintptr_t address);
static bool followsCall(const Profiler *profiler, uintptr_t address);
static bool onStack(const Profiler *profiler, uintptr_t address, size_t size);
static int scanStack(const Profiler *profiler, uintptr_t slot, uint64_t *frames, int count);
static int walk(
	const Profiler *profiler,
	uintptr_t pc,
	uintptr_t fp,
	uintptr_t sp,
	uint64_t *frames,
	int *exact
);
static void writeSample(Profiler *profiler, const uint64_t *frames, int count, int exact);
static void sample(Profiler *profiler);
static void *sampler(void *parameter);
static void setCodeRanges(Profiler *profiler);

Profiler *profiler_start(const wchar_t *path) {
	Profiler *profiler = calloc(1, sizeof *profiler);
	if (!profiler) {
		log_message("profiler: out of memory");
		return NULL;
	}
	atomic_init(&profiler->stop, false);
	pthread_t self = pthread_self();
	profiler->stackBase = (uintptr_t)pthread_get_stackaddr_np(self); // the high end
	profiler->stackLimit = profiler->stackBase - pthread_get_stacksize_np(self);
	setCodeRanges(profiler);
	platform_initLock(&profiler->lock);
	profiler->file = platform_openFile(path, "wb");
	profiler->target = mach_thread_self();
	if (profiler->file) {
		fprintf(profiler->file, "# fm %llx\n", (unsigned long long)game_base());
		profiler->started = pthread_create(&profiler->thread, NULL, sampler, profiler) == 0;
	}
	if (!profiler->started) {
		log_message("profiler: cannot start");
		profiler_stop(profiler);
		return NULL;
	}
	return profiler;
}

void profiler_mark(Profiler *profiler, const char *text) {
	if (!profiler) {
		return;
	}
	platform_lock(&profiler->lock);
	fprintf(profiler->file, "# %s\n", text);
	platform_unlock(&profiler->lock);
}

void profiler_stop(Profiler *profiler) {
	if (!profiler) {
		return;
	}
	if (profiler->started) {
		atomic_store_explicit(&profiler->stop, true, memory_order_relaxed);
		pthread_join(profiler->thread, NULL);
	}
	if (profiler->target) {
		(void)mach_port_deallocate(mach_task_self(), profiler->target);
	}
	if (profiler->file) {
		(void)fclose(profiler->file);
	}
	platform_destroyLock(&profiler->lock);
	free(profiler);
}

static bool inCode(const Profiler *profiler, uintptr_t address) {
	for (int i = 0; i < profiler->codeCount; ++i) {
		if (address >= profiler->code[i].start && address < profiler->code[i].end) {
			return true;
		}
	}
	return false;
}

// True if the instruction before `address` is a call, so `address` is plausibly a return address.
static bool followsCall(const Profiler *profiler, uintptr_t address) {
#ifdef __aarch64__
	if (!inCode(profiler, address - 4) || address % 4) {
		return false;
	}
	uint32_t instruction = 0;
	memcpy(&instruction, (const void *)(address - 4), sizeof instruction); // NOLINT
	bool branchLink = (instruction & 0xFC000000U) == 0x94000000U;          // BL
	bool branchLinkRegister = (instruction & 0xFFFFFC1FU) == 0xD63F0000U;  // BLR
	return branchLink || branchLinkRegister;
#else
	if (!inCode(profiler, address - 8)) {
		return false;
	}
	uint8_t code[8];
	memcpy(code, (const void *)(address - 8), sizeof code); // NOLINT(performance-no-int-to-ptr)
	bool rel32 = code[3] == 0xE8;
	bool indirect2 = code[6] == 0xFF && (code[7] & 0x38) == 0x10; // call r/m (no displacement)
	bool indirect3 = code[5] == 0xFF && (code[6] & 0x38) == 0x10; // call [r+disp8]
	bool indirect6 = code[2] == 0xFF && (code[3] & 0x38) == 0x10; // call [rip+disp32]
	bool indirect7 = code[1] == 0xFF && (code[2] & 0x38) == 0x10; // call [r+disp32] with SIB
	return rel32 || indirect2 || indirect3 || indirect6 || indirect7;
#endif
}

static bool onStack(const Profiler *profiler, uintptr_t address, size_t size) {
	return address >= profiler->stackLimit && address + size <= profiler->stackBase;
}

// Appends every plausible return address into the game found from `slot` up the stack, stale
// ones included. Reads only the target's stack and the game's code.
static int scanStack(const Profiler *profiler, uintptr_t slot, uint64_t *frames, int count) {
	for (int i = 0;
		i < SCAN_SLOTS && count < MAX_FRAMES && onStack(profiler, slot, sizeof(uintptr_t));
		++i, slot += sizeof(uintptr_t)) {
		uintptr_t value = *(const uintptr_t *)slot; // NOLINT(performance-no-int-to-ptr)
		if (inCode(profiler, value) && followsCall(profiler, value)) {
			frames[count++] = value;
		}
	}
	return count;
}

// Walks the (suspended) target's frame-pointer chain while it stays on the stack and in the game,
// then scans on from the last frame. Returns the frame count; `exact` the walked ones.
// Must not allocate: the target may hold the heap lock.
static int walk(
	const Profiler *profiler,
	uintptr_t pc,
	uintptr_t fp,
	uintptr_t sp,
	uint64_t *frames,
	int *exact
) {
	int count = 0;
	frames[count++] = pc;
	while (count < MAX_FRAMES && onStack(profiler, fp, 2 * sizeof(uintptr_t)) && fp >= sp) {
		const uintptr_t *record = (const uintptr_t *)fp; // NOLINT(performance-no-int-to-ptr)
		uintptr_t next = record[0];
		uintptr_t returnAddress = record[1];
		if (!returnAddress || (next && next <= fp)) {
			break;
		}
		frames[count++] = returnAddress;
		if (!next) {
			break;
		}
		sp = fp;
		fp = next;
	}
	*exact = count;
	return count < MAX_FRAMES ? scanStack(profiler, sp, frames, count) : count;
}

static void writeSample(Profiler *profiler, const uint64_t *frames, int count, int exact) {
	platform_lock(&profiler->lock);
	fputc('s', profiler->file);
	for (int i = 0; i < count; ++i) {
		fprintf(profiler->file, i == exact ? " | %llx" : " %llx", (unsigned long long)frames[i]);
	}
	fputc('\n', profiler->file);
	platform_unlock(&profiler->lock);
}

static void sample(Profiler *profiler) {
	uint64_t frames[MAX_FRAMES];
	if (thread_suspend(profiler->target) != KERN_SUCCESS) {
		return;
	}
	int exact = 0;
	int count = 0;
#ifdef __aarch64__
	arm_thread_state64_t state;
	mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
	if (thread_get_state(profiler->target, ARM_THREAD_STATE64, (thread_state_t)&state, &stateCount) ==
			KERN_SUCCESS) {
		count = walk(
			profiler,
			(uintptr_t)arm_thread_state64_get_pc(state),
			(uintptr_t)arm_thread_state64_get_fp(state),
			(uintptr_t)arm_thread_state64_get_sp(state),
			frames,
			&exact
		);
	}
#else
	x86_thread_state64_t state;
	mach_msg_type_number_t stateCount = x86_THREAD_STATE64_COUNT;
	if (thread_get_state(profiler->target, x86_THREAD_STATE64, (thread_state_t)&state, &stateCount) ==
			KERN_SUCCESS) {
		count = walk(profiler, state.__rip, state.__rbp, state.__rsp, frames, &exact);
	}
#endif
	thread_resume(profiler->target);
	if (count > 0) {
		writeSample(profiler, frames, count, exact);
	}
}

static void *sampler(void *parameter) {
	Profiler *profiler = parameter;
	(void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	while (!atomic_load_explicit(&profiler->stop, memory_order_relaxed)) {
		struct timespec delay = {.tv_sec = 0, .tv_nsec = INTERVAL_NS};
		(void)nanosleep(&delay, NULL);
		sample(profiler);
	}
	return NULL;
}

static void setCodeRanges(Profiler *profiler) {
	// NOLINTNEXTLINE(performance-no-int-to-ptr): the game's image header.
	const struct mach_header_64 *header = (const struct mach_header_64 *)game_base();
	if (!header) {
		return;
	}
	const uint8_t *command = (const uint8_t *)(header + 1);
	uint64_t textAddress = 0;
	for (uint32_t i = 0; i < header->ncmds; ++i) {
		const struct load_command *load = (const struct load_command *)command;
		if (load->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *segment = (const struct segment_command_64 *)command;
			if (strcmp(segment->segname, SEG_TEXT) == 0) {
				textAddress = segment->vmaddr;
			}
			if ((segment->initprot & VM_PROT_EXECUTE) && profiler->codeCount < MAX_CODE_RANGES) {
				uintptr_t start = game_base() + (uintptr_t)(segment->vmaddr - textAddress);
				profiler->code[profiler->codeCount++] = (Range){start, start + segment->vmsize};
			}
		}
		command += load->cmdsize;
	}
}
