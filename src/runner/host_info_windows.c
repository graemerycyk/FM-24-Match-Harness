// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_info.h"
#include "platform.h"

#include <stdio.h>
#include <windows.h>


void hostInfo_read(HostInfo *host) {
	*host = (HostInfo){0};
	host->logicalCpus = (unsigned)platform_processorCount();
	MEMORYSTATUSEX memory = {.dwLength = sizeof memory};
	if (GlobalMemoryStatusEx(&memory)) {
		host->memoryBytes = memory.ullTotalPhys;
	}
	(void)snprintf(host->os, sizeof host->os, "Windows");
}
