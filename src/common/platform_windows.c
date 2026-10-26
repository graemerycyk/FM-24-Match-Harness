// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>


typedef struct ThreadStart {
	void (*run)(void *argument);
	void *argument;
} ThreadStart;

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

static DWORD WINAPI threadMain(LPVOID parameter) {
	ThreadStart start = *(ThreadStart *)parameter;
	free(parameter);
	start.run(start.argument);
	return 0;
}
