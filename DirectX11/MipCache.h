#pragma once

#include <dxgiformat.h>
#include <stdint.h>
#include <string>
#include <vector>

// Mod textures stored without mip levels are sampled from their full size
// however small they appear. The lower levels are generated once, on a
// background thread, kept in <cache_directory>\mips and used from the next
// time the texture is loaded.

struct DdsTexture2D {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t mips = 1;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	size_t data_offset = 0;
};

// A DDS of one 2D texture (no array, cube or volume) in a format the
// generator handles, with at least its first level present
bool ParseDdsTexture2D(const uint8_t *data, size_t size, DdsTexture2D *info);

bool MipCacheEnabled();

// Whether the texture lacks levels the generator can make and is to be given
// them. Uncompressed textures only on an explicit request, which also ignores
// generate_texture_mips_min_size.
bool MipCacheWanted(const DdsTexture2D &info, bool explicit_request);

bool MipCacheIsSrgb(DXGI_FORMAT format);

// The sRGB variant of a format the generator handles; others unchanged
DXGI_FORMAT MipCacheSrgbFormat(DXGI_FORMAT format);

size_t MipCacheRowPitch(DXGI_FORMAT format, uint32_t width);

enum class MipCacheLookup {
	HIT,
	MISS,
	// Tried before and the generator cannot make levels for it
	NEVER,
};

// From the cache file's header alone; MipCacheLoad still verifies the levels
MipCacheLookup MipCacheProbe(uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb_filter);

// levels receives levels 1..n-1 of the full chain, in DDS order
MipCacheLookup MipCacheLoad(uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb_filter,
		std::vector<uint8_t> *levels);

// Generates the levels of the file with these contents on a background thread
void MipCacheQueue(const std::wstring &path, uint32_t crc, uint64_t size, bool srgb_filter);

// A file this session has loaded with its levels looked up: its entry of the
// last session's list is skipped
void MipCacheNoteLoaded(const std::wstring &path);

// A file the last session loaded; the background thread finds out whether it
// needs levels
void MipCacheQueueFromLastSession(const std::wstring &path);

void MipCacheStop();
