// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// FM24Viewer.app, the macOS fixture viewer, which fm24ctl opens fixtures in as fm24ctl.exe does in
// its own window on Windows (see main.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>


#define MACOS_VIEWER_APP "FM24Viewer.app"
// cmake/macos_viewer.cmake reads the bundle identifier from here.
#define MACOS_VIEWER_IDENTIFIER "io.github.graemerycyk.fm24-match-harness.viewer"
// What `fm24ctl` with no arguments exports, in the user's temporary directory; the viewer shows a
// fixture at that path as an unsaved export.
#define MACOS_VIEWER_TEMPORARY_FIXTURE L"temp-fixture.json"

// Hands `fixture` to the viewer and returns without waiting for it: 0 once the system has opened
// it, 1 if it cannot be opened, or 2 (after saying how to build it) if there is no viewer.
int macosViewer_open(const wchar_t *fixture);

// The viewer that goes with an fm24ctl in `directory`: the bundle whose Contents/MacOS that is,
// else FM24Viewer.app inside it. False if neither is there; the system may still know the viewer
// by its bundle identifier.
bool macosViewer_find(const char *directory, char *app, size_t size);
