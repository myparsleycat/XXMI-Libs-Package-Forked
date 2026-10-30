#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <deque>
#include <stdio.h>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

using std::min;
using std::max;
using VkBuffer = UINT64;
using VkDeviceSize = UINT64;
using ConstantsKey = std::tuple<VkBuffer, VkDeviceSize, UINT, UINT>;
using D3D12_RESOURCE_STATES = UINT;

struct GpuResource { UINT64 GetGPUVirtualAddress() { return 0x20000000; } };
struct ShimResource {
	std::vector<BYTE> shadow;
	UINT64 version = 0;
	bool shadow_pending = false, upload_pending = false, cpu_only = true;
	bool foreign = false;
	UINT64 foreign_va = 0;
	GpuResource *resource = nullptr;
	UINT64 upload_va = 0, upload_version = 0, upload_offset = 0, upload_size = 0;
	UINT upload_epoch = 0;
};
struct ShimBuffer { ShimResource r; };
using ID3D11Buffer = ShimBuffer;
static ShimResource* ShimResourceOf(ShimBuffer *buffer) { return &buffer->r; }
static bool Dx12ShimTransition(ShimResource *, D3D12_RESOURCE_STATES) { return true; }
static UINT dx12_shim_epoch = 1;
namespace Profiling { namespace Spike {
	enum Category { VK_SHADOW_FILL };
	struct Scope { explicit Scope(Category) {} };
} }

static std::vector<BYTE> input;
static size_t read_bytes, read_calls, waits;
struct Upload { UINT64 va; std::vector<BYTE> bytes; };
static std::deque<Upload> uploads;
static size_t upload_bytes;
static bool fail_upload;
static void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
static void finish_refresh_of(const ShimResource *) { waits++; }
static void read_constants(BYTE *dst, VkBuffer buffer, VkDeviceSize offset, size_t bytes)
{
	require(buffer == 7 && offset <= input.size() && bytes <= input.size() - offset, "source range");
	memcpy(dst, input.data() + offset, bytes);
	read_bytes += bytes;
	read_calls++;
}
static BYTE* Dx12ShimUpload(UINT64 size, UINT64 alignment, void *, void *, UINT64 *va)
{
	require(alignment == 256, "CB upload alignment");
	if (fail_upload) {
		fail_upload = false;
		*va = 0;
		return nullptr;
	}
	uploads.push_back({ 0x10000000 + uploads.size() * 0x100000, std::vector<BYTE>((size_t)size, 0xee) });
	*va = uploads.back().va;
	upload_bytes += (size_t)size;
	return uploads.back().bytes.data();
}
static const BYTE* uploaded(UINT64 va)
{
	for (const Upload &upload : uploads)
		if (va >= upload.va && va - upload.va < upload.bytes.size())
			return upload.bytes.data() + (va - upload.va);
	throw std::runtime_error("unknown upload address");
}

// The production fill and publication reset run against an isolated mapped-memory reader.
#include "bin/shadow_impl.inc"

static void check_reads(UINT stride, bool bounded)
{
	const UINT bytes = 65539, base = 37;
	input.resize(base + bytes);
	for (size_t i = 0; i < input.size(); i++)
		input[i] = (BYTE)(i * 37 + i / 251);
	ShimBuffer object;
	object.r.shadow.assign(bytes + 255, 0xee);
	auto &source = constants_sources[&object.r];
	source.key = { 7, base, bytes, stride };
	republish(&object);
	read_bytes = read_calls = waits = 0;
	VkScopeFillShadow(&object.r, 48, 12);
	require(!memcmp(object.r.shadow.data() + 48, input.data() + base + 48, 12), "small range values");
	printf("%s small read: %zu bytes, %zu calls\n", stride ? "SSBO" : "CB", read_bytes, read_calls);
	if (bounded)
		require(read_bytes == (stride ? 4096 : 256), "CB reads at most 256 bytes; SSBO retains 4 KiB pages");
	size_t before = read_bytes;
	VkScopeFillShadow(&object.r, 48, 12);
	require(read_bytes == before, "repeated range reread");
	VkScopeFillShadow(&object.r, 4093, 8);
	VkScopeFillShadow(&object.r, 65536, 3);
	require(!memcmp(object.r.shadow.data() + 4093, input.data() + base + 4093, 8), "cross-page values");
	require(!memcmp(object.r.shadow.data() + 65536, input.data() + base + 65536, 3), "partial final block");
	require(object.r.shadow_pending, "sparse reads must leave the remaining data pending");
	VkScopeFillShadow(&object.r, 0, UINT64_MAX);
	require(!memcmp(object.r.shadow.data(), input.data() + base, bytes), "full read after sparse ranges");
	require(read_bytes == bytes && !source.pages_left && !object.r.shadow_pending, "full read consumes every byte once");
	require(object.r.shadow[bytes] == 0xee, "read outside resource size");
	std::fill(input.begin(), input.end(), 91);
	republish(&object);
	source.stale.assign((bytes + SHADOW_PAGE - 1) / SHADOW_PAGE, 0);
	VkScopeFillShadow(&object.r, 4093, 8);
	require(object.r.shadow[4093] == 91 && object.r.shadow[4100] == 91, "publication invalidates valid blocks");
	require(source.stale[0] && source.stale[1] && !source.stale[2], "partial reads mark their 4 KiB upload pages stale");
	VkScopeFillShadow(&object.r, 0, UINT64_MAX);
	require(std::all_of(source.stale.begin(), source.stale.end(), [](BYTE value) { return value == 1; }), "full read marks all upload pages");
	before = read_bytes;
	UINT64 version = object.r.version;
	VkScopeFillShadow(&object.r, bytes, 4);
	VkScopeFillShadow(&object.r, 0, 0);
	require(read_bytes == before && object.r.version == version, "empty or outside reads must not mutate data");
	require(waits >= 8, "shadow access must finish an outstanding refresh first");
	constants_sources.erase(&object.r);
	printf("PASS: %s sparse, repeated, boundary, full reads and publication invalidation\n", stride ? "SSBO" : "CB");
}

static void check_uploads(bool bounded)
{
	ShimBuffer object;
	object.r.shadow.resize(65536);
	for (size_t i = 0; i < object.r.shadow.size(); i++)
		object.r.shadow[i] = (BYTE)(i * 17 + i / 257);
	UINT64 first = range_address(&object, 256, 256);
	require(!memcmp(uploaded(first), object.r.shadow.data() + 256, 256), "CB range address and contents");
	printf("256-byte CB range upload: %zu bytes\n", upload_bytes);
	if (bounded)
		require(upload_bytes == 256, "a 256-byte CB view must not upload the whole buffer");
	size_t before = upload_bytes;
	require(range_address(&object, 256, 256) == first && upload_bytes == before, "same version, epoch and range reuse");
	UINT64 other = range_address(&object, 4096, 256);
	require(!memcmp(uploaded(other), object.r.shadow.data() + 4096, 256), "different CB range");
	if (bounded) {
		before = upload_bytes;
		range_address(&object, 256, 256);
		range_address(&object, 4096, 256);
		require(upload_bytes == before, "ranges bound together must not upload again in turn");
	}
	object.r.shadow[256] ^= 0xff;
	object.r.version++;
	UINT64 changed = range_address(&object, 256, 256);
	require(uploaded(changed)[0] == object.r.shadow[256] && uploaded(first)[0] != uploaded(changed)[0], "writes preserve previously recorded upload data");
	UINT64 whole = range_address(&object, 0, 65536);
	require(!memcmp(uploaded(whole), object.r.shadow.data(), 65536), "whole upload after partial uploads");
	before = upload_bytes;
	require(range_address(&object, 4096, 256) == whole + 4096 && upload_bytes == before, "aligned subrange of a cached full upload");
	if (bounded) {
		UINT64 unaligned = range_address(&object, 257, 4);
		require(!(unaligned & 255) && !memcmp(uploaded(unaligned), object.r.shadow.data() + 257, 4), "subrange GPU address alignment");
		require(uploaded(unaligned)[4] == 0 && uploaded(unaligned)[255] == 0, "CB upload padding is zero");
		fail_upload = true;
		require(!range_address(&object, 4096, 256), "upload allocation failure");
		require(range_address(&object, 257, 4) == unaligned, "failed allocation preserves the previous cache entry");
		require(!range_address(&object, 65536, 256) && !range_address(&object, 0, 0), "empty and outside CB ranges");
	}
	dx12_shim_epoch++;
	require(range_address(&object, 256, 256) != changed, "a new submission epoch requires a new upload");
	object.r.foreign = true;
	object.r.foreign_va = 0x30000000;
	require(range_address(&object, 256, 256) == object.r.foreign_va + 256, "foreign CB address offset");
	object.r.foreign = false;
	object.r.cpu_only = false;
	GpuResource gpu;
	object.r.resource = &gpu;
	require(range_address(&object, 256, 256) == gpu.GetGPUVirtualAddress() + 256, "GPU CB address offset");
	printf("PASS: CB upload ranges, reuse, version and epoch invalidation, retained uploads and GPU addresses\n");
}

int main(int argc, char **argv)
{
	try {
		bool bounded = argc > 1 && !strcmp(argv[1], "--bounded");
		check_reads(0, bounded);
		check_reads(16, bounded);
		check_uploads(bounded);
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
