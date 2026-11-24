#pragma once

#include <dxgiformat.h>
#include <stdint.h>
#include <atomic>
#include <vector>

struct MipGenSettings {
	uint32_t bc7_uber_level = 0;
	uint32_t bc7_max_partitions = 16;
	uint32_t bc1_level = 6;
};

enum class MipGenResult {
	OK,
	UNSUPPORTED,	// format or size the generator does not handle
	STOPPED,
	REFUSED,	// content the target format cannot keep, e.g. BC1 punch-through alpha
};

// Formats GenerateLowerMips accepts, as stored in the file (sRGB variants included)
bool MipGenSupportsFormat(DXGI_FORMAT format);

// Bytes of one level of a width x height texture of the format; 0 if unsupported
size_t MipGenLevelBytes(DXGI_FORMAT format, uint32_t width, uint32_t height);

// Number of levels of a full chain down to 1x1, mip 0 included
uint32_t MipGenFullChainLevels(uint32_t width, uint32_t height);

// Builds levels 1..n-1 of a full chain from mip 0, in the order a DDS stores
// them, into out. mip0 holds exactly MipGenLevelBytes(format, width, height)
// bytes. srgb_filter averages the colour channels in linear light. The result
// depends only on the inputs and the settings.
MipGenResult GenerateLowerMips(const uint8_t *mip0, uint32_t width, uint32_t height,
		DXGI_FORMAT format, bool srgb_filter, const MipGenSettings &settings,
		const std::atomic<bool> *stop, std::vector<uint8_t> *out);

// Identifies the encoder and settings, to tell caches made by other versions apart
uint32_t MipGenEncoderVersion(const MipGenSettings &settings);
