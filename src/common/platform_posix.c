// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


enum {
	UTF8_PATH_LENGTH = 4096,              // a UTF-8 path, up to 4 bytes per character
	THREAD_STACK_SIZE = 16 * 1024 * 1024, // engine calls nest deeply; macOS defaults to 512 KB
};

typedef struct ThreadStart {
	void (*run)(void *argument);
	void *argument;
} ThreadStart;

static bool systemPath(const wchar_t *path, char *out, const char *action);
static bool syncDescriptor(int descriptor);
static bool
listEntries(DIR *directory, PlatformDirectoryVisit *visit, void *context, bool *stopped);
static size_t encodeUtf8(uint32_t codePoint, char *out);
static size_t decodeUtf8(const unsigned char *text, uint32_t *codePoint);
static void *threadMain(void *parameter);

FILE *platform_openFile(const wchar_t *path, const char *mode) {
	char utf8[UTF8_PATH_LENGTH];
	return platform_toUtf8(path, utf8, sizeof utf8) ? fopen(utf8, mode) : NULL;
}

bool platform_removeFile(const wchar_t *path) {
	char utf8[UTF8_PATH_LENGTH];
	if (!platform_toUtf8(path, utf8, sizeof utf8)) {
		fprintf(stderr, "cannot remove %ls: the path is not valid text\n", path);
		return false;
	}
	if (unlink(utf8) == 0 || errno == ENOENT) {
		return true;
	}
	fprintf(stderr, "cannot remove %s (%s)\n", utf8, platform_errorText(errno).text);
	return false;
}

bool platform_syncFile(FILE *file) {
	return fflush(file) == 0 && syncDescriptor(fileno(file));
}

bool platform_syncDirectory(const wchar_t *path) {
	char utf8[UTF8_PATH_LENGTH];
	if (!systemPath(path, utf8, "write to the disk")) {
		return false;
	}
	int descriptor = open(utf8, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	bool ok = descriptor >= 0 && syncDescriptor(descriptor);
	int error = errno;
	if (descriptor >= 0) {
		close(descriptor);
	}
	if (!ok) {
		fprintf(stderr, "cannot write %s to the disk (%s)\n", utf8, platform_errorText(error).text);
	}
	return ok;
}

bool platform_replaceFile(const wchar_t *from, const wchar_t *to) {
	char source[UTF8_PATH_LENGTH];
	char target[UTF8_PATH_LENGTH];
	if (!systemPath(from, source, "rename") || !systemPath(to, target, "replace")) {
		return false;
	}
	if (rename(source, target) == 0) {
		return true;
	}
	fprintf(stderr, "cannot rename %s to %s (%s)\n", source, target, platform_errorText(errno).text);
	return false;
}

bool platform_renameFolder(const wchar_t *from, const wchar_t *to) {
	return platform_replaceFile(from, to);
}

bool platform_makeDirectory(const wchar_t *path) {
	char utf8[UTF8_PATH_LENGTH];
	if (!systemPath(path, utf8, "create")) {
		return false;
	}
	if (mkdir(utf8, 0755) == 0) {
		return true;
	}
	int error = errno;
	struct stat status;
	if (error == EEXIST && stat(utf8, &status) == 0 && S_ISDIR(status.st_mode)) {
		return true;
	}
	fprintf(stderr, "cannot create the folder %s (%s)\n", utf8, platform_errorText(error).text);
	return false;
}

bool platform_listDirectory(const wchar_t *path, PlatformDirectoryVisit *visit, void *context) {
	char utf8[UTF8_PATH_LENGTH];
	if (!systemPath(path, utf8, "read")) {
		return false;
	}
	DIR *directory = opendir(utf8);
	bool stopped = false;
	bool ok = directory && listEntries(directory, visit, context, &stopped);
	int error = errno;
	if (directory) {
		(void)closedir(directory);
	}
	if (!ok && !stopped) {
		fprintf(stderr, "cannot read the folder %s (%s)\n", utf8, platform_errorText(error).text);
	}
	return ok;
}

bool platform_removeDirectory(const wchar_t *path) {
	char utf8[UTF8_PATH_LENGTH];
	if (!systemPath(path, utf8, "remove")) {
		return false;
	}
	if (rmdir(utf8) == 0 || errno == ENOENT) {
		return true;
	}
	fprintf(stderr, "cannot remove the folder %s (%s)\n", utf8, platform_errorText(errno).text);
	return false;
}

bool platform_freeBytes(const wchar_t *path, uint64_t *bytes) {
	char utf8[UTF8_PATH_LENGTH];
	struct statfs volume;
	if (!systemPath(path, utf8, "check the free space of")) {
		return false;
	}
	if (statfs(utf8, &volume) != 0) {
		fprintf(
			stderr, "cannot read the free space of %s (%s)\n", utf8, platform_errorText(errno).text
		);
		return false;
	}
	*bytes = (uint64_t)volume.f_bavail * volume.f_bsize;
	return true;
}

PlatformLockResult platform_lockFile(const wchar_t *path, PlatformFileLock *lock) {
	char utf8[UTF8_PATH_LENGTH];
	if (!systemPath(path, utf8, "lock")) {
		return PLATFORM_LOCK_FAILED;
	}
	int descriptor = open(utf8, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (descriptor < 0) {
		fprintf(stderr, "cannot open %s (%s)\n", utf8, platform_errorText(errno).text);
		return PLATFORM_LOCK_FAILED;
	}
	if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
		int error = errno;
		close(descriptor);
		if (error == EWOULDBLOCK) {
			return PLATFORM_LOCK_HELD;
		}
		fprintf(stderr, "cannot lock %s (%s)\n", utf8, platform_errorText(error).text);
		return PLATFORM_LOCK_FAILED;
	}
	*lock = descriptor;
	return PLATFORM_LOCK_TAKEN;
}

void platform_unlockFile(PlatformFileLock lock) {
	close(lock);
}

bool platform_toUtf8(const wchar_t *text, char *out, size_t size) {
	size_t length = 0;
	for (; *text; ++text) {
		char bytes[4];
		size_t count = encodeUtf8((uint32_t)*text, bytes);
		if (!count || length + count >= size) {
			return false;
		}
		memcpy(out + length, bytes, count);
		length += count;
	}
	if (length >= size) {
		return false;
	}
	out[length] = 0;
	return true;
}

bool platform_fromUtf8(const char *text, wchar_t *out, size_t size) {
	size_t length = 0;
	const unsigned char *bytes = (const unsigned char *)text;
	while (*bytes) {
		uint32_t codePoint = 0;
		size_t count = decodeUtf8(bytes, &codePoint);
		if (!count || length + 1 >= size) {
			return false;
		}
		out[length++] = (wchar_t)codePoint;
		bytes += count;
	}
	if (length >= size) {
		return false;
	}
	out[length] = 0;
	return true;
}

int platform_compareIgnoringCase(const char *a, const char *b) {
	return strcasecmp(a, b);
}

int platform_compareIgnoringCaseN(const char *a, const char *b, size_t length) {
	return strncasecmp(a, b, length);
}

int platform_compareWideIgnoringCase(const wchar_t *a, const wchar_t *b) {
	return wcscasecmp(a, b);
}

unsigned long platform_processId(void) {
	return (unsigned long)getpid();
}

unsigned long platform_processorCount(void) {
	long count = sysconf(_SC_NPROCESSORS_ONLN);
	return count > 0 ? (unsigned long)count : 1;
}

uint64_t platform_ticks(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}

uint64_t platform_tickFrequency(void) {
	return 1000000000U;
}

uint64_t platform_uptimeTicks(void) {
	struct timespec now;
	clock_gettime(CLOCK_UPTIME_RAW, &now);
	return (uint64_t)now.tv_sec * 1000000000U + (uint64_t)now.tv_nsec;
}

uint64_t platform_threadCpuTime(void) {
	struct timespec used;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &used) != 0) {
		return 0;
	}
	return (uint64_t)used.tv_sec * 10000000U + (uint64_t)used.tv_nsec / 100U;
}

void platform_timeOfDay(int *hour, int *minute, int *second, int *millisecond) {
	struct timeval now;
	struct tm local = {0};
	gettimeofday(&now, NULL);
	time_t seconds = now.tv_sec;
	localtime_r(&seconds, &local);
	*hour = local.tm_hour;
	*minute = local.tm_min;
	*second = local.tm_sec;
	*millisecond = (int)(now.tv_usec / 1000);
}

bool platform_utcTime(time_t seconds, struct tm *out) {
	return gmtime_r(&seconds, out) != NULL;
}

void platform_sleep(unsigned milliseconds) {
	struct timespec delay = {
		.tv_sec = milliseconds / 1000, .tv_nsec = (long)(milliseconds % 1000) * 1000000};
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
	}
}

PlatformErrorText platform_errorText(int error) {
	PlatformErrorText description = {""};
	if (strerror_r(error, description.text, sizeof description.text) != 0) {
		(void)snprintf(description.text, sizeof description.text, "error %d", error);
	}
	return description;
}

void platform_initLock(PlatformLock *lock) {
	pthread_mutex_init(lock, NULL);
}

void platform_lock(PlatformLock *lock) {
	pthread_mutex_lock(lock);
}

void platform_unlock(PlatformLock *lock) {
	pthread_mutex_unlock(lock);
}

void platform_destroyLock(PlatformLock *lock) {
	pthread_mutex_destroy(lock);
}

bool platform_startThread(PlatformThread *thread, void (*run)(void *argument), void *argument) {
	ThreadStart *start = malloc(sizeof *start);
	if (!start) {
		return false;
	}
	*start = (ThreadStart){.run = run, .argument = argument};
	pthread_attr_t attributes;
	bool ok = pthread_attr_init(&attributes) == 0;
	if (ok) {
		ok = pthread_attr_setstacksize(&attributes, THREAD_STACK_SIZE) == 0 &&
				 pthread_create(thread, &attributes, threadMain, start) == 0;
		pthread_attr_destroy(&attributes);
	}
	if (!ok) {
		free(start);
	}
	return ok;
}

void platform_joinThreads(PlatformThread *threads, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		pthread_join(threads[i], NULL);
	}
}

// `path` as UTF-8 for a system call; prints why and returns false if it cannot be.
static bool systemPath(const wchar_t *path, char *out, const char *action) {
	if (platform_toUtf8(path, out, UTF8_PATH_LENGTH)) {
		return true;
	}
	fprintf(stderr, "cannot %s %ls: the path is not valid text or is too long\n", action, path);
	return false;
}

// F_FULLFSYNC has the drive write its cache too; fsync only hands the data to it.
static bool syncDescriptor(int descriptor) {
	return fcntl(descriptor, F_FULLFSYNC) == 0 || fsync(descriptor) == 0;
}

// Visits each entry of an open directory but "." and ".."; false with errno set if one cannot be
// read, or with `stopped` set if `visit` stopped.
static bool
listEntries(DIR *directory, PlatformDirectoryVisit *visit, void *context, bool *stopped) {
	wchar_t name[1024]; // a name of up to 255 UTF-16 units
	for (;;) {
		errno = 0;
		// NOLINTNEXTLINE(concurrency-mt-unsafe): each listing reads a stream of its own
		const struct dirent *entry = readdir(directory);
		if (!entry) {
			return errno == 0;
		}
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		bool isDirectory = entry->d_type == DT_DIR;
		struct stat status;
		if (entry->d_type == DT_UNKNOWN) {
			if (fstatat(dirfd(directory), entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
				return false;
			}
			isDirectory = S_ISDIR(status.st_mode);
		}
		if (!platform_fromUtf8(entry->d_name, name, sizeof name / sizeof name[0])) {
			errno = EILSEQ;
			return false;
		}
		if (!visit(context, name, isDirectory)) {
			*stopped = true;
			return false;
		}
	}
}

// Writes `codePoint` as UTF-8; 0 if it is not a Unicode scalar value.
static size_t encodeUtf8(uint32_t codePoint, char *out) {
	unsigned char *bytes = (unsigned char *)out;
	if (codePoint < 0x80) {
		bytes[0] = (unsigned char)codePoint;
		return 1;
	}
	if (codePoint < 0x800) {
		bytes[0] = (unsigned char)(0xC0 | codePoint >> 6);
		bytes[1] = (unsigned char)(0x80 | (codePoint & 0x3F));
		return 2;
	}
	if (codePoint >= 0xD800 && codePoint <= 0xDFFF) {
		return 0;
	}
	if (codePoint < 0x10000) {
		bytes[0] = (unsigned char)(0xE0 | codePoint >> 12);
		bytes[1] = (unsigned char)(0x80 | (codePoint >> 6 & 0x3F));
		bytes[2] = (unsigned char)(0x80 | (codePoint & 0x3F));
		return 3;
	}
	if (codePoint < 0x110000) {
		bytes[0] = (unsigned char)(0xF0 | codePoint >> 18);
		bytes[1] = (unsigned char)(0x80 | (codePoint >> 12 & 0x3F));
		bytes[2] = (unsigned char)(0x80 | (codePoint >> 6 & 0x3F));
		bytes[3] = (unsigned char)(0x80 | (codePoint & 0x3F));
		return 4;
	}
	return 0;
}

// Reads one UTF-8 sequence; 0 if it is malformed, overlong or not a Unicode scalar value.
static size_t decodeUtf8(const unsigned char *text, uint32_t *codePoint) {
	unsigned char lead = text[0];
	size_t count = 0;
	if (lead < 0x80) {
		count = 1;
	} else if (lead >= 0xC2 && lead < 0xE0) {
		count = 2;
	} else if (lead >= 0xE0 && lead < 0xF0) {
		count = 3;
	} else if (lead >= 0xF0 && lead < 0xF5) {
		count = 4;
	} else {
		return 0;
	}
	uint32_t value = count == 1 ? lead : lead & (0x7F >> count);
	for (size_t i = 1; i < count; ++i) {
		if ((text[i] & 0xC0) != 0x80) {
			return 0;
		}
		value = value << 6 | (text[i] & 0x3F);
	}
	static const uint32_t smallest[] = {0, 0, 0x80, 0x800, 0x10000};
	if (value < smallest[count] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
		return 0;
	}
	*codePoint = value;
	return count;
}

static void *threadMain(void *parameter) {
	ThreadStart start = *(ThreadStart *)parameter;
	free(parameter);
	start.run(start.argument);
	return NULL;
}
