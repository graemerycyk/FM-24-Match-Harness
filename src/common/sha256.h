// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

// SHA-256 (FIPS 180-4), for the hashes in run and batch manifests: fixtures, seed lists, outputs.
#pragma once

#include <stddef.h>
#include <stdint.h>


enum {
	SHA256_DIGEST_LENGTH = 32,
	SHA256_HEX_LENGTH = 2 * SHA256_DIGEST_LENGTH + 1, // lower-case hex and its NUL
};

typedef struct Sha256 {
	uint32_t state[8];
	uint64_t length; // bytes added so far
	uint8_t block[64];
	size_t used; // bytes of `block` waiting for the rest of it
} Sha256;

void sha256_start(Sha256 *hash);
void sha256_add(Sha256 *hash, const void *bytes, size_t size);
// The digest of everything added; start again to reuse `hash`.
void sha256_finish(Sha256 *hash, uint8_t digest[SHA256_DIGEST_LENGTH]);

// A digest as lower-case hex.
void sha256_hex(const uint8_t digest[SHA256_DIGEST_LENGTH], char hex[SHA256_HEX_LENGTH]);
// The digest of `size` bytes, as lower-case hex.
void sha256_hexOf(const void *bytes, size_t size, char hex[SHA256_HEX_LENGTH]);
