#include "MipGen.h"

#include <math.h>
#include <string.h>
#include <mutex>

#include "bc7enc/bc7enc.h"
#include "bc7enc/bc7decomp.h"
#define RGBCX_IMPLEMENTATION
#include "bc7enc/rgbcx.h"

namespace {

enum class Kind { NONE, BC1, BC3, BC4, BC5, BC7, RGBA8 };

Kind kind_of(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: return Kind::BC1;
	case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: return Kind::BC3;
	case DXGI_FORMAT_BC4_UNORM: return Kind::BC4;
	case DXGI_FORMAT_BC5_UNORM: return Kind::BC5;
	case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB: return Kind::BC7;
	case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return Kind::RGBA8;
	default: return Kind::NONE;
	}
}

uint32_t block_bytes(Kind kind)
{
	switch (kind) {
	case Kind::BC1: case Kind::BC4: return 8;
	case Kind::NONE: case Kind::RGBA8: return 0;
	default: return 16;
	}
}

// Linear light in 16 bits, and back to the nearest sRGB code
uint16_t srgb_to_linear[256];
uint8_t linear_to_srgb[65536];
std::once_flag init_once;

void init_tables()
{
	unsigned i, code = 0;

	for (i = 0; i < 256; i++) {
		double c = i / 255.0;
		double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
		srgb_to_linear[i] = (uint16_t)floor(l * 65535.0 + 0.5);
	}
	for (i = 0; i < 65536; i++) {
		while (code < 255 && (unsigned)srgb_to_linear[code + 1] + srgb_to_linear[code] < 2 * i)
			code++;
		linear_to_srgb[i] = (uint8_t)code;
	}

	bc7enc_compress_block_init();
	rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
}

struct Image {
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

inline uint8_t average(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return (uint8_t)(((unsigned)a + b + c + d + 2) >> 2);
}

inline uint8_t average_srgb(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	unsigned sum = (unsigned)srgb_to_linear[a] + srgb_to_linear[b] + srgb_to_linear[c] + srgb_to_linear[d];
	return linear_to_srgb[(sum + 2) >> 2];
}

// One row of the next level from two rows of this one; an odd last column or
// row of the source is folded into the texel before it
void downsample_row(const uint8_t *row0, const uint8_t *row1, uint32_t src_width,
		uint8_t *dst, uint32_t dst_width, bool srgb)
{
	for (uint32_t x = 0; x < dst_width; x++) {
		uint32_t x0 = 2 * x, x1 = x0 + 1 < src_width ? x0 + 1 : x0;
		const uint8_t *a = row0 + 4 * x0, *b = row0 + 4 * x1, *c = row1 + 4 * x0, *d = row1 + 4 * x1;
		for (int ch = 0; ch < 3; ch++)
			dst[4 * x + ch] = srgb ? average_srgb(a[ch], b[ch], c[ch], d[ch]) : average(a[ch], b[ch], c[ch], d[ch]);
		dst[4 * x + 3] = average(a[3], b[3], c[3], d[3]);
	}
}

void downsample(const Image &src, Image *dst, bool srgb)
{
	dst->width = src.width > 1 ? src.width / 2 : 1;
	dst->height = src.height > 1 ? src.height / 2 : 1;
	dst->rgba.resize((size_t)dst->width * dst->height * 4);
	for (uint32_t y = 0; y < dst->height; y++) {
		uint32_t y0 = 2 * y, y1 = y0 + 1 < src.height ? y0 + 1 : y0;
		downsample_row(&src.rgba[(size_t)y0 * src.width * 4], &src.rgba[(size_t)y1 * src.width * 4],
				src.width, &dst->rgba[(size_t)y * dst->width * 4], dst->width, srgb);
	}
}

// Decodes one block to 16 RGBA texels; false if it holds a transparent BC1 texel
bool decode_block(Kind kind, const uint8_t *block, uint8_t *pixels)
{
	switch (kind) {
	case Kind::BC1:
		rgbcx::unpack_bc1(block, pixels, true, rgbcx::bc1_approx_mode::cBC1Ideal);
		for (int i = 0; i < 16; i++) {
			if (pixels[4 * i + 3] != 255)
				return false;
		}
		return true;
	case Kind::BC3:
		rgbcx::unpack_bc3(block, pixels, rgbcx::bc1_approx_mode::cBC1Ideal);
		return true;
	case Kind::BC4:
		memset(pixels, 0, 64);
		rgbcx::unpack_bc4(block, pixels, 4);
		for (int i = 0; i < 16; i++)
			pixels[4 * i + 3] = 255;
		return true;
	case Kind::BC5:
		memset(pixels, 0, 64);
		rgbcx::unpack_bc5(block, pixels, 0, 1, 4);
		for (int i = 0; i < 16; i++)
			pixels[4 * i + 3] = 255;
		return true;
	case Kind::BC7:
		if (!bc7decomp::unpack_bc7(block, (bc7decomp::color_rgba *)pixels))
			memset(pixels, 0, 64);
		return true;
	default:
		return false;
	}
}

void encode_block(Kind kind, const uint8_t *pixels, uint8_t *block, const bc7enc_compress_block_params &bc7,
		const MipGenSettings &settings)
{
	switch (kind) {
	case Kind::BC1: rgbcx::encode_bc1(settings.bc1_level, block, pixels, false, false); break;
	case Kind::BC3: rgbcx::encode_bc3(settings.bc1_level, block, pixels); break;
	case Kind::BC4: rgbcx::encode_bc4(block, pixels, 4); break;
	case Kind::BC5: rgbcx::encode_bc5(block, pixels, 0, 1, 4); break;
	case Kind::BC7: bc7enc_compress_block(block, pixels, &bc7); break;
	default: break;
	}
}

// Writes the level held in img, in the file's format, to dst
bool encode_level(Kind kind, const Image &img, uint8_t *dst, const bc7enc_compress_block_params &bc7,
		const MipGenSettings &settings, const std::atomic<bool> *stop)
{
	if (kind == Kind::RGBA8) {
		memcpy(dst, img.rgba.data(), img.rgba.size());
		return true;
	}

	uint32_t blocks_wide = (img.width + 3) / 4, blocks_high = (img.height + 3) / 4;
	uint32_t bytes = block_bytes(kind);
	uint8_t pixels[64];

	for (uint32_t by = 0; by < blocks_high; by++) {
		if (stop && stop->load(std::memory_order_relaxed))
			return false;
		for (uint32_t bx = 0; bx < blocks_wide; bx++) {
			// A level smaller than a block repeats its last row and column
			for (uint32_t y = 0; y < 4; y++) {
				uint32_t sy = by * 4 + y < img.height ? by * 4 + y : img.height - 1;
				for (uint32_t x = 0; x < 4; x++) {
					uint32_t sx = bx * 4 + x < img.width ? bx * 4 + x : img.width - 1;
					memcpy(pixels + 4 * (y * 4 + x), &img.rgba[((size_t)sy * img.width + sx) * 4], 4);
				}
			}
			encode_block(kind, pixels, dst, bc7, settings);
			dst += bytes;
		}
	}
	return true;
}

// Level 1 straight from the stored mip 0, two block rows at a time, so that
// mip 0 is never held decoded in full
MipGenResult first_level(Kind kind, const uint8_t *mip0, uint32_t width, uint32_t height, bool srgb,
		const std::atomic<bool> *stop, Image *out)
{
	out->width = width / 2;
	out->height = height / 2;
	out->rgba.resize((size_t)out->width * out->height * 4);

	if (kind == Kind::RGBA8) {
		for (uint32_t y = 0; y < out->height; y++) {
			if (stop && (y & 63) == 0 && stop->load(std::memory_order_relaxed))
				return MipGenResult::STOPPED;
			downsample_row(mip0 + (size_t)2 * y * width * 4, mip0 + (size_t)(2 * y + 1) * width * 4,
					width, &out->rgba[(size_t)y * out->width * 4], out->width, srgb);
		}
		return MipGenResult::OK;
	}

	uint32_t blocks_wide = width / 4, blocks_high = height / 4, bytes = block_bytes(kind);
	std::vector<uint8_t> strip((size_t)width * 8 * 4);
	uint8_t pixels[64];

	for (uint32_t by = 0; by < blocks_high; by += 2) {
		if (stop && stop->load(std::memory_order_relaxed))
			return MipGenResult::STOPPED;
		uint32_t rows = by + 1 < blocks_high ? 2 : 1;
		for (uint32_t r = 0; r < rows; r++) {
			const uint8_t *block = mip0 + ((size_t)(by + r) * blocks_wide) * bytes;
			for (uint32_t bx = 0; bx < blocks_wide; bx++, block += bytes) {
				if (!decode_block(kind, block, pixels))
					return MipGenResult::REFUSED;
				for (uint32_t y = 0; y < 4; y++)
					memcpy(&strip[((size_t)(r * 4 + y) * width + bx * 4) * 4], pixels + 16 * y, 16);
			}
		}
		for (uint32_t y = 0; y < rows * 2; y++) {
			downsample_row(&strip[(size_t)(2 * y) * width * 4], &strip[(size_t)(2 * y + 1) * width * 4],
					width, &out->rgba[((size_t)(by * 2 + y)) * out->width * 4], out->width, srgb);
		}
	}
	return MipGenResult::OK;
}

} // namespace

bool MipGenSupportsFormat(DXGI_FORMAT format)
{
	return kind_of(format) != Kind::NONE;
}

size_t MipGenLevelBytes(DXGI_FORMAT format, uint32_t width, uint32_t height)
{
	Kind kind = kind_of(format);

	if (kind == Kind::NONE || !width || !height)
		return 0;
	if (kind == Kind::RGBA8)
		return (size_t)width * height * 4;
	return (size_t)((width + 3) / 4) * ((height + 3) / 4) * block_bytes(kind);
}

uint32_t MipGenFullChainLevels(uint32_t width, uint32_t height)
{
	uint32_t levels = 1, size = width > height ? width : height;

	while (size > 1) {
		size >>= 1;
		levels++;
	}
	return levels;
}

uint32_t MipGenEncoderVersion(const MipGenSettings &settings)
{
	// Raise the first number when the output of the same settings changes
	return (1u << 24) ^ (settings.bc7_uber_level << 16) ^ (settings.bc7_max_partitions << 8) ^ settings.bc1_level;
}

MipGenResult GenerateLowerMips(const uint8_t *mip0, uint32_t width, uint32_t height,
		DXGI_FORMAT format, bool srgb_filter, const MipGenSettings &settings,
		const std::atomic<bool> *stop, std::vector<uint8_t> *out)
{
	Kind kind = kind_of(format);
	uint32_t levels = MipGenFullChainLevels(width, height), level;
	bc7enc_compress_block_params bc7;
	size_t total = 0, at = 0;
	Image current, next;
	MipGenResult result;

	out->clear();
	if (kind == Kind::NONE || width < 4 || height < 4 || width % 4 || height % 4)
		return MipGenResult::UNSUPPORTED;

	std::call_once(init_once, init_tables);

	bc7enc_compress_block_params_init(&bc7);
	if (srgb_filter)
		bc7enc_compress_block_params_init_perceptual_weights(&bc7);
	else
		bc7enc_compress_block_params_init_linear_weights(&bc7);
	bc7.m_uber_level = settings.bc7_uber_level;
	bc7.m_max_partitions_mode = settings.bc7_max_partitions;

	for (level = 1; level < levels; level++) {
		uint32_t w = width >> level, h = height >> level;
		total += MipGenLevelBytes(format, w ? w : 1, h ? h : 1);
	}
	out->resize(total);

	result = first_level(kind, mip0, width, height, srgb_filter, stop, &current);
	if (result != MipGenResult::OK) {
		out->clear();
		return result;
	}

	for (level = 1; level < levels; level++) {
		if (!encode_level(kind, current, out->data() + at, bc7, settings, stop)) {
			out->clear();
			return MipGenResult::STOPPED;
		}
		at += MipGenLevelBytes(format, current.width, current.height);
		if (level + 1 < levels) {
			downsample(current, &next, srgb_filter);
			current.width = next.width;
			current.height = next.height;
			current.rgba.swap(next.rgba);
		}
	}
	return MipGenResult::OK;
}
