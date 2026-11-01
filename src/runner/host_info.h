// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The computer fm24ctl runs on, as a run's manifest records it: host_info_macos.c reads it with
// sysctl, and host_info_windows.c has the processor count and memory.
#pragma once

#include <stdbool.h>
#include <stdint.h>


typedef struct HostInfo {
	char model[64];            // "MacBookAir10,1"; "" if unknown
	char cpu[128];             // "Apple M1"; "" if unknown
	unsigned logicalCpus;      // 0 if unknown
	unsigned performanceCores; // Apple silicon's two kinds of core; 0 if unknown
	unsigned efficiencyCores;
	uint64_t memoryBytes; // 0 if unknown
	char os[64];          // "macOS 27.0" or "Windows"
	char osBuild[32];     // "25A5279m"; "" if unknown
	bool translated;      // fm24ctl itself runs under Rosetta
} HostInfo;

void hostInfo_read(HostInfo *host);
