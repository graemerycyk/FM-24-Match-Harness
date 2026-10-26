// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// What the macOS viewer's Launch FM24 sheet offers: the saves `fm24ctl launch --save` finds, the
// .fm files in the cloud/games and games folders of FM24's data folder, and Rosetta on Apple
// silicon. The data folder is the one Steam's launch option gives the game with user_data_location
// when the option runs the harness's launcher (as `launch` then starts the game through Steam),
// else the usual one in the home folder. This mirrors macos_game.c's dataFolder and listSaves
// without --user-data, reading Steam's settings the same way; tests/macos_test.c checks that both
// find the same saves in the same folders. Swift imports this header (see module.modulemap), so it
// uses only standard headers. Nothing here writes a file or contacts the game.
//
// Text is UTF-8. A function that fills `out` returns the text's full length, as snprintf does,
// and writes at most `size` bytes including the NUL.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// Which data folder the saves are in.
typedef enum SaveListData {
	SAVE_LIST_USUAL,   // ~/Library/Application Support/Sports Interactive/Football Manager 2024
	SAVE_LIST_STEAM,   // the folder Steam's launch option gives the game
	SAVE_LIST_UNCLEAR, // the launch option sets it in a way fm24ctl does not follow: no saves
	SAVE_LIST_NO_HOME, // the home folder is not an absolute path: no saves
} SaveListData;

// The saves found, newest first (by modification time, then cloud/games before games, then name).
typedef struct SaveList SaveList;

// Lists the saves of the user whose home folder is `home`. NULL only if memory runs out.
SaveList *saveList_read(const char *home);
void saveList_free(SaveList *list);

SaveListData saveList_data(const SaveList *list);
// The data folder ("" for SAVE_LIST_UNCLEAR and SAVE_LIST_NO_HOME).
size_t saveList_dataFolder(const SaveList *list, char *out, size_t size);
size_t saveList_count(const SaveList *list);
// The save's name as fm24ctl reports it (its file name without .fm), its .fm file, the folder it
// is in (0 for cloud/games, 1 for games: saves.h's SavesFolder) and when it was last written, in
// seconds since 1970. Out of range gives "", 0 and 0.
size_t saveList_name(const SaveList *list, size_t index, char *out, size_t size);
size_t saveList_path(const SaveList *list, size_t index, char *out, size_t size);
unsigned saveList_folder(const SaveList *list, size_t index);
int64_t saveList_modified(const SaveList *list, size_t index);

// True if Steam's launch option for FM24, as the Steam account that signed in last has it saved,
// runs the harness's launcher in `home` with the game's command, as fm24ctl's launchOptionSet
// finds: `fm24ctl launch` then starts the game through Steam (the tested route), and otherwise
// opens it directly.
bool saveList_launchOptionSet(const char *home);

// True if `text` is a save's name for `launch --save`, which it looks up in the save folders,
// rather than the path of its .fm file: it has no "/" and does not end with .fm (saves_isName).
bool saveList_isName(const char *text);

// True on Apple silicon, where the sheet offers the game's x86_64 slice under Rosetta: sysctl's
// hw.optional.arm64, which it also reports to a process under Rosetta, as fm24ctl's launch reads
// it.
bool saveList_appleSilicon(void);
