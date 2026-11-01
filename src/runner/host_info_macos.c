// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_info.h"

#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>


static void readText(const char *name, char *out, size_t size);
static uint64_t readNumber(const char *name);

void hostInfo_read(HostInfo *host) {
	*host = (HostInfo){0};
	readText("hw.model", host->model, sizeof host->model);
	readText("machdep.cpu.brand_string", host->cpu, sizeof host->cpu);
	host->logicalCpus = (unsigned)readNumber("hw.logicalcpu");
	// Apple silicon's performance and efficiency cores; an Intel Mac has neither name.
	host->performanceCores = (unsigned)readNumber("hw.perflevel0.physicalcpu");
	host->efficiencyCores = (unsigned)readNumber("hw.perflevel1.physicalcpu");
	host->memoryBytes = readNumber("hw.memsize");
	char version[32] = "";
	readText("kern.osproductversion", version, sizeof version);
	if (version[0]) {
		(void)snprintf(host->os, sizeof host->os, "macOS %s", version);
	}
	readText("kern.osversion", host->osBuild, sizeof host->osBuild);
	host->translated = readNumber("sysctl.proc_translated") == 1;
}

// A text sysctl, or "" if it is missing or does not fit.
static void readText(const char *name, char *out, size_t size) {
	size_t length = size;
	if (sysctlbyname(name, out, &length, NULL, 0) != 0 || length == 0 || length > size ||
			out[length - 1] != 0) {
		out[0] = 0;
	}
}

// A number sysctl of 4 or 8 bytes, or 0 if it is missing.
static uint64_t readNumber(const char *name) {
	uint8_t bytes[8] = {0};
	size_t length = sizeof bytes;
	if (sysctlbyname(name, bytes, &length, NULL, 0) != 0) {
		return 0;
	}
	if (length == sizeof(uint32_t)) {
		uint32_t value = 0;
		memcpy(&value, bytes, sizeof value);
		return value;
	}
	uint64_t value = 0;
	if (length == sizeof value) {
		memcpy(&value, bytes, sizeof value);
	}
	return value;
}
