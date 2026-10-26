// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "save_list.h"

#include "buffer.h"
#include "macos_game.h"
#include "saves.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysctl.h>


// As in macos_game.c.
enum { PATH_LENGTH = 4096, VDF_DEPTH = 8, VDF_KEY = 64 };

static const char steamAppId[] = "2252570";
static const unsigned long long steamIdBase = 76561197960265728ULL; // SteamID64 of account ID 0
static const char dataFolderPath[] =
	"Library/Application Support/Sports Interactive/Football Manager 2024"; // under the home folder
static const char harnessFolderPath[] = "Library/Application Support/fm24-harness";
static const char extension[] = ".fm";

typedef struct SaveEntry {
	char *name;
	char *path;
	unsigned folder;
	struct timespec modified;
} SaveEntry;

struct SaveList {
	SaveListData data;
	char folder[PATH_LENGTH];
	SaveEntry *entries;
	size_t count;
	size_t capacity;
};

// A `"key" "value"` pair of a Valve KeyValues text, with the keys of the blocks it is in.
typedef struct VdfPair {
	const char (*blocks)[VDF_KEY]; // outermost first
	size_t depth;
	const char *key;
	const char *value;
} VdfPair;
typedef bool VdfVisit(void *context, const VdfPair *pair); // false to stop

// The value of a block's key: the launch option.
typedef struct VdfLookup {
	char *out;
	size_t size;
	bool found;
} VdfLookup;

// The Steam account that signed in last, from loginusers.vdf.
typedef struct AccountSearch {
	unsigned long long best; // a SteamID64
	unsigned long long bestTimestamp;
	bool mostRecent;
} AccountSearch;

static size_t copyOut(const char *text, char *out, size_t size);
static SaveListData dataFolder(const char *home, char *out, size_t size);
static bool throughSteam(const char *home, char *option, size_t size);
static bool readFile(const char *path, ByteBuffer *out);
static const char *vdfSkip(const char *c);
static char vdfUnescape(char c);
static const char *vdfString(const char *c, char *out, size_t size);
static int vdfToken(const char **cursor, char *out, size_t size);
static void vdfVisit(const char *text, VdfVisit *visit, void *context);
static bool vdfVisitFile(const char *path, VdfVisit *visit, void *context);
static bool visitLoginUser(void *context, const VdfPair *pair);
static bool currentAccount(const char *root, char *out, size_t size);
static bool visitLaunchOption(void *context, const VdfPair *pair);
static bool readLaunchOption(const char *home, char *option, size_t size);
static bool optionWord(const char **cursor, char *out, size_t size);
static SaveListData optionDataFolder(const char *option, char *out, size_t size);
static bool listFolder(SaveList *list, unsigned folder);
static bool addEntry(
	SaveList *list,
	const char *name,
	const char *path,
	unsigned folder,
	const struct stat *status
);
static int newestFirst(const void *first, const void *second);

SaveList *saveList_read(const char *home) {
	SaveList *list = calloc(1, sizeof *list);
	if (!list) {
		return NULL;
	}
	list->data = dataFolder(home, list->folder, sizeof list->folder);
	if (list->data == SAVE_LIST_UNCLEAR || list->data == SAVE_LIST_NO_HOME) {
		list->folder[0] = 0;
		return list;
	}
	for (unsigned folder = 0; folder < SAVES_FOLDER_COUNT; ++folder) {
		if (!listFolder(list, folder)) {
			saveList_free(list);
			return NULL;
		}
	}
	if (list->count > 1) {
		qsort(list->entries, list->count, sizeof list->entries[0], newestFirst);
	}
	return list;
}

void saveList_free(SaveList *list) {
	if (!list) {
		return;
	}
	for (size_t i = 0; i < list->count; ++i) {
		free(list->entries[i].name);
		free(list->entries[i].path);
	}
	free(list->entries);
	free(list);
}

SaveListData saveList_data(const SaveList *list) {
	return list->data;
}

size_t saveList_dataFolder(const SaveList *list, char *out, size_t size) {
	return copyOut(list->folder, out, size);
}

size_t saveList_count(const SaveList *list) {
	return list->count;
}

size_t saveList_name(const SaveList *list, size_t index, char *out, size_t size) {
	return copyOut(index < list->count ? list->entries[index].name : "", out, size);
}

size_t saveList_path(const SaveList *list, size_t index, char *out, size_t size) {
	return copyOut(index < list->count ? list->entries[index].path : "", out, size);
}

unsigned saveList_folder(const SaveList *list, size_t index) {
	return index < list->count ? list->entries[index].folder : 0;
}

int64_t saveList_modified(const SaveList *list, size_t index) {
	return index < list->count ? (int64_t)list->entries[index].modified.tv_sec : 0;
}

bool saveList_launchOptionSet(const char *home) {
	char option[PATH_LENGTH];
	return home && home[0] == '/' && throughSteam(home, option, sizeof option);
}

bool saveList_isName(const char *text) {
	return saves_isName(text);
}

bool saveList_appleSilicon(void) {
	int arm64 = 0;
	size_t size = sizeof arm64;
	return sysctlbyname("hw.optional.arm64", &arm64, &size, NULL, 0) == 0 && arm64 == 1;
}

static size_t copyOut(const char *text, char *out, size_t size) {
	int written = snprintf(out, size, "%s", text);
	if (written < 0) {
		if (size) {
			out[0] = 0;
		}
		return 0;
	}
	return (size_t)written;
}

// FM24's data folder, as fm24ctl's dataFolder finds it without --user-data: the one Steam's launch
// option gives the game if that option runs the harness's launcher, so that `launch` starts the
// game through Steam (launchOptionSet), else the usual one in the home folder.
static SaveListData dataFolder(const char *home, char *out, size_t size) {
	out[0] = 0;
	if (!home || home[0] != '/') {
		return SAVE_LIST_NO_HOME;
	}
	char option[PATH_LENGTH];
	SaveListData given = throughSteam(home, option, sizeof option)
												 ? optionDataFolder(option, out, size)
												 : SAVE_LIST_USUAL;
	if (given != SAVE_LIST_USUAL) {
		return given;
	}
	int written = snprintf(out, size, "%s/%s", home, dataFolderPath);
	if (written <= 0 || (size_t)written >= size) {
		out[0] = 0;
		return SAVE_LIST_NO_HOME;
	}
	return SAVE_LIST_USUAL;
}

// True if the launch option, read into `option`, runs the harness's launcher in absolute `home`
// with the game's command (fm24ctl's launchOptionSet).
static bool throughSteam(const char *home, char *option, size_t size) {
	char launcher[PATH_LENGTH];
	int written =
		snprintf(launcher, sizeof launcher, "%s/%s/%s", home, harnessFolderPath, MACOS_GAME_LAUNCHER);
	return written > 0 && (size_t)written < sizeof launcher && readLaunchOption(home, option, size) &&
				 strstr(option, launcher) && strstr(option, "%command%");
}

// A whole file, as files_read reads it for macos_game.c (whose 64 KiB buffer is too big for the
// stacks of the viewer's worker threads).
static bool readFile(const char *path, ByteBuffer *out) {
	buffer_clear(out);
	FILE *file = fopen(path, "rb");
	if (!file) {
		return false;
	}
	uint8_t chunk[1 << 12];
	bool ok = true;
	for (;;) {
		size_t got = fread(chunk, 1, sizeof chunk, file);
		if (got > 0 && !buffer_append(out, chunk, got)) {
			ok = false;
			break;
		}
		if (got < sizeof chunk) {
			ok = !ferror(file);
			break;
		}
	}
	return fclose(file) == 0 && ok;
}

// Skips white space and // comments.
static const char *vdfSkip(const char *c) {
	for (;;) {
		while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') {
			++c;
		}
		if (c[0] != '/' || c[1] != '/') {
			return c;
		}
		while (*c && *c != '\n') {
			++c;
		}
	}
}

static char vdfUnescape(char c) {
	switch (c) {
		case 'n': return '\n';
		case 't': return '\t';
		default: return c;
	}
}

// Copies a string token at `c` (quoted, or bare up to white space, a brace or a quote) into `out`,
// unescaped and cut to fit. Returns the position after it.
static const char *vdfString(const char *c, char *out, size_t size) {
	size_t length = 0;
	bool quoted = *c == '"';
	for (c += quoted; *c && (quoted ? *c != '"' : !strchr(" \t\r\n{}\"", *c)); ++c) {
		char next = *c;
		if (quoted && next == '\\' && c[1]) {
			next = vdfUnescape(*++c);
		}
		if (length + 1 < size) {
			out[length++] = next;
		}
	}
	out[length] = 0;
	return c + (quoted && *c == '"');
}

// Reads the next token of a Valve KeyValues text at `*cursor`: 's' for a string (unescaped into
// `out`, cut to fit), '{', '}', '[' for a platform condition, or 0 at the end.
static int vdfToken(const char **cursor, char *out, size_t size) {
	const char *c = vdfSkip(*cursor);
	int kind = *c == '{' || *c == '}' || *c == '[' ? *c : *c ? 's' : 0;
	out[0] = 0;
	*cursor = kind == '{' || kind == '}' ? c + 1 : kind ? vdfString(c, out, size) : c;
	return kind;
}

// Calls `visit` for each `"key" "value"` pair of a Valve KeyValues text, in order, until it returns
// false.
static void vdfVisit(const char *text, VdfVisit *visit, void *context) {
	char blocks[VDF_DEPTH][VDF_KEY];
	char key[VDF_KEY];
	char value[PATH_LENGTH];
	size_t depth = 0;
	size_t skipped = 0; // blocks deeper than VDF_DEPTH
	bool keyed = false;
	const char *cursor = text;
	for (int kind = vdfToken(&cursor, value, sizeof value); kind;
		kind = vdfToken(&cursor, value, sizeof value)) {
		if (kind == 's' && !keyed) {
			(void)snprintf(key, sizeof key, "%s", value);
			keyed = true;
		} else if (kind == 's') {
			VdfPair pair = {blocks, depth, key, value};
			if (!skipped && !visit(context, &pair)) {
				return;
			}
			keyed = false;
		} else if (kind == '{') {
			if (skipped || depth == VDF_DEPTH) {
				++skipped;
			} else {
				(void)snprintf(blocks[depth++], VDF_KEY, "%s", keyed ? key : "");
			}
			keyed = false;
		} else if (kind == '}') {
			if (skipped) {
				--skipped;
			} else if (depth) {
				--depth;
			}
			keyed = false;
		}
	}
}

static bool vdfVisitFile(const char *path, VdfVisit *visit, void *context) {
	ByteBuffer text = {0};
	bool read = readFile(path, &text);
	if (read) {
		vdfVisit(buffer_string(&text), visit, context);
	}
	buffer_free(&text);
	return read;
}

// loginusers.vdf's "users" > SteamID64 > "MostRecent" or the latest "Timestamp".
static bool visitLoginUser(void *context, const VdfPair *pair) {
	AccountSearch *search = context;
	if (pair->depth != 2 || strcasecmp(pair->blocks[0], "users") != 0) {
		return true;
	}
	unsigned long long id = strtoull(pair->blocks[1], NULL, 10);
	if (strcasecmp(pair->key, "MostRecent") == 0 && strcmp(pair->value, "1") == 0) {
		search->best = id;
		search->mostRecent = true;
	} else if (strcasecmp(pair->key, "Timestamp") == 0 && !search->mostRecent) {
		unsigned long long timestamp = strtoull(pair->value, NULL, 10);
		if (timestamp >= search->bestTimestamp) {
			search->best = id;
			search->bestTimestamp = timestamp;
		}
	}
	return true;
}

// The userdata folder name (account ID) of the Steam account whose settings apply: the only one,
// else the one that signed in last.
static bool currentAccount(const char *root, char *out, size_t size) {
	char users[PATH_LENGTH];
	int written = snprintf(users, sizeof users, "%s/userdata", root);
	DIR *directory = written > 0 && (size_t)written < sizeof users ? opendir(users) : NULL;
	if (!directory) {
		return false;
	}
	int accounts = 0;
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
		if (entry->d_name[0] >= '1' && entry->d_name[0] <= '9' &&
				strspn(entry->d_name, "0123456789") == strlen(entry->d_name) && accounts++ == 0) {
			(void)snprintf(out, size, "%s", entry->d_name);
		}
	}
	closedir(directory);
	if (accounts <= 1) {
		return accounts == 1;
	}
	char logins[PATH_LENGTH];
	written = snprintf(logins, sizeof logins, "%s/config/loginusers.vdf", root);
	AccountSearch search = {0};
	if (written <= 0 || (size_t)written >= sizeof logins ||
			!vdfVisitFile(logins, visitLoginUser, &search) || search.best <= steamIdBase) {
		return false;
	}
	written = snprintf(out, size, "%llu", search.best - steamIdBase);
	return written > 0 && (size_t)written < size;
}

// The launch option of app 2252570 ("apps" > "2252570" > "LaunchOptions").
static bool visitLaunchOption(void *context, const VdfPair *pair) {
	VdfLookup *lookup = context;
	if (pair->depth >= 2 && strcasecmp(pair->key, "LaunchOptions") == 0 &&
			strcmp(pair->blocks[pair->depth - 1], steamAppId) == 0 &&
			strcasecmp(pair->blocks[pair->depth - 2], "apps") == 0) {
		int written = snprintf(lookup->out, lookup->size, "%s", pair->value);
		lookup->found = written > 0 && (size_t)written < lookup->size;
		return false;
	}
	return true;
}

// The current Steam account's launch option for Football Manager 2024, if it has one.
static bool readLaunchOption(const char *home, char *option, size_t size) {
	char root[PATH_LENGTH];
	char account[64];
	char config[PATH_LENGTH];
	int written = snprintf(root, sizeof root, "%s/Library/Application Support/Steam", home);
	if (written <= 0 || (size_t)written >= sizeof root ||
			!currentAccount(root, account, sizeof account)) {
		return false;
	}
	written = snprintf(config, sizeof config, "%s/userdata/%s/config/localconfig.vdf", root, account);
	VdfLookup lookup = {option, size, false};
	return written > 0 && (size_t)written < sizeof config &&
				 vdfVisitFile(config, visitLaunchOption, &lookup) && lookup.found;
}

// The next word of a launch option at `*cursor`, split and unquoted as a shell does: white space
// separates words, '...' and "..." quote, and a backslash escapes the next character (in "...",
// only ", \, $ and `). `out` must have room for the whole option. False at the end.
static bool optionWord(const char **cursor, char *out, size_t size) {
	const char *c = *cursor + strspn(*cursor, " \t\r\n");
	if (!*c) {
		*cursor = c;
		return false;
	}
	size_t length = 0;
	char quote = 0;
	for (; *c && (quote || !strchr(" \t\r\n", *c)); ++c) {
		char next = *c;
		if (quote != '\'' && next == '\\' && c[1] && (!quote || strchr("\"\\$`", c[1]))) {
			next = *++c;
		} else if (quote ? next == quote : next == '"' || next == '\'') {
			quote = quote ? 0 : next;
			continue;
		}
		if (length + 1 < size) {
			out[length++] = next;
		}
	}
	out[length] = 0;
	*cursor = c;
	return true;
}

// The data folder that Steam's launch option `option` gives the game: `-user_data_location=<dir>`
// or `--user_data_location=<dir>` (SAVE_LIST_STEAM), none (SAVE_LIST_USUAL), or any other mention
// of the setting, a folder that is not absolute or two different folders (SAVE_LIST_UNCLEAR).
static SaveListData optionDataFolder(const char *option, char *out, size_t size) {
	static const char setting[] = "user_data_location=";
	char word[PATH_LENGTH];
	SaveListData found = SAVE_LIST_USUAL;
	for (const char *cursor = option; optionWord(&cursor, word, sizeof word);) {
		if (!strstr(word, "user_data_location")) {
			continue;
		}
		const char *name = word + strspn(word, "-");
		const char *value = name + sizeof setting - 1;
		bool named = name > word && name <= word + 2 && strncmp(name, setting, sizeof setting - 1) == 0;
		if (!named || value[0] != '/' || (found == SAVE_LIST_STEAM && strcmp(out, value) != 0)) {
			out[0] = 0;
			return SAVE_LIST_UNCLEAR;
		}
		int written = snprintf(out, size, "%s", value);
		if (written <= 0 || (size_t)written >= size) {
			out[0] = 0;
			return SAVE_LIST_UNCLEAR;
		}
		found = SAVE_LIST_STEAM;
	}
	return found;
}

// Adds the saves in one of the data folder's save folders: the regular files, or links to them,
// whose names end in ".fm" after at least one character (as saves.c's search takes them) and fit
// fm24ctl's names and paths. False only if memory runs out.
static bool listFolder(SaveList *list, unsigned folder) {
	char path[PATH_LENGTH];
	int written = snprintf(path, sizeof path, "%s/%s", list->folder, saves_folderName(folder));
	DIR *directory = written > 0 && (size_t)written < sizeof path ? opendir(path) : NULL;
	if (!directory) {
		return true;
	}
	bool ok = true;
	// NOLINTNEXTLINE(concurrency-mt-unsafe): the directory stream is this function's own.
	for (struct dirent *entry = readdir(directory); ok && entry; entry = readdir(directory)) {
		size_t length = strlen(entry->d_name);
		size_t suffix = sizeof extension - 1;
		char file[PATH_LENGTH];
		char name[SAVES_NAME_LENGTH];
		struct stat status;
		written = snprintf(file, sizeof file, "%s/%s", path, entry->d_name);
		bool save = length > suffix && strcmp(entry->d_name + length - suffix, extension) == 0 &&
								written > 0 && (size_t)written < sizeof file && stat(file, &status) == 0 &&
								S_ISREG(status.st_mode) && saves_nameOfPath(entry->d_name, name, sizeof name);
		if (save) {
			ok = addEntry(list, name, file, folder, &status);
		}
	}
	closedir(directory);
	return ok;
}

static bool addEntry(
	SaveList *list,
	const char *name,
	const char *path,
	unsigned folder,
	const struct stat *status
) {
	if (list->count == list->capacity) {
		size_t capacity = list->capacity ? 2 * list->capacity : 16;
		SaveEntry *entries = realloc(list->entries, capacity * sizeof *entries);
		if (!entries) {
			return false;
		}
		list->entries = entries;
		list->capacity = capacity;
	}
	SaveEntry entry = {
		.name = strdup(name),
		.path = strdup(path),
		.folder = folder,
		.modified = status->st_mtimespec,
	};
	if (!entry.name || !entry.path) {
		free(entry.name);
		free(entry.path);
		return false;
	}
	list->entries[list->count++] = entry;
	return true;
}

// The newest first, then cloud/games before games, then by name.
static int newestFirst(const void *first, const void *second) {
	const SaveEntry *a = first;
	const SaveEntry *b = second;
	if (a->modified.tv_sec != b->modified.tv_sec) {
		return a->modified.tv_sec > b->modified.tv_sec ? -1 : 1;
	}
	if (a->modified.tv_nsec != b->modified.tv_nsec) {
		return a->modified.tv_nsec > b->modified.tv_nsec ? -1 : 1;
	}
	if (a->folder != b->folder) {
		return a->folder < b->folder ? -1 : 1;
	}
	return strcmp(a->name, b->name);
}
