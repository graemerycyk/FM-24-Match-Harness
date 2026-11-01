// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The operating-system services the shared code uses: files and directories by wide path, text
// conversions and comparisons, clocks, threads and locks. platform_windows.c implements them for
// Windows and platform_posix.c for macOS.
//
// Paths stay `wchar_t` strings on every platform: UTF-16 on Windows, as wmain receives them, and
// UTF-32 on macOS, decoded from the UTF-8 the system uses. Both cross the job protocol as UTF-8.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif


#ifdef _WIN32
#define PLATFORM_PATH_SEPARATOR L'\\'
#else
#define PLATFORM_PATH_SEPARATOR L'/'
#endif

// printf-style format checking; MinGW needs the gnu_printf flavour for C99 conversions.
#ifdef __MINGW32__
#define PLATFORM_PRINTF(formatIndex, firstArgument)                                                \
	__attribute__((format(gnu_printf, formatIndex, firstArgument)))
#else
#define PLATFORM_PRINTF(formatIndex, firstArgument)                                                \
	__attribute__((format(printf, formatIndex, firstArgument)))
#endif

#ifdef _WIN32
typedef CRITICAL_SECTION PlatformLock;
typedef HANDLE PlatformThread;
typedef HANDLE PlatformFileLock;
#else
typedef pthread_mutex_t PlatformLock;
typedef pthread_t PlatformThread;
typedef int PlatformFileLock;
#endif

typedef enum PlatformLockResult {
	PLATFORM_LOCK_TAKEN,
	PLATFORM_LOCK_HELD,   // another process (or another lock of this one) holds it
	PLATFORM_LOCK_FAILED, // the file cannot be opened; printed why
} PlatformLockResult;

// One entry of a directory: its name and whether it is a directory itself (not a link to one).
// False stops the listing.
typedef bool PlatformDirectoryVisit(void *context, const wchar_t *name, bool isDirectory);

// fopen with a wide path; `mode` is fopen's ("rb", "wb", "ab", "w").
FILE *platform_openFile(const wchar_t *path, const char *mode);

// Deletes a file. True if it is gone, including when it never existed; prints why if not.
bool platform_removeFile(const wchar_t *path);

// Flushes `file` and has the system write it through to the disk: F_FULLFSYNC on macOS (fsync on a
// file system without it), FlushFileBuffers on Windows. False if either step fails.
bool platform_syncFile(FILE *file);
// Has the system write a directory's entries to the disk, such as a file just renamed into it
// (macOS; on Windows there is nothing to do). Prints why and returns false on failure.
bool platform_syncDirectory(const wchar_t *path);
// Renames the file or directory `from` to `to` in one step, replacing a file at `to`. Prints why
// and returns false on failure.
bool platform_replaceFile(const wchar_t *from, const wchar_t *to);
// Renames the folder `from` to `to`, which must not exist (an empty folder there may be replaced).
// Prints why and returns false on failure.
bool platform_renameFolder(const wchar_t *from, const wchar_t *to);
// Creates a directory; true also if it is one already. Prints why and returns false on failure.
bool platform_makeDirectory(const wchar_t *path);
// Visits the entries of a directory but "." and "..", in no set order. False if it cannot be read
// (printed why) or `visit` stopped it.
bool platform_listDirectory(const wchar_t *path, PlatformDirectoryVisit *visit, void *context);
// Deletes an empty directory. True if it is gone, including when it never existed; prints why if
// not.
bool platform_removeDirectory(const wchar_t *path);
// The bytes this user can still write to the volume holding the directory `path`. Prints why and
// returns false if they cannot be read.
bool platform_freeBytes(const wchar_t *path, uint64_t *bytes);
// An exclusive lock on the file at `path`, created if it is missing, taken without waiting. It
// lasts until platform_unlockFile or the process ends: flock on macOS, a handle that shares nothing
// on Windows.
PlatformLockResult platform_lockFile(const wchar_t *path, PlatformFileLock *lock);
void platform_unlockFile(PlatformFileLock lock);

// UTF-8 for job files and messages; false (with `out` undefined) on invalid text or overflow.
bool platform_toUtf8(const wchar_t *text, char *out, size_t size);
// A wide copy of UTF-8 text; false on overflow (and on invalid UTF-8 outside Windows, which
// substitutes it).
bool platform_fromUtf8(const char *text, wchar_t *out, size_t size);

// Comparisons ignoring ASCII case (and, for wide text, the C library's notion of case).
int platform_compareIgnoringCase(const char *a, const char *b);
int platform_compareIgnoringCaseN(const char *a, const char *b, size_t length);
int platform_compareWideIgnoringCase(const wchar_t *a, const wchar_t *b);

unsigned long platform_processId(void);
unsigned long platform_processorCount(void);

// A monotonic clock, `platform_tickFrequency` ticks per second.
uint64_t platform_ticks(void);
uint64_t platform_tickFrequency(void);
// As platform_ticks, but without the time the computer was asleep (which macOS's monotonic clock
// counts).
uint64_t platform_uptimeTicks(void);
// CPU time the calling thread has used, in 100 ns units (0 if unknown).
uint64_t platform_threadCpuTime(void);
// The local time of day.
void platform_timeOfDay(int *hour, int *minute, int *second, int *millisecond);
// `seconds` since 1970 as a UTC date and time; false if it cannot be represented.
bool platform_utcTime(time_t seconds, struct tm *out);
// Suspends the calling thread for about `milliseconds`.
void platform_sleep(unsigned milliseconds);

void platform_initLock(PlatformLock *lock);
void platform_lock(PlatformLock *lock);
void platform_unlock(PlatformLock *lock);
void platform_destroyLock(PlatformLock *lock);

#ifndef _WIN32
// An errno value's description (strerror_r), returned by value so that it is safe on any thread;
// use it in one expression, as in platform_errorText(errno).text.
typedef struct PlatformErrorText {
	char text[128];
} PlatformErrorText;
PlatformErrorText platform_errorText(int error);
#endif

// Starts `run(argument)` on a new thread; false if it cannot be created.
bool platform_startThread(PlatformThread *thread, void (*run)(void *argument), void *argument);
// Waits for threads started by platform_startThread and releases them.
void platform_joinThreads(PlatformThread *threads, size_t count);
