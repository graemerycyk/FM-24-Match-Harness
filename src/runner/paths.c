// SPDX-FileCopyrightText: © 2026 Phil Armstead <philarmstead@mailbox.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "paths.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>


bool paths_executableDirectory(wchar_t *out, size_t size) {
	if (!size || size > MAXDWORD) {
		return false;
	}
	DWORD length = GetModuleFileNameW(NULL, out, (DWORD)size);
	if (length == 0 || length >= size) {
		return false;
	}
	wchar_t *slash = wcsrchr(out, L'\\');
	if (!slash) {
		return false;
	}
	*slash = 0;
	return true;
}

bool paths_jobDirectory(wchar_t *out, size_t size) {
	return paths_executableDirectory(out, size);
}

bool paths_join(wchar_t *out, size_t size, const wchar_t *directory, const wchar_t *name) {
	int written = swprintf(out, size, L"%ls\\%ls", directory, name);
	return written > 0 && (size_t)written < size;
}

bool paths_absolute(const wchar_t *path, wchar_t *out, size_t size) {
	if (!size || size > MAXDWORD) {
		return false;
	}
	DWORD length = GetFullPathNameW(path, (DWORD)size, out, NULL);
	return length > 0 && length < size;
}

PathsFolder paths_parentFolder(const wchar_t *path) {
	wchar_t *folder = _wcsdup(path);
	wchar_t *slash = folder ? wcsrchr(folder, L'\\') : NULL;
	wchar_t *forward = folder ? wcsrchr(folder, L'/') : NULL;
	if (forward && (!slash || forward > slash)) {
		slash = forward;
	}
	PathsFolder found = PATHS_FOLDER_UNCHECKED;
	if (slash) {
		slash[slash == folder || slash[-1] == L':' ? 1 : 0] = 0; // "C:\name" is in "C:\"
		DWORD attributes = GetFileAttributesW(folder);
		DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
		if (attributes != INVALID_FILE_ATTRIBUTES) {
			found =
				(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? PATHS_FOLDER_FOUND : PATHS_FOLDER_NOT_FOLDER;
			DWORD own = found == PATHS_FOLDER_FOUND ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
			if (own != INVALID_FILE_ATTRIBUTES && (own & FILE_ATTRIBUTE_DIRECTORY) != 0) {
				found = PATHS_FILE_IS_FOLDER; // which "wb" cannot open
			}
		} else if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
			found = PATHS_FOLDER_MISSING;
		} else if (error == ERROR_DIRECTORY) {
			found = PATHS_FOLDER_NOT_FOLDER;
		} else if (error == ERROR_ACCESS_DENIED) {
			found = PATHS_FOLDER_DENIED;
		}
	}
	free(folder);
	return found;
}

bool paths_toUtf8(const wchar_t *text, char *out, size_t size) {
	return size > 0 && size <= INT_MAX &&
				 WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, (int)size, NULL, NULL) >
					 0;
}
