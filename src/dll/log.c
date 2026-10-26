// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"

#include "platform.h"
#include "protocol.h"

#include <stdarg.h>
#include <stdio.h>


static FILE *logFile;

void log_open(const wchar_t *directory) {
	wchar_t path[HARNESS_PATH_LENGTH];
	if (swprintf(
				path,
				HARNESS_PATH_LENGTH,
				L"%ls%lc%ls",
				directory,
				(wint_t)PLATFORM_PATH_SEPARATOR,
				HARNESS_LOG_FILE
			) > 0) {
		logFile = platform_openFile(path, "ab");
	}
}

void log_message(const char *format, ...) {
	if (!logFile) {
		return;
	}
	int hour = 0;
	int minute = 0;
	int second = 0;
	int millisecond = 0;
	platform_timeOfDay(&hour, &minute, &second, &millisecond);
	fprintf(logFile, "%02d:%02d:%02d.%03d ", hour, minute, second, millisecond);
	va_list args;
	va_start(args, format);
	vfprintf(logFile, format, args);
	va_end(args);
	fputc('\n', logFile);
	(void)fflush(logFile);
}
