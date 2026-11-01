// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// Football Manager 2024 on macOS: finding the Steam install and the running game, installing the
// harness and starting the game with it.
//
// A running game cannot be injected on macOS (it is signed with the hardened runtime, which denies
// other processes its task port). Instead the game loads the harness dylib when it starts: it is
// entitled to accept DYLD_INSERT_LIBRARIES and libraries of other signers. `install` writes the
// dylib and a launcher script to ~/Library/Application Support/fm24-harness; Steam runs the script
// through the game's launch option, `/bin/sh "<launcher>" %command%`, and the script starts the
// game with the dylib (and Steam's own overlay libraries). `launch` starts the game the same way
// without Steam's launch option, or through Steam once the option is set.
//
// `launch --save` has the game's own start screen load a save: the game's auto-play file
// (`--auto_play_game_file`) names it, and nothing else (see saves.h). Through Steam, which passes
// no arguments, fm24ctl leaves the file as a one-shot request, named after its own process, that
// the launcher hands to the game it starts next while that fm24ctl still runs; a direct start
// passes it itself. fm24ctl then waits until the harness reports the save loaded and idle
// (session.h). It never quits, saves or switches the save of a running game.
//
// `launch --rosetta` starts the game's x86_64 slice under Rosetta on Apple silicon: a direct start
// asks open for it, and through Steam fm24ctl leaves a second one-shot request of the same kind,
// for which the launcher runs that slice. A running game's slice is only checked.
// Commands whose jobs are for a save make sure of it the same way first (macosGame_ensureSave).
#pragma once

#include "session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <wchar.h>


// The launcher script's file name, next to the installed dylib.
#define MACOS_GAME_LAUNCHER "launch-fm24.sh"

int macosGame_install(int argc, wchar_t **argv);
int macosGame_launch(int argc, wchar_t **argv);

// Before a command's job for `need->save`: waits until the running game has that save loaded and
// idle, or starts the game with it if no game is running, as session_plan says. Returns 0 to submit
// the job, with the save's in-game date in `date` if given ("" if unknown, or for another save),
// else the exit code. Prints what it does.
int macosGame_ensureSave(const SessionNeed *need, char *date, size_t size);
// As macosGame_ensureSave, also giving how the wait for the save ended in `outcome` if given
// (SESSION_WAITING if there was none), as a batch tells a busy game from another save.
int macosGame_checkSave(const SessionNeed *need, char *date, size_t size, SessionOutcome *outcome);

// Installs the current dylib and launcher (quietly if they are already current), and deletes the
// auto-play and Rosetta requests and files next to them that no running process needs. Writes the
// launcher's path to `launcher`. Prints why and returns false on failure.
bool macosGame_installHarness(bool verbose, char *launcher, size_t size);

// True if the Steam account that signed in last gives Football Manager 2024 a launch option that
// runs `launcher` with the game's command, as Steam has saved it.
bool macosGame_launchOptionSet(const char *launcher);

typedef enum MacosDataFolder {
	MACOS_DATA_FOUND,
	MACOS_DATA_UNCLEAR, // Steam's launch option sets it in a way fm24ctl does not follow
	MACOS_DATA_NO_HOME, // HOME is not set, or the path does not fit
} MacosDataFolder;

// FM24's data folder for the game the harness's launcher starts, where a command's job or a batch
// looks its save up: the one Steam's launch option gives the game when the option runs the
// launcher, else the usual one in the home folder. Prints nothing.
MacosDataFolder macosGame_dataFolder(char *out, size_t size);

// The running Football Manager 2024's process ID (0 if it is not running) and executable path.
pid_t macosGame_findRunning(char *path, size_t size);

// True if process `pid` runs under Rosetta; false if it does not, or has exited.
bool macosGame_underRosetta(pid_t pid);

// True if a running game serves this protocol's harness: its harness holds the socket's lock.
bool macosGame_harnessListening(void);

// True if process `pid` is running.
bool macosGame_running(pid_t pid);

// Quits the game, process `pid`, for a batch whose plan allows it (kill_game=1): SIGTERM, up to
// `graceS` seconds to quit, then SIGKILL, then until its harness's lock is free. Leaves any other
// process alone. True once it has gone; prints why not.
bool macosGame_stop(pid_t pid, unsigned graceS);

// The memory process `pid` uses, its physical footprint (Activity Monitor's Memory column), or 0
// if it cannot be read.
uint64_t macosGame_residentBytes(pid_t pid);

// The installed fm.app, in the Steam library that holds app 2252570.
bool macosGame_findInstalled(char *app, size_t size);

// True if the executable at `path` belongs to a Football Manager 2024 bundle.
bool macosGame_isGameExecutable(const char *path);

// The fm.app bundle `path` names: the bundle or its Contents/MacOS/fm, trailing slashes allowed.
// False unless it is Football Manager 2024's.
bool macosGame_bundle(const char *path, char *app, size_t size);
