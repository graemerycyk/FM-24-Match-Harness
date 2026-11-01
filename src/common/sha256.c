// SPDX-FileCopyrightText: © 2026 Graeme Rycyk <grae@workanywhere.io>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sha256.h"

#include <string.h>


static const uint32_t roundConstants[64] = {
	0x428a2f98,
	0x71374491,
	0xb5c0fbcf,
	0xe9b5dba5,
	0x3956c25b,
	0x59f111f1,
	0x923f82a4,
	0xab1c5ed5,
	0xd807aa98,
	0x12835b01,
	0x243185be,
	0x550c7dc3,
	0x72be5d74,
	0x80deb1fe,
	0x9bdc06a7,
	0xc19bf174,
	0xe49b69c1,
	0xefbe4786,
	0x0fc19dc6,
	0x240ca1cc,
	0x2de92c6f,
	0x4a7484aa,
	0x5cb0a9dc,
	0x76f988da,
	0x983e5152,
	0xa831c66d,
	0xb00327c8,
	0xbf597fc7,
	0xc6e00bf3,
	0xd5a79147,
	0x06ca6351,
	0x14292967,
	0x27b70a85,
	0x2e1b2138,
	0x4d2c6dfc,
	0x53380d13,
	0x650a7354,
	0x766a0abb,
	0x81c2c92e,
	0x92722c85,
	0xa2bfe8a1,
	0xa81a664b,
	0xc24b8b70,
	0xc76c51a3,
	0xd192e819,
	0xd6990624,
	0xf40e3585,
	0x106aa070,
	0x19a4c116,
	0x1e376c08,
	0x2748774c,
	0x34b0bcb5,
	0x391c0cb3,
	0x4ed8aa4a,
	0x5b9cca4f,
	0x682e6ff3,
	0x748f82ee,
	0x78a5636f,
	0x84c87814,
	0x8cc70208,
	0x90befffa,
	0xa4506ceb,
	0xbef9a3f7,
	0xc67178f2,
};

static uint32_t rotateRight(uint32_t value, unsigned count);
static void compress(uint32_t state[8], const uint8_t block[64]);

void sha256_start(Sha256 *hash) {
	static const uint32_t initial[8] = {
		0x6a09e667,
		0xbb67ae85,
		0x3c6ef372,
		0xa54ff53a,
		0x510e527f,
		0x9b05688c,
		0x1f83d9ab,
		0x5be0cd19,
	};
	memcpy(hash->state, initial, sizeof initial);
	hash->length = 0;
	hash->used = 0;
}

void sha256_add(Sha256 *hash, const void *bytes, size_t size) {
	const uint8_t *next = bytes;
	hash->length += size;
	if (hash->used) {
		size_t taken = sizeof hash->block - hash->used;
		if (taken > size) {
			taken = size;
		}
		memcpy(hash->block + hash->used, next, taken);
		hash->used += taken;
		next += taken;
		size -= taken;
		if (hash->used < sizeof hash->block) {
			return;
		}
		compress(hash->state, hash->block);
		hash->used = 0;
	}
	for (; size >= sizeof hash->block; next += sizeof hash->block, size -= sizeof hash->block) {
		compress(hash->state, next);
	}
	memcpy(hash->block, next, size);
	hash->used = size;
}

void sha256_finish(Sha256 *hash, uint8_t digest[SHA256_DIGEST_LENGTH]) {
	uint64_t bits = hash->length * 8;
	uint8_t padding[sizeof hash->block + 8] = {0x80};
	// A 0x80 byte, zeros up to 56 bytes into a block, then the length in bits, big-endian.
	size_t zeros = (hash->used < 56 ? 56 : 120) - hash->used;
	for (size_t i = 0; i < 8; ++i) {
		padding[zeros + i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_add(hash, padding, zeros + 8);
	for (size_t i = 0; i < 8; ++i) {
		digest[4 * i] = (uint8_t)(hash->state[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(hash->state[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(hash->state[i] >> 8);
		digest[4 * i + 3] = (uint8_t)hash->state[i];
	}
}

void sha256_hex(const uint8_t digest[SHA256_DIGEST_LENGTH], char hex[SHA256_HEX_LENGTH]) {
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
		hex[2 * i] = digits[digest[i] >> 4];
		hex[2 * i + 1] = digits[digest[i] & 0x0F];
	}
	hex[SHA256_HEX_LENGTH - 1] = 0;
}

void sha256_hexOf(const void *bytes, size_t size, char hex[SHA256_HEX_LENGTH]) {
	Sha256 hash;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	sha256_start(&hash);
	sha256_add(&hash, bytes, size);
	sha256_finish(&hash, digest);
	sha256_hex(digest, hex);
}

static uint32_t rotateRight(uint32_t value, unsigned count) {
	return value >> count | value << (32 - count);
}

static void compress(uint32_t state[8], const uint8_t block[64]) {
	uint32_t words[64];
	for (size_t i = 0; i < 16; ++i) {
		words[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
							 (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
	}
	for (size_t i = 16; i < 64; ++i) {
		uint32_t low = words[i - 15];
		uint32_t high = words[i - 2];
		words[i] = words[i - 16] + (rotateRight(low, 7) ^ rotateRight(low, 18) ^ low >> 3) +
							 words[i - 7] + (rotateRight(high, 17) ^ rotateRight(high, 19) ^ high >> 10);
	}
	uint32_t a = state[0];
	uint32_t b = state[1];
	uint32_t c = state[2];
	uint32_t d = state[3];
	uint32_t e = state[4];
	uint32_t f = state[5];
	uint32_t g = state[6];
	uint32_t h = state[7];
	for (size_t i = 0; i < 64; ++i) {
		uint32_t choice = (e & f) ^ (~e & g);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t first = h + (rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25)) + choice +
										 roundConstants[i] + words[i];
		uint32_t second = (rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22)) + majority;
		h = g;
		g = f;
		f = e;
		e = d + first;
		d = c;
		c = b;
		b = a;
		a = first + second;
	}
	state[0] += a;
	state[1] += b;
	state[2] += c;
	state[3] += d;
	state[4] += e;
	state[5] += f;
	state[6] += g;
	state[7] += h;
}
