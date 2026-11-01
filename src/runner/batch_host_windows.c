// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// The batch's host on Windows, which has no gzip: batches there use compress=none.

#include "batch_store.h"
#include "platform.h"


static bool compress(void *context, const wchar_t *path);
static FILE *openDecompressed(void *context, const wchar_t *path, void **handle);
static bool closeDecompressed(void *context, FILE *file, void *handle);
static bool syncFile(void *context, FILE *file);
static bool syncDirectory(void *context, const wchar_t *path);
static bool freeBytes(void *context, const wchar_t *path, uint64_t *bytes);
static time_t utcNow(void *context);

void batchHost_real(BatchHost *host) {
	*host = (BatchHost){
		.compress = compress,
		.openDecompressed = openDecompressed,
		.closeDecompressed = closeDecompressed,
		.syncFile = syncFile,
		.syncDirectory = syncDirectory,
		.freeBytes = freeBytes,
		.utcNow = utcNow,
	};
}

static bool compress(void *context, const wchar_t *path) {
	(void)context;
	(void)path;
	fputs("Windows has no gzip; batches there use compress=none\n", stderr);
	return false;
}

static FILE *openDecompressed(void *context, const wchar_t *path, void **handle) {
	(void)context;
	(void)path;
	*handle = NULL;
	fputs("Windows has no gzip to decompress a chunk with\n", stderr);
	return NULL;
}

static bool closeDecompressed(void *context, FILE *file, void *handle) {
	(void)context;
	(void)handle;
	return fclose(file) == 0;
}

static bool syncFile(void *context, FILE *file) {
	(void)context;
	return platform_syncFile(file);
}

static bool syncDirectory(void *context, const wchar_t *path) {
	(void)context;
	return platform_syncDirectory(path);
}

static bool freeBytes(void *context, const wchar_t *path, uint64_t *bytes) {
	(void)context;
	return platform_freeBytes(path, bytes);
}

static time_t utcNow(void *context) {
	(void)context;
	return time(NULL);
}
