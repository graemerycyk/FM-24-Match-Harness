// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// Path helpers for the runner (wide paths: UTF-16 as Windows hands them to wmain, UTF-32 on macOS).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>


// The directory fm24ctl.exe lives in (also where the DLL, job and result files are).
bool paths_executableDirectory(wchar_t *out, size_t size);

// Where job input files go: the game reads them. fm24ctl.exe's directory on Windows; on macOS the
// user's temporary directory, which the game can always read, wherever fm24ctl is installed.
bool paths_jobDirectory(wchar_t *out, size_t size);

// `directory` + the platform's separator + `name`.
bool paths_join(wchar_t *out, size_t size, const wchar_t *directory, const wchar_t *name);

// An absolute version of a path given on the command line (the DLL runs with fm.exe's working
// directory).
bool paths_absolute(const wchar_t *path, wchar_t *out, size_t size);

// Whether a file can be written at absolute path `path` as far as its folder goes: the folder
// must exist (the file itself need not), `path` must not be a folder and, on macOS, the folder
// must let the user write the file in place if it exists, or make files in the folder if not.
typedef enum PathsFolder {
	PATHS_FOLDER_FOUND,
	PATHS_FILE_IS_FOLDER,      // the path itself is a folder
	PATHS_FOLDER_MISSING,      // it, or a folder above it, does not exist
	PATHS_FOLDER_NOT_FOLDER,   // it, or a folder above it, is a file
	PATHS_FOLDER_DENIED,       // permissions keep the user out of a folder above it (Windows: or it)
	PATHS_FOLDER_NOT_WRITABLE, // its own permissions do not let the user make files in it (macOS)
	PATHS_FOLDER_LOCKED,       // it is locked (Finder's Get Info), for every app (macOS)
	PATHS_FILE_NOT_WRITABLE,   // the file exists, and the user may not write it (macOS)
	PATHS_FILE_LOCKED,         // the file exists and is locked, for every app (macOS)
	PATHS_FOLDER_PRIVACY,      // macOS keeps this process out (EPERM), not necessarily the game
	PATHS_FOLDER_UNCHECKED,    // it could not be checked otherwise (the write may still work)
} PathsFolder;

PathsFolder paths_parentFolder(const wchar_t *path);

// UTF-8 for job files and messages.
bool paths_toUtf8(const wchar_t *text, char *out, size_t size);
