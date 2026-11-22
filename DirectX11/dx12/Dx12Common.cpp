#include "Dx12Common.h"

#include <immintrin.h>
#include <intrin.h>

#include "util.h"

#include "../globals.h"
#include "../IniHandler.h"

#include "Bridge.h"
#include "HashDump.h"
#include "UploadHashReadback.h"

Dx12Stats dx12_stats;
Dx12Memory dx12_memory;
volatile UINT dx12_frame;
volatile UINT dx12_raytracing_frame;

static bool has_avx2()
{
	int regs[4];

	__cpuid(regs, 0);
	if (regs[0] < 7)
		return false;
	// AVX and OSXSAVE, then the OS saving the XMM and YMM state
	__cpuid(regs, 1);
	if ((regs[2] & 0x18000000) != 0x18000000 || (_xgetbv(0) & 6) != 6)
		return false;
	__cpuidex(regs, 7, 0);
	return (regs[1] & 0x20) != 0;
}

static const bool avx2_stream_loads = has_avx2();

static void copy_write_combined_sse41(BYTE *dst, const BYTE *src, size_t count)
{
	size_t head, i;

	head = (16 - ((uintptr_t)src & 15)) & 15;
	if (head > count)
		head = count;

	memcpy(dst, src, head);
	for (i = head; i + 16 <= count; i += 16)
		_mm_storeu_si128((__m128i*)(dst + i), _mm_stream_load_si128((__m128i*)(src + i)));
	memcpy(dst + i, src + i, count - i);
}

static void copy_write_combined_avx2(BYTE *dst, const BYTE *src, size_t count)
{
	size_t head, i;

	head = (32 - ((uintptr_t)src & 31)) & 31;
	if (head > count)
		head = count;

	memcpy(dst, src, head);
	for (i = head; i + 32 <= count; i += 32)
		_mm256_storeu_si256((__m256i*)(dst + i), _mm256_stream_load_si256((const __m256i*)(src + i)));
	_mm256_zeroupper();
	memcpy(dst + i, src + i, count - i);
}

uint32_t Dx12Crc32cWriteCombined(uint32_t hash, const BYTE *src, size_t length)
{
	__declspec(align(32)) BYTE chunk[16 * 1024];
	size_t count;

	while (length) {
		count = length < sizeof(chunk) ? length : sizeof(chunk);
		if (avx2_stream_loads)
			copy_write_combined_avx2(chunk, src, count);
		else
			copy_write_combined_sse41(chunk, src, count);

		hash = crc32c_hw(hash, chunk, count);
		src += count;
		length -= count;
	}

	return hash;
}

void Dx12LogForwardedQuery(const char *proxy, REFIID riid, HRESULT hr)
{
	static SRWLOCK lock = SRWLOCK_INIT;
	static GUID seen[64];
	static unsigned count;
	bool is_new = true;
	unsigned i;

	AcquireSRWLockExclusive(&lock);
	for (i = 0; i < count && is_new; i++)
		is_new = !IsEqualGUID(seen[i], riid);
	if (is_new && count < ARRAYSIZE(seen))
		seen[count++] = riid;
	else
		is_new = false;
	ReleaseSRWLockExclusive(&lock);

	if (is_new)
		Dx12LogInfo("%s forwarded QueryInterface(%s) to the original = 0x%x\n", proxy, NameFromIID(riid).c_str(), hr);
}

void Dx12FramePresented()
{
	static ULONGLONG last_report;
	static unsigned frames, reports;
	ULONGLONG now = GetTickCount64();
	double seconds, per_frame;
	LONG64 draws, dispatches, closed, executed, calls, blas, rays;

	dx12_frame++;
	Dx12FinishUploadHashes();
	Dx12DumpFramePresented();

	// Without the shim there is no engine to load the mods, but what the
	// game uses can still be compared with them
	if (!dx12_shim && !G->gConfigInitialized
	    && (GetSystemTicks() - G->ticks_at_launch) / 1'000'000.0f > G->gConfigInitializationDelay)
		LoadSkippedIncludes();

	if (!last_report)
		last_report = now;
	frames++;
	if (now - last_report < 5000)
		return;

	draws = InterlockedExchange64(&dx12_stats.draws, 0);
	dispatches = InterlockedExchange64(&dx12_stats.dispatches, 0);
	closed = InterlockedExchange64(&dx12_stats.lists_closed, 0);
	executed = InterlockedExchange64(&dx12_stats.lists_executed, 0);
	calls = InterlockedExchange64(&dx12_stats.execute_calls, 0);
	blas = InterlockedExchange64(&dx12_stats.blas_builds, 0);
	rays = InterlockedExchange64(&dx12_stats.dispatch_rays, 0);
	seconds = (now - last_report) / 1000.0;
	per_frame = 1.0 / frames;

	// Keeps a long session from filling the log at the default log level
	const unsigned startup_reports = 12;
	const bool info = ++reports <= startup_reports;
	if (info) {
		Dx12LogInfo("%u frames in %.1fs (%.1f fps), per frame: %.0f draws, %.0f dispatches, %.1f command lists closed, %.1f executed in %.1f calls\n",
			frames, seconds, frames / seconds, draws * per_frame, dispatches * per_frame,
			closed * per_frame, executed * per_frame, calls * per_frame);
	} else {
		Dx12LogDebug("%u frames in %.1fs (%.1f fps), per frame: %.0f draws, %.0f dispatches, %.1f command lists closed, %.1f executed in %.1f calls\n",
			frames, seconds, frames / seconds, draws * per_frame, dispatches * per_frame,
			closed * per_frame, executed * per_frame, calls * per_frame);
	}
	if (blas || rays) {
		Dx12LogInfo("Ray tracing: %.1f acceleration structure builds and %.1f DispatchRays per frame\n",
			blas * per_frame, rays * per_frame);
	}
	Dx12HashReport(info, reports == startup_reports);
	if (LogFile)
		fflush(LogFile);

	frames = 0;
	last_report = now;
}
