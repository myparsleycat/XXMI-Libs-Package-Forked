#include "DirectX11/dx12/TextureHash.h"
#include "crc32c.h"

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>

class __declspec(uuid("61b20af2-bd23-4f1c-8007-717e87857444")) TestResource final : public IUnknown {
	ULONG refs = 1;
	UINT64 va, bytes;
	D3D12_HEAP_TYPE heap;

public:
	void (*destroyed)(void*) = nullptr;
	unsigned hashes_forgotten = 0;

	TestResource(UINT64 va, UINT64 bytes, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT)
		: va(va), bytes(bytes), heap(heap) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
	{
		if (iid != __uuidof(TestResource))
			return E_NOINTERFACE;
		*object = this;
		AddRef();
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }

	ULONG STDMETHODCALLTYPE Release() override
	{
		if (!--refs && destroyed)
			destroyed(this);
		return refs;
	}

	D3D12_RESOURCE_DESC GetDesc() const
	{
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		return desc;
	}

	HRESULT GetHeapProperties(D3D12_HEAP_PROPERTIES *properties, D3D12_HEAP_FLAGS *) const
	{
		properties->Type = heap;
		return S_OK;
	}

	UINT64 GetGPUVirtualAddress() const { return va; }
};

static void Dx12NotifyDestruction(TestResource *resource, void (*callback)(void*))
{
	resource->destroyed = callback;
}

static void Dx12ForgetBufferHashes(TestResource *resource)
{
	resource->hashes_forgotten++;
}

#define ID3D12Resource TestResource
#include "tracker.inc"
#include "dred.inc"
#undef ID3D12Resource

static void track(TestResource *resource, LONG state = D3D12_RESOURCE_STATE_COMMON)
{
	void *object = resource;
	Dx12TrackResource(__uuidof(TestResource), &object, state);
}

static TestResource* find_buffer(UINT64 va, UINT64 expected_offset)
{
	TestResource *resource = nullptr;
	UINT64 offset = 0;
	if (Dx12FindBuffer(va, &resource, &offset))
		assert(offset == expected_offset);
	return resource;
}

static void test_va_stamps()
{
	const UINT64 mib = 1ull << 20, page = 1ull << 16;
	const UINT64 base = 4096 * mib, start = base + page;
	const UINT64 bytes = 256 * mib, last = start + bytes - 1;
	const UINT64 unrelated = base - page;
	UINT64 outside = Dx12FindBufferStamp(unrelated);
	UINT64 first_stamp = Dx12FindBufferStamp(start);
	UINT64 last_stamp = Dx12FindBufferStamp(last);

	TestResource small_buffer(start + page, page);
	track(&small_buffer);
	assert(Dx12FindBufferStamp(start) == first_stamp);
	assert(find_buffer(start + page, 0) == &small_buffer);
	small_buffer.Release();
	assert(Dx12FindBufferStamp(start) == first_stamp);

	TestResource large(start, bytes);
	track(&large);
	assert(Dx12FindBufferStamp(unrelated) == outside);
	assert(Dx12FindBufferStamp(start) != first_stamp);
	assert(Dx12FindBufferStamp(last) != last_stamp);
	assert(find_buffer(last, bytes - 1) == &large);

	const volatile LONG64 *counters[DX12_VA_STAMP_COUNTERS] = {};
	Dx12FindBufferStampCounters(last, counters);
	UINT64 total = 0;
	for (const volatile LONG64 *counter : counters) {
		if (counter)
			total += (UINT64)*counter;
	}
	assert(total == Dx12FindBufferStamp(last));

	last_stamp = Dx12FindBufferStamp(last);
	TestResource replacement(start, page);
	track(&replacement);
	assert(Dx12FindBufferStamp(last) != last_stamp);
	assert(find_buffer(last, 0) == nullptr);
	assert(find_buffer(start, 0) == &replacement);
	first_stamp = Dx12FindBufferStamp(start);
	large.Release();
	assert(Dx12FindBufferStamp(start) == first_stamp);
	assert(find_buffer(start, 0) == &replacement);
	replacement.Release();
	assert(Dx12FindBufferStamp(start) != first_stamp);
	assert(find_buffer(start, 0) == nullptr);
	assert(Dx12FindBufferStamp(unrelated) == outside);

	TestResource huge(start, 272 * mib);
	track(&huge);
	assert(Dx12FindBufferStamp(unrelated) != outside);
	outside = Dx12FindBufferStamp(unrelated);
	huge.Release();
	assert(Dx12FindBufferStamp(unrelated) != outside);

	first_stamp = Dx12FindBufferStamp(start);
	TestResource readback(start, bytes, D3D12_HEAP_TYPE_READBACK);
	track(&readback, D3D12_RESOURCE_STATE_COPY_DEST);
	Dx12ResourceInfo info = {};
	assert(Dx12GetResourceInfo(&readback, &info));
	assert(info.va == start && info.state == D3D12_RESOURCE_STATE_COPY_DEST);
	assert(Dx12FindBufferStamp(start) == first_stamp);
	assert(find_buffer(start, 0) == nullptr);
	readback.Release();
	assert(!Dx12GetResourceInfo(&readback, &info));
	assert(Dx12FindBufferStamp(start) == first_stamp);
	assert(readback.hashes_forgotten == 1);
}

static char fault_note[512];

static void capture_fault_note(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(fault_note, sizeof(fault_note), format, args);
	va_end(args);
}

static void test_dred_readback()
{
	const UINT64 start = 1ull << 32;
	const UINT64 bytes = 1ull << 16;
	TestResource readback(start, bytes, D3D12_HEAP_TYPE_READBACK);
	track(&readback, D3D12_RESOURCE_STATE_COPY_DEST);

	note_game_buffer(start + 17, capture_fault_note);
	assert(strstr(fault_note, "65536 bytes, heap type 3, at 0x11 in it"));
	note_game_buffer(start + bytes, capture_fault_note);
	assert(strstr(fault_note, "no buffer of the game there"));
	readback.Release();
	note_game_buffer(start + 17, capture_fault_note);
	assert(strstr(fault_note, "no buffer of the game there"));

	TestResource buffer(start, bytes);
	track(&buffer);
	note_game_buffer(start + 17, capture_fault_note);
	assert(strstr(fault_note, "65536 bytes, heap type 1, at 0x11 in it"));
	buffer.Release();
}

static void test_dred_readback_overlap(bool same_start)
{
	const UINT64 start = 1ull << 32, page = 1ull << 16;
	const UINT64 preferred_start = same_start ? start : start + page;
	TestResource buffers[] = {
		{ start, 3 * page, D3D12_HEAP_TYPE_READBACK },
		{ preferred_start, 2 * page, D3D12_HEAP_TYPE_READBACK },
	};
	TestResource *other = &buffers[0], *preferred = &buffers[1];

	// Put the preferred buffer in a later shard so a first-match lookup fails.
	if (shard_of(other) > shard_of(preferred)) {
		std::swap(*other, *preferred);
		std::swap(other, preferred);
	}
	assert(shard_of(other) < shard_of(preferred));
	if (same_start) {
		track(other, D3D12_RESOURCE_STATE_COPY_DEST);
		track(preferred, D3D12_RESOURCE_STATE_COPY_DEST);
	} else {
		track(preferred, D3D12_RESOURCE_STATE_COPY_DEST);
		track(other, D3D12_RESOURCE_STATE_COPY_DEST);
	}

	note_game_buffer(preferred_start + 17, capture_fault_note);
	assert(strstr(fault_note, "131072 bytes, heap type 3, at 0x11 in it"));
	preferred->Release();
	note_game_buffer(preferred_start + 17, capture_fault_note);
	assert(strstr(fault_note, same_start ? "196608 bytes, heap type 3, at 0x11 in it"
	                                    : "196608 bytes, heap type 3, at 0x10011 in it"));
	other->Release();
}

volatile UINT dx12_frame;
Dx12TextureStats dx12_texture_stats;
static struct { int dx12_defer_upload_hash = 1; } settings, *G = &settings;
static UINT clock_calls, crc_calls;
static LONG64 clock_step;

static BOOL test_counter(LARGE_INTEGER *ticks)
{
	ticks->QuadPart = clock_step * ++clock_calls;
	return TRUE;
}

static BOOL test_frequency(LARGE_INTEGER *frequency)
{
	frequency->QuadPart = 1000000;
	return TRUE;
}

static uint32_t crc32c_hw(uint32_t crc, const BYTE *data, size_t bytes)
{
	crc_calls++;
	return crc32c_append(crc, data, bytes);
}

uint32_t Dx12Crc32cWriteCombined(uint32_t crc, const BYTE *data, size_t bytes)
{
	return crc32c_hw(crc, data, bytes);
}

#define QueryPerformanceCounter test_counter
#define QueryPerformanceFrequency test_frequency
#include "texture.inc"
#undef QueryPerformanceFrequency
#undef QueryPerformanceCounter

static void test_texture_rows(UINT row_bytes, UINT pitch, size_t pause_at, UINT rows = 53)
{
	const size_t limit = (rows - 1) * row_bytes + min(row_bytes, 19u);
	std::vector<BYTE> data(rows * pitch, 0xff), packed(limit);

	for (size_t i = 0; i < limit; i++)
		data[(i / row_bytes) * pitch + i % row_bytes] = packed[i] = (BYTE)((i * 17) ^ (i >> 5));

	Dx12TextureUpload upload = {};
	upload.row_pitch = pitch;
	upload.rows = rows;
	upload.row_bytes = row_bytes;
	upload.limit = limit;
	upload.crc = 0x12345678;
	const uint32_t expected = crc32c_append(upload.crc, packed.data(), packed.size());
	const auto bytes_before = dx12_texture_stats.hashed_bytes;
	const auto finished_before = dx12_texture_stats.hashed;

	dx12_frame++;
	clock_calls = crc_calls = 0;
	clock_step = pause_at ? 500 : 1;
	bool complete = Dx12HashTextureRows(data.data(), &upload, true, true);
	assert(complete == !pause_at);
	if (pause_at) {
		assert(clock_calls == 2);
		assert(upload.hashed == pause_at);
		assert(dx12_texture_stats.hashed_bytes - bytes_before == upload.hashed);
		assert(dx12_texture_stats.hashed == finished_before);
		const UINT64 offset = Dx12TextureUploadStart(&upload);
		std::vector<BYTE> readback(data.data() + offset, data.data() + offset + Dx12TextureUploadSpan(&upload));
		assert(Dx12HashTextureRows(readback.data(), &upload, false, false));
	} else {
		const size_t chunks = (limit + 16383) / 16384;
		assert(clock_calls == chunks + 1);
		if (pitch == row_bytes)
			assert(crc_calls == chunks);
	}
	assert(upload.crc == expected && upload.hashed == limit);
	assert(dx12_texture_stats.hashed_bytes - bytes_before == limit);
	assert(dx12_texture_stats.hashed == finished_before + 1);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (!strcmp(argv[1], "va")) {
		test_va_stamps();
	} else if (!strcmp(argv[1], "dred")) {
		test_dred_readback();
	} else if (!strcmp(argv[1], "dred-overlap-va")) {
		test_dred_readback_overlap(false);
	} else if (!strcmp(argv[1], "dred-overlap-serial")) {
		test_dred_readback_overlap(true);
	} else if (!strcmp(argv[1], "padded")) {
		test_texture_rows(1, 256, 64, 4096);
	} else {
		assert(!strcmp(argv[1], "texture"));
		test_texture_rows(1024, 1024, 0);
		test_texture_rows(1000, 1024, 0);
		test_texture_rows(1000, 1024, 16 * 1024);
		test_texture_rows(1000, 1000, 16 * 1024);
	}
	printf("%s: OK\n", argv[1]);
}
