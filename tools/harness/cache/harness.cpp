#include "DirectX11/ResourceHash.h"
#include <stdio.h>
#include <stdexcept>

FILE *LogFile = stderr;
LogVerbosity gLogVerbosity = LogVerbosity::WARNING;

// Compile the production cache definitions without the resource-tracking and GPU hooks.
#include "bin/cache_impl.inc"

static void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static void check_snapshots()
{
	ResourceHandleInfo source;
	unsigned reads = 0;
	auto fill = [&](uint8_t value) {
		return [&, value](uint8_t *dst, size_t offset, size_t size) {
			reads++;
			memset(dst, value, size);
		};
	};
	source.SetLazyDataCache(8193, fill(17));
	require(source.GetCachedData(0, 4)[0] == 17, "initial lazy read");
	source.CacheRegionHash({ 0, 4 }, 123);
	source.SetLazyDataCache(8193, fill(29));
	require(!source.GetCachedRegionHash({ 0, 4 }), "republishing must invalidate region hashes");
	require(source.GetCachedData(0, 4)[0] == 29 && reads == 2, "republishing must reread bytes");
	ResourceHandleInfo view;
	view.InitializeDataCache(257, 128);
	view.cached_data = source.cached_data;
	require(view.GetCachedData(0, 4)[0] == 29, "shared view offset");
	source.SetLazyDataCache(8193, fill(41));
	require(source.GetCachedData(0, 4)[0] == 41, "new shared snapshot");
	require(view.GetCachedData(0, 4)[0] == 29, "republishing must preserve an existing shared snapshot");
	source.SetLazyDataCache(257, fill(53));
	require(source.GetCachedData(256, 1)[256] == 53, "resized partial final block");
	source.ClearDataCache();
	require(!source.cached_data && !source.cached_data_size, "clearing a lazy cache");
	source.SetLazyDataCache(513, fill(67));
	require(source.GetCachedData(512, 1)[512] == 67, "reading after clear");
	printf("PASS: snapshot renewal, shared views, resize and clear\n");
}

static void check_reads(bool bounded, bool coalesced)
{
	std::vector<uint8_t> input(65539);
	for (size_t i = 0; i < input.size(); i++)
		input[i] = (uint8_t)(i * 37 + i / 251);
	ResourceHandleInfo source;
	size_t calls = 0, bytes = 0;
	source.SetLazyDataCache(input.size(), [&](uint8_t *dst, size_t offset, size_t size) {
		require(offset <= input.size() && size <= input.size() - offset, "read outside source");
		calls++;
		bytes += size;
		memcpy(dst, input.data() + offset, size);
	});
	require(!memcmp(source.GetCachedData(48, 12) + 48, input.data() + 48, 12), "small read values");
	printf("small read: %zu bytes, %zu calls\n", bytes, calls);
	if (bounded)
		require(bytes <= 256, "a small value read must fetch at most 256 bytes");
	size_t before = calls;
	source.GetCachedData(48, 12);
	require(calls == before, "repeated reads must reuse valid bytes");
	ResourceHandleInfo view;
	view.InitializeDataCache(520, 250);
	view.cached_data = source.cached_data;
	require(!memcmp(view.GetCachedData(3, 520 - 3) + 3, input.data() + 253, 517), "shared view crossing block boundaries");
	source.GetCachedData(4093, 8);
	require(!memcmp(source.GetCachedData() + 4093, input.data() + 4093, 8), "crossing a 4KiB boundary");
	before = calls;
	require(!memcmp(source.GetCachedData(0, input.size()), input.data(), input.size()), "large read after sparse reads");
	require(bytes == input.size(), "valid source bytes must not be fetched again");
	if (coalesced)
		require(calls - before <= 3, "large reads must coalesce consecutive missing blocks");
	printf("PASS: sparse, repeated, offset, boundary and complete reads (%zu bytes, %zu calls)\n", bytes, calls);
}

static void benchmark(bool shared)
{
	ResourceHandleInfo source, view;
	const unsigned iterations = 100000;
	size_t bytes = 0;
	unsigned replacements = 0;
	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	for (unsigned i = 0; i < iterations; i++) {
		uint8_t *previous = source.cached_data.get();
		source.SetLazyDataCache(65536, [&, i](uint8_t *dst, size_t offset, size_t size) {
			bytes += size;
			memset(dst, (uint8_t)i, size);
		});
		replacements += source.cached_data.get() != previous;
		require(source.GetCachedData(48, 12)[48] == (uint8_t)i, "benchmark snapshot value");
		if (shared) {
			view.InitializeDataCache(65536);
			view.cached_data = source.cached_data;
		}
	}
	QueryPerformanceCounter(&end);
	printf("bench %s: %.3f ms, %u storage replacements, %zu bytes\n", shared ? "shared" : "exclusive",
		1000.0 * (end.QuadPart - start.QuadPart) / freq.QuadPart, replacements, bytes);
}

int main(int argc, char **argv)
{
	try {
		bool bounded = argc > 1 && !strcmp(argv[1], "--bounded");
		bool coalesced = bounded || (argc > 1 && !strcmp(argv[1], "--coalesced"));
		check_snapshots();
		check_reads(bounded, coalesced);
		benchmark(false);
		benchmark(true);
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
