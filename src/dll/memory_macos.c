// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// memory.h on macOS: reads go through mach_vm_read_overwrite on our own task, so a bad address
// fails the read instead of crashing the game.

#include "memory.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <stdlib.h>
#include <string.h>


enum { SCAN_CHUNK = 1 << 20 };

static bool isHeap(const vm_region_submap_info_data_64_t *info);
static bool isHeapTag(unsigned int tag);
static void scanRegion(
	uint8_t *base,
	size_t size,
	uint8_t *chunk,
	uintptr_t value,
	void (*visit)(void *context, uint8_t *address),
	void *context
);

bool memory_read(const void *address, void *out, size_t size) {
	mach_vm_size_t got = 0;
	return mach_vm_read_overwrite(
					 mach_task_self(),
					 (mach_vm_address_t)(uintptr_t)address,
					 size,
					 (mach_vm_address_t)(uintptr_t)out,
					 &got
				 ) == KERN_SUCCESS &&
				 got == size;
}

bool memory_readPointer(const void *base, ptrdiff_t offset, uint8_t **out) {
	*out = NULL;
	return base && memory_read((const uint8_t *)base + offset, out, sizeof *out) && *out;
}

bool memory_readU16(const void *base, ptrdiff_t offset, uint16_t *out) {
	return base && memory_read((const uint8_t *)base + offset, out, sizeof *out);
}

bool memory_readU32(const void *base, ptrdiff_t offset, uint32_t *out) {
	return base && memory_read((const uint8_t *)base + offset, out, sizeof *out);
}

uint16_t memory_loadU16(const uint8_t *bytes) {
	uint16_t value = 0;
	memcpy(&value, bytes, sizeof value);
	return value;
}

uint32_t memory_loadU32(const uint8_t *bytes) {
	uint32_t value = 0;
	memcpy(&value, bytes, sizeof value);
	return value;
}

uint64_t memory_loadU64(const uint8_t *bytes) {
	uint64_t value = 0;
	memcpy(&value, bytes, sizeof value);
	return value;
}

void memory_scanForQword(
	uintptr_t value,
	void (*visit)(void *context, uint8_t *address),
	void *context
) {
	uint8_t *chunk = malloc(SCAN_CHUNK);
	if (!chunk) {
		return;
	}
	mach_vm_address_t address = 0;
	natural_t depth = 0;
	for (;;) {
		mach_vm_size_t size = 0;
		vm_region_submap_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
		if (mach_vm_region_recurse(
					mach_task_self(), &address, &size, &depth, (vm_region_recurse_info_t)&info, &count
				) != KERN_SUCCESS) {
			break;
		}
		if (info.is_submap) {
			++depth;
			continue;
		}
		if (isHeap(&info)) {
			// NOLINTNEXTLINE(performance-no-int-to-ptr): a region of our own address space.
			scanRegion((uint8_t *)(uintptr_t)address, (size_t)size, chunk, value, visit, context);
		}
		address += size;
	}
	free(chunk);
}

// Read/write memory that is not mapped from a file and that malloc, or an untagged anonymous
// mapping, provides: where the game's objects live (the counterpart of MEM_PRIVATE +
// PAGE_READWRITE on Windows). The share mode is no guide: macOS reports a malloc region as shared
// while anything else maps or references part of it (a buffer the GPU uses, for example), and the
// game's fixtures and teams then sit in "shared" regions. Graphics, device and Rosetta memory
// carry their own tags.
static bool isHeap(const vm_region_submap_info_data_64_t *info) {
	bool readWrite = (info->protection & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE)) ==
									 (VM_PROT_READ | VM_PROT_WRITE);
	return readWrite && !info->external_pager && isHeapTag(info->user_tag);
}

static bool isHeapTag(unsigned int tag) {
	switch (tag) {
		case 0: // an untagged anonymous mapping
		case VM_MEMORY_MALLOC:
		case VM_MEMORY_MALLOC_SMALL:
		case VM_MEMORY_MALLOC_LARGE:
		case VM_MEMORY_MALLOC_HUGE:
		case VM_MEMORY_REALLOC:
		case VM_MEMORY_MALLOC_TINY:
		case VM_MEMORY_MALLOC_LARGE_REUSABLE:
		case VM_MEMORY_MALLOC_LARGE_REUSED:
		case VM_MEMORY_MALLOC_NANO:
		case VM_MEMORY_MALLOC_MEDIUM:
#ifdef VM_MEMORY_MALLOC_PROB_GUARD
		case VM_MEMORY_MALLOC_PROB_GUARD:
#endif
			return true;
		default: return false;
	}
}

// Scans one region in chunks for qwords equal to `value`.
static void scanRegion(
	uint8_t *base,
	size_t size,
	uint8_t *chunk,
	uintptr_t value,
	void (*visit)(void *context, uint8_t *address),
	void *context
) {
	for (size_t offset = 0; offset < size; offset += SCAN_CHUNK) {
		size_t length = size - offset < SCAN_CHUNK ? size - offset : SCAN_CHUNK;
		uint8_t *start = base + offset;
		bool overlapsChunk = start < chunk + SCAN_CHUNK && chunk < start + length;
		if (overlapsChunk || !memory_read(start, chunk, length)) {
			continue; // the scan's own buffer holds copies of what it found
		}
		for (size_t i = 0; i + sizeof(uintptr_t) <= length; i += sizeof(uintptr_t)) {
			uintptr_t found = 0;
			memcpy(&found, chunk + i, sizeof found);
			if (found == value) {
				visit(context, base + offset + i);
			}
		}
	}
}
