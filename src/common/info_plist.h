// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// String values of an app bundle's Info.plist, as far as a plain look at an XML property list finds
// them, for an edition whose game is a Unity app (EDITION_ENGINE_PLUGIN): its executable's name
// (CFBundleExecutable), identifier and version may change with a build, so they are read rather
// than assumed. Binary property lists are not read.
#pragma once

#include <stdbool.h>
#include <stddef.h>


enum { INFO_PLIST_MAX = 65536 }; // the longest Info.plist read

// The <string> value of the first <key> named `key` in the property list `text`, with XML's five
// entities decoded. False if there is none, or it is empty or does not fit `size`.
bool infoPlist_find(const char *text, const char *key, char *out, size_t size);

// The same in the bundle `bundle`'s Contents/Info.plist (`bundle` is the .app folder).
bool infoPlist_read(const char *bundle, const char *key, char *out, size_t size);

// The executable `bundle` names for itself, Contents/MacOS/<CFBundleExecutable>, if its Info.plist
// has the identifier `identifier`. Where the property list names no executable, it is
// `fallback`. False if the bundle is another's, or the path does not fit `size`.
bool infoPlist_executable(
	const char *bundle,
	const char *identifier,
	const char *fallback,
	char *out,
	size_t size
);
