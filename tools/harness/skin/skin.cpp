// A D3D12 renderer that skins a mesh the way Unity does on D3D12: a compute
// dispatch reads vertices (t0), weights (t1) and bones (t2) and writes the
// skinned vertices (u0), as many as a root constant at b0 says. A draw then
// reads the skinned vertices as vb0 with the mesh's index buffer.
//
// The mod in run/Mods/skin (make_mod.py) replaces the mesh with its own,
// skinned there: the game mesh is a small square on the left, the mod's a
// wide bar on the right.
//
// skin.exe [frames=120] [nodll] [deform] [dump=file.bmp] [queuelife] [queuegone] [devicelife] [second] [others=N] [second_at=N] [drawfirst] [unhashed] [reuse_at=N] [replace_at=N] [reupload_at=N] [fault_at=N] [stall_at=N] [rs11] [place] [noheap] [bindings]
// Prints what is drawn where the game's and the mod's meshes go. Exits with 1
// when D3D12 fails, an argument is unknown or a lifetime case goes wrong.
// queuelife: before that, does what RTSS did to ZZZ: holds a queue by an
// interface the queue proxy forwards, lets go of the proxy, then hands the
// proxy to D3D11On12CreateDevice (needs dx12_debug_layer for the interface).
// queuegone: what RTSS did to ZZZ after the game made its swap chain again:
// hands D3D11On12CreateDevice a queue that has been destroyed, which is to be
// refused without being touched, and once the swap chain presented, to be
// taken as the queue it presents on.
// devicelife: the same with the device, before the one the frames use is made.
// second: another game mesh above the square, of as many vertices but with
// weights and indices of its own, which no mod is for.
// others=N: that mesh skinned and drawn N more times, each to a buffer of its
// own; prints the time recording and executing a frame took.
// second_at=N: the second mesh only from frame N on.
// drawfirst: each frame draws what the frame before skinned, then skins.
// unhashed: the weights are copied into their buffers from other GPU
// buffers, so nothing could hash what is in them.
// reuse_at=N: until frame N the buffer the square is skinned to has the second
// mesh skinned to it and drawn with its indices instead.
// replace_at=N: the second mesh is skinned to a placed buffer and drawn with
// its indices; at frame N that buffer makes way for a larger one at the same
// address, which a mesh of 320 vertices is skinned to and drawn with the
// square's indices; from frame 2N the second mesh is skinned to a buffer of
// its own again and drawn where it was.
// reupload_at=N: at frame N the square's index buffer is written again by a copy
// from another GPU buffer, which nothing can hash, and at frame 2N by one from
// upload memory again, as a game that moves or streams in a mesh would.
// fault_at=N: at frame N the square is drawn from the address of a buffer that
// has been released, a page fault that removes the device; exits with 0 once
// it is removed, after giving dx12_dred time to write its report.
// stall_at=N: after frame N, with the window in front, presents nothing for
// 4 s, for dx12_stall_dump_seconds.
// rs11: root signatures of version 1.1, as Unreal makes them, in place of 1.0.
// place: the draws' vertex shader adds a vector it reads from a root constant
// buffer (b0) to the positions, which is all zeros.
// noheap: unbinds descriptor heaps before drawing; the game's draw shaders
// use no descriptor tables, so only a mod's shaders can need a view heap.
// bindings: adds a 768-byte vertex constant buffer with distinct values in
// its 256-byte regions, and a pixel SRV at t0 for binding checks.

#include <windows.h>
#include "../common/window.h"
#include <d3d12.h>
#include <d3d11on12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

#define CHECK(x) do { HRESULT hr__ = (x); if (FAILED(hr__)) { printf("FAILED %s = 0x%08x at line %d\n", #x, (unsigned)hr__, __LINE__); fflush(stdout); ExitProcess(1); } } while (0)

static const UINT WIDTH = 640, HEIGHT = 360, BACK_BUFFERS = 3;
// A grid of GX x GY vertices, the square of the game
static const UINT GX = 20, GY = 15, GAME_VERTICES = GX * GY;
static const UINT GAME_INDICES = (GX - 1) * (GY - 1) * 6;

struct Vertex { float p[3], n[3], t[4]; };
struct Weight { float w[4]; UINT i[4]; };

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static IDXGISwapChain3 *swap_chain;
static ID3D12Resource *back_buffers[BACK_BUFFERS];
static ID3D12DescriptorHeap *rtv_heap, *gpu_heap;
static UINT rtv_size, view_size;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_value;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

static ID3D12Resource* create_buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state, bool uav = false)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};
	ID3D12Resource *resource;

	heap.Type = type;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
	CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL, IID_PPV_ARGS(&resource)));
	return resource;
}

static void transition(ID3D12GraphicsCommandList *list, ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER barrier = {};

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	list->ResourceBarrier(1, &barrier);
}

static void wait_for(UINT64 value)
{
	if (fence->GetCompletedValue() >= value)
		return;
	CHECK(fence->SetEventOnCompletion(value, fence_event));
	WaitForSingleObject(fence_event, INFINITE);
}

static UINT64 signal()
{
	CHECK(queue->Signal(fence, ++fence_value));
	return fence_value;
}

static std::vector<char> read_file(const char *name)
{
	char path[MAX_PATH], *slash;
	std::vector<char> data;
	FILE *f;

	GetModuleFileNameA(NULL, path, MAX_PATH);
	slash = strrchr(path, '\\');
	strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), name);
	if (fopen_s(&f, path, "rb")) {
		printf("cannot read %s\n", path);
		ExitProcess(1);
	}
	fseek(f, 0, SEEK_END);
	data.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	fread(data.data(), 1, data.size(), f);
	fclose(f);
	return data;
}

// Uploads data to a default heap buffer of exactly that size
static ID3D12Resource* upload(ID3D12GraphicsCommandList *list, std::vector<ID3D12Resource*> *staging, const void *data, UINT64 bytes,
		D3D12_RESOURCE_STATES state)
{
	ID3D12Resource *buffer = create_buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
	ID3D12Resource *source = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	void *cpu;

	CHECK(source->Map(0, NULL, &cpu));
	memcpy(cpu, data, bytes);
	source->Unmap(0, NULL);
	list->CopyBufferRegion(buffer, 0, source, 0, bytes);
	transition(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, state);
	staging->push_back(source);
	return buffer;
}

// The same, but filled by a copy from another default heap buffer
static ID3D12Resource* upload_unhashed(ID3D12GraphicsCommandList *list, std::vector<ID3D12Resource*> *staging, const void *data,
		UINT64 bytes, D3D12_RESOURCE_STATES state)
{
	ID3D12Resource *source = upload(list, staging, data, bytes, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12Resource *buffer = create_buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);

	list->CopyBufferRegion(buffer, 0, source, 0, bytes);
	transition(list, buffer, D3D12_RESOURCE_STATE_COPY_DEST, state);
	staging->push_back(source);
	return buffer;
}

static bool signature_1_1;

static ID3D12RootSignature* root_signature(const D3D12_ROOT_SIGNATURE_DESC *desc)
{
	ID3DBlob *blob = NULL, *errors = NULL;
	ID3D12RootSignature *signature;
	D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
	std::vector<D3D12_ROOT_PARAMETER1> parameters(desc->NumParameters);
	std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges(desc->NumParameters);
	HRESULT hr;

	for (UINT i = 0; i < desc->NumParameters; i++) {
		const D3D12_ROOT_PARAMETER &from = desc->pParameters[i];

		parameters[i].ParameterType = from.ParameterType;
		parameters[i].ShaderVisibility = from.ShaderVisibility;
		if (from.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
			parameters[i].Constants = from.Constants;
		} else if (from.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
			for (UINT j = 0; j < from.DescriptorTable.NumDescriptorRanges; j++) {
				const D3D12_DESCRIPTOR_RANGE &range = from.DescriptorTable.pDescriptorRanges[j];

				ranges[i].push_back({ range.RangeType, range.NumDescriptors, range.BaseShaderRegister, range.RegisterSpace,
					D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, range.OffsetInDescriptorsFromTableStart });
			}
			parameters[i].DescriptorTable = { (UINT)ranges[i].size(), ranges[i].data() };
		} else {
			parameters[i].Descriptor = { from.Descriptor.ShaderRegister, from.Descriptor.RegisterSpace,
				D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE };
		}
	}
	versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
	versioned.Desc_1_1 = { desc->NumParameters, parameters.data(), desc->NumStaticSamplers, desc->pStaticSamplers, desc->Flags };
	hr = signature_1_1 ? D3D12SerializeVersionedRootSignature(&versioned, &blob, &errors)
		: D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
	if (FAILED(hr)) {
		printf("root signature: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
		ExitProcess(1);
	}
	CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)));
	blob->Release();
	return signature;
}

static void check_picture(ID3D12Resource *back_buffer, const char *dump)
{
	D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *list;
	ID3D12Resource *readback;
	UINT64 bytes;
	BYTE *data;

	device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, NULL, NULL, &bytes);
	readback = create_buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
	CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
	CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&list)));
	transition(list, back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;
	src.pResource = back_buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
	transition(list, back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
	CHECK(list->Close());
	queue->ExecuteCommandLists(1, (ID3D12CommandList**)&list);
	wait_for(signal());

	CHECK(readback->Map(0, NULL, (void**)&data));
	// In NDC: the game's square spans x -0.8..-0.2, the mod's bar (static)
	// x 0.1..0.5, and shifted by its compute shader x 0.5..0.9. The mod's
	// vertices drawn with the game's indices reach its lower rows only.
	struct { const char *name; float x, y; } spots[] = {
		{ "game square", -0.5f, 0.0f },
		{ "mod bar where it is in its file", 0.3f, 0.0f },
		{ "mod bar, its lower rows", 0.3f, -0.15f },
		{ "mod bar where its compute shader moves it", 0.7f, 0.0f },
		{ "  ... with the game's vertices: count wrong", 0.7f, 0.6f },
		{ "  ... with the game's vertices: stride wrong", 0.7f, -0.6f },
		{ "  ... with the game's vertices: data wrong", 0.7f, 0.3f },
		{ "second game square", -0.5f, 0.65f },
		{ "mod bar moved by rigid bone", 0.3f, 0.8f },
	};
	for (auto &spot : spots) {
		UINT px = (UINT)((spot.x * 0.5f + 0.5f) * WIDTH), py = (UINT)((1.0f - (spot.y * 0.5f + 0.5f)) * HEIGHT);
		const BYTE *p = data + (UINT64)py * footprint.Footprint.RowPitch + px * 4;
		bool drawn = !(p[0] < 40 && p[1] < 40 && p[2] < 60);

		printf("  %-44s %s (%u,%u,%u)\n", spot.name, drawn ? "DRAWN" : "empty", p[0], p[1], p[2]);
	}
	if (dump) {
		FILE *f;
		if (!fopen_s(&f, dump, "wb")) {
			BITMAPFILEHEADER file = {};
			BITMAPINFOHEADER info = {};
			file.bfType = 0x4d42;
			file.bfOffBits = sizeof(file) + sizeof(info);
			file.bfSize = file.bfOffBits + WIDTH * HEIGHT * 4;
			info.biSize = sizeof(info);
			info.biWidth = WIDTH;
			info.biHeight = -(LONG)HEIGHT;
			info.biPlanes = 1;
			info.biBitCount = 32;
			fwrite(&file, sizeof(file), 1, f);
			fwrite(&info, sizeof(info), 1, f);
			for (UINT y = 0; y < HEIGHT; y++) {
				for (UINT x = 0; x < WIDTH; x++) {
					const BYTE *p = data + (UINT64)y * footprint.Footprint.RowPitch + x * 4;
					BYTE bgra[4] = { p[2], p[1], p[0], 255 };
					fwrite(bgra, 4, 1, f);
				}
			}
			fclose(f);
		}
	}
	readback->Unmap(0, NULL);
	readback->Release();
	list->Release();
	allocator->Release();
}

int main(int argc, char **argv)
{
	UINT frame_count = 120, i, f;
	bool load_dll = true, deform = false, queue_life = false, queue_gone = false, device_life = false, second = false;
	UINT others = 0, second_at = 0, reuse_at = 0, replace_at = 0, reupload_at = 0, fault_at = 0, stall_at = 0;
	bool draw_first = false, unhashed = false, place_vector = false, no_heap = false, bindings = false, rigid = false, unused_blend = false;
	const char *dump = NULL;
	HMODULE dll = NULL;

	for (int a = 1; a < argc; a++) {
		if (!strncmp(argv[a], "frames=", 7)) frame_count = atoi(argv[a] + 7);
		else if (!strncmp(argv[a], "dump=", 5)) dump = argv[a] + 5;
		else if (!strcmp(argv[a], "nodll")) load_dll = false;
		else if (!strcmp(argv[a], "deform")) deform = true;
		else if (!strcmp(argv[a], "queuelife")) queue_life = true;
		else if (!strcmp(argv[a], "devicelife")) device_life = true;
		else if (!strcmp(argv[a], "queuegone")) queue_gone = true;
		else if (!strcmp(argv[a], "second")) second = true;
		else if (!strncmp(argv[a], "others=", 7)) others = atoi(argv[a] + 7);
		else if (!strncmp(argv[a], "second_at=", 10)) second_at = atoi(argv[a] + 10);
		else if (!strcmp(argv[a], "drawfirst")) draw_first = true;
		else if (!strcmp(argv[a], "unhashed")) unhashed = true;
		else if (!strncmp(argv[a], "reuse_at=", 9)) reuse_at = atoi(argv[a] + 9);
		else if (!strncmp(argv[a], "replace_at=", 11)) replace_at = atoi(argv[a] + 11);
		else if (!strncmp(argv[a], "reupload_at=", 12)) reupload_at = atoi(argv[a] + 12);
		else if (!strncmp(argv[a], "fault_at=", 9)) fault_at = atoi(argv[a] + 9);
		else if (!strncmp(argv[a], "stall_at=", 9)) stall_at = atoi(argv[a] + 9);
		else if (!strcmp(argv[a], "rs11")) signature_1_1 = true;
		else if (!strcmp(argv[a], "place")) place_vector = true;
		else if (!strcmp(argv[a], "noheap")) no_heap = true;
		else if (!strcmp(argv[a], "bindings")) bindings = place_vector = true;
		else if (!strcmp(argv[a], "rigid")) rigid = true;
		else if (!strcmp(argv[a], "unusedblend")) unused_blend = true;
		else {
			printf("unknown argument %s\n", argv[a]);
			return 1;
		}
	}

	if (load_dll) {
		wchar_t path[MAX_PATH];

		GetModuleFileNameW(NULL, path, MAX_PATH);
		wcscpy_s(wcsrchr(path, L'\\') + 1, 16, L"d3d11.dll");
		if (!(dll = LoadLibraryW(path))) {
			printf("could not load %ls: %lu\n", path, GetLastError());
			return 1;
		}
	}

	WNDCLASSW window_class = {};
	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"SkinHarness";
	RegisterClassW(&window_class);
	RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowW(L"SkinHarness", L"SkinHarness", WS_OVERLAPPEDWINDOW, 50, 50, rect.right - rect.left,
		rect.bottom - rect.top, NULL, NULL, window_class.hInstance, NULL);
	show_harness_window(hwnd, stall_at != 0);

	IDXGIFactory4 *factory;
	IDXGISwapChain1 *swap_chain1;
	CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));

	if (device_life) {
		PFN_D3D11ON12_CREATE_DEVICE create_on12 = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(
			dll ? dll : LoadLibraryW(L"d3d11.dll"), "D3D11On12CreateDevice");
		D3D12_COMMAND_QUEUE_DESC desc = {};
		ID3D12Device *short_lived, *real;
		ID3D12CommandQueue *real_queue;
		IUnknown *held = NULL;

		CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&short_lived)));
		// Forwarded by the proxy: the original device, whose queues are not
		// proxies either
		CHECK(short_lived->QueryInterface(__uuidof(ID3D12DebugDevice), (void**)&held));
		CHECK(held->QueryInterface(IID_PPV_ARGS(&real)));
		CHECK(real->CreateCommandQueue(&desc, IID_PPV_ARGS(&real_queue)));
		short_lived->Release();
		for (i = 0; i < 4; i++) {
			IUnknown *queues[] = { real_queue };
			ID3D11Device *on12;

			CHECK(create_on12(short_lived, 0, NULL, 0, queues, 1, 0, &on12, NULL, NULL));
			on12->Release();
		}
		real_queue->Release();
		real->Release();
		held->Release();
		printf("devicelife: D3D11On12CreateDevice took the device 4 times after the proxy was let go of\n");
		fflush(stdout);
	}

	CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

	if (queue_life) {
		PFN_D3D11ON12_CREATE_DEVICE create_on12 = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(
			dll ? dll : LoadLibraryW(L"d3d11.dll"), "D3D11On12CreateDevice");
		ID3D12CommandQueue *short_lived;
		IUnknown *held = NULL;

		CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&short_lived)));
		CHECK(short_lived->QueryInterface(__uuidof(ID3D12DebugCommandQueue), (void**)&held));
		short_lived->Release();
		for (i = 0; i < 4; i++) {
			IUnknown *queues[] = { short_lived };
			ID3D11Device *on12;

			CHECK(create_on12(device, 0, NULL, 0, queues, 1, 0, &on12, NULL, NULL));
			on12->Release();
		}
		held->Release();
		printf("queuelife: D3D11On12CreateDevice took the queue 4 times after the proxy was let go of\n");
		fflush(stdout);
	}

	PFN_D3D11ON12_CREATE_DEVICE create_on12 = NULL;
	ID3D12CommandQueue *gone = NULL;
	if (queue_gone) {
		ID3D11Device *on12 = NULL;
		IUnknown *queues[1];
		HRESULT hr;

		create_on12 = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(
			dll ? dll : LoadLibraryW(L"d3d11.dll"), "D3D11On12CreateDevice");
		CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&gone)));
		gone->Release();

		// Nothing else may be made where the proxy was, or a live object
		// there would be refused as the destroyed one
		for (size_t size = 16; size <= 256; size += 8) {
			void *blocks[64];

			for (int i = 0; i < 64; i++)
				blocks[i] = malloc(size);
			for (int i = 0; i < 64; i++) {
				if (blocks[i] == (void*)gone) {
					printf("queuegone: an allocation of %zu bytes took the destroyed queue's address\n", size);
					return 1;
				}
			}
			for (int i = 0; i < 64; i++)
				free(blocks[i]);
		}

		queues[0] = gone;
		hr = create_on12(device, 0, NULL, 0, queues, 1, 0, &on12, NULL, NULL);
		printf("queuegone: D3D11On12CreateDevice with a destroyed queue returned 0x%x%s\n", hr,
			hr == E_INVALIDARG && !on12 ? "" : " (expected 0x80070057 and no device)");
		fflush(stdout);
		if (hr != E_INVALIDARG || on12)
			return 1;

		// Queues made afterwards are alive and taken, whatever address
		// they got
		for (int i = 0; i < 8; i++) {
			ID3D12CommandQueue *alive;

			CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&alive)));
			queues[0] = alive;
			hr = create_on12(device, 0, NULL, 0, queues, 1, 0, &on12, NULL, NULL);
			if (FAILED(hr) || !on12 || alive == gone) {
				printf("queuegone: a live queue %p was refused or took the destroyed one's address (0x%x)\n", alive, hr);
				return 1;
			}
			on12->Release();
			on12 = NULL;
			alive->Release();
		}
		printf("queuegone: 8 live queues made afterwards were taken\n");
		fflush(stdout);
	}

	DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
	swap_desc.Width = WIDTH;
	swap_desc.Height = HEIGHT;
	swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_desc.SampleDesc.Count = 1;
	swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_desc.BufferCount = BACK_BUFFERS;
	swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &swap_desc, NULL, NULL, &swap_chain1));
	CHECK(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)));

	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	view_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heap_desc.NumDescriptors = BACK_BUFFERS;
	CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = 1024;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu_heap)));
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	for (i = 0; i < BACK_BUFFERS; i++) {
		CHECK(swap_chain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i])));
		device->CreateRenderTargetView(back_buffers[i], NULL, { rtv.ptr + i * rtv_size });
	}
	CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

	// The skinning pipeline: table of t0-t2 and u0, root constant b0
	D3D12_DESCRIPTOR_RANGE ranges[2] = {};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 3;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 3;
	D3D12_ROOT_PARAMETER skin_parameters[2] = {};
	skin_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	skin_parameters[0].Constants.Num32BitValues = 1;
	skin_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	skin_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	skin_parameters[1].DescriptorTable.NumDescriptorRanges = 2;
	skin_parameters[1].DescriptorTable.pDescriptorRanges = ranges;
	skin_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	D3D12_ROOT_SIGNATURE_DESC skin_signature_desc = {};
	skin_signature_desc.NumParameters = 2;
	skin_signature_desc.pParameters = skin_parameters;
	ID3D12RootSignature *skin_signature = root_signature(&skin_signature_desc);
	std::vector<char> skin_cs = read_file(rigid ? "skin_rigid_cs.cso" : "skin_cs.cso");
	D3D12_COMPUTE_PIPELINE_STATE_DESC skin_pso_desc = {};
	skin_pso_desc.pRootSignature = skin_signature;
	skin_pso_desc.CS = { skin_cs.data(), skin_cs.size() };
	ID3D12PipelineState *skin_pso;
	CHECK(device->CreateComputePipelineState(&skin_pso_desc, IID_PPV_ARGS(&skin_pso)));

	// The drawing pipeline: positions from vb0 only, and with place the
	// vector at b0
	D3D12_ROOT_PARAMETER draw_parameters[2] = {};
	draw_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	draw_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
	D3D12_DESCRIPTOR_RANGE draw_srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
	draw_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	draw_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	draw_parameters[1].DescriptorTable = { 1, &draw_srv };
	D3D12_ROOT_SIGNATURE_DESC draw_signature_desc = {};
	draw_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	draw_signature_desc.NumParameters = bindings ? 2 : place_vector ? 1 : 0;
	draw_signature_desc.pParameters = draw_parameters;
	ID3D12RootSignature *draw_signature = root_signature(&draw_signature_desc);
	std::vector<char> vs = read_file(bindings ? "draw_vs_bindings.cso" : place_vector ? "draw_vs_place.cso" : "draw_vs.cso");
	std::vector<char> ps = read_file(bindings ? "draw_ps_bindings.cso" : "draw_ps.cso");
	UINT place_bytes = bindings ? 768 : 256;
	ID3D12Resource *place_buffer = create_buffer(place_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	void *place_data;
	CHECK(place_buffer->Map(0, NULL, &place_data));
	memset(place_data, 0, place_bytes);
	if (bindings) {
		static_cast<float*>(place_data)[65] = 0.75f;
		static_cast<float*>(place_data)[129] = 0.5f;
	}
	place_buffer->Unmap(0, NULL);
	std::vector<D3D12_INPUT_ELEMENT_DESC> layout = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	if (unused_blend)
		layout.push_back({ "BLENDINDICES", 0, DXGI_FORMAT_R32_UINT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 });
	D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_pso_desc = {};
	draw_pso_desc.pRootSignature = draw_signature;
	draw_pso_desc.VS = { vs.data(), vs.size() };
	draw_pso_desc.PS = { ps.data(), ps.size() };
	draw_pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	draw_pso_desc.SampleMask = ~0u;
	draw_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	draw_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	draw_pso_desc.RasterizerState.DepthClipEnable = TRUE;
	draw_pso_desc.InputLayout = { layout.data(), (UINT)layout.size() };
	draw_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	draw_pso_desc.NumRenderTargets = 1;
	draw_pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	draw_pso_desc.SampleDesc.Count = 1;
	ID3D12PipelineState *draw_pso;
	CHECK(device->CreateGraphicsPipelineState(&draw_pso_desc, IID_PPV_ARGS(&draw_pso)));

	// The mesh: a square from x -0.8 to -0.2, bone 0 moves it by y -0.0
	std::vector<Vertex> vertices(GAME_VERTICES);
	std::vector<Weight> weights(GAME_VERTICES);
	std::vector<UINT> indices;
	for (UINT y = 0; y < GY; y++) {
		for (UINT x = 0; x < GX; x++) {
			Vertex &v = vertices[y * GX + x];
			v = {};
			v.p[0] = -0.8f + 0.6f * x / (GX - 1);
			v.p[1] = -0.3f + 0.6f * y / (GY - 1);
			v.p[2] = 0.5f;
			v.n[2] = 1;
			v.t[0] = 1;
			v.t[3] = 1;
			weights[y * GX + x] = { { 1, 0, 0, 0 }, { 0, 0, 0, 0 } };
		}
	}
	for (UINT y = 0; y + 1 < GY; y++) {
		for (UINT x = 0; x + 1 < GX; x++) {
			UINT a = y * GX + x, b = a + 1, c = a + GX, d = c + 1;
			indices.insert(indices.end(), { a, b, c, b, d, c });
		}
	}
	float bones[2 * 4 * 4] = {};
	bones[16 + 7] = 0.8f;

	// The second mesh: a square from y 0.45 to 0.85, its weights split over
	// two entries of bone 0 and its triangles wound the other way
	std::vector<Vertex> vertices2(vertices);
	std::vector<Weight> weights2(GAME_VERTICES);
	std::vector<UINT> indices2;
	for (i = 0; i < GAME_VERTICES; i++) {
		vertices2[i].p[1] = 0.45f + 0.4f * (i / GX) / (GY - 1);
		weights2[i] = { { 0.5f, 0.5f, 0, 0 }, { 0, 0, 0, 0 } };
	}
	for (i = 0; i < indices.size(); i += 3)
		indices2.insert(indices2.end(), { indices[i], indices[i + 2], indices[i + 1] });
	if (others)
		second = true;

	ID3D12CommandAllocator *allocators[BACK_BUFFERS];
	for (i = 0; i < BACK_BUFFERS; i++)
		CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
	ID3D12GraphicsCommandList *list;
	CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0], NULL, IID_PPV_ARGS(&list)));
	std::vector<ID3D12Resource*> staging;
	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	ID3D12Resource *vertex_buffer = upload(list, &staging, vertices.data(), vertices.size() * sizeof(Vertex), read);
	std::vector<UINT> rigid_weights(GAME_VERTICES, 0);
	ID3D12Resource *weight_buffer = rigid ? upload(list, &staging, rigid_weights.data(), rigid_weights.size() * sizeof(UINT), read) : (unhashed ? upload_unhashed : upload)(list, &staging, weights.data(), weights.size() * sizeof(Weight), read);
	ID3D12Resource *bone_buffer = upload(list, &staging, bones, sizeof(bones), read);
	ID3D12Resource *index_buffer = upload(list, &staging, indices.data(), indices.size() * sizeof(UINT), D3D12_RESOURCE_STATE_INDEX_BUFFER);
	ID3D12Resource *index_staging = staging.back(), *index_gpu = NULL;
	if (reupload_at)
		index_gpu = upload(list, &staging, indices.data(), indices.size() * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_SOURCE);
	ID3D12Resource *skinned = create_buffer(GAME_VERTICES * sizeof(Vertex), D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
	ID3D12Resource *vertex_buffer2 = NULL, *weight_buffer2 = NULL, *index_buffer2 = NULL;
	std::vector<ID3D12Resource*> skinned2;
	if (second || reuse_at || replace_at) {
		vertex_buffer2 = upload(list, &staging, vertices2.data(), vertices2.size() * sizeof(Vertex), read);
		weight_buffer2 = (unhashed ? upload_unhashed : upload)(list, &staging, weights2.data(), weights2.size() * sizeof(Weight), read);
		index_buffer2 = upload(list, &staging, indices2.data(), indices2.size() * sizeof(UINT), D3D12_RESOURCE_STATE_INDEX_BUFFER);
		for (i = 0; second && i <= others && i < 250; i++)
			skinned2.push_back(create_buffer(GAME_VERTICES * sizeof(Vertex), D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true));
	}
	CHECK(list->Close());
	queue->ExecuteCommandLists(1, (ID3D12CommandList**)&list);
	wait_for(signal());

	// The views of the skinning dispatch, written into the shader visible heap
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = gpu_heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE table = gpu_heap->GetGPUDescriptorHandleForHeapStart();
	ID3D12Resource *srv_buffers[3] = { vertex_buffer, weight_buffer, bone_buffer };
	UINT srv_counts[3] = { GAME_VERTICES, GAME_VERTICES, rigid ? 2u : 1u }, srv_strides[3] = { sizeof(Vertex), rigid ? 4u : (UINT)sizeof(Weight), 64 };
	for (i = 0; i < 3; i++) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
		srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Buffer.NumElements = srv_counts[i];
		srv.Buffer.StructureByteStride = srv_strides[i];
		device->CreateShaderResourceView(srv_buffers[i], &srv, { cpu.ptr + i * view_size });
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	uav.Buffer.NumElements = GAME_VERTICES;
	uav.Buffer.StructureByteStride = sizeof(Vertex);
	device->CreateUnorderedAccessView(skinned, NULL, &uav, { cpu.ptr + 3 * view_size });
	// A table of four views a copy of the second mesh, from descriptor 4 on
	for (UINT m = 0; m < skinned2.size(); m++) {
		ID3D12Resource *buffers2[3] = { vertex_buffer2, weight_buffer2, bone_buffer };
		for (i = 0; i < 3; i++) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = srv_counts[i];
			srv.Buffer.StructureByteStride = srv_strides[i];
			device->CreateShaderResourceView(buffers2[i], &srv, { cpu.ptr + (4 * (m + 1) + i) * view_size });
		}
		device->CreateUnorderedAccessView(skinned2[m], NULL, &uav, { cpu.ptr + (4 * (m + 1) + 3) * view_size });
	}

	D3D12_GPU_DESCRIPTOR_HANDLE table_reuse = { table.ptr + 1000 * view_size };
	if (reuse_at) {
		ID3D12Resource *buffers2[3] = { vertex_buffer2, weight_buffer2, bone_buffer };
		for (i = 0; i < 3; i++) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = srv_counts[i];
			srv.Buffer.StructureByteStride = srv_strides[i];
			device->CreateShaderResourceView(buffers2[i], &srv, { cpu.ptr + (1000 + i) * view_size });
		}
		device->CreateUnorderedAccessView(skinned, NULL, &uav, { cpu.ptr + 1003 * view_size });
	}

	auto write_table = [&](UINT at, ID3D12Resource *source, ID3D12Resource *weight, UINT count, ID3D12Resource *output) {
		ID3D12Resource *buffers[3] = { source, weight, bone_buffer };
		UINT counts[3] = { count, count, 1 };
		D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav = uav;
		for (UINT v = 0; v < 3; v++) {
			D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
			srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Buffer.NumElements = counts[v];
			srv.Buffer.StructureByteStride = srv_strides[v];
			device->CreateShaderResourceView(buffers[v], &srv, { cpu.ptr + (at + v) * view_size });
		}
		output_uav.Buffer.NumElements = count;
		device->CreateUnorderedAccessView(output, NULL, &output_uav, { cpu.ptr + (at + 3) * view_size });
	};
	// replace_at: the mesh of 320 vertices, the heap the two placed buffers
	// share their address in and the second mesh's buffer of its own
	const UINT REPLACE_VERTICES = 320;
	ID3D12Heap *replace_heap = NULL;
	ID3D12Resource *placed = NULL, *own2 = NULL, *vertex_buffer3 = NULL, *weight_buffer3 = NULL;
	auto place = [&](UINT64 bytes) {
		D3D12_RESOURCE_DESC desc = {};
		ID3D12Resource *resource;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		CHECK(device->CreatePlacedResource(replace_heap, 0, &desc, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, NULL, IID_PPV_ARGS(&resource)));
		return resource;
	};
	if (replace_at) {
		std::vector<Vertex> vertices3(REPLACE_VERTICES, vertices[0]);
		std::vector<Weight> weights3(REPLACE_VERTICES, weights[0]);
		D3D12_HEAP_DESC heap_desc3 = {};
		for (i = 0; i < GAME_VERTICES; i++)
			vertices3[i] = vertices[i];
		heap_desc3.SizeInBytes = 65536;
		heap_desc3.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap_desc3.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
		CHECK(device->CreateHeap(&heap_desc3, IID_PPV_ARGS(&replace_heap)));
		CHECK(allocators[0]->Reset());
		CHECK(list->Reset(allocators[0], NULL));
		vertex_buffer3 = upload(list, &staging, vertices3.data(), vertices3.size() * sizeof(Vertex), read);
		weight_buffer3 = upload(list, &staging, weights3.data(), weights3.size() * sizeof(Weight), read);
		CHECK(list->Close());
		queue->ExecuteCommandLists(1, (ID3D12CommandList**)&list);
		wait_for(signal());
		placed = place(GAME_VERTICES * sizeof(Vertex));
		own2 = create_buffer(GAME_VERTICES * sizeof(Vertex), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
		write_table(1004, vertex_buffer2, weight_buffer2, GAME_VERTICES, placed);
		write_table(1012, vertex_buffer2, weight_buffer2, GAME_VERTICES, own2);
	}

	D3D12_VERTEX_BUFFER_VIEW vb_view = { skinned->GetGPUVirtualAddress(), GAME_VERTICES * (UINT)sizeof(Vertex), sizeof(Vertex) };
	D3D12_INDEX_BUFFER_VIEW ib_view = { index_buffer->GetGPUVirtualAddress(), (UINT)(indices.size() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
	D3D12_INDEX_BUFFER_VIEW ib_view_reuse = ib_view;
	if (index_buffer2)
		ib_view_reuse = { index_buffer2->GetGPUVirtualAddress(), (UINT)(indices2.size() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
	UINT64 frame_fences[BACK_BUFFERS] = {};
	D3D12_VIEWPORT viewport = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
	D3D12_RECT scissor = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
	const float clear[4] = { 0.1f, 0.1f, 0.15f, 1 };
	MSG message;
	LARGE_INTEGER frequency, before, after;
	double recording = 0;
	UINT timed_frames = 0;
	QueryPerformanceFrequency(&frequency);

	for (f = 0; f < frame_count; f++) {
		UINT back = swap_chain->GetCurrentBackBufferIndex();

		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		wait_for(frame_fences[back]);
		if (replace_at && f == replace_at) {
			wait_for(signal());
			placed->Release();
			placed = place(REPLACE_VERTICES * sizeof(Vertex));
			write_table(1008, vertex_buffer3, weight_buffer3, REPLACE_VERTICES, placed);
		}
		D3D12_VERTEX_BUFFER_VIEW vb_square = vb_view;
		if (fault_at && f == fault_at) {
			wait_for(signal());
			ID3D12Resource *gone = create_buffer(GAME_VERTICES * sizeof(Vertex), D3D12_HEAP_TYPE_DEFAULT,
				D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			vb_square.BufferLocation = gone->GetGPUVirtualAddress();
			gone->Release();
		}
		UINT phase = f < replace_at ? 0 : f < 2 * replace_at ? 1 : 2;
		ID3D12Resource *replace_output = phase == 2 ? own2 : placed;
		UINT replace_count = phase == 1 ? REPLACE_VERTICES : GAME_VERTICES;
		QueryPerformanceCounter(&before);
		CHECK(allocators[back]->Reset());
		CHECK(list->Reset(allocators[back], NULL));

		// deform: the game writes the vertices it skins every frame, as it
		// does those of a face with blend shapes, before skinning them
		if (deform) {
			transition(list, vertex_buffer, read, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(vertex_buffer, 0, staging[0], 0, vertices.size() * sizeof(Vertex));
			transition(list, vertex_buffer, D3D12_RESOURCE_STATE_COPY_DEST, read);
		}
		if (reupload_at && (f == reupload_at || f == 2 * reupload_at)) {
			transition(list, index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(index_buffer, 0, f == reupload_at ? index_gpu : index_staging, 0, indices.size() * sizeof(UINT));
			transition(list, index_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		}
		UINT meshes2 = f >= second_at ? (UINT)skinned2.size() : 0;
		auto skin_meshes = [&]() {
			list->SetDescriptorHeaps(1, &gpu_heap);
			transition(list, skinned, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			list->SetComputeRootSignature(skin_signature);
			list->SetPipelineState(skin_pso);
			list->SetComputeRoot32BitConstant(0, GAME_VERTICES, 0);
			list->SetComputeRootDescriptorTable(1, f < reuse_at ? table_reuse : table);
			list->Dispatch((GAME_VERTICES + 63) / 64, 1, 1);
			transition(list, skinned, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			if (replace_at) {
				transition(list, replace_output, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				list->SetComputeRoot32BitConstant(0, replace_count, 0);
				list->SetComputeRootDescriptorTable(1, { table.ptr + (1004 + 4 * phase) * view_size });
				list->Dispatch((replace_count + 63) / 64, 1, 1);
				transition(list, replace_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			}
			for (UINT m = 0; m < meshes2; m++) {
				transition(list, skinned2[m], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				list->SetComputeRootDescriptorTable(1, { table.ptr + 4 * (m + 1) * view_size });
				list->Dispatch((GAME_VERTICES + 63) / 64, 1, 1);
				transition(list, skinned2[m], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			}
		};
		auto draw_meshes = [&]() {
			if (no_heap)
				list->SetDescriptorHeaps(0, &gpu_heap);
			transition(list, back_buffers[back], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			D3D12_CPU_DESCRIPTOR_HANDLE target = { rtv.ptr + back * rtv_size };
			list->OMSetRenderTargets(1, &target, FALSE, NULL);
			list->ClearRenderTargetView(target, clear, 0, NULL);
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
			list->SetGraphicsRootSignature(draw_signature);
			if (place_vector)
				list->SetGraphicsRootConstantBufferView(0, place_buffer->GetGPUVirtualAddress());
			if (bindings)
				list->SetGraphicsRootDescriptorTable(1, gpu_heap->GetGPUDescriptorHandleForHeapStart());
			list->SetPipelineState(draw_pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->IASetVertexBuffers(0, 1, &vb_square);
			list->IASetIndexBuffer(f < reuse_at ? &ib_view_reuse : &ib_view);
			list->DrawIndexedInstanced(GAME_INDICES, 1, 0, 0, 0);
			if (replace_at) {
				D3D12_VERTEX_BUFFER_VIEW vb_view3 = { replace_output->GetGPUVirtualAddress(), replace_count * (UINT)sizeof(Vertex), sizeof(Vertex) };
				list->IASetVertexBuffers(0, 1, &vb_view3);
				list->IASetIndexBuffer(phase == 1 ? &ib_view : &ib_view_reuse);
				list->DrawIndexedInstanced(GAME_INDICES, 1, 0, 0, 0);
			}
			for (UINT m = 0; m < meshes2; m++) {
				D3D12_VERTEX_BUFFER_VIEW vb_view2 = { skinned2[m]->GetGPUVirtualAddress(), GAME_VERTICES * (UINT)sizeof(Vertex), sizeof(Vertex) };
				D3D12_INDEX_BUFFER_VIEW ib_view2 = { index_buffer2->GetGPUVirtualAddress(), (UINT)(indices2.size() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
				list->IASetVertexBuffers(0, 1, &vb_view2);
				list->IASetIndexBuffer(&ib_view2);
				list->DrawIndexedInstanced(GAME_INDICES, 1, 0, 0, 0);
			}
			transition(list, back_buffers[back], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		};
		if (draw_first) {
			draw_meshes();
			skin_meshes();
		} else {
			skin_meshes();
			draw_meshes();
		}
		CHECK(list->Close());
		queue->ExecuteCommandLists(1, (ID3D12CommandList**)&list);
		if (fault_at && f == fault_at) {
			HRESULT reason = S_OK;

			for (int waited = 0; waited < 10000 && reason == S_OK; waited += 50) {
				Sleep(50);
				reason = device->GetDeviceRemovedReason();
			}
			printf("fault_at: removed reason 0x%08x\n", (unsigned)reason);
			fflush(stdout);
			Sleep(3000);
			ExitProcess(reason == S_OK ? 1 : 0);
		}
		QueryPerformanceCounter(&after);
		if (f >= 60) {
			recording += (double)(after.QuadPart - before.QuadPart) / frequency.QuadPart;
			timed_frames++;
		}

		if (f + 1 == frame_count) {
			wait_for(signal());
			printf("frame %u:\n", f);
			check_picture(back_buffers[back], dump);
		}
		CHECK(swap_chain->Present(0, 0));
		frame_fences[back] = signal();
		if (stall_at && f == stall_at) {
			SetForegroundWindow(hwnd);
			Sleep(4000);
			printf("stall_at: the window was %sin front\n", GetForegroundWindow() == hwnd ? "" : "not ");
		}
	}
	wait_for(signal());
	if (timed_frames)
		printf("recording and executing %.4f ms a frame (%u skinned meshes)\n", recording * 1000 / timed_frames, 1 + (UINT)skinned2.size());

	if (queue_gone) {
		IUnknown *queues[] = { gone };
		ID3D11Device *on12 = NULL;
		HRESULT hr = create_on12(device, 0, NULL, 0, queues, 1, 0, &on12, NULL, NULL);

		printf("queuegone: after the swap chain presented, D3D11On12CreateDevice with the destroyed queue returned 0x%x%s\n",
			hr, SUCCEEDED(hr) && on12 ? "" : " (expected a device on the presenting queue)");
		fflush(stdout);
		if (FAILED(hr) || !on12)
			return 1;
		on12->Release();
	}
	return 0;
}
