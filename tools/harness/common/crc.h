// CRC32C as the DLL computes its resource hashes, and the way to give data a
// hash of one's choosing: four bytes of it are solved for.
#pragma once
#include <stdint.h>
#include <string.h>

static uint32_t crc_table[256], crc_back[256];

static void crc_init()
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
		crc_table[i] = c;
	}
	for (uint32_t i = 0; i < 256; i++)
		crc_back[crc_table[i] >> 24] = i;
}

static uint32_t crc_raw(uint32_t state, const void *data, size_t bytes)
{
	const uint8_t *p = (const uint8_t*)data;
	for (size_t i = 0; i < bytes; i++)
		state = crc_table[(state ^ p[i]) & 0xff] ^ (state >> 8);
	return state;
}

// What crc32c_hw(seed, data, bytes) of the DLL returns
static uint32_t crc32c(uint32_t seed, const void *data, size_t bytes)
{
	return ~crc_raw(~seed, data, bytes);
}

static uint32_t crc_unstep(uint32_t state, uint8_t byte)
{
	uint32_t i = crc_back[state >> 24];
	return ((state ^ crc_table[i]) << 8) | (i ^ byte);
}

// Rewrites data[bytes - 4 .. bytes) so that the CRC32C of data followed by
// suffix is target.
static void crc_forge(uint8_t *data, size_t bytes, const void *suffix, size_t suffix_bytes, uint32_t target)
{
	uint32_t before = crc_raw(~0u, data, bytes - 4);
	uint32_t after = ~target;
	const uint8_t *s = (const uint8_t*)suffix;
	for (size_t i = suffix_bytes; i-- > 0;)
		after = crc_unstep(after, s[i]);
	for (int i = 0; i < 4; i++)
		after = crc_unstep(after, 0);
	uint32_t patch = after ^ before;
	memcpy(data + bytes - 4, &patch, 4);
}
