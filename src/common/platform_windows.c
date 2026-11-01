// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.h"

#include <io.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>


typedef struct ThreadStart {
	void (*run)(void *argument);
	void *argument;
} ThreadStart;

static void printFailure(const char *action, const wchar_t *path, DWORD error);
static DWORD WINAPI threadMain(LPVOID parameter);

FILE *platform_openFile(const wchar_t *path, const char *mode) {
	wchar_t wideMode[8];
	size_t length = strlen(mode);
	if (length >= sizeof wideMode / sizeof wideMode[0]) {
		return NULL;
	}
	for (size_t i = 0; i <= length; ++i) {
		wideMode[i] = (wchar_t)(unsigned char)mode[i];
	}
	return _wfopen(path, wideMode);
}

bool platform_removeFile(const wchar_t *path) {
	if (DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND) {
		return true;
	}
	DWORD error = GetLastError();
	char text[4096];
	fprintf(
		stderr,
		"cannot remove %s (Windows error %lu)\n",
		platform_toUtf8(path, text, sizeof text) ? text : "a temporary file",
		error
	);
	return false;
}

bool platform_syncFile(FILE *file) {
	if (fflush(file) != 0) {
		return false;
	}
	intptr_t handle = _get_osfhandle(_fileno(file));
	return handle != -1 && FlushFileBuffers((HANDLE)handle);
}

bool platform_syncDirectory(const wchar_t *path) {
	(void)path; // NTFS journals its directory entries; MoveFileExW writes through
	return true;
}

bool platform_replaceFile(const wchar_t *from, const wchar_t *to) {
	if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		return true;
	}
	DWORD error = GetLastError();
	char source[4096];
	char target[4096];
	fprintf(
		stderr,
		"cannot rename %s to %s (Windows error %lu)\n",
		platform_toUtf8(from, source, sizeof source) ? source : "a file",
		platform_toUtf8(to, target, sizeof target) ? target : "another",
		error
	);
	return false;
}

bool platform_renameFolder(const wchar_t *from, const wchar_t *to) {
	// MOVEFILE_REPLACE_EXISTING cannot be used with a folder.
	if (MoveFileExW(from, to, MOVEFILE_WRITE_THROUGH)) {
		return true;
	}
	DWORD error = GetLastError();
	printFailure("rename the folder", from, error);
	return false;
}

bool platform_makeDirectory(const wchar_t *path) {
	if (CreateDirectoryW(path, NULL)) {
		return true;
	}
	DWORD error = GetLastError();
	DWORD attributes = GetFileAttributesW(path);
	if (error == ERROR_ALREADY_EXISTS && attributes != INVALID_FILE_ATTRIBUTES &&
			(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		return true;
	}
	printFailure("create the folder", path, error);
	return false;
}

bool platform_listDirectory(const wchar_t *path, PlatformDirectoryVisit *visit, void *context) {
	size_t length = wcslen(path);
	wchar_t *pattern = malloc((length + 3) * sizeof *pattern);
	if (!pattern) {
		printFailure("read the folder", path, ERROR_NOT_ENOUGH_MEMORY);
		return false;
	}
	wmemcpy(pattern, path, length);
	wmemcpy(pattern + length, L"\\*", 3);
	WIN32_FIND_DATAW entry;
	HANDLE search = FindFirstFileW(pattern, &entry);
	free(pattern);
	if (search == INVALID_HANDLE_VALUE) {
		printFailure("read the folder", path, GetLastError());
		return false;
	}
	bool ok = true;
	do {
		const wchar_t *name = entry.cFileName;
		if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) {
			continue;
		}
		DWORD attributes = entry.dwFileAttributes;
		bool isDirectory =
			(attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
		ok = visit(context, name, isDirectory);
	} while (ok && FindNextFileW(search, &entry));
	DWORD error = ok ? GetLastError() : ERROR_NO_MORE_FILES;
	FindClose(search);
	if (error != ERROR_NO_MORE_FILES) {
		printFailure("read the folder", path, error);
		return false;
	}
	return ok;
}

bool platform_removeDirectory(const wchar_t *path) {
	if (RemoveDirectoryW(path)) {
		return true;
	}
	DWORD error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
		return true;
	}
	printFailure("remove the folder", path, error);
	return false;
}

bool platform_freeBytes(const wchar_t *path, uint64_t *bytes) {
	ULARGE_INTEGER available;
	if (!GetDiskFreeSpaceExW(path, &available, NULL, NULL)) {
		printFailure("read the free space of", path, GetLastError());
		return false;
	}
	*bytes = available.QuadPart;
	return true;
}

PlatformLockResult platform_lockFile(const wchar_t *path, PlatformFileLock *lock) {
	HANDLE file = CreateFileW(
		path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL
	);
	if (file != INVALID_HANDLE_VALUE) {
		*lock = file;
		return PLATFORM_LOCK_TAKEN;
	}
	DWORD error = GetLastError();
	if (error == ERROR_SHARING_VIOLATION) {
		return PLATFORM_LOCK_HELD;
	}
	printFailure("lock", path, error);
	return PLATFORM_LOCK_FAILED;
}

void platform_unlockFile(PlatformFileLock lock) {
	CloseHandle(lock);
}

bool platform_toUtf8(const wchar_t *text, char *out, size_t size) {
	return size > 0 && size <= INT_MAX &&
				 WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, (int)size, NULL, NULL) >
					 0;
}

bool platform_fromUtf8(const char *text, wchar_t *out, size_t size) {
	return size > 0 && size <= INT_MAX &&
				 MultiByteToWideChar(CP_UTF8, 0, text, -1, out, (int)size) > 0;
}

int platform_compareIgnoringCase(const char *a, const char *b) {
	return _stricmp(a, b);
}

int platform_compareIgnoringCaseN(const char *a, const char *b, size_t length) {
	return _strnicmp(a, b, length);
}

int platform_compareWideIgnoringCase(const wchar_t *a, const wchar_t *b) {
	return _wcsicmp(a, b);
}

unsigned long platform_processId(void) {
	return GetCurrentProcessId();
}

unsigned long platform_processorCount(void) {
	return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

uint64_t platform_ticks(void) {
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return (uint64_t)counter.QuadPart;
}

uint64_t platform_tickFrequency(void) {
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	return (uint64_t)frequency.QuadPart;
}

uint64_t platform_uptimeTicks(void) {
	ULONGLONG units = 0; // 100 ns, without the time asleep
	QueryUnbiasedInterruptTime(&units);
	uint64_t frequency = platform_tickFrequency();
	return units / 10000000U * frequency + units % 10000000U * frequency / 10000000U;
}

uint64_t platform_threadCpuTime(void) {
	FILETIME created;
	FILETIME exited;
	FILETIME kernel;
	FILETIME user;
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
		return 0;
	}
	ULARGE_INTEGER kernelTime = {.LowPart = kernel.dwLowDateTime, .HighPart = kernel.dwHighDateTime};
	ULARGE_INTEGER userTime = {.LowPart = user.dwLowDateTime, .HighPart = user.dwHighDateTime};
	return kernelTime.QuadPart + userTime.QuadPart;
}

void platform_timeOfDay(int *hour, int *minute, int *second, int *millisecond) {
	SYSTEMTIME time;
	GetLocalTime(&time);
	*hour = time.wHour;
	*minute = time.wMinute;
	*second = time.wSecond;
	*millisecond = time.wMilliseconds;
}

bool platform_utcTime(time_t seconds, struct tm *out) {
	return gmtime_s(out, &seconds) == 0;
}

void platform_sleep(unsigned milliseconds) {
	Sleep(milliseconds);
}

void platform_initLock(PlatformLock *lock) {
	InitializeCriticalSection(lock);
}

void platform_lock(PlatformLock *lock) {
	EnterCriticalSection(lock);
}

void platform_unlock(PlatformLock *lock) {
	LeaveCriticalSection(lock);
}

void platform_destroyLock(PlatformLock *lock) {
	DeleteCriticalSection(lock);
}

bool platform_startThread(PlatformThread *thread, void (*run)(void *argument), void *argument) {
	ThreadStart *start = malloc(sizeof *start);
	if (!start) {
		return false;
	}
	*start = (ThreadStart){.run = run, .argument = argument};
	*thread = CreateThread(NULL, 0, threadMain, start, 0, NULL);
	if (!*thread) {
		free(start);
		return false;
	}
	return true;
}

void platform_joinThreads(PlatformThread *threads, size_t count) {
	if (!count) {
		return;
	}
	WaitForMultipleObjects((DWORD)count, threads, TRUE, INFINITE);
	for (size_t i = 0; i < count; ++i) {
		CloseHandle(threads[i]);
	}
}

static void printFailure(const char *action, const wchar_t *path, DWORD error) {
	char text[4096];
	fprintf(
		stderr,
		"cannot %s %s (Windows error %lu)\n",
		action,
		platform_toUtf8(path, text, sizeof text) ? text : "a path that is not valid text",
		error
	);
}

static DWORD WINAPI threadMain(LPVOID parameter) {
	ThreadStart start = *(ThreadStart *)parameter;
	free(parameter);
	start.run(start.argument);
	return 0;
}
