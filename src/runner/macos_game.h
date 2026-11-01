// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The game on macOS: finding the Steam install and the running game, installing the
// harness and starting the game with it.
//
// A running game cannot be injected on macOS (it is signed with the hardened runtime, which denies
// other processes its task port). Instead the game loads the harness dylib when it starts: it is
// entitled to accept DYLD_INSERT_LIBRARIES and libraries of other signers. `install` writes the
// dylib and a launcher script to ~/Library/Application Support/fmNN-harness; Steam runs the script
// through the game's launch option, `/bin/sh "<launcher>" %command%`, and the script starts the
// game with the dylib (and Steam's own overlay libraries). `launch` starts the game the same way
// without Steam's launch option, or through Steam once the option is set.
//
// `launch --save` has the game's own start screen load a save: the game's auto-play file
// (`--auto_play_game_file`) names it, and nothing else (see saves.h). Through Steam, which passes
// no arguments, fmNNctl leaves the file as a one-shot request, named after its own process, that
// the launcher hands to the game it starts next while that fmNNctl still runs; a direct start
// passes it itself. fmNNctl then waits until the harness reports the save loaded and idle
// (session.h). It never quits, saves or switches the save of a running game.
//
// `launch --rosetta` starts the game's x86_64 slice under Rosetta on Apple silicon: a direct start
// asks open for it, and through Steam fmNNctl leaves a second one-shot request of the same kind,
// for which the launcher runs that slice. A running game's slice is only checked.
// Commands whose jobs are for a save make sure of it the same way first (macosGame_ensureSave).
#pragma once

#include "edition.h"
#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <wchar.h>


// The launcher script's file name, next to the installed dylib.
#define MACOS_GAME_LAUNCHER EDITION_LAUNCHER

int macosGame_install(int argc, wchar_t **argv);
int macosGame_launch(int argc, wchar_t **argv);

// Before a command's job for `need->save`: waits until the running game has that save loaded and
// idle, or starts the game with it if no game is running, as session_plan says. Returns 0 to submit
// the job, with the save's in-game date in `date` if given ("" if unknown, or for another save),
// else the exit code. Prints what it does.
#if EDITION_ENGINE_PLUGIN
// For a fixture that does not record its save (`need->unrecorded`), it waits the same way until
// the running game has a save loaded and idle, whichever it is, and stops if no game is running.
#endif
int macosGame_ensureSave(const SessionNeed *need, char *date, size_t size);

// Installs the current dylib and launcher (quietly if they are already current), and deletes the
// auto-play and Rosetta requests and files next to them that no running process needs. Writes the
// launcher's path to `launcher`. Prints why and returns false on failure.
bool macosGame_installHarness(bool verbose, char *launcher, size_t size);

// True if the Steam account that signed in last gives the game a launch option that
// runs `launcher` with the game's command, as Steam has saved it.
bool macosGame_launchOptionSet(const char *launcher);

// The running game's process ID (0 if it is not running) and executable path.
pid_t macosGame_findRunning(char *path, size_t size);

// The installed fm.app, in the Steam library that holds the game's Steam app.
bool macosGame_findInstalled(char *app, size_t size);

// True if the executable at `path` belongs to the game's bundle.
#if EDITION_ENGINE_PLUGIN
// It is the one the bundle's Info.plist names (EDITION_GAME_EXECUTABLE if it names none), in a
// bundle with the game's identifier.
#endif
bool macosGame_isGameExecutable(const char *path);

// The fm.app bundle `path` names: the bundle or its Contents/MacOS/fm, trailing slashes allowed.
// False unless it is the game's.
bool macosGame_bundle(const char *path, char *app, size_t size);

#if EDITION_ENGINE_PLUGIN
// The game's executable in the bundle `app`, as its Info.plist names it. False unless the bundle
// is the game's.
bool macosGame_appExecutable(const char *app, char *out, size_t size);
#endif
