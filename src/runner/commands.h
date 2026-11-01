// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// The runner's commands; each returns the process exit code (0 ok, 1 failed, 2 usage, 3 the game is
// not in the state the command needs).
#pragma once

#include "edition.h"

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>


// How replace-home and replace-away take the team, in their usage and their prompt for one. Where
// the game's builds' clubs' names have not been located (the engine plugin's, so far), the harness
// finds a team by its unique ID only.
#if EDITION_ENGINE_PLUGIN
#define COMMANDS_TEAM_ARGUMENT "<team unique ID>"
#define COMMANDS_TEAM_PROMPT "Enter a team's unique ID."
#else
#define COMMANDS_TEAM_ARGUMENT "<team ID or club name>"
#define COMMANDS_TEAM_PROMPT "Enter a team ID or club short name (up to 255 UTF-8 bytes)."
#endif

unsigned long commands_defaultThreads(void);
int commands_run(int argc, wchar_t **argv);
int commands_runWithProgress(int argc, wchar_t **argv, uintptr_t progressWindow);
int commands_getNextFixture(int argc, wchar_t **argv);
int commands_encode(int argc, wchar_t **argv);
int commands_decode(int argc, wchar_t **argv);
int commands_replaceTeam(int argc, wchar_t **argv);
// As above, also returning a failure message for the viewer.
int commands_replaceTeamWithResult(int argc, wchar_t **argv, char *result, size_t resultSize);
int commands_listTeams(int argc, wchar_t **argv);
// As above, also returning the outcome line for the viewer.
int commands_listTeamsWithResult(int argc, wchar_t **argv, char *result, size_t resultSize);
int commands_listPlayers(int argc, wchar_t **argv);
// As above, also returning the outcome line for the viewer.
int commands_listPlayersWithResult(int argc, wchar_t **argv, char *result, size_t resultSize);
int commands_getPlayer(int argc, wchar_t **argv);
int commands_injectPlayer(int argc, wchar_t **argv);
// As above, also returning the outcome line for the viewer.
int commands_injectPlayerWithResult(int argc, wchar_t **argv, char *result, size_t resultSize);
int commands_status(int argc, wchar_t **argv);
int commands_probe(int argc, wchar_t **argv); // probe.c
