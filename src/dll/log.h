// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// The DLL's log file (harness.log next to the DLL), timestamped and flushed per line.
#pragma once

#include "platform.h"

#include <wchar.h>


void log_open(const wchar_t *directory);

void log_message(const char *format, ...) PLATFORM_PRINTF(1, 2);
