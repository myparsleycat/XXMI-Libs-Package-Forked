#include "DeviceRemoved.h"

#include <atomic>
#include <vector>

#include "Bridge.h"
#include "Dx12Common.h"
#include "HashDump.h"
#include "ResourceTracker.h"

#include "util.h"
#include "../globals.h"
#include "../vk/VkDrawPath.h"

static const char* breadcrumb_op(D3D12_AUTO_BREADCRUMB_OP op)
{
	static const char *names[] = {
		"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
		"CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles", "ResolveSubresource",
		"ClearRenderTargetView", "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle",
		"Present", "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
		"AtomicCopyBufferUINT", "AtomicCopyBufferUINT64", "ResolveSubresourceRegion", "WriteBufferImmediate",
		"DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2", "ProcessFrames1",
		"BuildRaytracingAccelerationStructure", "EmitRaytracingAccelerationStructurePostbuildInfo",
		"CopyRaytracingAccelerationStructure", "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand",
		"EstimateMotion", "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
		"ExecuteExtensionCommand", "DispatchMesh",
	};

	return (unsigned)op < ARRAYSIZE(names) ? names[op] : "another operation";
}

static const wchar_t* breadcrumb_context(const D3D12_AUTO_BREADCRUMB_NODE1 *node, UINT32 index)
{
	for (UINT i = 0; i < node->BreadcrumbContextsCount; i++) {
		if (node->pBreadcrumbContexts[i].BreadcrumbIndex == index && node->pBreadcrumbContexts[i].pContextString)
			return node->pBreadcrumbContexts[i].pContextString;
	}
	return NULL;
}

static void note_breadcrumbs(ID3D12DeviceRemovedExtendedData1 *dred, void (*note)(const char *format, ...))
{
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
	unsigned complete = 0;

	if (FAILED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
		note("  DRED has no breadcrumbs\n");
		return;
	}
	note("  Command lists the GPU had not got through:\n");
	for (const D3D12_AUTO_BREADCRUMB_NODE1 *node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
		UINT32 last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
		const wchar_t *name = node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"";

		if (last >= node->BreadcrumbCount) {
			complete++;
			continue;
		}
		note("    list %p \"%ls\"%s on queue %p \"%ls\": %u of %u operations done\n",
			node->pCommandList, name, !wcsncmp(name, L"3DMigoto mods", 13) ? " (ours)" : "",
			node->pCommandQueue, node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"",
			last, node->BreadcrumbCount);
		for (UINT32 i = last >= 3 ? last - 3 : 0; i < node->BreadcrumbCount && i < last + 4; i++) {
			const wchar_t *context = breadcrumb_context(node, i);

			note("      %s%u %s%s%ls%s\n", i == last ? "-> " : "   ", i, breadcrumb_op(node->pCommandHistory[i]),
				context ? " \"" : "", context ? context : L"", context ? "\"" : "");
		}
	}
	note("    and %u it had got through\n", complete);
}

static void note_game_buffer(UINT64 va, void (*note)(const char *format, ...))
{
	ID3D12Resource *resource;
	Dx12ResourceInfo info;
	UINT64 offset;

	if (!Dx12FindBuffer(va, &resource, &offset)) {
		note("    no buffer of the game there\n");
		return;
	}
	if (!Dx12GetResourceInfo(resource, &info)) {
		note("    at the address: %p, a buffer of the game, at 0x%llx in it\n", resource, offset);
		return;
	}
	note("    at the address: %p, a buffer of the game, %llu bytes, heap type %d, at 0x%llx in it%s (%u of its Evict calls held back)\n",
		resource, info.desc.Width, info.heap_type, offset, info.game_evicted ? ", evicted by the game" : "",
		info.evictions_held);
}

static void note_allocations(const char *what, const D3D12_DRED_ALLOCATION_NODE1 *node,
		std::vector<const void*> *objects, void (*note)(const char *format, ...))
{
	unsigned shown;

	for (shown = 0; node && shown < 8; node = node->pNext, shown++) {
		note("    %s: %p \"%ls\", type %u\n", what, node->pObject, node->ObjectNameW ? node->ObjectNameW : L"",
			node->AllocationType);
		if (objects)
			objects->push_back(node->pObject);
	}
}

void Dx12NoteDred(ID3D12Device *device, void (*note)(const char *format, ...))
{
	ID3D12DeviceRemovedExtendedData1 *dred = NULL;
	D3D12_DRED_PAGE_FAULT_OUTPUT1 page_fault = {};
	std::vector<const void*> there;

	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
		note("  DRED is not available\n");
		return;
	}
	note_breadcrumbs(dred, note);

	if (FAILED(dred->GetPageFaultAllocationOutput1(&page_fault)) || !page_fault.PageFaultVA) {
		note("  DRED has no page fault\n");
	} else {
		note("  Page fault at 0x%llx\n", page_fault.PageFaultVA);
		note_allocations("there now", page_fault.pHeadExistingAllocationNode, &there, note);
		note_allocations("freed recently", page_fault.pHeadRecentFreedAllocationNode, NULL, note);
		note_game_buffer(page_fault.PageFaultVA, note);
		Dx12ShimNoteFault(page_fault.PageFaultVA, there, note);
	}
	dred->Release();
}

static HMODULE watch_module;
static std::atomic<bool> watching;

static bool window_in_front()
{
	HWND window = GetForegroundWindow();
	DWORD process = 0;

	if (!window)
		return false;
	GetWindowThreadProcessId(window, &process);
	return process == GetCurrentProcessId();
}

static void note_removed(ID3D12Device *device, HRESULT reason)
{
	DXGI_QUERY_VIDEO_MEMORY_INFO memory;

	// VkModsDeviceLost reports it for a Vulkan game, from the Vulkan side
	if (vk_mods_active)
		return;

	Dx12ReportNote("*** Frame %u (FRAME_NUMBER %u): the D3D12 device has been removed, reason 0x%x\n",
		dx12_frame, G->frame_no, reason);
	if (Dx12ShimProcessMemory(&memory)) {
		Dx12ReportNote("  GPU memory: %llu of the %llu MB budget in use; mods have %llu MB, of which %d resources of %llu MB are evicted\n",
			memory.CurrentUsage >> 20, memory.Budget >> 20,
			(UINT64)(dx12_memory.buffer_bytes + dx12_memory.texture_bytes) >> 20,
			dx12_memory.evicted, (UINT64)dx12_memory.evicted_bytes >> 20);
	}
	if (G->dx12_dred)
		Dx12NoteDred(device, Dx12ReportNote);
	if (LogFile)
		fflush(LogFile);
}

static DWORD WINAPI watch_thread(void *param)
{
	ID3D12Device *device = (ID3D12Device*)param;
	ID3D12Fence *fence = NULL;
	HANDLE removed = CreateEvent(NULL, FALSE, FALSE, NULL);
	ULONGLONG now, presented_time = GetTickCount64();
	UINT presented = dx12_frame;
	bool dumped = false;
	UINT seconds;
	HRESULT reason;

	// Every fence of a removed device completes. Polled as well, in case
	// a driver does not signal it.
	if (removed && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
		fence->SetEventOnCompletion(UINT64_MAX, removed);

	for (;;) {
		if (removed)
			WaitForSingleObject(removed, 1000);
		else
			Sleep(1000);

		reason = device->GetDeviceRemovedReason();
		if (reason != S_OK) {
			note_removed(device, reason);
			break;
		}

		// The key can change on a reload of the configuration
		seconds = G->dx12_stall_dump_seconds;
		now = GetTickCount64();
		if (dumped || !seconds || presented != dx12_frame || !window_in_front()) {
			presented = dx12_frame;
			presented_time = now;
		} else if (now - presented_time >= seconds * 1000ull) {
			Dx12ReportNote("*** Frame %u (FRAME_NUMBER %u): nothing presented for %u s with the game's window in front\n",
				presented, G->frame_no, (UINT)((now - presented_time) / 1000));
			write_minidump(NULL);
			dumped = true;
		}
	}

	if (fence)
		fence->Release();
	if (removed)
		CloseHandle(removed);
	device->Release();
	FreeLibraryAndExitThread(watch_module, 0);
}

void Dx12WatchDevice(ID3D12Device *device)
{
	HANDLE thread;

	if (!G->dx12_dred && !G->dx12_stall_dump_seconds)
		return;
	if (watching.exchange(true))
		return;

	// The thread holds a reference on this DLL until it exits, like the
	// prefetch thread of ResourceLoading.cpp, so an unload cannot unmap
	// the code it is running
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&watch_thread, &watch_module)) {
		watching = false;
		return;
	}
	device->AddRef();
	thread = CreateThread(NULL, 0, watch_thread, device, 0, NULL);
	if (!thread) {
		Dx12LogInfo("*** No thread could be started to watch ID3D12Device %p\n", device);
		device->Release();
		FreeLibrary(watch_module);
		watching = false;
		return;
	}
	CloseHandle(thread);
	Dx12LogInfo("Watching ID3D12Device %p:%s%s\n", device, G->dx12_dred ? " DRED report when removed" : " removal",
		G->dx12_stall_dump_seconds ? ", minidump when presents stop" : "");
}
