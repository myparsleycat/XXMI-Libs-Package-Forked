#include "GpuWatch.h"

#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <share.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>

#include "Globals.h"
#include "log.h"

namespace GpuWatch {

bool enabled = false;
bool debug_layer = false;

namespace {

const UINT64 RING_SIZE = 1 << 15;
const ULONGLONG STALL_MS = 1000;
const UINT64 REPORT_FINISHED = 8;
const UINT64 REPORT_PENDING = 32;

enum class Kind : UINT8 { CALL, SECTION, COMMAND_LIST };

struct Marker {
	UINT64 seq;
	UINT64 hashes[5];
	UINT64 frame;
	UINT args[3];
	DrawCall call;
	Kind kind;
	wchar_t section[160];
};

std::vector<Marker> ring;
SRWLOCK lock = SRWLOCK_INIT;
FILE *file;
ID3D11Fence *fence;
ID3D11DeviceContext4 *watched_context;
ID3D11Device *watched_device;
ID3D11Device1 *attached_device;
ID3D11DeviceContext1 *attached_context;
ID3D11InfoQueue *info_queue;
std::unordered_set<UINT64> debug_sites;
std::atomic<UINT64> generation{0};
std::atomic<UINT64> submitted{0};
std::atomic<UINT64> signalled{0};
UINT64 interval = 1;
bool thread_started;

// The SDK headers stay out of the tree: these match GFSDK_Aftermath 2.27, and the
// library refuses other versions, which the report then shows.
const uint32_t AFTERMATH_VERSION = 0x21B;
const uint32_t AFTERMATH_SUCCESS = 1;
const uint32_t AFTERMATH_WATCH_DX = 1;
const uint32_t AFTERMATH_SHADER_ERROR_REPORTING = 0x10;
const uint32_t AFTERMATH_STATUS_NOT_STARTED = 0;
const uint32_t AFTERMATH_STATUS_FAILED = 2;
const uint32_t AFTERMATH_STATUS_FINISHED = 4;
const ULONGLONG AFTERMATH_DUMP_TIMEOUT_MS = 30000;

typedef void (*AftermathDumpCallback)(const void *dump, uint32_t size, void *user);
typedef uint32_t (*AftermathEnable)(uint32_t version, uint32_t apis, uint32_t flags, AftermathDumpCallback dump, void *shader_info, void *description, void *marker, void *user);
typedef uint32_t (*AftermathInitialize)(uint32_t version, uint32_t flags, ID3D11Device *device);
typedef uint32_t (*AftermathGetStatus)(uint32_t *status);

bool aftermath_wanted;
bool aftermath_tried;
bool aftermath_device_done;
AftermathInitialize aftermath_initialize;
AftermathGetStatus aftermath_get_status;
std::wstring dump_folder;
std::atomic<int> dump_count{0};
std::atomic<bool> dump_wait_done{false};

void write_time()
{
	SYSTEMTIME now;
	GetLocalTime(&now);
	fprintf(file, "[%02u:%02u:%02u.%03u] ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
}

const char *call_name(DrawCall call)
{
	switch (call) {
		case DrawCall::Draw: return "Draw";
		case DrawCall::DrawIndexed: return "DrawIndexed";
		case DrawCall::DrawInstanced: return "DrawInstanced";
		case DrawCall::DrawIndexedInstanced: return "DrawIndexedInstanced";
		case DrawCall::DrawInstancedIndirect: return "DrawInstancedIndirect";
		case DrawCall::DrawIndexedInstancedIndirect: return "DrawIndexedInstancedIndirect";
		case DrawCall::DrawAuto: return "DrawAuto";
		case DrawCall::Dispatch: return "Dispatch";
		case DrawCall::DispatchIndirect: return "DispatchIndirect";
		default: return "unknown";
	}
}

void write_marker(const char *prefix, const Marker &m)
{
	fprintf(file, "  %s #%llu frame %llu: ", prefix, m.seq, m.frame);
	if (m.kind == Kind::COMMAND_LIST) {
		fprintf(file, "ExecuteCommandList of a deferred context\n");
		return;
	}
	if (m.kind == Kind::SECTION) {
		fprintf(file, "mod draw or dispatch of [%S], the game has bound", m.section);
	} else {
		fprintf(file, "game %s(%u, %u, %u)", call_name(m.call), m.args[0], m.args[1], m.args[2]);
		if (m.call == DrawCall::Dispatch || m.call == DrawCall::DispatchIndirect) {
			fprintf(file, " cs=%016llx\n", m.hashes[0]);
			return;
		}
	}
	static const char *const names[] = { "vs", "hs", "ds", "gs", "ps" };
	for (int i = 0; i < 5; i++) {
		if (m.hashes[i])
			fprintf(file, " %s=%016llx", names[i], m.hashes[i]);
	}
	fprintf(file, "\n");
}

void write_markers(const char *prefix, UINT64 first, UINT64 last)
{
	std::vector<Marker> copies;
	AcquireSRWLockShared(&lock);
	for (UINT64 seq = first; seq <= last; seq++) {
		const Marker &m = ring[seq & (RING_SIZE - 1)];
		if (m.seq == seq)
			copies.push_back(m);
	}
	ReleaseSRWLockShared(&lock);
	if (copies.size() != last - first + 1)
		fprintf(file, "  (%llu of these are no longer kept)\n", last - first + 1 - copies.size());
	for (const Marker &m : copies)
		write_marker(prefix, m);
}

void write_video_memory(ID3D11Device *device)
{
	IDXGIDevice *dxgi_device = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIAdapter3 *adapter3 = NULL;
	DXGI_QUERY_VIDEO_MEMORY_INFO info;

	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
	    SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
	    SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))) &&
	    SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
		fprintf(file, "  video memory of the process: %llu MB of a budget of %llu MB\n",
				info.CurrentUsage / (1024 * 1024), info.Budget / (1024 * 1024));
	}
	if (adapter3)
		adapter3->Release();
	if (adapter)
		adapter->Release();
	if (dxgi_device)
		dxgi_device->Release();
}

void write_report(UINT64 finished, ID3D11Device *device)
{
	UINT64 last = submitted.load(std::memory_order_acquire);
	UINT64 first_finished = finished >= REPORT_FINISHED ? finished - REPORT_FINISHED + 1 : 1;

	fprintf(file, "  finished markers: %llu, submitted: %llu, frame on the CPU: %llu\n", finished, last, (UINT64)G->frame_no);
	write_video_memory(device);
	if (finished)
		write_markers("finished", first_finished, finished);
	if (last > finished) {
		// The GPU runs them in order, so it is on one of the calls up to the
		// next fence signal, or on a copy or clear between them.
		UINT64 last_candidate = min(last, finished + interval);
		write_markers("STUCK ON", finished + 1, last_candidate);
		if (last > last_candidate)
			write_markers("pending ", last_candidate + 1, min(last, last_candidate + REPORT_PENDING));
	}
	fflush(file);
}

// Polls without sleeping: a GPU fault removes the device with no stall before
// it, and the fence reads UINT64_MAX from then on, so the last value read before
// that has to be recent to say where the GPU was.
DWORD WINAPI watch_thread(void *)
{
	UINT64 seen_generation = 0;
	UINT64 finished = 0;
	ULONGLONG progress_time = GetTickCount64();
	bool stalled = false;
	ID3D11Fence *current_fence = NULL;
	ID3D11Device *device = NULL;

	for (;;) {
		SwitchToThread();

		if (generation.load(std::memory_order_acquire) != seen_generation) {
			if (current_fence)
				current_fence->Release();
			if (device)
				device->Release();
			current_fence = NULL;
			device = NULL;

			AcquireSRWLockShared(&lock);
			seen_generation = generation.load(std::memory_order_relaxed);
			if (fence) {
				current_fence = fence;
				current_fence->AddRef();
				device = watched_device;
				device->AddRef();
			}
			ReleaseSRWLockShared(&lock);

			finished = 0;
			stalled = false;
			progress_time = GetTickCount64();
		}

		if (!current_fence) {
			Sleep(50);
			continue;
		}

		UINT64 completed = current_fence->GetCompletedValue();
		ULONGLONG now = GetTickCount64();

		if (completed == UINT64_MAX) {
			write_time();
			fprintf(file, "device removed: reason 0x%08x, %llu ms after the last GPU progress\n", device->GetDeviceRemovedReason(), now - progress_time);
			write_report(finished, device);
			// The next device, if the game makes one, comes with a new generation
			current_fence->Release();
			device->Release();
			current_fence = NULL;
			device = NULL;
		} else if (completed > finished) {
			if (stalled) {
				write_time();
				fprintf(file, "the GPU went on after %.2f s\n", (now - progress_time) / 1000.0);
				fflush(file);
			}
			finished = completed;
			progress_time = now;
			stalled = false;
		} else if (signalled.load(std::memory_order_acquire) <= finished) {
			progress_time = now;
			stalled = false;
		} else if (!stalled && now - progress_time >= STALL_MS) {
			stalled = true;
			write_time();
			fprintf(file, "no GPU progress for %.2f s\n", (now - progress_time) / 1000.0);
			write_report(finished, device);
		}
	}
}

Marker *begin_marker(ID3D11DeviceContext *context, UINT64 *seq)
{
	if (context != attached_context || !fence)
		return NULL;

	*seq = submitted.load(std::memory_order_relaxed) + 1;
	AcquireSRWLockExclusive(&lock);
	Marker *m = &ring[*seq & (RING_SIZE - 1)];
	m->seq = *seq;
	m->frame = G->frame_no;
	return m;
}

// A signal costs a flush, about 20 us on an RTX 4070 SUPER, so by default only
// every interval-th call gets one.
void signal(UINT64 seq)
{
	signalled.store(seq, std::memory_order_release);
	watched_context->Signal(fence, seq);
}

// The debug layer validates at the draw, so what it has stored now belongs to the
// call just marked. Every message is written once per message and call site.
void write_debug_messages(const Marker &m)
{
	UINT64 count = info_queue->GetNumStoredMessages();
	if (!count)
		return;

	for (UINT64 i = 0; i < count; i++) {
		SIZE_T length = 0;
		if (FAILED(info_queue->GetMessage(i, NULL, &length)))
			continue;
		std::vector<char> storage(length);
		D3D11_MESSAGE *message = (D3D11_MESSAGE *)storage.data();
		if (FAILED(info_queue->GetMessage(i, message, &length)))
			continue;

		UINT64 site = (UINT64)message->ID ^ (m.hashes[0] * 31) ^ (m.hashes[4] * 37);
		if (m.kind == Kind::SECTION) {
			for (const wchar_t *c = m.section; *c; c++)
				site = site * 131 + *c;
		}
		if (!debug_sites.insert(site).second)
			continue;

		write_time();
		fprintf(file, "debug layer, severity %d, id %d: %s\n", message->Severity, message->ID, message->pDescription);
		write_marker("after", m);
		fflush(file);
	}
	info_queue->ClearStoredMessages();
}

void end_marker(UINT64 seq)
{
	ReleaseSRWLockExclusive(&lock);
	submitted.store(seq, std::memory_order_release);
	if (seq % interval == 0)
		signal(seq);
	if (info_queue)
		write_debug_messages(ring[seq & (RING_SIZE - 1)]);
}

void aftermath_dump_callback(const void *dump, uint32_t size, void *)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%sd3d11_gpu_crash_%d.nv-gpudmp", dump_folder.c_str(), ++dump_count);
	FILE *out = _wfsopen(path, L"wb", _SH_DENYNO);
	if (out) {
		fwrite(dump, 1, size, out);
		fclose(out);
	}
	write_time();
	fprintf(file, "Aftermath wrote %u bytes to %ls\n", size, out ? path : L"nothing, the file did not open");
	fflush(file);
}

// Aftermath collects the dump in this process, so the process has to outlive that.
// Only the first wait counts: a game may go on calling Present after a removal
// that Aftermath never makes a dump of.
void wait_for_dump()
{
	if (dump_wait_done.load())
		return;

	ULONGLONG start = GetTickCount64();
	uint32_t status = AFTERMATH_STATUS_NOT_STARTED;
	while (GetTickCount64() - start < AFTERMATH_DUMP_TIMEOUT_MS) {
		if (aftermath_get_status(&status) == AFTERMATH_SUCCESS && (status == AFTERMATH_STATUS_FAILED || status == AFTERMATH_STATUS_FINISHED))
			break;
		Sleep(50);
	}
	if (!dump_wait_done.exchange(true)) {
		write_time();
		fprintf(file, "Aftermath dump status %u after %llu ms (2 failed, 4 finished)\n", status, GetTickCount64() - start);
		fflush(file);
	}
}

// Unity ends the process with an int3 when it finds the device removed outside Present.
LONG CALLBACK aftermath_exception_handler(EXCEPTION_POINTERS *info)
{
	uint32_t status;
	if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT
			&& aftermath_get_status(&status) == AFTERMATH_SUCCESS && status != AFTERMATH_STATUS_NOT_STARTED)
		wait_for_dump();
	return EXCEPTION_CONTINUE_SEARCH;
}

}

void before_device_create()
{
	if (!aftermath_wanted || aftermath_tried)
		return;
	aftermath_tried = true;

	std::wstring path = dump_folder + L"GFSDK_Aftermath_Lib.x64.dll";
	HMODULE library = LoadLibraryW(path.c_str());
	DWORD load_error = library ? 0 : GetLastError();
	uint32_t result = 0;
	if (library) {
		AftermathEnable enable = (AftermathEnable)GetProcAddress(library, "GFSDK_Aftermath_EnableGpuCrashDumps");
		AftermathInitialize initialize = (AftermathInitialize)GetProcAddress(library, "GFSDK_Aftermath_DX11_Initialize");
		AftermathGetStatus get_status = (AftermathGetStatus)GetProcAddress(library, "GFSDK_Aftermath_GetCrashDumpStatus");
		if (enable && initialize && get_status)
			result = enable(AFTERMATH_VERSION, AFTERMATH_WATCH_DX, 0, aftermath_dump_callback, NULL, NULL, NULL, NULL);
		if (result == AFTERMATH_SUCCESS) {
			aftermath_initialize = initialize;
			aftermath_get_status = get_status;
			AddVectoredExceptionHandler(1, aftermath_exception_handler);
		}
	}

	write_time();
	fprintf(file, "Aftermath crash dumps: load error %u, result 0x%x\n", load_error, result);
	fflush(file);
}

// Aftermath supports one device, the first it is given. In EFMI that is the one it
// draws with; a game that makes a throwaway device first gets no shader error
// reporting, the crash dumps do not depend on this.
void after_device_create(ID3D11Device *device)
{
	if (!aftermath_initialize || aftermath_device_done || !device)
		return;
	aftermath_device_done = true;

	uint32_t result = aftermath_initialize(AFTERMATH_VERSION, AFTERMATH_SHADER_ERROR_REPORTING, device);
	write_time();
	fprintf(file, "Aftermath device %p: result 0x%x\n", device, result);
	fflush(file);
}

void after_present(HRESULT result)
{
	if (aftermath_get_status && (result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET))
		wait_for_dump();
}

void init(int calls_per_signal, bool use_debug_layer, bool use_aftermath, const wchar_t *log_folder)
{
	if (file || (calls_per_signal <= 0 && !use_aftermath))
		return;

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%sd3d11_gpu_watch.txt", log_folder);
	file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	dump_folder = log_folder;
	aftermath_wanted = use_aftermath;

	if (calls_per_signal > 0) {
		interval = calls_per_signal;
		debug_layer = use_debug_layer;
		ring.resize(RING_SIZE);
		enabled = true;
		write_time();
		fprintf(file, "GPU watch started, a fence signal every %llu calls, debug layer %d\n", interval, debug_layer);
		fflush(file);
	}
}

void frame(ID3D11Device1 *device, ID3D11DeviceContext1 *context)
{
	if (device == attached_device && context == attached_context) {
		UINT64 seq = submitted.load(std::memory_order_relaxed);
		if (fence && seq != signalled.load(std::memory_order_relaxed))
			signal(seq);
		return;
	}

	ID3D11Device5 *device5 = NULL;
	ID3D11DeviceContext4 *context4 = NULL;
	ID3D11Fence *new_fence = NULL;
	HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&device5));
	if (SUCCEEDED(hr))
		hr = context->QueryInterface(IID_PPV_ARGS(&context4));
	if (SUCCEEDED(hr))
		hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&new_fence));

	ID3D11Fence *old_fence;
	ID3D11DeviceContext4 *old_context;
	ID3D11Device *old_device;

	AcquireSRWLockExclusive(&lock);
	old_fence = fence;
	old_context = watched_context;
	old_device = watched_device;
	fence = new_fence;
	watched_context = context4;
	watched_device = device5;
	attached_device = device;
	attached_context = context;
	generation++;
	submitted.store(0, std::memory_order_release);
	signalled.store(0, std::memory_order_release);
	for (Marker &m : ring)
		m.seq = 0;
	ReleaseSRWLockExclusive(&lock);

	if (info_queue) {
		info_queue->Release();
		info_queue = NULL;
	}
	if (debug_layer && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
		D3D11_MESSAGE_SEVERITY hidden[] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
		D3D11_INFO_QUEUE_FILTER filter = {};
		filter.DenyList.NumSeverities = ARRAYSIZE(hidden);
		filter.DenyList.pSeverityList = hidden;
		info_queue->PushStorageFilter(&filter);
		info_queue->SetMuteDebugOutput(TRUE);
	}

	write_time();
	fprintf(file, "watching device %p: fence result 0x%08x, device creation flags 0x%x, debug layer messages %d\n", device, hr, device->GetCreationFlags(), info_queue != NULL);
	fflush(file);

	if (old_fence)
		old_fence->Release();
	if (old_context)
		old_context->Release();
	if (old_device)
		old_device->Release();

	if (new_fence && !thread_started) {
		HMODULE self;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&watch_thread, &self);
		HANDLE thread = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
		if (thread) {
			thread_started = true;
			CloseHandle(thread);
		}
	}
}

void mark_call(ID3D11DeviceContext *context, const DrawCallInfo &info, const UINT64 hashes[5])
{
	UINT64 seq;
	Marker *m = begin_marker(context, &seq);
	if (!m)
		return;

	m->kind = Kind::CALL;
	m->call = info.type;
	memcpy(m->hashes, hashes, sizeof(m->hashes));
	switch (info.type) {
		case DrawCall::Dispatch:
			m->args[0] = info.ThreadGroupCountX;
			m->args[1] = info.ThreadGroupCountY;
			m->args[2] = info.ThreadGroupCountZ;
			break;
		case DrawCall::DrawIndexed:
		case DrawCall::DrawIndexedInstanced:
			m->args[0] = info.IndexCount;
			m->args[1] = info.InstanceCount;
			m->args[2] = info.FirstIndex;
			break;
		default:
			m->args[0] = info.VertexCount;
			m->args[1] = info.InstanceCount;
			m->args[2] = info.FirstVertex;
			break;
	}
	end_marker(seq);
}

void mark_section(ID3D11DeviceContext *context, const wchar_t *ini_section, const UINT64 hashes[5])
{
	UINT64 seq;
	Marker *m = begin_marker(context, &seq);
	if (!m)
		return;

	m->kind = Kind::SECTION;
	memcpy(m->hashes, hashes, sizeof(m->hashes));
	wcsncpy_s(m->section, ini_section, _TRUNCATE);
	end_marker(seq);
}

void mark_command_list(ID3D11DeviceContext *context)
{
	UINT64 seq;
	Marker *m = begin_marker(context, &seq);
	if (!m)
		return;

	m->kind = Kind::COMMAND_LIST;
	end_marker(seq);
}

}
