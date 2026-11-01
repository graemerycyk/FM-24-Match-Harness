// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// FMNNViewer.app, the macOS fixture viewer, which fmNNctl opens fixtures in as fmNNctl.exe does in
// its own window on Windows (see main.c).
#pragma once

#include "edition.h"

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>


#define MACOS_VIEWER_APP EDITION_VIEWER_APP
#define MACOS_VIEWER_IDENTIFIER EDITION_VIEWER_ID
// What `fmNNctl` with no arguments exports, in the user's temporary directory; the viewer shows a
// fixture at that path as an unsaved export. Each edition has its own, so one edition's export
// never replaces the other's.
#define MACOS_VIEWER_TEMPORARY_FIXTURE L"" EDITION_TEMPORARY_FIXTURE

// Hands `fixture` to the viewer and returns without waiting for it: 0 once the system has opened
// it, 1 if it cannot be opened, or 2 (after saying how to build it) if there is no viewer.
int macosViewer_open(const wchar_t *fixture);

// The viewer that goes with an fmNNctl in `directory`: the bundle whose Contents/MacOS that is,
// else FMNNViewer.app inside it. False if neither is there; the system may still know the viewer
// by its bundle identifier.
bool macosViewer_find(const char *directory, char *app, size_t size);
