#include "Bridge.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>

#include "DeviceRemoved.h"
#include "Dx12CommandList.h"
#include "Dx12CommandQueue.h"
#include "Dx12Device.h"
#include "DrawPath.h"
#include "ShimContext.h"
#include "ShimDevice.h"
#include "ShimPipeline.h"
#include "HashDump.h"
#include "ResourceTracker.h"

#include "util.h"

#include "../globals.h"
#include "../HackerContext.h"
#include "../HackerDevice.h"
#include "../IniHandler.h"
#include "../ModLoading.h"
#include "../profiling.h"

Dx12Shim *dx12_shim;
bool dx12_unloading;
std::atomic<bool> dx12_d3d11_presented;

// dx12_shim for Dx12IsD3D11Game, which is called without the shim's lock
static std::atomic<bool> shim_created;

bool Dx12IsD3D11Game()
{
	return dx12_d3d11_presented && !shim_created;
}
UINT dx12_shim_epoch = 1;
UINT dx12_shim_view_increment;

static Dx12Shim shim;

struct Allocator {
	ID3D12CommandAllocator *allocator;
	// Of the last submission recorded with it
	UINT64 fence;
};

static std::vector<Allocator> allocators;
static size_t current_allocator;
static ID3D12GraphicsCommandList *shim_list;
static bool list_open;
// Read without the lock by Dx12ShimFlush
static volatile bool list_used;

// See Dx12ShimEarlyList
static ID3D12GraphicsCommandList *early_list;
static std::vector<Allocator> early_allocators;
static size_t current_early_allocator;
static bool early_open;
static void (*before_submit)();

static ID3D12Fence *fence;
static UINT64 fence_signalled;
static UINT64 last_shim_signal;
static HANDLE fence_event;
// Mods are off after the first, so there are few
static std::vector<UINT64> discarded;

struct Retired {
	IUnknown *object;
	UINT64 fence;
};
static std::deque<Retired> retired;

// What the lists executed on another queue than ours pinned is held until a
// fence of our own that queue signals says they are done with it
struct ForeignQueue {
	ID3D12CommandQueue *queue;
	ID3D12Fence *fence = nullptr;
	UINT64 signalled = 0;
	std::atomic<bool> gone{false};
	bool owns_queue = false;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> retry_queue;
	ULONGLONG retry_signal_after = 0;
	std::vector<IUnknown*> unsignalled;
	std::deque<Retired> retired;
	// Not referenced, each is in retired with the same value: what its
	// lists use of our resources until it is done with them, see game_uses
	std::deque<Retired> uses;
	// The value of our own fence it was last made to wait for
	UINT64 waited = 0;
	// Whether the queue is unordered, the frame until which
	// it stays one at least, for how many frames it was made one the last
	// time, and the frame it stopped being one, see UNORDERED_FRAMES
	bool unordered = false;
	UINT unordered_until = 0;
	UINT unordered_frames = 0;
	UINT ordered_since = 0;
};
static std::vector<ForeignQueue*> foreign_queues;
static ForeignQueue* foreign_queue(ID3D12CommandQueue *queue);
static SRWLOCK unordered_queues_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D12CommandQueue*, ForeignQueue*> unordered_queues;
static std::atomic<bool> have_unordered_queues{false};
static void release_retired();
static bool reclaim_foreign_pins(ForeignQueue *foreign);

static std::vector<ShimResource*> touched;

// How many unexecuted lists of the game use each D3D12 resource of ours,
// counting only what Dx12ShimUseOnGameList pinned. An execution on another
// queue than ours counts too until that queue is done with it: what we write
// after handing it the list is not after it for the GPU.
struct GameUses {
	UINT pending;
	UINT foreign;
};
static std::unordered_map<IUnknown*, GameUses> game_uses;

// With the lock held
static void end_game_use(IUnknown *object, UINT GameUses::*kind)
{
	auto found = game_uses.find(object);

	if (found == game_uses.end() || !(found->second.*kind))
		return;
	--(found->second.*kind);
	if (!found->second.pending && !found->second.foreign)
		game_uses.erase(found);
}

static void end_game_uses(const std::vector<IUnknown*> *pins, UINT GameUses::*kind)
{
	for (IUnknown *object : *pins)
		end_game_use(object, kind);
}

static bool game_list_uses(IUnknown *object)
{
	auto found = game_uses.find(object);
	if (found == game_uses.end())
		return false;
	if (found->second.pending)
		return true;
	for (ForeignQueue *foreign : foreign_queues) {
		if (!foreign->uses.empty() && reclaim_foreign_pins(foreign) && !game_uses.count(object))
			return false;
	}
	return true;
}

static CRITICAL_SECTION shim_lock;
static INIT_ONCE shim_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_shim_lock(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	// Held for a draw of the game at a time by the threads recording its
	// command lists, which is short enough to wait out without sleeping
	InitializeCriticalSectionAndSpinCount(&shim_lock, 4000);
	return TRUE;
}

void Dx12ShimLock()
{
	InitOnceExecuteOnce(&shim_lock_once, init_shim_lock, NULL, NULL);
	// Only a contended lock is worth the cost of timing it:
	if (!Profiling::Spike::threshold) {
		EnterCriticalSection(&shim_lock);
		return;
	}
	if (TryEnterCriticalSection(&shim_lock))
		return;
	Profiling::Spike::Scope spike(Profiling::Spike::LOCK_WAIT, L"the D3D12 shim's");
	EnterCriticalSection(&shim_lock);
}

void Dx12ShimUnlock()
{
	LeaveCriticalSection(&shim_lock);
}

bool Dx12ShimLockHeld()
{
	return shim_lock.OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId();
}

void Dx12ShimNotImplemented(const char *method, bool *reported)
{
	if (*reported)
		return;
	*reported = true;
	Dx12LogInfo("*** %s is not implemented on D3D12\n", method);
}

void Dx12LogCloseFailure(ID3D12GraphicsCommandList *list, HRESULT hr)
{
	static std::atomic<unsigned> logged;
	wchar_t name[128] = {};
	UINT size = sizeof(name) - sizeof(wchar_t);

	if (SUCCEEDED(hr) || logged++ >= 100)
		return;
	list->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name);
	Dx12DumpNote("*** Command list %p '%S' failed to close: 0x%x, executing it removes the device\n", list, name, hr);
}

static void log_debug_message(D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, const char *description)
{
	static const char *const severities[] = { "corruption", "error", "warning", "info", "message" };
	static SRWLOCK lock = SRWLOCK_INIT;
	static unsigned logged;
	static std::unordered_map<int, unsigned> noted;
	char line[2048];
	bool wanted;

	// A warning repeated every frame would fill the disk, and one kind of
	// them is not to keep the others out. Errors are all kept: the one that
	// removes the device may be of a kind seen many times before it.
	AcquireSRWLockExclusive(&lock);
	if (severity <= D3D12_MESSAGE_SEVERITY_ERROR)
		wanted = true;
	else if (severity == D3D12_MESSAGE_SEVERITY_WARNING)
		wanted = noted[id]++ < 8;
	else
		wanted = logged++ < 1000;
	ReleaseSRWLockExclusive(&lock);
	if (!wanted)
		return;

	if (severity > D3D12_MESSAGE_SEVERITY_WARNING) {
		Dx12LogInfo("debug layer %s %d: %s\n", severities[min(severity, 4)], id, description);
		return;
	}
	if (_snprintf_s(line, _TRUNCATE, "*** debug layer %s %d: %s\n", severities[min(severity, 4)], id, description) < 0)
		line[sizeof(line) - 2] = '\n';
	Dx12LogInfo("%s", line);
	// The log may be off where the report is on. The thread may be one
	// writing the report, which takes the call to the debug layer.
	Dx12DumpReportLineIfFree(line);
}

static void __stdcall debug_message_callback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
		D3D12_MESSAGE_ID id, LPCSTR description, void *context)
{
	log_debug_message(severity, id, description);
}

void Dx12LogDebugMessages()
{
	static bool said, callback;
	ID3D12InfoQueue *queue;
	ID3D12InfoQueue1 *queue1;
	D3D12_MESSAGE *message;
	std::vector<BYTE> storage;
	UINT64 count, i;
	SIZE_T length;
	DWORD cookie;

	if (!G->dx12_debug_layer)
		return;
	if (FAILED(shim.device->QueryInterface(IID_PPV_ARGS(&queue)))) {
		if (!said)
			Dx12DumpNote("*** dx12_debug_layer: the device has no debug layer, which comes with the Graphics Tools optional feature of Windows\n");
		said = true;
		return;
	}

	count = queue->GetNumStoredMessages();
	if (!said) {
		said = true;
		// What the queue holds from before is still taken from it below
		if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&queue1)))) {
			callback = SUCCEEDED(queue1->RegisterMessageCallback(debug_message_callback,
					D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &cookie));
			queue1->Release();
		}
		Dx12DumpNote("dx12_debug_layer: the debug layer is on, its errors and warnings follow %s\n",
				callback ? "as each call is made" : "at each present");
	} else if (callback) {
		count = 0;
	}

	for (i = 0; i < count; i++) {
		length = 0;
		queue->GetMessage(i, NULL, &length);
		storage.resize(length);
		message = (D3D12_MESSAGE*)storage.data();
		if (SUCCEEDED(queue->GetMessage(i, message, &length)))
			log_debug_message(message->Severity, message->ID, message->pDescription);
	}
	queue->ClearStoredMessages();
	queue->Release();
}

// -----------------------------------------------------------------------------
// Upload memory

const UINT64 UPLOAD_PAGE_SIZE = 4 << 20;
// Pages the GPU is still reading are kept whatever their number, as letting
// go of them frees nothing until it is done. Of those it is done with, the
// ones beyond UPLOAD_PAGES_KEPT go once unused for UPLOAD_PAGE_IDLE_MS, so a
// frame uploading more than the kept pages hold takes the pages of the frames
// before it instead of making new ones.
const size_t UPLOAD_PAGES_KEPT = 8;
const ULONGLONG UPLOAD_PAGE_IDLE_MS = 1000;

struct UploadPage {
	ID3D12Resource *buffer;
	BYTE *cpu;
	UINT64 va;
	UINT64 used;
	UINT64 fence;
	// Of a big one, see upload_big_idle, and when it was last retired
	UINT64 capacity;
	ULONGLONG retired_time;
};
static std::vector<UploadPage> upload_active;
static std::vector<UploadPage> upload_idle;
// An upload larger than a quarter of a page has a buffer to itself. Those
// are kept a while for the next ones, up to UPLOAD_BIG_KEPT bytes: a mod that
// appears uploads up to hundreds of MB in a frame or two, and making an
// upload buffer waits on the CPU until it is resident, which takes up to
// 100 ms for one of 128 MB while the game is loading too. Kept long enough
// for the next characters loaded in the same burst, e.g. looked at one after
// another in a menu.
const UINT64 UPLOAD_BIG_KEPT = 512 << 20;
const ULONGLONG UPLOAD_BIG_IDLE_MS = 15000;
// Big ones are this much apart in size: rounding up to powers of two made
// 128 MB of a 100 MB texture
const UINT64 UPLOAD_BIG_STEP = 16 << 20;
static std::vector<UploadPage> upload_big_active;
static std::vector<UploadPage> upload_big_idle;
static UINT64 upload_big_idle_bytes;
static std::atomic<bool> upload_not_zeroed_refused;

const UINT64 STAGED_UPLOAD_MIN = 1 << 20;
struct IdleStagedUpload {
	Dx12StagedUpload upload;
	ULONGLONG returned;
};
struct RetiredStagedUpload {
	Dx12StagedUpload upload;
	UINT64 fence;
};
static std::mutex staged_upload_lock;
static std::vector<IdleStagedUpload> idle_staged_uploads;
static UINT64 idle_staged_bytes;
static bool staged_uploads_closed;
static std::vector<RetiredStagedUpload> retired_staged_uploads;

// not_zeroed only for a buffer whose every byte the GPU reads is written
// first, which the copies out of a big upload buffer are
static bool create_upload_buffer(UINT64 size, UploadPage *page, bool not_zeroed = false)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};
	D3D12_RANGE no_read = {};
	HRESULT hr = E_FAIL;

	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	*page = {};
	Profiling::Spike::Scope spike(Profiling::Spike::DX12_MOD_RESOURCE, L"upload page");

	// Saves the clearing of the memory. Windows before 10 2004 refuses the
	// flag.
	if (not_zeroed && !upload_not_zeroed_refused.load(std::memory_order_relaxed)) {
		hr = shim.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&page->buffer));
		if (FAILED(hr))
			upload_not_zeroed_refused.store(true, std::memory_order_relaxed);
	}
	if (FAILED(hr) && FAILED(shim.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&page->buffer)))) {
		Dx12LogInfo("*** Failed to create a %llu byte upload buffer\n", size);
		return false;
	}
	if (FAILED(page->buffer->Map(0, &no_read, (void**)&page->cpu))) {
		page->buffer->Release();
		return false;
	}
	page->va = page->buffer->GetGPUVirtualAddress();
	return true;
}

bool Dx12ShimAcquireStagedUpload(UINT64 size, Dx12StagedUpload *upload)
{
	UploadPage page;
	UINT64 capacity = size;

	if (size >= STAGED_UPLOAD_MIN) {
		std::lock_guard<std::mutex> lock(staged_upload_lock);
		auto best = idle_staged_uploads.end();

		for (auto it = idle_staged_uploads.begin(); it != idle_staged_uploads.end(); it++) {
			if (it->upload.capacity >= size && (best == idle_staged_uploads.end()
					|| it->upload.capacity < best->upload.capacity))
				best = it;
		}
		if (best != idle_staged_uploads.end()) {
			*upload = best->upload;
			idle_staged_bytes -= upload->capacity;
			idle_staged_uploads.erase(best);
			return true;
		}
		if (size > UPLOAD_BIG_STEP && size <= UINT64_MAX - (UPLOAD_BIG_STEP - 1))
			capacity = ShimAlign(size, UPLOAD_BIG_STEP);
	}
	if (!create_upload_buffer(capacity, &page, true))
		return false;
	*upload = { page.buffer, page.cpu, capacity };
	return true;
}

static void return_staged_upload(const Dx12StagedUpload &upload)
{
	if (upload.capacity >= STAGED_UPLOAD_MIN) {
		std::lock_guard<std::mutex> lock(staged_upload_lock);

		if (!staged_uploads_closed && upload.capacity <= UPLOAD_BIG_KEPT - idle_staged_bytes) {
			idle_staged_uploads.push_back({ upload, GetTickCount64() });
			idle_staged_bytes += upload.capacity;
			return;
		}
	}
	upload.buffer->Release();
}

void Dx12ShimRecycleStagedUpload(const Dx12StagedUpload &upload, UINT64 copy_fence)
{
	if (!upload.buffer)
		return;
	if (copy_fence && !Dx12ShimFenceReached(copy_fence))
		retired_staged_uploads.push_back({ upload, copy_fence });
	else
		return_staged_upload(upload);
}

static void trim_staged_uploads(ULONGLONG now)
{
	std::vector<IdleStagedUpload> expired;

	{
		std::lock_guard<std::mutex> lock(staged_upload_lock);
		auto first_kept = idle_staged_uploads.begin();

		while (first_kept != idle_staged_uploads.end() && now >= first_kept->returned
				&& now - first_kept->returned >= UPLOAD_BIG_IDLE_MS) {
			idle_staged_bytes -= first_kept->upload.capacity;
			first_kept++;
		}
		expired.assign(idle_staged_uploads.begin(), first_kept);
		idle_staged_uploads.erase(idle_staged_uploads.begin(), first_kept);
	}
	for (const IdleStagedUpload &entry : expired)
		entry.upload.buffer->Release();
}

static void close_staged_upload_pool()
{
	std::vector<IdleStagedUpload> idle;

	{
		std::lock_guard<std::mutex> lock(staged_upload_lock);
		staged_uploads_closed = true;
		idle.swap(idle_staged_uploads);
		idle_staged_bytes = 0;
	}
	for (const IdleStagedUpload &entry : idle)
		entry.upload.buffer->Release();
}

static void release_retired_staged_uploads(UINT64 completed)
{
	retired_staged_uploads.erase(std::remove_if(retired_staged_uploads.begin(), retired_staged_uploads.end(),
		[completed](const RetiredStagedUpload &entry) {
			if (entry.fence > completed)
				return false;
			return_staged_upload(entry.upload);
			return true;
		}), retired_staged_uploads.end());
}

BYTE* Dx12ShimUpload(UINT64 size, UINT64 alignment, ID3D12Resource **buffer, UINT64 *offset, UINT64 *va)
{
	UINT64 completed, start;
	UploadPage page;
	size_t i;

	if (size > UPLOAD_PAGE_SIZE / 4) {
		auto best = upload_big_idle.end();

		completed = fence->GetCompletedValue();
		for (auto it = upload_big_idle.begin(); it != upload_big_idle.end(); it++) {
			if (it->fence <= completed && it->capacity >= size
					&& (best == upload_big_idle.end() || it->capacity < best->capacity))
				best = it;
		}
		if (best != upload_big_idle.end()) {
			page = *best;
			upload_big_idle.erase(best);
			upload_big_idle_bytes -= page.capacity;
		} else {
			// Rounded up so that it fits more of the next ones
			UINT64 capacity = UPLOAD_PAGE_SIZE;
			while (capacity < size && capacity < UPLOAD_BIG_STEP)
				capacity *= 2;
			if (capacity < size)
				capacity = (size + UPLOAD_BIG_STEP - 1) / UPLOAD_BIG_STEP * UPLOAD_BIG_STEP;
			if (!create_upload_buffer(capacity, &page, true))
				return NULL;
			page.capacity = capacity;
		}
		upload_big_active.push_back(page);
		if (buffer)
			*buffer = page.buffer;
		if (offset)
			*offset = 0;
		if (va)
			*va = page.va;
		return page.cpu;
	}

	start = UPLOAD_PAGE_SIZE;
	if (!upload_active.empty())
		start = (upload_active.back().used + alignment - 1) / alignment * alignment;

	if (start + size > UPLOAD_PAGE_SIZE) {
		completed = fence->GetCompletedValue();
		for (i = 0; i < upload_idle.size(); i++) {
			if (upload_idle[i].fence <= completed)
				break;
		}
		if (i < upload_idle.size()) {
			page = upload_idle[i];
			upload_idle.erase(upload_idle.begin() + i);
		} else if (!create_upload_buffer(UPLOAD_PAGE_SIZE, &page)) {
			return NULL;
		} else {
			InterlockedIncrement64(&dx12_stats.upload_pages_created);
		}
		page.used = 0;
		upload_active.push_back(page);
		start = 0;
	}

	UploadPage &active = upload_active.back();
	active.used = start + size;
	if (buffer)
		*buffer = active.buffer;
	if (offset)
		*offset = start;
	if (va)
		*va = active.va + start;
	return active.cpu + start;
}

// Also called at every present, as nothing may be submitted for a while
static void release_idle_upload_buffers(UINT64 completed, ULONGLONG now)
{
	// Over UPLOAD_BIG_KEPT, the oldest go first
	for (size_t i = 0; i < upload_big_idle.size(); ) {
		UploadPage &page = upload_big_idle[i];
		if (page.fence > completed || (upload_big_idle_bytes <= UPLOAD_BIG_KEPT
				&& now - page.retired_time < UPLOAD_BIG_IDLE_MS)) {
			i++;
			continue;
		}
		upload_big_idle_bytes -= page.capacity;
		page.buffer->Release();
		upload_big_idle.erase(upload_big_idle.begin() + i);
	}
}

// At every submission, and at present for when none follows a burst of uploads
static void release_idle_upload_pages(UINT64 completed, ULONGLONG now)
{
	size_t i, done;

	if (upload_idle.size() <= UPLOAD_PAGES_KEPT)
		return;
	done = (size_t)std::count_if(upload_idle.begin(), upload_idle.end(), [=](const UploadPage &page) { return page.fence <= completed; });
	for (i = upload_idle.size(); i-- > 0 && done > UPLOAD_PAGES_KEPT;) {
		if (upload_idle[i].fence > completed || now - upload_idle[i].retired_time < UPLOAD_PAGE_IDLE_MS)
			continue;
		upload_idle[i].buffer->Release();
		upload_idle.erase(upload_idle.begin() + i);
		done--;
		InterlockedIncrement64(&dx12_stats.upload_pages_released);
	}
}

static void retire_upload_pages()
{
	UINT64 completed = fence->GetCompletedValue();
	ULONGLONG now = GetTickCount64();
	size_t i, keep = 0;

	// Keep filling the last page where this submission left off, or every
	// submission with something small to upload would take a page of its
	// own. It gets the fence value of the last submission that used it
	// when it is retired.
	if (!upload_active.empty() && upload_active.back().used < UPLOAD_PAGE_SIZE)
		keep = 1;

	for (i = 0; i + keep < upload_active.size(); i++) {
		upload_active[i].fence = fence_signalled;
		upload_active[i].retired_time = now;
		upload_idle.push_back(upload_active[i]);
	}
	upload_active.erase(upload_active.begin(), upload_active.end() - keep);

	for (UploadPage &page : upload_big_active) {
		page.fence = fence_signalled;
		page.retired_time = now;
		upload_big_idle.push_back(page);
		upload_big_idle_bytes += page.capacity;
	}
	upload_big_active.clear();
	release_idle_upload_buffers(completed, now);
	release_idle_upload_pages(completed, now);
}

// -----------------------------------------------------------------------------
// Descriptors shaders can use

const UINT VIEW_PAGE_SIZE = 4096;
const UINT SAMPLER_PAGE_SIZE = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

struct DescriptorPage {
	ID3D12DescriptorHeap *heap;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	UINT used;
	UINT64 fence;
};

struct DescriptorPages {
	D3D12_DESCRIPTOR_HEAP_TYPE type;
	UINT size;
	UINT increment;
	// heap is NULL until the first allocation
	DescriptorPage current;
	std::vector<DescriptorPage> idle;
};
static DescriptorPages view_pages = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_PAGE_SIZE };
static DescriptorPages sampler_pages = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_PAGE_SIZE };

using SamplerTableKey = std::pair<UINT, std::array<SIZE_T, SHIM_SAMPLER_SLOTS>>;
static std::map<SamplerTableKey, UINT> sampler_tables;
static ID3D12DescriptorHeap *bound_view_heap;
static ID3D12DescriptorHeap *bound_sampler_heap;

static bool next_descriptor_page(DescriptorPages *pages)
{
	UINT64 completed = fence->GetCompletedValue();
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	DescriptorPage page = {};
	size_t i;

	if (pages->current.heap) {
		pages->current.fence = Dx12ShimPendingFence();
		pages->idle.push_back(pages->current);
		pages->current = {};
	}

	for (i = 0; i < pages->idle.size(); i++) {
		if (pages->idle[i].fence <= completed) {
			pages->current = pages->idle[i];
			pages->current.used = 0;
			pages->idle.erase(pages->idle.begin() + i);
			return true;
		}
	}

	desc.Type = pages->type;
	desc.NumDescriptors = pages->size;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(shim.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&page.heap)))) {
		Dx12LogInfo("*** Failed to create a shader visible descriptor heap of type %d\n", pages->type);
		return false;
	}
	page.cpu = page.heap->GetCPUDescriptorHandleForHeapStart();
	page.gpu = page.heap->GetGPUDescriptorHandleForHeapStart();
	pages->current = page;
	return true;
}

static bool reserve_descriptors(DescriptorPages *pages, UINT count)
{
	if (pages->current.heap && pages->current.used + count <= pages->size)
		return true;
	return count <= pages->size && next_descriptor_page(pages);
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocViews(UINT count, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};

	*gpu = {};
	if (!reserve_descriptors(&view_pages, count))
		return cpu;

	cpu.ptr = view_pages.current.cpu.ptr + (SIZE_T)view_pages.current.used * view_pages.increment;
	gpu->ptr = view_pages.current.gpu.ptr + (UINT64)view_pages.current.used * view_pages.increment;
	view_pages.current.used += count;
	return cpu;
}

bool Dx12ShimReserveSamplers(UINT count)
{
	ID3D12DescriptorHeap *heap = sampler_pages.current.heap;

	bool reserved = !count || reserve_descriptors(&sampler_pages, count);
	if (sampler_pages.current.heap != heap)
		sampler_tables.clear();
	return reserved;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12ShimSamplerTable(const D3D12_CPU_DESCRIPTOR_HANDLE *samplers, UINT count)
{
	DescriptorPage *page = &sampler_pages.current;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
	SamplerTableKey key = {count, {}};
	UINT i, start;

	if (!page->heap || count > key.second.size() || page->used + count > sampler_pages.size)
		return gpu;

	for (i = 0; i < count; i++)
		key.second[i] = samplers[i].ptr;

	auto found = sampler_tables.find(key);
	if (found != sampler_tables.end()) {
		start = found->second;
	} else {
		start = page->used;
		page->used += count;
		cpu.ptr = page->cpu.ptr + (SIZE_T)start * sampler_pages.increment;
		shim.device->CopyDescriptors(1, &cpu, &count, count, samplers, NULL, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		sampler_tables.emplace(key, start);
	}

	gpu.ptr = page->gpu.ptr + (UINT64)start * sampler_pages.increment;
	return gpu;
}

void Dx12ShimBindHeaps()
{
	ID3D12DescriptorHeap *heaps[2];
	UINT count = 0;

	if (view_pages.current.heap == bound_view_heap && sampler_pages.current.heap == bound_sampler_heap)
		return;

	bound_view_heap = view_pages.current.heap;
	bound_sampler_heap = sampler_pages.current.heap;
	if (bound_view_heap)
		heaps[count++] = bound_view_heap;
	if (bound_sampler_heap)
		heaps[count++] = bound_sampler_heap;
	if (count)
		Dx12ShimList()->SetDescriptorHeaps(count, heaps);
}

// -----------------------------------------------------------------------------
// Descriptors of views and samplers

struct DescriptorPool {
	UINT chunk;
	UINT increment;
	std::vector<ID3D12DescriptorHeap*> heaps;
	std::vector<SIZE_T> free;
};
static DescriptorPool pools[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = { { 4096 }, { 256 }, { 256 }, { 64 } };

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	DescriptorPool *pool = &pools[type];
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
	ID3D12DescriptorHeap *heap;
	SIZE_T start;
	UINT i;

	if (pool->free.empty()) {
		desc.Type = type;
		desc.NumDescriptors = pool->chunk;
		if (FAILED(shim.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
			Dx12LogInfo("*** Failed to create a descriptor heap of type %d\n", type);
			return handle;
		}
		pool->heaps.push_back(heap);
		start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
		for (i = pool->chunk; i-- > 0;)
			pool->free.push_back(start + (SIZE_T)i * pool->increment);
	}

	handle.ptr = pool->free.back();
	pool->free.pop_back();
	return handle;
}

void Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	if (!handle.ptr)
		return;

	pools[type].free.push_back(handle.ptr);
	// The next sampler to get this descriptor must not be taken for the
	// one that had it
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
		sampler_tables.clear();
}

// -----------------------------------------------------------------------------
// Resource states

const D3D12_RESOURCE_STATES READ_STATES = D3D12_RESOURCE_STATE_GENERIC_READ
	| D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

static bool read_only(D3D12_RESOURCE_STATES state)
{
	return state && !(state & ~READ_STATES);
}

static bool prepare_transition(ShimResource *resource, bool write, bool overwrite, bool *retry = nullptr)
{
	if (resource->present_queue && resource->present_queue != shim.queue)
		return false;
	if (!resource->resource || !Dx12ShimUseResource(resource)
	    || (!resource->deferred_writes.empty() && !ShimApplyDeferredWrites(resource))
	    || (resource->write_pending && !ShimFlushWrite(resource))) {
		if (retry)
			*retry = ShimNotReady(resource);
		return false;
	}

	// What is about to be written is in use by lists of the game that
	// were recorded before and will be executed after: they keep it as it
	// is, see Bridge.h
	if (write && !resource->foreign && game_list_uses(resource->resource)
	    && !ShimRenameResource(resource, !overwrite)) {
		if (retry)
			*retry = true;
		return false;
	}
	if (write)
		resource->version++;

	if ((!resource->foreign || resource->present_queue) && resource->touched_epoch != dx12_shim_epoch) {
		resource->touched_epoch = dx12_shim_epoch;
		if (resource->present_queue)
			Dx12ShimDeferRelease(resource->resource);
		else
			touched.push_back(resource);
	}
	return true;
}

static void uav_barrier(ShimResource *resource)
{
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	barrier.UAV.pResource = resource->resource;
	Profiling::Spike::SelfTime self_time(Profiling::Spike::CL_D3D12_BARRIER);
	Dx12ShimList()->ResourceBarrier(1, &barrier);
}

static bool transition_state(ShimResource *resource, UINT index, D3D12_RESOURCE_STATES &before,
		D3D12_RESOURCE_STATES state)
{
	D3D12_RESOURCE_BARRIER barrier = {};

	if (read_only(before) && read_only(state))
		state |= before;
	if (before == state)
		return state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource->resource;
	barrier.Transition.Subresource = index;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = state;
	Profiling::Spike::SelfTime self_time(Profiling::Spike::CL_D3D12_BARRIER);
	Dx12ShimList()->ResourceBarrier(1, &barrier);
	before = state;
	return false;
}

void Dx12ShimSetState(ShimResource *resource, D3D12_RESOURCE_STATES state)
{
	auto &states = resource->subresource_states;
	bool uav = false;
	if (states.empty()) {
		uav = transition_state(resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, resource->state, state);
	} else {
		for (UINT i = 0; i < states.size(); i++)
			uav |= transition_state(resource, i, states[i], state);
		if (std::all_of(states.begin(), states.end(), [&](auto entry) { return entry == states.front(); })) {
			resource->state = states.front();
			states.clear();
		}
	}
	if (uav)
		uav_barrier(resource);
}

static UINT texture_plane_size(const ShimResource *resource)
{
	return resource->desc.MipLevels * ShimTextureArraySize(resource->desc);
}

static void split_states(ShimResource *resource, UINT count)
{
	if (resource->subresource_states.empty())
		resource->subresource_states.assign(count, resource->state);
}

bool Dx12ShimTransition(ShimResource *resource, D3D12_RESOURCE_STATES state, bool overwrite)
{
	if (!prepare_transition(resource, !!(state & ~READ_STATES), overwrite))
		return false;
	Dx12ShimSetState(resource, state);
	return true;
}

bool Dx12ShimTransitionSubresource(ShimResource *resource, UINT subresource, D3D12_RESOURCE_STATES state, bool overwrite)
{
	UINT count = texture_plane_size(resource) * ShimPlaneCount(resource);
	if (subresource >= count)
		return false;
	if (count == 1)
		return Dx12ShimTransition(resource, state, overwrite);
	if (!prepare_transition(resource, !!(state & ~READ_STATES), false))
		return false;
	split_states(resource, count);
	if (transition_state(resource, subresource, resource->subresource_states[subresource], state))
		uav_barrier(resource);
	return true;
}

static void transition_view(ShimResource *resource, const ShimView::Range &range, UINT planes, D3D12_RESOURCE_STATES state)
{
	UINT plane_size = texture_plane_size(resource);
	UINT plane_count = ShimPlaneCount(resource);
	if (range.mips * range.slices == plane_size && planes == (1u << plane_count) - 1) {
		Dx12ShimSetState(resource, state);
		return;
	}
	split_states(resource, plane_size * plane_count);
	bool uav = false;
	for (UINT plane = 0; plane < plane_count; plane++) {
		if (!(planes & (1u << plane)))
			continue;
		for (UINT slice = range.slice; slice < range.slice + range.slices; slice++) {
			for (UINT mip = range.mip; mip < range.mip + range.mips; mip++) {
				UINT index = plane * plane_size + slice * resource->desc.MipLevels + mip;
				uav |= transition_state(resource, index, resource->subresource_states[index], state);
			}
		}
	}
	if (uav)
		uav_barrier(resource);
}

bool Dx12ShimTransitionView(const ShimView *view, D3D12_RESOURCE_STATES state, UINT planes, bool *retry)
{
	ShimResource *resource = view->resource;
	planes &= view->range.planes;
	if (!planes || !view->range.mips || !view->range.slices)
		return false;
	if (!prepare_transition(resource, !!(state & ~READ_STATES), false, retry))
		return false;
	if (resource->dimension == D3D11_RESOURCE_DIMENSION_BUFFER)
		Dx12ShimSetState(resource, state);
	else
		transition_view(resource, view->range, planes, state);
	return true;
}

bool Dx12ShimTransitionDepthStencil(const ShimView *view)
{
	UINT writable = view->range.planes & ~view->read_only_planes;
	if (!view->read_only_planes)
		return Dx12ShimTransitionView(view, D3D12_RESOURCE_STATE_DEPTH_WRITE);
	if (!writable)
		return Dx12ShimTransitionView(view, D3D12_RESOURCE_STATE_DEPTH_READ);
	if (!Dx12ShimTransitionView(view, D3D12_RESOURCE_STATE_DEPTH_WRITE, writable))
		return false;
	transition_view(view->resource, view->range, view->read_only_planes, D3D12_RESOURCE_STATE_DEPTH_READ);
	return true;
}

void Dx12ShimForgetResource(ShimResource *resource)
{
	size_t i;

	for (i = 0; i < touched.size(); i++) {
		if (touched[i] == resource) {
			touched.erase(touched.begin() + i);
			return;
		}
	}
}

// -----------------------------------------------------------------------------
// Residency

// Smaller resources are not worth the time it takes to make them resident
const UINT64 EVICTED_BYTES_MIN = 1 << 20;
const ULONGLONG EVICT_INTERVAL_MS = 1000;

static std::vector<ShimResource*> residents;
// GetTickCount64 at the last present, for Dx12ShimUseResource
static ULONGLONG present_time;
static ULONGLONG evicted_time;
// Counts the presents, and the one after which making a resource resident
// again last failed
static UINT presents = 1, failed_present;

void Dx12ShimTrackResidency(ShimResource *resource, bool tracked)
{
	if (tracked) {
		resource->residency_index = residents.size();
		resource->used_time = present_time;
		residents.push_back(resource);
		return;
	}

	residents.back()->residency_index = resource->residency_index;
	residents[resource->residency_index] = residents.back();
	residents.pop_back();
	if (resource->evicted) {
		resource->evicted = false;
		dx12_memory.evicted--;
		dx12_memory.evicted_bytes -= resource->allocated;
	}
}

bool Dx12ShimProcessMemory(DXGI_QUERY_VIDEO_MEMORY_INFO *info)
{
	static IDXGIAdapter3 *adapter;
	static bool failed;
	IDXGIFactory4 *factory;

	if (!adapter && !failed) {
		failed = true;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			failed = FAILED(factory->EnumAdapterByLuid(shim.device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
			factory->Release();
		}
	}
	return adapter && SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, info));
}

static void describe_resource(char *text, size_t size, const ShimResource *resource)
{
	if (resource->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		sprintf_s(text, size, "a buffer of the mods, %llu bytes%s", resource->desc.Width,
			resource->evicted ? ", evicted" : "");
	else
		sprintf_s(text, size, "a texture of the mods, %llu x %u, %u mips, format %d%s", resource->desc.Width,
			resource->desc.Height, resource->desc.MipLevels, resource->desc.Format,
			resource->evicted ? ", evicted" : "");
}

static bool buffer_at(ID3D12Resource *buffer, UINT64 va)
{
	UINT64 start = buffer->GetGPUVirtualAddress();

	return start && va >= start && va < start + buffer->GetDesc().Width;
}

void Dx12ShimNoteFault(UINT64 va, const std::vector<const void*> &objects, void (*note)(const char *format, ...))
{
	const std::vector<UploadPage> *pages[] = { &upload_active, &upload_idle, &upload_big_active, &upload_big_idle };
	char text[256];
	UINT found = 0;
	bool locked = false;

	// Called on a thread of its own once the device is gone. Whoever has
	// the lock may be waiting for the GPU, which the removal ends.
	InitOnceExecuteOnce(&shim_lock_once, init_shim_lock, NULL, NULL);
	for (int waited = 0; waited < 2000 && !(locked = TryEnterCriticalSection(&shim_lock)); waited += 10)
		Sleep(10);
	if (!locked) {
		note("    what of the mods is there is not known: the lock of the D3D12 shim stayed taken\n");
		return;
	}

	for (const ShimResource *resource : residents) {
		if (!resource->resource)
			continue;
		if (std::find(objects.begin(), objects.end(), resource->resource) != objects.end()) {
			describe_resource(text, sizeof(text), resource);
			note("    among the allocations there: %p, %s\n", resource->resource, text);
			found++;
		}
		if (resource->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
			continue;
		if (buffer_at(resource->resource, va)) {
			describe_resource(text, sizeof(text), resource);
			note("    at the address: %p, %s\n", resource->resource, text);
			found++;
		}
		for (const ShimResource::Spare &spare : resource->spares) {
			if (buffer_at(spare.resource, va)) {
				describe_resource(text, sizeof(text), resource);
				note("    at the address: %p, a resource kept for renaming %s\n", spare.resource, text);
				found++;
			}
		}
	}
	for (const std::vector<UploadPage> *list : pages) {
		for (const UploadPage &page : *list) {
			if (buffer_at(page.buffer, va)) {
				note("    at the address: %p, an upload buffer of the mods\n", page.buffer);
				found++;
			}
		}
	}
	if (!found)
		note("    nothing of the mods there\n");
	LeaveCriticalSection(&shim_lock);
}

// Nothing recorded or executed, or yet to be, uses it: none of our
// submissions since the GPU finished the last that did, nothing recorded on
// our list since (touched in this epoch is not enough: the fence also moves
// on for the game's lists while ours stays empty), and no list of the game,
// which keeps a reference until it has been executed (Dx12ShimPin), as does
// whatever else is yet to use it
static bool resource_idle(ShimResource *resource, ULONGLONG idle_since)
{
	if (resource->staged || resource->evicted || resource->foreign || !resource->resource || resource->allocated < EVICTED_BYTES_MIN
	    || resource->used_time > idle_since || !Dx12ShimFenceReached(resource->used_fence)
	    || (resource->touched_epoch == dx12_shim_epoch
		&& (list_used || !resource->subresource_states.empty() || resource->state != D3D12_RESOURCE_STATE_COMMON)))
		return false;
	resource->resource->AddRef();
	return resource->resource->Release() == 1;
}

// Evicts the resources idle since then, oldest first, until that many bytes
// are freed. Whether any was.
static bool evict(UINT64 bytes, ULONGLONG idle_since)
{
	std::vector<std::pair<ULONGLONG, ShimResource*>> idle;
	std::vector<ID3D12Pageable*> pageables;
	UINT64 freed = 0;

	for (ShimResource *resource : residents) {
		if (resource_idle(resource, idle_since))
			idle.push_back({ resource->used_time, resource });
	}
	std::sort(idle.begin(), idle.end());
	for (auto &entry : idle) {
		if (freed >= bytes)
			break;
		// Its transition to the common state at the next submission
		// has nothing to do, and would make it resident again
		if (entry.second->touched_epoch == dx12_shim_epoch) {
			Dx12ShimForgetResource(entry.second);
			entry.second->touched_epoch = 0;
		}
		pageables.push_back(entry.second->resource);
		entry.second->evicted = true;
		freed += entry.second->allocated;
		dx12_memory.evicted++;
		dx12_memory.evicted_bytes += entry.second->allocated;
		dx12_memory.evictions++;
		dx12_memory.eviction_bytes += entry.second->allocated;
	}
	if (pageables.empty())
		return false;
	shim.device->Evict((UINT)pageables.size(), pageables.data());
	return true;
}

static void evict_idle()
{
	DXGI_QUERY_VIDEO_MEMORY_INFO memory;
	ULONGLONG idle_ms = (ULONGLONG)G->dx12_evict_idle_seconds * 1000;
	UINT64 limit;

	if (present_time - evicted_time < EVICT_INTERVAL_MS)
		return;
	evicted_time = present_time;
	if (!G->dx12_evict_idle) {
		ModLoadSetDx12SparePressure(false);
		return;
	}
	if (!Dx12ShimProcessMemory(&memory))
		return;
	limit = memory.Budget / 100 * G->dx12_evict_budget_percent;
	const UINT64 excess = memory.CurrentUsage > limit ? memory.CurrentUsage - limit : 0;
	const UINT64 released = ModLoadSetDx12SparePressure(excess != 0);
	if (excess > released && present_time >= idle_ms)
		evict(excess - released, present_time - idle_ms);
}

// CreateCommittedResource waits on the CPU until what it made is resident,
// a millisecond or two for a texture of a few tens of MB, and so does
// MakeResident for an evicted one. Instead a resource of a mod is made
// resident on the GPU's time, and a queue about to execute what uses resources
// of the mods waits there for the last that a list may use. NULL where that
// cannot be done:
static ID3D12Device3 *residency_device;
static ID3D12Fence *residency_fence;
static std::mutex residency_lock;
static UINT64 residency_asked;
static std::atomic<UINT64> residency_needed{0};

static HRESULT enqueue_make_resident(ID3D12Pageable *pageable, UINT64 *residency,
		Dx12ResourceCreationTiming *timing = nullptr)
{
	LARGE_INTEGER start, acquired, end;

	if (timing)
		QueryPerformanceCounter(&start);
	// The fence is signalled in the order of the values
	std::lock_guard<std::mutex> lock(residency_lock);
	if (timing)
		QueryPerformanceCounter(&acquired);
	UINT64 value = residency_asked + 1;
	HRESULT hr = residency_device->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1, &pageable, residency_fence, value);
	if (timing) {
		QueryPerformanceCounter(&end);
		timing->residency_lock_ticks += acquired.QuadPart - start.QuadPart;
		timing->residency_request_ticks += end.QuadPart - acquired.QuadPart;
		timing->residency_requested = true;
	}

	if (SUCCEEDED(hr)) {
		residency_asked = value;
		*residency = value;
	}
	return hr;
}

void Dx12ShimNeedResidency(UINT64 value)
{
	UINT64 needed = residency_needed.load(std::memory_order_relaxed);

	while (needed < value && !residency_needed.compare_exchange_weak(needed, value,
			std::memory_order_release, std::memory_order_relaxed)) {}
}

static void residency_init()
{
	if (FAILED(shim.device->QueryInterface(IID_PPV_ARGS(&residency_device))))
		return;
	if (FAILED(shim.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&residency_fence)))) {
		residency_device->Release();
		residency_device = NULL;
	}
}

static HRESULT create_committed_default_resource(const D3D12_HEAP_PROPERTIES &heap, D3D12_HEAP_FLAGS flags,
		const D3D12_RESOURCE_DESC *desc, ID3D12Resource **resource, Dx12ResourceCreationTiming *timing)
{
	LARGE_INTEGER start, end;

	if (timing)
		QueryPerformanceCounter(&start);
	HRESULT hr = shim.device->CreateCommittedResource(&heap, flags, desc,
			D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(resource));
	if (timing) {
		QueryPerformanceCounter(&end);
		timing->create_ticks += end.QuadPart - start.QuadPart;
		timing->create_calls++;
	}
	return hr;
}

HRESULT Dx12ShimCreateResource(const D3D12_RESOURCE_DESC *desc, ID3D12Resource **resource,
		UINT64 *residency, Dx12ResourceCreationTiming *timing)
{
	D3D12_HEAP_PROPERTIES heap = {};
	UINT64 value;
	HRESULT hr;

	if (residency)
		*residency = 0;
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if (residency_device && SUCCEEDED(create_committed_default_resource(heap, D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT,
			desc, resource, timing))) {
		hr = enqueue_make_resident(*resource, &value, timing);
		if (SUCCEEDED(hr)) {
			if (residency)
				*residency = value;
			else
				Dx12ShimNeedResidency(value);
			return hr;
		}
		(*resource)->Release();
		*resource = NULL;
	}

	return create_committed_default_resource(heap, D3D12_HEAP_FLAG_NONE, desc, resource, timing);
}

HRESULT Dx12ShimCreateResidentSpare(const D3D12_RESOURCE_DESC &desc, ID3D12Resource **resource)
{
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	return create_committed_default_resource(heap, D3D12_HEAP_FLAG_NONE, &desc, resource, nullptr);
}

// Every use of an evicted resource is recorded after this and executed by a
// queue that calls Dx12ShimResidencyWait first, so it need not be resident
// before then. MakeResident would hold the draw up until it is, which with a
// party of a few characters' mods switched in is many of them in a row.
static HRESULT make_resident_again(ID3D12Pageable *pageable)
{
	UINT64 value;
	HRESULT hr;

	if (!residency_device)
		return shim.device->MakeResident(1, &pageable);
	hr = enqueue_make_resident(pageable, &value);
	if (SUCCEEDED(hr))
		Dx12ShimNeedResidency(value);
	return hr;
}

bool Dx12ShimReadGameResource(ID3D12Resource *resource)
{
	static unsigned noted;
	Dx12GameReadResult result = Dx12GameRead(resource, GetTickCount64(), Dx12ShimPendingFence(), make_resident_again);
	D3D12_RESOURCE_DESC desc;

	// The fence of the read is signalled with the next submission even if
	// the caller records nothing after all, which would otherwise hold its
	// Evict calls back for good
	if (result != DX12_GAME_READ_FAILED)
		Dx12ShimList();
	if (result == DX12_GAME_READ_RESIDENT)
		return true;
	if (result == DX12_GAME_READ_RESTORED)
		dx12_memory.game_restores++;
	else
		dx12_memory.game_restore_failures++;
	if (noted < 20) {
		noted++;
		desc = resource->GetDesc();
		Dx12LogInfo("A resource of the game that it had evicted was to be read by a list of ours (%s, %llu x %u, format %u): %s\n",
			desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? "buffer" : "texture", desc.Width, desc.Height, desc.Format,
			result == DX12_GAME_READ_RESTORED ? "made resident again" : "making it resident failed, the read is left out");
	}
	return result == DX12_GAME_READ_RESTORED;
}

static void trim_spares()
{
	static ULONGLONG trimmed_time;

	if (present_time - trimmed_time < EVICT_INTERVAL_MS)
		return;
	trimmed_time = present_time;
	ShimTrimSpares();
}

// Passes on the Evict calls of the game held back for reads of lists of ours
// that are over, see Dx12GameEvict
static void release_held_evictions()
{
	static ULONGLONG released_time;
	std::vector<Dx12HeldEviction> expired;
	ID3D12Pageable *pageable;

	if (present_time - released_time < EVICT_INTERVAL_MS)
		return;
	released_time = present_time;
	Dx12TakeExpiredEvictions(present_time - DX12_GAME_READ_HOLD_MS, Dx12ShimFenceReached, &expired);
	for (Dx12HeldEviction &held : expired) {
		pageable = held.resource;
		InterlockedAdd(&dx12_memory.game_evictions_passed, held.count);
		for (; held.count; held.count--)
			shim.device->Evict(1, &pageable);
		held.resource->Release();
	}
}

void Dx12ShimResidencyWait(ID3D12CommandQueue *queue)
{
	UINT64 value = residency_needed.load(std::memory_order_acquire);

	if (value && residency_fence->GetCompletedValue() < value)
		queue->Wait(residency_fence, value);
}

bool Dx12ShimUseResource(ShimResource *resource)
{
	static bool failed;
	LARGE_INTEGER start, end;
	ID3D12Pageable *pageable = resource->resource;
	HRESULT hr;

	resource->used_time = present_time;
	resource->used_fence = Dx12ShimPendingFence();
	if (!resource->evicted)
		return true;
	// Each try takes long, and a frame has many uses: once one failed,
	// the rest wait for the next present
	if (failed_present == presents) {
		dx12_memory.restore_failures++;
		return false;
	}

	QueryPerformanceCounter(&start);
	hr = make_resident_again(pageable);
	// With room made by evicting whatever else is not in use, short of
	// what was used since the last present
	if (FAILED(hr) && evict(resource->allocated, present_time - 1))
		hr = make_resident_again(pageable);
	QueryPerformanceCounter(&end);
	dx12_memory.restore_ticks += end.QuadPart - start.QuadPart;
	if (FAILED(hr)) {
		if (!failed)
			Dx12LogInfo("*** Failed to make a resource of a mod resident again, what uses it is left out until it can be: 0x%x\n", hr);
		failed = true;
		failed_present = presents;
		dx12_memory.restore_failures++;
		return false;
	}
	resource->evicted = false;
	dx12_memory.evicted--;
	dx12_memory.evicted_bytes -= resource->allocated;
	dx12_memory.restores++;
	dx12_memory.restore_bytes += resource->allocated;
	return true;
}

// -----------------------------------------------------------------------------
// Game lists

const UINT64 GAME_UPLOAD_BLOCK_SIZE = 1 << 20;

// For how long the lists of a queue go without the mods the first time it
// could not be made to wait for them. Each time again it is twice as long,
// unless the queue could wait for this many times as long as the last time.
const UINT UNORDERED_FRAMES = 30;
const UINT UNORDERED_FORGET_TIMES = 4;
const ULONGLONG FOREIGN_SIGNAL_RETRY_MS = 100;

const size_t GAME_UPLOAD_BLOCKS_KEPT = 8;

// The upload buffer being handed out for game lists. Those that are full are
// kept alive by the lists and buffers using them, and a few of them by a
// reference of ours as well: once that is the only one left, the lists have
// been executed and the buffers have their contents elsewhere, and the block
// is handed out again.
static UploadPage game_upload;
static std::vector<UploadPage> game_upload_full;

static bool next_game_upload(UploadPage *page)
{
	size_t i;

	for (i = 0; i < game_upload_full.size(); i++) {
		game_upload_full[i].buffer->AddRef();
		if (game_upload_full[i].buffer->Release() != 1)
			continue;
		*page = game_upload_full[i];
		page->used = 0;
		game_upload_full.erase(game_upload_full.begin() + i);
		return true;
	}
	return create_upload_buffer(GAME_UPLOAD_BLOCK_SIZE, page);
}

bool Dx12ShimPin(Dx12GraphicsCommandList *list, IUnknown *object)
{
	IUnknown **pinned = &list->mPinned[((UINT64)(uintptr_t)object * 0x9e3779b97f4a7c15ull) >> 59];

	// What was done to the object for the list is in the submission being
	// recorded, if not in one before it
	list->mShimFence = Dx12ShimPendingFence();
	if (*pinned == object)
		return false;
	*pinned = object;
	object->AddRef();
	list->mPins.push_back(object);
	return true;
}

static HRESULT signal_shim_queue(UINT64 value)
{
	Dx12QueueLock(shim.queue);
	HRESULT hr = shim.queue->Signal(fence, value);
	if (SUCCEEDED(hr)) {
		last_shim_signal = value;
		Dx12QueueSignalled(shim.queue, fence, value);
	}
	Dx12QueueUnlock(shim.queue);
	if (FAILED(hr))
		Dx12LogInfo("*** Shim queue could not signal fence %llu (0x%x)\n", value, hr);
	return hr;
}

static void set_queue_unordered(ForeignQueue *foreign, bool unordered, const char *why = "");

static bool submit_foreign_signal(ForeignQueue *foreign)
{
	if (!foreign->fence) {
		ID3D12Device *device;
		if (FAILED(foreign->queue->GetDevice(IID_PPV_ARGS(&device))))
			return false;
		HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&foreign->fence));
		device->Release();
		if (FAILED(hr))
			return false;
	}
	UINT64 next = foreign->signalled + 1;
	if (FAILED(foreign->queue->Signal(foreign->fence, next)))
		return false;
	foreign->signalled = next;
	return true;
}

static bool signal_foreign_queue(ForeignQueue *foreign)
{
	if (foreign->unsignalled.empty())
		return true;
	if (!submit_foreign_signal(foreign)) {
		// Only the original queue can signal completion of those executions.
		foreign->retry_queue = foreign->queue;
		foreign->retry_signal_after = GetTickCount64() + FOREIGN_SIGNAL_RETRY_MS;
		return false;
	}
	for (IUnknown *object : foreign->unsignalled) {
		foreign->uses.push_back({object, foreign->signalled});
		foreign->retired.push_back({object, foreign->signalled});
	}
	foreign->unsignalled.clear();
	return true;
}

void Dx12ShimPinsExecuted(ID3D12CommandQueue *queue, std::vector<IUnknown*> *pins, std::vector<IUnknown*> *first)
{
	Dx12ShimLock();
	if (dx12_shim && queue == shim.queue) {
		end_game_uses(first, &GameUses::pending);
		// What another thread recorded since the queue proxy had our
		// list executed was promised the value signalled next
		Dx12ShimSubmit();
		signal_shim_queue(++fence_signalled);
		InterlockedIncrement64(&dx12_stats.pin_signals);
		for (IUnknown *object : *pins)
			retired.push_back({ object, fence_signalled });
	} else {
		ForeignQueue *foreign = foreign_queue(queue);

		InterlockedIncrement64(&dx12_stats.pin_lates);
		for (IUnknown *object : *pins) {
			game_uses[object].foreign++;
			foreign->unsignalled.push_back(object);
		}
		end_game_uses(first, &GameUses::pending);
		if (!signal_foreign_queue(foreign)) {
			if (!foreign->unordered) {
				foreign->unordered_frames = UNORDERED_FRAMES;
				set_queue_unordered(foreign, true, "the queue could not signal a fence for its pins");
			}
			foreign->unordered_until = dx12_frame + foreign->unordered_frames;
		}
	}
	pins->clear();
	Dx12ShimUnlock();
}

bool Dx12ShimQueueUnordered(ID3D12CommandQueue *queue)
{
	if (!queue || !have_unordered_queues.load())
		return false;
	AcquireSRWLockShared(&unordered_queues_lock);
	auto found = unordered_queues.find(queue);
	bool unordered = found != unordered_queues.end() && !found->second->gone.load();
	ReleaseSRWLockShared(&unordered_queues_lock);
	return unordered;
}

static void set_queue_unordered(ForeignQueue *foreign, bool unordered, const char *why)
{
	if (foreign->unordered == unordered)
		return;
	AcquireSRWLockExclusive(&unordered_queues_lock);
	if (unordered) {
		unordered_queues[foreign->queue] = foreign;
	} else {
		auto found = unordered_queues.find(foreign->queue);
		if (found != unordered_queues.end() && found->second == foreign)
			unordered_queues.erase(found);
	}
	have_unordered_queues = !unordered_queues.empty();
	ReleaseSRWLockExclusive(&unordered_queues_lock);
	foreign->unordered = unordered;
	dx12_draw_generation++;
	if (foreign->gone.load())
		return;
	if (unordered) {
		Dx12LogInfo("*** ID3D12CommandQueue %p cannot safely use mod resources for at least %u frames: %s\n",
			foreign->queue, foreign->unordered_frames, why);
		Dx12DumpNote("*** Frame %u: ID3D12CommandQueue %p cannot safely use mod resources for at least %u frames: %s\n",
			dx12_frame, foreign->queue, foreign->unordered_frames, why);
	} else {
		Dx12LogInfo("ID3D12CommandQueue %p can be made to wait for the mods again\n", foreign->queue);
	}
}

void Dx12ShimQueueWaits(ID3D12CommandQueue *queue, UINT64 needed, bool uses_mods)
{
	ForeignQueue *foreign;
	bool ordered = true;
	char why[512] = "";

	Dx12ShimLock();
	if (dx12_shim && queue != shim.queue) {
		foreign = foreign_queue(queue);
		if (!signal_foreign_queue(foreign)) {
			ordered = false;
			strcpy_s(why, "the queue could not signal a fence for its pins");
		}
		// Nothing was recorded since for a value not signalled yet
		needed = needed ? min(needed, fence_signalled) : fence_signalled;
		if (ordered && foreign->waited < needed && fence->GetCompletedValue() < needed) {
			// Our queue may be behind a wait for what this one is
			// yet to be handed, and would then never get to it
			if (last_shim_signal < needed)
				signal_shim_queue(fence_signalled);
			if (!Dx12QueueCanWait(queue, fence, needed, foreign->unordered ? NULL : why, sizeof(why))) {
				ordered = false;
			} else if (uses_mods) {
				HRESULT hr = Dx12QueueWait(queue, fence, needed);
				if (SUCCEEDED(hr)) {
					foreign->waited = needed;
					InterlockedIncrement64(&dx12_stats.queue_waits);
				} else {
					ordered = false;
					_snprintf_s(why, sizeof(why), _TRUNCATE, "ID3D12CommandQueue::Wait failed with 0x%x", hr);
				}
			}
		}

		if (!ordered) {
			if (uses_mods)
				InterlockedIncrement64(&dx12_stats.queue_waits_skipped);
			if (!foreign->unordered) {
				if (!foreign->unordered_frames
				    || dx12_frame - foreign->ordered_since >= foreign->unordered_frames * UNORDERED_FORGET_TIMES)
					foreign->unordered_frames = UNORDERED_FRAMES;
				else
					foreign->unordered_frames = min(foreign->unordered_frames * 2, 1u << 20);
				set_queue_unordered(foreign, true, why);
			}
			foreign->unordered_until = dx12_frame + foreign->unordered_frames;
		} else if (foreign->unordered && (int)(dx12_frame - foreign->unordered_until) >= 0) {
			set_queue_unordered(foreign, false);
			foreign->ordered_since = dx12_frame;
		}
	}
	Dx12ShimUnlock();
}

void Dx12ShimPinsInherited(Dx12GraphicsCommandList *list, Dx12GraphicsCommandList *bundle)
{
	Dx12ShimLock();
	for (IUnknown *object : bundle->mPins) {
		ID3D12Resource *resource;

		if (!Dx12ShimPin(list, object))
			continue;
		// A resource is in use by the list now, see game_uses. The
		// bundle's own uses stay until it is reset: it may be
		// executed again by another list, with what it was recorded
		// with.
		if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
			game_uses[object].pending++;
			resource->Release();
		}
	}
	Dx12ShimUnlock();
}

void Dx12ShimPinsAbandoned(std::vector<IUnknown*> *pins, bool executed)
{
	if (!executed) {
		Dx12ShimLock();
		end_game_uses(pins, &GameUses::pending);
		Dx12ShimUnlock();
	}
	for (IUnknown *object : *pins)
		object->Release();
	pins->clear();
}

void Dx12ShimFramePresented()
{
	release_retired();
	present_time = GetTickCount64();
	if (!upload_big_idle.empty())
		release_idle_upload_buffers(fence->GetCompletedValue(), present_time);
	release_idle_upload_pages(fence->GetCompletedValue(), present_time);
	trim_staged_uploads(present_time);
	presents++;
	evict_idle();
	release_held_evictions();
	trim_spares();
}

bool Dx12ShimUseOnGameList(ShimResource *resource, Dx12GraphicsCommandList *list)
{
	if (!resource->resource || resource->foreign || !Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COMMON))
		return false;

	if (Dx12ShimPin(list, resource->resource))
		game_uses[resource->resource].pending++;
	if (resource->prepare_spares_on_pin) {
		resource->prepare_spares_on_pin = false;
		ModLoadDx12SpareBuffer(resource->desc, resource->allocated, false);
	}
	return true;
}

// Copies the shadow of a cpu_only buffer to where the game's lists can read
// it, bytes of it with the padding. alone is for a buffer of its own, where
// the contents start at the start of a resource.
static bool upload_for_game_lists(ShimResource *resource, UINT64 bytes, bool alone)
{
	UploadPage page;
	UINT64 start;
	BYTE *cpu;

	if (alone || bytes > GAME_UPLOAD_BLOCK_SIZE / 4) {
		if (!create_upload_buffer(bytes, &page))
			return false;
		start = 0;
	} else {
		if (!game_upload.buffer || game_upload.used + bytes > GAME_UPLOAD_BLOCK_SIZE) {
			if (!next_game_upload(&page))
				return false;
			if (game_upload.buffer) {
				if (game_upload_full.size() == GAME_UPLOAD_BLOCKS_KEPT) {
					game_upload_full.front().buffer->Release();
					game_upload_full.erase(game_upload_full.begin());
				}
				game_upload_full.push_back(game_upload);
			}
			game_upload = page;
		}
		page = game_upload;
		page.buffer->AddRef();
		start = game_upload.used;
		game_upload.used += bytes;
	}

	cpu = page.cpu + start;
	ShimReadShadow(resource);
	memcpy(cpu, resource->shadow.data(), resource->shadow.size());
	memset(cpu + resource->shadow.size(), 0, bytes - resource->shadow.size());

	if (resource->game_upload)
		resource->game_upload->Release();
	resource->game_upload = page.buffer;
	resource->game_upload_va = page.va + start;
	resource->game_upload_version = resource->version;
	return true;
}

UINT64 Dx12ShimGameBufferAddress(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT *size)
{
	ShimResource *resource = ShimResourceOf(buffer);
	UINT64 bytes;

	if (!resource->cpu_only) {
		if (!Dx12ShimUseOnGameList(resource, list))
			return 0;
		*size = (UINT)min(resource->desc.Width, (UINT64)UINT_MAX);
		return resource->resource->GetGPUVirtualAddress();
	}

	bytes = (resource->shadow.size() + 255) & ~(UINT64)255;
	*size = (UINT)bytes;
	if (!bytes)
		return 0;

	if ((!resource->game_upload || resource->game_upload_version != resource->version)
	    && !upload_for_game_lists(resource, bytes, false))
		return 0;

	Dx12ShimPin(list, resource->game_upload);
	return resource->game_upload_va;
}

bool Dx12ShimGameBufferView(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT stride, ID3D12Resource **view_resource,
		UINT64 *offset, UINT *size)
{
	ShimResource *resource = ShimResourceOf(buffer);
	D3D11_BUFFER_DESC desc;
	UINT64 bytes;

	if (!stride)
		return false;

	if (!resource->cpu_only) {
		if (!Dx12ShimUseOnGameList(resource, list))
			return false;
		buffer->GetDesc(&desc);
		*view_resource = resource->resource;
		*offset = 0;
		*size = desc.ByteWidth;
		return true;
	}

	bytes = (resource->shadow.size() + 255) & ~(UINT64)255;
	if (!bytes)
		return false;

	if (!resource->game_upload || resource->game_upload_version != resource->version
	    || (resource->game_upload_va - resource->game_upload->GetGPUVirtualAddress()) % stride) {
		if (!upload_for_game_lists(resource, bytes, true))
			return false;
	}

	Dx12ShimPin(list, resource->game_upload);
	*view_resource = resource->game_upload;
	*offset = resource->game_upload_va - resource->game_upload->GetGPUVirtualAddress();
	*size = (UINT)resource->shadow.size();
	return true;
}

// -----------------------------------------------------------------------------
// Command list and fence

void Dx12ShimDeferRelease(IUnknown *object)
{
	object->AddRef();
	retired.push_back({ object, Dx12ShimPendingFence() });
}

static bool release_completed(std::deque<Retired> &entries, UINT64 completed)
{
	bool released = false;
	while (!entries.empty() && entries.front().fence <= completed) {
		IUnknown *object = entries.front().object;
		entries.pop_front();
		object->Release();
		released = true;
	}
	return released;
}

static bool release_foreign_pins(ForeignQueue *foreign, UINT64 completed)
{
	while (!foreign->uses.empty() && foreign->uses.front().fence <= completed) {
		end_game_use(foreign->uses.front().object, &GameUses::foreign);
		foreign->uses.pop_front();
	}
	return release_completed(foreign->retired, completed);
}

static bool reclaim_foreign_pins(ForeignQueue *foreign)
{
	if (!foreign->fence || foreign->retired.empty())
		return false;
	UINT64 completed = foreign->fence->GetCompletedValue();
	return completed != UINT64_MAX && completed >= foreign->retired.front().fence
		&& release_foreign_pins(foreign, completed);
}

bool Dx12ShimReclaimRetired()
{
	bool released = false;
	if (!retired.empty() && retired.front().fence <= last_shim_signal) {
		UINT64 completed = fence->GetCompletedValue();
		if (completed != UINT64_MAX)
			released = release_completed(retired, completed);
	}
	for (ForeignQueue *foreign : foreign_queues)
		released |= reclaim_foreign_pins(foreign);
	return released;
}

static void release_retired()
{
	UINT64 completed = fence->GetCompletedValue();

	if (completed == UINT64_MAX)
		close_staged_upload_pool();
	release_retired_staged_uploads(completed);

	release_completed(retired, completed);

	bool device_removed = completed == UINT64_MAX;
	for (auto entry = foreign_queues.begin(); entry != foreign_queues.end();) {
		ForeignQueue *foreign = *entry;
		bool gone = foreign->gone.load();
		if (!device_removed && foreign->retry_queue && !foreign->unsignalled.empty()
		    && GetTickCount64() >= foreign->retry_signal_after)
			signal_foreign_queue(foreign);
		UINT64 finished = device_removed ? UINT64_MAX
			: foreign->fence && !foreign->retired.empty() ? foreign->fence->GetCompletedValue() : 0;
		release_foreign_pins(foreign, finished);
		if (device_removed) {
			end_game_uses(&foreign->unsignalled, &GameUses::foreign);
			for (IUnknown *object : foreign->unsignalled)
				object->Release();
			foreign->unsignalled.clear();
		}
		// Releasing the last queue reference can wait for pending GPU work.
		if (foreign->unsignalled.empty() && foreign->retired.empty())
			foreign->retry_queue.Reset();
		if (gone || device_removed)
			set_queue_unordered(foreign, false);
		if ((!gone && !(foreign->owns_queue && !foreign->unordered))
		    || !foreign->retired.empty() || !foreign->unsignalled.empty()) {
			++entry;
			continue;
		}
		entry = foreign_queues.erase(entry);
		if (foreign->fence)
			foreign->fence->Release();
		if (foreign->owns_queue)
			foreign->queue->Release();
		delete foreign;
	}
}

static const GUID foreign_queue_guid = {0x6e272c1c, 0xd74b, 0x4c39, {0x93, 0x3e, 0xf9, 0x35, 0x81, 0x76, 0x7e, 0x60}};

static void foreign_queue_destroyed(void *context)
{
	auto *foreign = static_cast<ForeignQueue *>(context);
	Dx12ForgetQueueOrdering(foreign->queue);
	foreign->gone.store(true);
	dx12_draw_generation++;
}

static ForeignQueue* foreign_queue(ID3D12CommandQueue *queue)
{
	for (ForeignQueue *known : foreign_queues) {
		if (known->queue == queue && !known->gone.load())
			return known;
	}

	auto *foreign = new ForeignQueue();
	foreign->queue = queue;
	if (!Dx12NotifyDestruction(queue, foreign_queue_guid, foreign_queue_destroyed, foreign)) {
		queue->AddRef();
		foreign->owns_queue = true;
	}
	foreign_queues.push_back(foreign);
	return foreign;
}

UINT64 Dx12ShimPendingFence()
{
	return fence_signalled + 1;
}

bool Dx12ShimFenceReached(UINT64 value)
{
	return fence->GetCompletedValue() >= value;
}

bool Dx12ShimSubmissionDiscarded(UINT64 value)
{
	return std::find(discarded.begin(), discarded.end(), value) != discarded.end();
}

// An allocator of the pool the GPU is done with, reset
static size_t free_allocator(std::vector<Allocator> *pool)
{
	UINT64 completed = fence->GetCompletedValue();
	Allocator allocator;
	size_t i;

	for (i = 0; i < pool->size(); i++) {
		if ((*pool)[i].fence <= completed)
			break;
	}
	if (i == pool->size()) {
		allocator.fence = 0;
		// Recording to the allocator of an executing list is not an
		// option, so there is nothing to fall back to
		if (FAILED(shim.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.allocator)))) {
			Dx12LogInfo("*** Failed to create a command allocator\n");
			DoubleBeepExit();
		}
		pool->push_back(allocator);
	}
	(*pool)[i].allocator->Reset();
	return i;
}

ID3D12GraphicsCommandList* Dx12ShimList()
{
	list_used = true;
	if (list_open)
		return shim_list;

	current_allocator = free_allocator(&allocators);
	shim_list->Reset(allocators[current_allocator].allocator, NULL);
	list_open = true;
	bound_view_heap = NULL;
	bound_sampler_heap = NULL;
	return shim_list;
}

ID3D12GraphicsCommandList* Dx12ShimEarlyList()
{
	if (early_open)
		return early_list;

	// Which is what submits it
	Dx12ShimList();
	current_early_allocator = free_allocator(&early_allocators);
	if (early_list) {
		early_list->Reset(early_allocators[current_early_allocator].allocator, NULL);
	} else if (FAILED(shim.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			early_allocators[current_early_allocator].allocator, NULL, IID_PPV_ARGS(&early_list)))) {
		early_list = NULL;
		return NULL;
	} else {
		early_list->SetName(L"3DMigoto mods, early");
	}
	early_open = true;
	return early_list;
}

// A list that failed to close cannot be reset either. If no new one can be
// made, the old one keeps failing to close and is never executed.
static void replace_list(ID3D12GraphicsCommandList **list, ID3D12CommandAllocator *allocator, const wchar_t *name)
{
	ID3D12GraphicsCommandList *replacement;

	if (FAILED(shim.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&replacement)))) {
		Dx12LogInfo("*** Failed to create a command list to replace '%S'\n", name);
		return;
	}
	replacement->Close();
	replacement->SetName(name);
	(*list)->Release();
	*list = replacement;
}

void Dx12ShimBeforeSubmit(void (*callback)())
{
	before_submit = callback;
}

void Dx12ShimSubmit()
{
	ID3D12CommandList *lists[2];
	UINT count = 0;
	HRESULT hr = S_OK, shim_hr;

	if (before_submit)
		before_submit();

	if (!list_used) {
		// Nothing recorded since the last submission can be using
		// what was retired since
		if (!retired.empty() && fence->GetCompletedValue() >= fence_signalled) {
			for (Retired &entry : retired)
				entry.object->Release();
			retired.clear();
		}
		release_retired();
		return;
	}

	Profiling::Spike::Scope spike(Profiling::Spike::DX12_SUBMIT);

	// By index: the first transition of a resource in a submission is
	// what adds it, which none of these is
	for (size_t i = 0; i < touched.size(); i++)
		Dx12ShimSetState(touched[i], D3D12_RESOURCE_STATE_COMMON);
	touched.clear();

	if (early_open) {
		hr = early_list->Close();
		Dx12LogCloseFailure(early_list, hr);
		lists[count++] = early_list;
	}
	shim_hr = shim_list->Close();
	Dx12LogCloseFailure(shim_list, shim_hr);
	lists[count++] = shim_list;
	if (FAILED(hr) || FAILED(shim_hr)) {
		// Executing a list that failed to close removes the device. The
		// shim list was recorded after the early one, so neither runs;
		// the fence is still signalled for what waits on it.
		Dx12ModsFaulted(FAILED(hr) ? hr : shim_hr);
		discarded.push_back(fence_signalled + 1);
		if (FAILED(hr))
			replace_list(&early_list, early_allocators[current_early_allocator].allocator, L"3DMigoto mods, early");
		if (FAILED(shim_hr))
			replace_list(&shim_list, allocators[current_allocator].allocator, L"3DMigoto mods");
	} else {
		Dx12ShimResidencyWait(shim.queue);
		shim.queue->ExecuteCommandLists(count, lists);
	}
	signal_shim_queue(++fence_signalled);
	allocators[current_allocator].fence = fence_signalled;
	if (early_open)
		early_allocators[current_early_allocator].fence = fence_signalled;
	early_open = false;
	list_open = false;
	list_used = false;
	dx12_shim_epoch++;

	retire_upload_pages();
	release_retired();
}

void Dx12ShimFlush()
{
	// Whatever the lists about to be executed depend on was recorded
	// before they were closed, by the threads the game had them from
	if (!list_used)
		return;

	Dx12ShimLock();
	if (dx12_shim)
		Dx12ShimSubmit();
	Dx12ShimUnlock();
}

HRESULT Dx12ShimSubmitAndWait()
{
	Dx12ShimSubmit();

	UINT64 completed = fence->GetCompletedValue();
	if (completed == UINT64_MAX)
		return DXGI_ERROR_DEVICE_REMOVED;
	if (completed >= fence_signalled)
		return S_OK;

	HRESULT hr = last_shim_signal < fence_signalled ? signal_shim_queue(fence_signalled) : S_OK;
	if (SUCCEEDED(hr)) {
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_GPU_WAIT, L"for a mod to read back or a swap chain to resize");

		hr = fence->SetEventOnCompletion(fence_signalled, fence_event);
		if (SUCCEEDED(hr)) {
			const ULONGLONG deadline = GetTickCount64() + 10000;
			DWORD remaining = 10000;
			for (;;) {
				DWORD result = WaitForSingleObject(fence_event, remaining);
				if (result != WAIT_OBJECT_0) {
					hr = HRESULT_FROM_WIN32(result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
					break;
				}

				// A registration from an earlier timed-out wait can still signal this event.
				if (fence->GetCompletedValue() >= fence_signalled)
					break;
				ULONGLONG now = GetTickCount64();
				if (now >= deadline)
					break;
				remaining = (DWORD)(deadline - now);
			}
		}
	}
	completed = fence->GetCompletedValue();
	release_retired();
	if (completed == UINT64_MAX || (FAILED(hr) && FAILED(shim.device->GetDeviceRemovedReason())))
		return DXGI_ERROR_DEVICE_REMOVED;
	if (completed >= fence_signalled)
		return S_OK;
	if (SUCCEEDED(hr))
		hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
	Dx12LogInfo("*** Gave up waiting for the GPU to reach fence value %llu (0x%x)\n", fence_signalled, hr);
	return hr;
}

ID3D12Fence* Dx12ShimFence(UINT64 *signalled)
{
	if (signalled)
		*signalled = fence_signalled;
	return fence;
}

bool Dx12ShimSetQueue(ID3D12CommandQueue *queue)
{
	if (shim.queue == queue)
		return true;

	Dx12ShimSubmit();
	UINT64 completed = fence->GetCompletedValue();
	if (completed == UINT64_MAX)
		return false;
	ForeignQueue *foreign = foreign_queue(queue);
	if (!signal_foreign_queue(foreign))
		return false;
	if (completed < fence_signalled) {
		if (last_shim_signal < fence_signalled && FAILED(signal_shim_queue(fence_signalled)))
			return false;
		if (!Dx12QueueCanWait(queue, fence, fence_signalled))
			return false;
		if (FAILED(Dx12QueueWait(queue, fence, fence_signalled)))
			return false;
	}

	set_queue_unordered(foreign, false);
	queue->AddRef();
	ID3D12CommandQueue *previous = shim.queue;
	shim.queue = queue;
	if (completed < fence_signalled)
		retired.push_back({previous, fence_signalled});
	else
		previous->Release();
	return true;
}

bool Dx12ShimCreate(Dx12Device *device, Dx12CommandQueue *queue, bool hardware_adapter)
{
	static bool failed;
	Allocator allocator = {};
	UINT type;

	if (dx12_shim)
		return true;
	if (failed)
		return false;
	failed = true;

	// A device wrapped before the game's D3D11 swap chain presented
	if (Dx12IsD3D11Game()) {
		Dx12LogInfo("The game presents with D3D11, leaving the D3D12 swap chain alone\n");
		return false;
	}

	// Our objects are created on the original device: they are not the
	// game's, so nothing about them belongs in what the proxies record.
	shim.device = device->mOrig;
	shim.hardware_adapter = hardware_adapter;
	shim.queue = queue->mOrig;

	if (shim.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
		Dx12LogInfo("*** The swap chain's queue is not a direct queue, mods will not be applied\n");
		return false;
	}

	// The driver may present through D3D12 before the Vulkan device is known.
	// Keep its fence shareable so Vulkan can use the same engine later.
	HRESULT fence_hr = shim.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
	if (FAILED(fence_hr))
		fence_hr = shim.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
	if (FAILED(fence_hr)
	    || FAILED(shim.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.allocator)))
	    || FAILED(shim.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.allocator, NULL, IID_PPV_ARGS(&shim_list)))) {
		Dx12LogInfo("*** Failed to create the command list for mods, they will not be applied\n");
		return false;
	}
	shim_list->SetName(L"3DMigoto mods");
	fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	allocators.push_back(allocator);
	list_open = true;

	shim.device->AddRef();
	shim.queue->AddRef();
	residency_init();

	for (type = 0; type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; type++)
		pools[type].increment = shim.device->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)type);
	view_pages.increment = pools[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].increment;
	sampler_pages.increment = pools[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER].increment;
	dx12_shim_view_increment = view_pages.increment;

	// Everything from here on finds the device through this
	dx12_shim = &shim;
	if (!Dx12ShimPipelineInit()) {
		dx12_shim = NULL;
		return false;
	}

	shim.shim_device = new ShimDevice();
	shim.shim_context = new ShimContext();

	shim.hacker_device = new HackerDevice(shim.shim_device, shim.shim_context);
	// Not HackerContextFactory: frame analysis reads back what the game
	// renders in ways the shim has no answer to yet
	shim.hacker_context = new HackerContext(shim.shim_device, shim.shim_context);
	shim.hacker_device->SetHackerContext(shim.hacker_context);
	// The constructor leaves it uninitialised, and the engine takes
	// anything but NULL for a D3D11 swap chain
	shim.hacker_device->SetHackerSwapChain(NULL);
	shim.hacker_context->SetHackerDevice(shim.hacker_device);

	shim.hacker_device->Create3DMigotoResources();
	shim.hacker_context->Bind3DMigotoResources();
	if (!G->constants_run)
		shim.hacker_context->InitIniParams();

	Dx12LogInfo("HackerDevice %p and HackerContext %p created on the D3D11 shim of ID3D12Device %p, commands go to ID3D12CommandQueue %p\n",
		shim.hacker_device, shim.hacker_context, shim.device, shim.queue);
	shim_created = true;
	Dx12WatchDevice(shim.device);

	// The shader overrides were read before the game was known to be
	// D3D12, and are not read again at the first present when
	// config_initialization_delay says not to
	if (G->gConfigInitialized && !G->gReloadConfigPending && IniDx12HashesProvisional()) {
		Dx12LogInfo("Reloading the configuration for the hash_dx12 of the shader overrides\n");
		G->gReloadConfigPending = true;
	}
	return true;
}
