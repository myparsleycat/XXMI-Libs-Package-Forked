// A D3D12 renderer shaped like the game's, for timing the engine shim without
// the game: about a thousand small draws a frame over a few passes, a buffer
// per vertex stream of every mesh, descriptors copied into the shader visible
// heap for every draw, constant buffers and some vertex buffers handed out
// anew from an upload buffer every frame, and buffers uploaded every frame.
//
// harness.exe [frames=3000] [nodll] [meshes=350] [passes=3] [dump=file.bmp]
//             [key=VK] [keyframe=N] [uploads=60] [churn=1] [table=4] [heap=N]
//             [video] [dxil] [rootconst] [threads=1] [rereach=K] [contend]
//             [profile | profile=present] [test [move | evict [evictstop=N] | lowmip]]
//             [scene=file [slot=vb0] [appear=N]]
// Prints the average frame time of the frames after the first 300, in blocks.
//   dump=file.bmp  the last frame as a picture and its checksum
//   key=VK         a key press the shim sees at frame keyframe
//   uploads, churn the buffers handed out from the upload ring each frame
//   table=N        SRVs in the pixel shaders' table; heap=N its shader visible heap
//   video          a 1080p texture written whole every frame
//   dxil           the game's shaders compiled to DXIL (dxcompiler.dll next to it)
//   rootconst      root constants in the root signature
//   threads=1      passes after the first recorded on a thread of their own
//   test           the meshes and textures that Mods/test.ini looks for (check.sh)
//   move, evict    with test: see move_test and evict_test below
//   lowmip         with test: see lowmip_test below
//   scene=file     the parts of real mods (cpu/dx12.sh); appear=N: a mod at a time
//                  for N frames, CYCLES=N in the environment repeats them,
//                  STEADY profiles the steady frames in place of the first ones
// Exits with 1 when D3D12 fails, an argument is unknown or a part of a scene
// cannot be given its hash.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <dbghelp.h>
#include <map>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <atomic>
#include <thread>
#include <algorithm>
#include "../common/crc.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dbghelp.lib")

#define CHECK(x) do { HRESULT hr__ = (x); if (FAILED(hr__)) { printf("FAILED %s = 0x%08x at line %d\n", #x, (unsigned)hr__, __LINE__); fflush(stdout); ExitProcess(1); } } while (0)

static const UINT WIDTH = 640, HEIGHT = 360;
static const UINT BACK_BUFFERS = 3;
static const UINT PIPELINES = 8;
static const UINT TEXTURES = 64;
static const UINT TEXTURE_SIZE = 64;
static const UINT VERTICES = 24, INDICES = 36;
static const UINT MAX_DYNAMIC_UPLOADS = 256;
static UINT DYNAMIC_UPLOADS = 60;
static UINT churn = 1;
// rereach=K: every 8 frames the index buffers of K plain meshes are written
// again with the order of a triangle flipped, which makes the shim work out
// how far they reach once more (small buffers a game writes over and over)
static UINT rereach;
// contend: a thread makes and releases big committed resources the whole
// time, as a game streaming in its world does
static bool contend;
static std::atomic<bool> contend_stop;
static std::atomic<UINT> contend_rounds;
static bool test;
// move (with test): the positions of mesh A are moved by copies between
// buffers of the GPU, as an engine compacting its pools does: at frame 500 to
// pool 1 +65792, at 600 to pool 2 +256, at 700 pool 2 whole to pool 3 by
// CopyResource, at 800 to pool 1 +131072 in two halves. A stays hidden while
// the hash goes with it, up to 800, where no copy is of it whole. At 900 pool
// 2 +256 is copied to pool 1 +196608 by the second list, recorded before the
// first list writes other positions to pool 2 +256: executed in that order, A
// is drawn. At 1000 pool 2 +256 gets A's positions again and a list of its
// own copies them to pool 3 +4096 (A hidden); at 1100 pool 2 +256 gets other
// positions and that list is executed again unchanged (A drawn). At 1200 the
// second list copies half of the control mesh's positions to pool 3 +8192,
// recorded before the first list copies all of A's there (A drawn). At 1300
// the second list uploads other positions to pool 3 +12288, recorded before
// the first list copies A's there (A drawn). At 1400 pool 3 +16384 gets other
// positions; at 1450 a list of its own records an upload of A's there but is
// only executed at 1460, after the first list has copied pool 3 +16384 to
// pool 1 +262144 (A drawn). At 1600 a list of its own uploads A's positions
// from an upload buffer of its own to pool 3 +20480 (A hidden); at 1650 that
// upload buffer gets other positions, the first list uploads A's there and
// the list is executed again after it (A drawn). At 1690 pool 3 +24576 gets
// other positions; at 1700 the queue waits for a second queue before the
// first list, which uploads A's there, and the second queue copies them to
// pool 1 +327680 before it signals (A drawn: the copy runs first). At 1800
// the first list uploads A's to pool 3 +28672 and the queue signals, which
// the second queue waits for before it copies them to pool 1 +393216 (A
// hidden). At 1900 a list of its own records an upload of A's positions to
// pool 3 +32768, the first list then records an upload of other positions
// there, and the list of its own is executed before the first list draws A
// from there (A drawn). At 1950 the first list records an upload of A's
// positions to pool 3 +36864, a list of its own then records an upload of
// other positions there, and the first list draws A from there (A hidden);
// that list is executed last. The passes after the first draw over where A is
// looked at, so 1950 is told with passes=1: the second list cannot know which
// of the two uploads it reads. At 1980 a list of its own uploads A's
// positions to pool 3 +40960 and is executed at once, another list of its own
// records an upload of other positions there but is executed last, and the
// first list draws A from there in between (A hidden with passes=1).
static bool move_test;
static ID3D12Resource *race_uploads[4];
static ID3D12CommandAllocator *race_allocators[4];
static ID3D12GraphicsCommandList *race_lists[4];
static ID3D12Resource *again_upload;
static ID3D12CommandAllocator *again_allocator;
static ID3D12GraphicsCommandList *again_list;
static ID3D12CommandQueue *queue2;
static ID3D12Fence *queue2_fence;
static ID3D12CommandAllocator *queue2_allocators[2];
static ID3D12GraphicsCommandList *queue2_list;
static ID3D12Resource *late_upload;
static ID3D12CommandAllocator *late_allocator;
static ID3D12GraphicsCommandList *late_list;
static ID3D12Resource *move_pools[3];
static ID3D12CommandAllocator *move_allocator;
static ID3D12GraphicsCommandList *move_list;
// evict: the mesh buffers are evicted after every frame and made resident only
// right before the lists that draw them are executed, after an empty list, as
// an engine that manages residency does when it is over budget
static bool evict_test;
// evictstop=N: from frame N on the buffers are left as they are
static int evict_stop = -1;
static std::vector<ID3D12Pageable*> evicted_buffers;
static ID3D12GraphicsCommandList *empty_list;
// The meshes and textures of the test: see Mods/test.ini
static const UINT TEST_A = 30, TEST_B = 31, TEST_C = 32, TEST_CONTROL = 33, TEST_TEXTURE = 40, TEST_TEXTURE_B = 41;
// lowmip (with test): texture TEST_LOWMIP_FULL is twice the size of the others
// with a second mip level, and TEST_LOWMIP_COPY the size of the others with
// one level, filled by a copy of that second level, as texture streaming makes
// a texture without its top level. Mods/test.ini skips the draws with the
// first, and with match_lower_mip_textures those with the second.
static bool lowmip_test;
static const UINT TEST_LOWMIP_FULL = 43, TEST_LOWMIP_COPY = 44;
static BYTE test_vb[VERTICES * 12], test_other_vb[VERTICES * 12];
static UINT test_texels[TEXTURE_SIZE * TEXTURE_SIZE];
static const UINT RING_VBS = 30;
// table=N: SRVs in the table of the pixel shaders, all of them filled with
// textures for each draw
static UINT TABLE = 4;

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static IDXGISwapChain3 *swap_chain;
static ID3D12Resource *back_buffers[BACK_BUFFERS];
static ID3D12DescriptorHeap *rtv_heap, *cpu_heap, *gpu_heap;
static UINT rtv_size, view_size;
static ID3D12RootSignature *root_signature;
static ID3D12PipelineState *pipelines[PIPELINES];
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_value;

struct Mesh {
	UINT first = 0, count = 36;
	ID3D12Resource *vb[3], *ib;
	D3D12_VERTEX_BUFFER_VIEW views[3];
	D3D12_INDEX_BUFFER_VIEW index_view;
	UINT texture;
};
static std::vector<Mesh> meshes;
// scene=file: a line a mesh, "hash first count": the part of a real mod. The
// mesh gets an index buffer (slot=vb0: positions) with that hash, as the
// shim computes it, and is drawn with that range of it.
struct ScenePart {
	uint32_t hash;
	UINT first, count;
	// appear=N: the mod the part is of; the parts of mod g are drawn from
	// frame 300 + g * N for N frames only
	int group;
};
static UINT appear_frames;
static int appear_cycles = 1, appear_groups = 1;
static int visible_group = -1;
static std::vector<ScenePart> scene_parts;
static bool scene_on_vb0;
static UINT scene_failures;
static ID3D12Resource *textures[TEXTURES];
static ID3D12Resource *dynamic_buffers[MAX_DYNAMIC_UPLOADS];
// video: a 1080p texture written whole every frame, as a game playing a video into it
static bool video;
static const UINT VIDEO_WIDTH = 1920, VIDEO_HEIGHT = 1080;
static ID3D12Resource *video_texture, *video_uploads[3];
static BYTE *video_data[3];

struct Frame {
	ID3D12CommandAllocator *allocators[2];
	ID3D12Resource *ring;
	BYTE *ring_data;
	UINT64 fence;
};
static Frame frames[BACK_BUFFERS];
static ID3D12GraphicsCommandList *lists[2];
static const UINT64 RING_BYTES = 8 << 20;
static UINT GPU_HEAP_DESCRIPTORS = 65536;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, message, wparam, lparam);
}

static ID3D12Resource* create_buffer(UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
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

static bool use_dxil;
// rootconst: the pixel shaders scale their colour by root constants (b1) set
// once a list, which the engine has to set again after switching the root
// signature for a draw of a mod
static bool root_constants;

// SM 6.0 through dxcompiler.dll next to the exe, for the shim's DXIL paths
static ID3DBlob* compile_dxil(const char *source, const char *entry, const char *target, UINT variant)
{
	static IDxcCompiler3 *compiler;
	wchar_t wentry[32], wtarget[16], define[32];
	IDxcResult *result;
	IDxcBlob *object;
	ID3DBlob *code;
	HRESULT status;

	if (!compiler) {
		LoadLibraryA("dxil.dll");
		DxcCreateInstanceProc create = (DxcCreateInstanceProc)GetProcAddress(LoadLibraryA("dxcompiler.dll"), "DxcCreateInstance");
		if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)))) {
			printf("dxcompiler.dll missing\n");
			ExitProcess(1);
		}
	}
	swprintf_s(wentry, L"%S", entry);
	swprintf_s(wtarget, L"%.3S6_0", target);
	swprintf_s(define, L"VARIANT=%u", variant);
	const wchar_t *args[] = { L"-E", wentry, L"-T", wtarget, L"-D", define, L"-D", root_constants ? L"ROOTCONST=1" : L"ROOTCONST=0" };
	DxcBuffer buffer = { source, strlen(source), DXC_CP_UTF8 };
	CHECK(compiler->Compile(&buffer, args, ARRAYSIZE(args), NULL, IID_PPV_ARGS(&result)));
	result->GetStatus(&status);
	if (FAILED(status)) {
		IDxcBlobUtf8 *errors = NULL;
		result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), NULL);
		printf("shader: %s\n", errors ? errors->GetStringPointer() : "?");
		ExitProcess(1);
	}
	CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), NULL));
	CHECK(D3DCreateBlob(object->GetBufferSize(), &code));
	memcpy(code->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
	object->Release();
	result->Release();
	return code;
}

static ID3DBlob* compile(const char *source, const char *entry, const char *target, UINT variant)
{
	if (use_dxil)
		return compile_dxil(source, entry, target, variant);
	char value[16];
	D3D_SHADER_MACRO macros[3] = { { "VARIANT", value }, { "ROOTCONST", root_constants ? "1" : "0" }, { NULL, NULL } };
	ID3DBlob *code = NULL, *errors = NULL;

	sprintf_s(value, "%u", variant);
	if (FAILED(D3DCompile(source, strlen(source), NULL, macros, NULL, entry, target, 0, 0, &code, &errors))) {
		printf("shader: %s\n", errors ? (const char*)errors->GetBufferPointer() : "?");
		ExitProcess(1);
	}
	return code;
}

static const char shader_source[] =
	"cbuffer Draw : register(b0) { float4 placement; float4 tint; };\n"
	"Texture2D tex0 : register(t0);\n"
	"Texture2D tex1 : register(t1);\n"
	"SamplerState smp : register(s0);\n"
	"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };\n"
	"VSOut vs(float3 pos : POSITION, float2 uv : TEXCOORD0, float4 color : COLOR0) {\n"
	"	VSOut o;\n"
	"	o.pos = float4(pos.xy * placement.zw + placement.xy, pos.z, 1 + VARIANT * 0.0);\n"
	"	o.uv = uv; o.color = color; return o;\n"
	"}\n"
	"#if ROOTCONST\n"
	"cbuffer Extra : register(b1) { float4 scale; };\n"
	"#else\n"
	"static const float4 scale = 1;\n"
	"#endif\n"
	"float4 ps(VSOut i) : SV_Target0 {\n"
	"	return (tex0.Sample(smp, i.uv) * 0.5 + tex1.Sample(smp, i.uv) * 0.5) * i.color * tint * scale * (1 + VARIANT * 0.01);\n"
	"}\n";

static void create_pipelines()
{
	D3D12_DESCRIPTOR_RANGE range = {};
	D3D12_ROOT_PARAMETER parameters[3] = {};
	D3D12_STATIC_SAMPLER_DESC sampler = {};
	D3D12_ROOT_SIGNATURE_DESC signature = {};
	ID3DBlob *blob = NULL, *errors = NULL;
	UINT i;

	parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = TABLE;
	parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	parameters[1].DescriptorTable.NumDescriptorRanges = 1;
	parameters[1].DescriptorTable.pDescriptorRanges = &range;
	parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[2].Constants.ShaderRegister = 1;
	parameters[2].Constants.Num32BitValues = 4;
	parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	signature.NumParameters = root_constants ? 3 : 2;
	signature.pParameters = parameters;
	signature.NumStaticSamplers = 1;
	signature.pStaticSamplers = &sampler;
	signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
	CHECK(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
	CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature)));

	static const D3D12_INPUT_ELEMENT_DESC layout[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	for (i = 0; i < PIPELINES; i++) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
		ID3DBlob *vs = compile(shader_source, "vs", "vs_5_0", i);
		ID3DBlob *ps = compile(shader_source, "ps", "ps_5_0", i);

		desc.pRootSignature = root_signature;
		desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
		desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		desc.SampleMask = ~0u;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.InputLayout = { layout, ARRAYSIZE(layout) };
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[i])));
		vs->Release();
		ps->Release();
	}
}

static UINT rng_state = 12345;
static UINT rng()
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void create_lowmip_texture(ID3D12GraphicsCommandList *list, ID3D12Resource *upload, BYTE *data, UINT64 *at, UINT i)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	bool full = i == TEST_LOWMIP_FULL;
	UINT level, size, v;

	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = full ? TEXTURE_SIZE * 2 : TEXTURE_SIZE;
	desc.Height = full ? TEXTURE_SIZE * 2 : TEXTURE_SIZE;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = full ? 2 : 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&textures[i])));

	dst.pResource = textures[i];
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	if (!full) {
		src.pResource = textures[TEST_LOWMIP_FULL];
		src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.SubresourceIndex = 1;
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
		transition(list, textures[TEST_LOWMIP_FULL], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		transition(list, textures[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		return;
	}

	for (level = 0; level < 2; level++) {
		UINT *texels = (UINT*)(data + *at);

		size = TEXTURE_SIZE * 2 >> level;
		for (v = 0; v < size * size; v++)
			texels[v] = 0xff000000 | ((v * 2654435761u) >> 8 & 0xffffff) ^ (level * 0x5a5a5a);
		dst.SubresourceIndex = level;
		src.pResource = upload;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Offset = *at;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, size, size, 1, size * 4 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
		*at += size * size * 4;
	}
	// Until TEST_LOWMIP_COPY is copied from it
	transition(list, textures[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
}

// Everything the scene is made of, uploaded the way an engine does: written
// to an upload buffer and copied from there
static void create_scene(UINT mesh_count)
{
	const UINT64 vb_bytes[3] = { VERTICES * 12, VERTICES * 8, VERTICES * 16 };
	const UINT strides[3] = { 12, 8, 16 };
	const UINT64 ib_bytes = INDICES * 2;
	const UINT64 per_mesh = 1024;
	// lowmip: TEST_LOWMIP_FULL is the size of five of the others
	UINT64 upload_bytes = mesh_count * per_mesh + (UINT64)(TEXTURES + 4) * TEXTURE_SIZE * TEXTURE_SIZE * 4 + 65536;
	ID3D12Resource *upload = create_buffer(upload_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
	ID3D12CommandAllocator *allocator = frames[0].allocators[0];
	ID3D12GraphicsCommandList *list = lists[0];
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	BYTE *data;
	UINT64 at = 0;
	UINT i, j, v;

	CHECK(upload->Map(0, NULL, (void**)&data));
	CHECK(allocator->Reset());
	CHECK(list->Reset(allocator, NULL));

	meshes.resize(mesh_count);
	for (i = 0; i < mesh_count; i++) {
		Mesh *mesh = &meshes[i];
		float *positions = (float*)(data + at);
		float *uvs = (float*)(data + at + vb_bytes[0]);
		float *colors = (float*)(data + at + vb_bytes[0] + vb_bytes[1]);
		UINT16 *indices = (UINT16*)(data + at + vb_bytes[0] + vb_bytes[1] + vb_bytes[2]);
		UINT64 offset = at;

		// Six quads, each mesh a little different
		for (v = 0; v < VERTICES; v++) {
			UINT quad = v / 4, corner = v % 4;

			positions[v * 3 + 0] = (corner & 1 ? 1.0f : -1.0f) * (0.4f + quad * 0.1f) + (rng() % 1000) * 0.00001f;
			positions[v * 3 + 1] = (corner & 2 ? 1.0f : -1.0f) * (0.4f + quad * 0.1f);
			positions[v * 3 + 2] = 0.5f;
			uvs[v * 2 + 0] = corner & 1 ? 1.0f : 0.0f;
			uvs[v * 2 + 1] = corner & 2 ? 1.0f : 0.0f;
			colors[v * 4 + 0] = (rng() % 256) / 255.0f;
			colors[v * 4 + 1] = (rng() % 256) / 255.0f;
			colors[v * 4 + 2] = (rng() % 256) / 255.0f;
			colors[v * 4 + 3] = 1.0f;
		}
		// TEST_C starts out with what TEST_A has
		if (i == TEST_A)
			memcpy(test_vb, positions, sizeof(test_vb));
		if (test && i == TEST_C) {
			memcpy(test_other_vb, positions, sizeof(test_other_vb));
			memcpy(positions, test_vb, sizeof(test_vb));
		}
		for (j = 0; j < INDICES / 6; j++) {
			const UINT16 quad[6] = { 0, 1, 2, 2, 1, 3 };
			for (v = 0; v < 6; v++)
				indices[j * 6 + v] = (UINT16)(j * 4 + quad[v]);
		}

		for (j = 0; j < 3; j++) {
			mesh->vb[j] = create_buffer(vb_bytes[j], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(mesh->vb[j], 0, upload, offset, vb_bytes[j]);
			transition(list, mesh->vb[j], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			mesh->views[j] = { mesh->vb[j]->GetGPUVirtualAddress(), (UINT)vb_bytes[j], strides[j] };
			offset += vb_bytes[j];
		}
		mesh->ib = create_buffer(ib_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyBufferRegion(mesh->ib, 0, upload, offset, ib_bytes);
		transition(list, mesh->ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		mesh->index_view = { mesh->ib->GetGPUVirtualAddress(), (UINT)ib_bytes, DXGI_FORMAT_R16_UINT };
		mesh->texture = i % TEXTURES;
		at += per_mesh;
	}

	for (i = 0; i < scene_parts.size() && i < mesh_count; i++) {
		const ScenePart &part = scene_parts[i];
		Mesh *mesh = &meshes[i];
		// Two indices more than are drawn, for the bytes that are solved for
		UINT index_bytes = (part.first + part.count + 2) * 2;
		UINT position_bytes = VERTICES * 12 + 4;
		std::vector<BYTE> indices(index_bytes), positions(position_bytes);
		UINT desc[6] = {};
		void *mapped;

		for (v = 0; v < index_bytes / 2; v++)
			((UINT16*)indices.data())[v] = (UINT16)((v / 6 % 6) * 4 + "\0\1\2\2\1\3"[v % 6]);
		for (v = 0; v < VERTICES; v++) {
			float *position = (float*)positions.data() + v * 3;
			position[0] = ((v & 1) ? 1.0f : -1.0f) * (0.4f + v / 4 * 0.1f);
			position[1] = ((v & 2) ? 1.0f : -1.0f) * (0.4f + v / 4 * 0.1f);
			position[2] = 0.5f;
		}
		// A D3D11_BUFFER_DESC: ByteWidth, D3D11_USAGE_DEFAULT, BindFlags
		std::vector<BYTE> &forged = scene_on_vb0 ? positions : indices;
		desc[0] = (UINT)forged.size();
		desc[2] = scene_on_vb0 ? 1 : 2;
		crc_forge(forged.data(), forged.size(), desc, sizeof(desc), part.hash);
		if (crc32c(crc32c(0, forged.data(), forged.size()), desc, sizeof(desc)) != part.hash)
			scene_failures++;

		// The buffers the mesh had stay, the list copies to them
		ID3D12Resource *staging = create_buffer(index_bytes + position_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
		CHECK(staging->Map(0, NULL, &mapped));
		memcpy(mapped, indices.data(), index_bytes);
		memcpy((BYTE*)mapped + index_bytes, positions.data(), position_bytes);
		mesh->ib = create_buffer(index_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyBufferRegion(mesh->ib, 0, staging, 0, index_bytes);
		transition(list, mesh->ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
		mesh->index_view = { mesh->ib->GetGPUVirtualAddress(), index_bytes, DXGI_FORMAT_R16_UINT };
		if (scene_on_vb0) {
			mesh->vb[0] = create_buffer(position_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(mesh->vb[0], 0, staging, index_bytes, position_bytes);
			transition(list, mesh->vb[0], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			mesh->views[0] = { mesh->vb[0]->GetGPUVirtualAddress(), position_bytes, 12 };
		}
		mesh->first = part.first;
		mesh->count = part.count;
	}

	cpu = cpu_heap->GetCPUDescriptorHandleForHeapStart();
	for (i = 0; i < TEXTURES; i++) {
		D3D12_HEAP_PROPERTIES heap = {};
		D3D12_RESOURCE_DESC desc = {};
		D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
		UINT *texels = (UINT*)(data + at);

		if (lowmip_test && (i == TEST_LOWMIP_FULL || i == TEST_LOWMIP_COPY)) {
			create_lowmip_texture(list, upload, data, &at, i);
			device->CreateShaderResourceView(textures[i], NULL, cpu);
			cpu.ptr += view_size;
			continue;
		}

		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = TEXTURE_SIZE;
		desc.Height = TEXTURE_SIZE;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&textures[i])));

		for (v = 0; v < TEXTURE_SIZE * TEXTURE_SIZE; v++)
			texels[v] = 0xff000000 | ((i * 37 + 64) & 0xff) | (((v / TEXTURE_SIZE) * 3 + i) & 0xff) << 8 | ((v % TEXTURE_SIZE) * 3 & 0xff) << 16;

		if (i == TEST_TEXTURE)
			memcpy(test_texels, texels, sizeof(test_texels));

		dst.pResource = textures[i];
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		src.pResource = upload;
		src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		src.PlacedFootprint.Offset = at;
		src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, TEXTURE_SIZE, TEXTURE_SIZE, 1, TEXTURE_SIZE * 4 };
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
		transition(list, textures[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		at += TEXTURE_SIZE * TEXTURE_SIZE * 4;

		device->CreateShaderResourceView(textures[i], NULL, cpu);
		cpu.ptr += view_size;
	}

	for (i = 0; i < DYNAMIC_UPLOADS; i++)
		dynamic_buffers[i] = create_buffer(VERTICES * 12, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

	CHECK(list->Close());
	queue->ExecuteCommandLists(1, (ID3D12CommandList**)&list);
	wait_for(signal());
	upload->Release();
}

static std::atomic<UINT64> ring_at;
static UINT64 ring_alloc(Frame *frame, UINT64 bytes, void **data)
{
	UINT64 was = ring_at.load(), at;

	// Several threads record at once with threads=1
	do {
		at = (was + 255) & ~255ull;
	} while (!ring_at.compare_exchange_weak(was, at + bytes));

	if (at + bytes > RING_BYTES) {
		printf("out of ring\n");
		ExitProcess(1);
	}
	*data = frame->ring_data + at;
	return at;
}

static void save_bmp(const char *path, const BYTE *pixels, UINT pitch)
{
	BITMAPFILEHEADER file = {};
	BITMAPINFOHEADER info = {};
	FILE *f;
	UINT x, y;

	if (fopen_s(&f, path, "wb"))
		return;
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
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			const BYTE *p = pixels + (UINT64)y * pitch + x * 4;
			BYTE bgra[4] = { p[2], p[1], p[0], 255 };
			fwrite(bgra, 4, 1, f);
		}
	}
	fclose(f);
}

// Copies the back buffer about to be presented, for comparing pictures
static void dump_back_buffer(const char *path, ID3D12Resource *back_buffer)
{
	D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	ID3D12CommandAllocator *allocator;
	ID3D12GraphicsCommandList *list;
	ID3D12Resource *readback;
	UINT64 bytes;
	BYTE *data;
	unsigned long long sum = 0;
	UINT x, y;

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
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH * 4; x++)
			sum = sum * 31 + data[(UINT64)y * footprint.Footprint.RowPitch + x];
	}
	printf("picture checksum %016llx\n", sum);
	if (test) {
		const UINT watched[] = { TEST_A, TEST_B, TEST_C, TEST_CONTROL, TEST_TEXTURE, TEST_TEXTURE_B, TEST_TEXTURE + 2 };
		const char *names[] = { "A (vb matches)", "B (vb written to match)", "C (vb written not to match)", "control",
			"texture matches", "texture written to match", "texture control" };

		for (UINT w = 0; w < ARRAYSIZE(watched); w++) {
			float ndc_x = -0.95f + (watched[w] % 25) * 0.078f, ndc_y = -0.9f + (watched[w] / 25) * 0.12f;
			UINT px = (UINT)((ndc_x * 0.5f + 0.5f) * WIDTH), py = (UINT)((1.0f - (ndc_y * 0.5f + 0.5f)) * HEIGHT);
			const BYTE *p = data + (UINT64)py * footprint.Footprint.RowPitch + px * 4;
			// The colour the frame is cleared to
			bool hidden = abs(p[0] - 26) <= 1 && abs(p[1] - 26) <= 1 && abs(p[2] - 38) <= 1;

			printf("  mesh %3u %-28s %s\n", watched[w], names[w], hidden ? "hidden" : "drawn");
		}
		for (UINT w = TEST_LOWMIP_FULL; lowmip_test && w <= TEST_LOWMIP_COPY; w++) {
			float ndc_x = -0.95f + (w % 25) * 0.078f, ndc_y = -0.9f + (w / 25) * 0.12f;
			UINT px = (UINT)((ndc_x * 0.5f + 0.5f) * WIDTH), py = (UINT)((1.0f - (ndc_y * 0.5f + 0.5f)) * HEIGHT);
			const BYTE *p = data + (UINT64)py * footprint.Footprint.RowPitch + px * 4;
			bool hidden = abs(p[0] - 26) <= 1 && abs(p[1] - 26) <= 1 && abs(p[2] - 38) <= 1;

			printf("  mesh %3u %-28s %s\n", w, w == TEST_LOWMIP_FULL ? "lowmip full" : "lowmip copy", hidden ? "hidden" : "drawn");
		}
	}
	save_bmp(path, data, footprint.Footprint.RowPitch);
	readback->Unmap(0, NULL);
	readback->Release();
	list->Release();
	allocator->Release();
}

#include "../common/profile.inc"

// appear=N: the time of the frames of each mod, the first three apart
struct Appearance { int group; double ms[4], record[4]; UINT64 vram; };
static IDXGIAdapter3 *adapter3;
static UINT64 vram_budget;
static std::vector<Appearance> appearances;

static void report_appearances()
{
	std::vector<double> steady, first, record_first;
	double sum[3] = {}, sum_record = 0;
	std::vector<std::pair<double, int>> worst;
	UINT rest = appear_frames - 3;

	for (const Appearance &a : appearances)
		steady.push_back(a.ms[3] / rest);
	std::vector<double> sorted = steady;
	std::sort(sorted.begin(), sorted.end());
	double base = sorted[sorted.size() / 2];
	for (const Appearance &a : appearances) {
		for (int k = 0; k < 3; k++)
			sum[k] += a.ms[k] - base;
		first.push_back(a.ms[0] - base);
		record_first.push_back(a.record[0] - a.record[3] / rest);
		sum_record += a.record[0] - a.record[3] / rest;
		worst.push_back({ a.ms[0] + a.ms[1] + a.ms[2] - 3 * base, a.group });
	}
	auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)((v.size() - 1) * p)]; };
	printf("appear: %zu mods, steady frame %.3f ms (median, whole frame with waits)\n", appearances.size(), base);
	printf("  first frame over steady: total %.1f ms, mean %.2f, p50 %.2f, p90 %.2f, max %.2f\n",
		sum[0], sum[0] / appearances.size(), pct(first, 0.5), pct(first, 0.9), pct(first, 1));
	printf("  of that recording+executing: total %.1f ms, max %.2f\n", sum_record, pct(record_first, 1));
	printf("  second frame over steady: total %.1f ms; third %.1f ms\n", sum[1], sum[2]);
	std::sort(worst.begin(), worst.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	for (size_t n = 0; n < worst.size() && n < 10; n++)
		printf("  %8.2f ms  mod %d\n", worst[n].first, worst[n].second);
	for (size_t k = 0; k < appearances.size(); k += max((size_t)1, appearances.size() / 8))
		printf("  mod %d: %.1f ms first frame, VRAM in use %llu MB of a %llu MB budget\n", appearances[k].group,
			appearances[k].ms[0] - base, appearances[k].vram >> 20, vram_budget >> 20);
	printf("APPEAR total %.1f ms over %zu mods (first three frames over steady)\n", sum[0] + sum[1] + sum[2], appearances.size());
}

int main(int argc, char **argv)
{
	UINT frame_count = 3000, mesh_count = 350, passes = 3, key = 0, key_frame = 0;
	bool load_dll = true, profile = false, profile_present = false, threaded = false;
	const char *dump = NULL;
	WNDCLASSW window_class = {};
	DXGI_SWAP_CHAIN_DESC1 swap_desc = {};
	D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	IDXGIFactory4 *factory;
	IDXGISwapChain1 *swap_chain1;
	LARGE_INTEGER freq, start, now, block_start;
	std::vector<double> blocks;
	LARGE_INTEGER t0, t1, t2;
	INT64 record_ticks = 0, present_ticks = 0;
	UINT timed_frames = 0;
	std::atomic<UINT> gpu_cursor{ 0 };
	HWND hwnd;
	MSG message;
	UINT i, f;

	for (int a = 1; a < argc; a++) {
		if (!strncmp(argv[a], "frames=", 7)) frame_count = atoi(argv[a] + 7);
		else if (!strncmp(argv[a], "meshes=", 7)) mesh_count = atoi(argv[a] + 7);
		else if (!strcmp(argv[a], "slot=vb0")) scene_on_vb0 = true;
		else if (!strncmp(argv[a], "scene=", 6)) {
			FILE *file = fopen(argv[a] + 6, "r");
			ScenePart part;
			crc_init();
			char line[256];
			if (!file) {
				printf("cannot read the scene %s\n", argv[a] + 6);
				return 1;
			}
			while (file && fgets(line, sizeof(line), file)) {
				part.group = 0;
				if (sscanf(line, "%x %u %u %d", &part.hash, &part.first, &part.count, &part.group) >= 3)
					scene_parts.push_back(part);
			}
			if (file)
				fclose(file);
			printf("scene: %u parts of mods\n", (UINT)scene_parts.size());
		}
		else if (!strncmp(argv[a], "passes=", 7)) passes = atoi(argv[a] + 7);
		else if (!strncmp(argv[a], "dump=", 5)) dump = argv[a] + 5;
		else if (!strncmp(argv[a], "key=", 4)) key = strtoul(argv[a] + 4, NULL, 0);
		else if (!strncmp(argv[a], "keyframe=", 9)) key_frame = atoi(argv[a] + 9);
		else if (!strncmp(argv[a], "uploads=", 8)) DYNAMIC_UPLOADS = min(MAX_DYNAMIC_UPLOADS, (UINT)atoi(argv[a] + 8));
		else if (!strncmp(argv[a], "churn=", 6)) churn = atoi(argv[a] + 6);
		else if (!strcmp(argv[a], "nodll")) load_dll = false;
		else if (!strcmp(argv[a], "video")) video = true;
		else if (!strcmp(argv[a], "dxil")) use_dxil = true;
		else if (!strcmp(argv[a], "rootconst")) root_constants = true;
		else if (!strncmp(argv[a], "table=", 6)) TABLE = atoi(argv[a] + 6);
		else if (!strncmp(argv[a], "heap=", 5)) GPU_HEAP_DESCRIPTORS = atoi(argv[a] + 5);
		else if (!strcmp(argv[a], "profile")) profile = true;
		else if (!strcmp(argv[a], "threads=1")) threaded = true;
		else if (!strcmp(argv[a], "profile=present")) profile = profile_present = true;
		else if (!strcmp(argv[a], "test")) test = true;
		else if (!strcmp(argv[a], "evict")) evict_test = true;
		else if (!strcmp(argv[a], "move")) move_test = true;
		else if (!strcmp(argv[a], "lowmip")) lowmip_test = true;
		else if (!strncmp(argv[a], "evictstop=", 10)) evict_stop = atoi(argv[a] + 10);
		else if (!strncmp(argv[a], "appear=", 7)) appear_frames = atoi(argv[a] + 7);
		else if (!strncmp(argv[a], "rereach=", 8)) rereach = atoi(argv[a] + 8);
		else if (!strcmp(argv[a], "contend")) contend = true;
		else {
			printf("unknown argument %s\n", argv[a]);
			return 1;
		}
	}

	if (load_dll) {
		wchar_t path[MAX_PATH];

		GetModuleFileNameW(NULL, path, MAX_PATH);
		wcscpy_s(wcsrchr(path, L'\\') + 1, 16, L"d3d11.dll");
		if (!LoadLibraryW(path)) {
			printf("could not load %ls: %lu\n", path, GetLastError());
			return 1;
		}
	}

	window_class.lpfnWndProc = window_proc;
	window_class.hInstance = GetModuleHandleW(NULL);
	window_class.lpszClassName = L"Dx12Harness";
	RegisterClassW(&window_class);
	RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	hwnd = CreateWindowW(L"Dx12Harness", L"Dx12Harness", WS_OVERLAPPEDWINDOW, 50, 50, rect.right - rect.left, rect.bottom - rect.top,
			NULL, NULL, window_class.hInstance, NULL);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);

	CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
	CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
	{
		LUID luid = device->GetAdapterLuid();
		IDXGIAdapter1 *a1 = NULL;
		for (UINT k = 0; factory->EnumAdapters1(k, &a1) != DXGI_ERROR_NOT_FOUND; k++) {
			DXGI_ADAPTER_DESC1 d;
			a1->GetDesc1(&d);
			if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart)
				a1->QueryInterface(IID_PPV_ARGS(&adapter3));
			a1->Release();
		}
	}
	D3D12_COMMAND_QUEUE_DESC queue_desc = {};
	queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));

	swap_desc.Width = WIDTH;
	swap_desc.Height = HEIGHT;
	swap_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swap_desc.SampleDesc.Count = 1;
	swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swap_desc.BufferCount = BACK_BUFFERS;
	swap_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swap_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &swap_desc, NULL, NULL, &swap_chain1));
	CHECK(swap_chain1->QueryInterface(IID_PPV_ARGS(&swap_chain)));
	factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

	rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	view_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	heap_desc.NumDescriptors = BACK_BUFFERS;
	CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
	heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heap_desc.NumDescriptors = TEXTURES;
	CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&cpu_heap)));
	heap_desc.NumDescriptors = GPU_HEAP_DESCRIPTORS;
	heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&gpu_heap)));

	rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	for (i = 0; i < BACK_BUFFERS; i++) {
		CHECK(swap_chain->GetBuffer(i, IID_PPV_ARGS(&back_buffers[i])));
		device->CreateRenderTargetView(back_buffers[i], NULL, { rtv.ptr + i * rtv_size });
	}

	CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
	for (i = 0; i < BACK_BUFFERS; i++) {
		CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames[i].allocators[0])));
		CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames[i].allocators[1])));
		frames[i].ring = create_buffer(RING_BYTES, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
		CHECK(frames[i].ring->Map(0, NULL, (void**)&frames[i].ring_data));
		if (video) {
			video_uploads[i] = create_buffer((UINT64)VIDEO_WIDTH * VIDEO_HEIGHT * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
			CHECK(video_uploads[i]->Map(0, NULL, (void**)&video_data[i]));
			memset(video_data[i], 0x40 + i, (size_t)VIDEO_WIDTH * VIDEO_HEIGHT * 4);
		}
	}
	if (video) {
		D3D12_HEAP_PROPERTIES heap = {};
		D3D12_RESOURCE_DESC desc = {};

		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = VIDEO_WIDTH;
		desc.Height = VIDEO_HEIGHT;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, IID_PPV_ARGS(&video_texture)));
	}
	for (i = 0; i < 2; i++) {
		CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocators[i], NULL, IID_PPV_ARGS(&lists[i])));
		CHECK(lists[i]->Close());
	}

	create_pipelines();
	create_scene(mesh_count);
	if (appear_frames) {
		int groups = 0;
		for (UINT k = 0; k < scene_parts.size() && k < mesh_count; k++)
			groups = max(groups, scene_parts[k].group + 1);
		if (appear_frames < 4)
			appear_frames = 12;
		// CYCLES=N shows the groups N times over, so that each comes back after the others
		appear_cycles = getenv("CYCLES") ? max(1, atoi(getenv("CYCLES"))) : 1;
		appear_groups = max(1, groups);
		frame_count = 300 + groups * appear_frames * appear_cycles;
	}

	if (contend) {
		std::thread([] {
			while (!contend_stop) {
				ID3D12Resource *big = create_buffer(64 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
				ID3D12Resource *staging = create_buffer(16 << 20, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				staging->Release();
				big->Release();
				contend_rounds++;
			}
		}).detach();
	}

	if (profile) {
		samples.reserve(1 << 20);
		start_profile();
	}

	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	block_start = start;

	LARGE_INTEGER previous_end;
	QueryPerformanceCounter(&previous_end);
	for (f = 0; f < frame_count; f++) {
		int in_window = appear_frames && f >= 300 ? (int)((f - 300) % appear_frames) : -1;
		visible_group = appear_frames && f >= 300 ? (int)((f - 300) / appear_frames) % appear_groups : -1;
		if (appear_frames && profile)
			profiling = getenv("STEADY") ? in_window == 5 || in_window == 6 : in_window == 0 || in_window == 1;
		UINT index = swap_chain->GetCurrentBackBufferIndex();
		Frame *frame = &frames[index];
		D3D12_VIEWPORT viewport = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 };
		D3D12_RECT scissor = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
		const float clear[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
		D3D12_CPU_DESCRIPTOR_HANDLE cpu_start = cpu_heap->GetCPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu_start = gpu_heap->GetCPUDescriptorHandleForHeapStart();
		D3D12_GPU_DESCRIPTOR_HANDLE gpu_start = gpu_heap->GetGPUDescriptorHandleForHeapStart();
		D3D12_CPU_DESCRIPTOR_HANDLE target = { rtv.ptr + index * rtv_size };
		UINT pass;

		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
		if (key && f == key_frame) {
			keybd_event((BYTE)key, 0, 0, 0);
			keybd_event((BYTE)key, 0, KEYEVENTF_KEYUP, 0);
		}

		wait_for(frame->fence);
		if (evict_test && (int)f == evict_stop)
			evict_test = false;
		if (evict_test) {
			if (evicted_buffers.empty()) {
				for (Mesh &mesh : meshes) {
					for (ID3D12Resource *vb : mesh.vb) {
						if (vb && std::find(evicted_buffers.begin(), evicted_buffers.end(), vb) == evicted_buffers.end())
							evicted_buffers.push_back(vb);
					}
					if (mesh.ib && std::find(evicted_buffers.begin(), evicted_buffers.end(), mesh.ib) == evicted_buffers.end())
						evicted_buffers.push_back(mesh.ib);
				}
				ID3D12CommandAllocator *empty_allocator;
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&empty_allocator)));
				CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, empty_allocator, NULL, IID_PPV_ARGS(&empty_list)));
				CHECK(empty_list->Close());
			}
			wait_for(signal());
			CHECK(device->Evict((UINT)evicted_buffers.size(), evicted_buffers.data()));
		}
		QueryPerformanceCounter(&t0);
		// Not from the same place every time: what an engine hands out
		// of such a buffer depends on what came before in the frame
		ring_at = test ? 0 : (UINT64)(rng() % 4096) * 256;
		CHECK(frame->allocators[0]->Reset());
		CHECK(frame->allocators[1]->Reset());

		for (i = 0; i < 2; i++) {
			ID3D12GraphicsCommandList *list = lists[i];

			CHECK(list->Reset(frame->allocators[i], NULL));
			list->SetDescriptorHeaps(1, &gpu_heap);
			list->SetGraphicsRootSignature(root_signature);
			if (root_constants) {
				static const float one[4] = { 1, 1, 1, 1 };
				list->SetGraphicsRoot32BitConstants(2, 4, one, 0);
			}
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		}

		// Buffers uploaded anew every frame, as for what an engine
		// animates on the CPU
		for (i = 0; i < DYNAMIC_UPLOADS; i++) {
			void *data;
			UINT64 at = ring_alloc(frame, VERTICES * 12, &data);
			float *positions = (float*)data;
			UINT v;

			for (v = 0; v < VERTICES; v++) {
				positions[v * 3 + 0] = ((v & 1) ? 0.3f : -0.3f) + ((f + i) % 16) * 0.01f;
				positions[v * 3 + 1] = ((v & 2) ? 0.3f : -0.3f);
				positions[v * 3 + 2] = 0.5f;
			}
			transition(lists[0], dynamic_buffers[i], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(dynamic_buffers[i], 0, frame->ring, at, VERTICES * 12);
			transition(lists[0], dynamic_buffers[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		}

		if (video) {
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};

			*(UINT*)video_data[index] = f;
			dst.pResource = video_texture;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.pResource = video_uploads[index];
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, VIDEO_WIDTH, VIDEO_HEIGHT, 1, VIDEO_WIDTH * 4 };
			transition(lists[0], video_texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
			transition(lists[0], video_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}

		// What the mods matched or did not is written again with other
		// contents, which they then have to go by
		if (test && f == 400) {
			D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
			void *data;
			UINT64 at;

			at = ring_alloc(frame, sizeof(test_vb), &data);
			memcpy(data, test_vb, sizeof(test_vb));
			transition(lists[0], meshes[TEST_B].vb[0], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(meshes[TEST_B].vb[0], 0, frame->ring, at, sizeof(test_vb));
			transition(lists[0], meshes[TEST_B].vb[0], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

			at = ring_alloc(frame, sizeof(test_other_vb), &data);
			memcpy(data, test_other_vb, sizeof(test_other_vb));
			transition(lists[0], meshes[TEST_C].vb[0], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(meshes[TEST_C].vb[0], 0, frame->ring, at, sizeof(test_other_vb));
			transition(lists[0], meshes[TEST_C].vb[0], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

			// Texture data has to start at a multiple of 512 bytes
			ring_at = (ring_at.load() + 511) & ~511ull;
			at = ring_alloc(frame, sizeof(test_texels), &data);
			memcpy(data, test_texels, sizeof(test_texels));
			dst.pResource = textures[TEST_TEXTURE_B];
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.pResource = frame->ring;
			src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			src.PlacedFootprint.Offset = at;
			src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, TEXTURE_SIZE, TEXTURE_SIZE, 1, TEXTURE_SIZE * 4 };
			transition(lists[0], textures[TEST_TEXTURE_B], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
			transition(lists[0], textures[TEST_TEXTURE_B], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		}

		if (test && move_test && (f == 900 || f == 1000 || f == 1100)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb);
			void *data;
			UINT64 at;

			if (f == 900) {
				transition(lists[1], move_pools[1], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				transition(lists[1], move_pools[0], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[1]->CopyBufferRegion(move_pools[0], 196608, move_pools[1], 256, bytes);
				transition(lists[1], move_pools[1], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(lists[1], move_pools[0], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			}

			at = ring_alloc(frame, bytes, &data);
			memcpy(data, f == 1000 ? test_vb : test_other_vb, bytes);
			transition(lists[0], move_pools[1], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(move_pools[1], 256, frame->ring, at, bytes);
			transition(lists[0], move_pools[1], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);

			if (f == 1000) {
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&move_allocator)));
				CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, move_allocator, NULL, IID_PPV_ARGS(&move_list)));
				transition(move_list, move_pools[1], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				transition(move_list, move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				move_list->CopyBufferRegion(move_pools[2], 4096, move_pools[1], 256, bytes);
				transition(move_list, move_pools[1], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(move_list, move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				CHECK(move_list->Close());
			}
		}
		if (test && move_test && (f == 1200 || f == 1300)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb), to = f == 1200 ? 8192 : 12288;
			ID3D12Resource *a = meshes[TEST_A].vb[0];

			transition(lists[1], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
			if (f == 1200) {
				ID3D12Resource *control = meshes[TEST_CONTROL].vb[0];
				transition(lists[1], control, vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				lists[1]->CopyBufferRegion(move_pools[2], to, control, 0, bytes / 2);
				transition(lists[1], control, D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
			} else {
				void *data;
				UINT64 at = ring_alloc(frame, bytes, &data);
				memcpy(data, test_other_vb, bytes);
				lists[1]->CopyBufferRegion(move_pools[2], to, frame->ring, at, bytes);
			}
			transition(lists[1], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);

			transition(lists[0], a, vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(move_pools[2], to, a, 0, bytes);
			transition(lists[0], a, D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
			transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
		}
		if (test && move_test && (f == 1400 || f == 1450)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb);

			if (f == 1400) {
				void *data;
				UINT64 at = ring_alloc(frame, bytes, &data);
				memcpy(data, test_other_vb, bytes);
				transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[0]->CopyBufferRegion(move_pools[2], 16384, frame->ring, at, bytes);
				transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			} else {
				void *data;
				late_upload = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				CHECK(late_upload->Map(0, NULL, &data));
				memcpy(data, test_vb, bytes);
				late_upload->Unmap(0, NULL);
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&late_allocator)));
				CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, late_allocator, NULL, IID_PPV_ARGS(&late_list)));
				transition(late_list, move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				late_list->CopyBufferRegion(move_pools[2], 16384, late_upload, 0, bytes);
				transition(late_list, move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				CHECK(late_list->Close());

				transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				transition(lists[0], move_pools[0], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[0]->CopyBufferRegion(move_pools[0], 262144, move_pools[2], 16384, bytes);
				transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(lists[0], move_pools[0], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			}
		}
		if (test && move_test && (f == 1600 || f == 1650)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb);
			void *data;

			if (f == 1600) {
				again_upload = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				CHECK(again_upload->Map(0, NULL, &data));
				memcpy(data, test_vb, bytes);
				again_upload->Unmap(0, NULL);
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&again_allocator)));
				CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, again_allocator, NULL, IID_PPV_ARGS(&again_list)));
				transition(again_list, move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				again_list->CopyBufferRegion(move_pools[2], 20480, again_upload, 0, bytes);
				transition(again_list, move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				CHECK(again_list->Close());
			} else {
				CHECK(again_upload->Map(0, NULL, &data));
				memcpy(data, test_other_vb, bytes);
				again_upload->Unmap(0, NULL);
				UINT64 at = ring_alloc(frame, bytes, &data);
				memcpy(data, test_vb, bytes);
				transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[0]->CopyBufferRegion(move_pools[2], 20480, frame->ring, at, bytes);
				transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			}
		}
		if (test && move_test && (f == 1690 || f == 1700 || f == 1800)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb), from = f == 1800 ? 28672 : 24576;
			void *data;
			UINT64 at = ring_alloc(frame, bytes, &data);

			memcpy(data, f == 1690 ? test_other_vb : test_vb, bytes);
			transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
			lists[0]->CopyBufferRegion(move_pools[2], from, frame->ring, at, bytes);
			transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);

			if (f != 1690) {
				ID3D12CommandAllocator *&allocator = queue2_allocators[f == 1800];
				if (!queue2) {
					D3D12_COMMAND_QUEUE_DESC desc = {};
					desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
					CHECK(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue2)));
					CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue2_fence)));
				}
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
				if (queue2_list)
					CHECK(queue2_list->Reset(allocator, NULL));
				else
					CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&queue2_list)));
				transition(queue2_list, move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				transition(queue2_list, move_pools[0], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				queue2_list->CopyBufferRegion(move_pools[0], f == 1800 ? 393216 : 327680, move_pools[2], from, bytes);
				transition(queue2_list, move_pools[2], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(queue2_list, move_pools[0], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				CHECK(queue2_list->Close());
			}
		}
		if (test && move_test && (f == 1900 || f == 1950)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb), to = f == 1900 ? 32768 : 36864;
			const UINT r = f == 1950;
			void *data;
			UINT64 at = ring_alloc(frame, bytes, &data);

			memcpy(data, r ? test_vb : test_other_vb, bytes);
			race_uploads[r] = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
			CHECK(race_uploads[r]->Map(0, NULL, &data));
			memcpy(data, r ? test_other_vb : test_vb, bytes);
			race_uploads[r]->Unmap(0, NULL);
			CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&race_allocators[r])));
			CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, race_allocators[r], NULL, IID_PPV_ARGS(&race_lists[r])));

			// In the order of the comment above
			auto first = [&] {
				transition(lists[0], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[0]->CopyBufferRegion(move_pools[2], to, frame->ring, at, bytes);
				transition(lists[0], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			};
			if (r)
				first();
			transition(race_lists[r], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
			race_lists[r]->CopyBufferRegion(move_pools[2], to, race_uploads[r], 0, bytes);
			transition(race_lists[r], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
			CHECK(race_lists[r]->Close());
			if (!r) {
				first();
				queue->ExecuteCommandLists(1, (ID3D12CommandList**)&race_lists[r]);
			}
			meshes[TEST_A].views[0].BufferLocation = move_pools[2]->GetGPUVirtualAddress() + to;
		}
		if (test && move_test && f == 1980) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb), to = 40960;
			void *data;

			for (UINT r = 2; r < 4; r++) {
				race_uploads[r] = create_buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
				CHECK(race_uploads[r]->Map(0, NULL, &data));
				memcpy(data, r == 2 ? test_vb : test_other_vb, bytes);
				race_uploads[r]->Unmap(0, NULL);
				CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&race_allocators[r])));
				CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, race_allocators[r], NULL,
					IID_PPV_ARGS(&race_lists[r])));
				transition(race_lists[r], move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				race_lists[r]->CopyBufferRegion(move_pools[2], to, race_uploads[r], 0, bytes);
				transition(race_lists[r], move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				CHECK(race_lists[r]->Close());
			}
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&race_lists[2]);
			meshes[TEST_A].views[0].BufferLocation = move_pools[2]->GetGPUVirtualAddress() + to;
		}
		if (test && move_test && f == 1601)
			meshes[TEST_A].views[0].BufferLocation = move_pools[2]->GetGPUVirtualAddress() + 20480;
		if (test && move_test && (f == 1701 || f == 1801))
			meshes[TEST_A].views[0].BufferLocation = move_pools[0]->GetGPUVirtualAddress() + (f == 1701 ? 327680 : 393216);
		if (test && move_test && f == 1451)
			meshes[TEST_A].views[0].BufferLocation = move_pools[0]->GetGPUVirtualAddress() + 262144;
		// Drawn from where those copies went, from the next frame on
		if (test && move_test && (f == 1201 || f == 1301))
			meshes[TEST_A].views[0].BufferLocation = move_pools[2]->GetGPUVirtualAddress() + (f == 1201 ? 8192 : 12288);
		if (test && move_test && f == 901)
			meshes[TEST_A].views[0].BufferLocation = move_pools[0]->GetGPUVirtualAddress() + 196608;
		if (test && move_test && f == 1001)
			meshes[TEST_A].views[0].BufferLocation = move_pools[2]->GetGPUVirtualAddress() + 4096;

		if (test && move_test && (f == 500 || f == 600 || f == 700 || f == 800)) {
			const D3D12_RESOURCE_STATES vb_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
			const UINT64 bytes = sizeof(test_vb);
			ID3D12GraphicsCommandList *list = lists[0];
			D3D12_VERTEX_BUFFER_VIEW *view = &meshes[TEST_A].views[0];

			if (!move_pools[0]) {
				for (ID3D12Resource *&pool : move_pools)
					pool = create_buffer(1 << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
			}
			if (f == 500) {
				ID3D12Resource *from = meshes[TEST_A].vb[0];
				transition(list, from, vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				list->CopyBufferRegion(move_pools[0], 65792, from, 0, bytes);
				transition(list, from, D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(list, move_pools[0], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				view->BufferLocation = move_pools[0]->GetGPUVirtualAddress() + 65792;
			} else if (f == 600) {
				transition(list, move_pools[0], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				list->CopyBufferRegion(move_pools[1], 256, move_pools[0], 65792, bytes);
				transition(list, move_pools[0], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(list, move_pools[1], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				view->BufferLocation = move_pools[1]->GetGPUVirtualAddress() + 256;
			} else if (f == 700) {
				transition(list, move_pools[1], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				list->CopyResource(move_pools[2], move_pools[1]);
				transition(list, move_pools[1], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(list, move_pools[2], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				view->BufferLocation = move_pools[2]->GetGPUVirtualAddress() + 256;
			} else {
				transition(list, move_pools[2], vb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
				transition(list, move_pools[0], vb_state, D3D12_RESOURCE_STATE_COPY_DEST);
				list->CopyBufferRegion(move_pools[0], 131072, move_pools[2], 256, bytes / 2);
				list->CopyBufferRegion(move_pools[0], 131072 + bytes / 2, move_pools[2], 256 + bytes / 2, bytes / 2);
				transition(list, move_pools[2], D3D12_RESOURCE_STATE_COPY_SOURCE, vb_state);
				transition(list, move_pools[0], D3D12_RESOURCE_STATE_COPY_DEST, vb_state);
				view->BufferLocation = move_pools[0]->GetGPUVirtualAddress() + 131072;
			}
		}

		if (rereach && f >= 300 && f % 8 == 0) {
			UINT plain = (UINT)scene_parts.size();
			for (i = 0; i < rereach && plain < mesh_count; i++) {
				// Not those whose vb0 is a dynamic or ring one
				UINT m = plain + (f / 8 * rereach + i) % (mesh_count - plain);
				if (m % 17 == 3 || m % 12 == 5)
					continue;
				Mesh *mesh = &meshes[m];
				UINT16 *indices;
				UINT64 at = ring_alloc(frame, INDICES * 2, (void**)&indices);
				UINT j, v;

				for (j = 0; j < INDICES / 6; j++) {
					const UINT16 quad[6] = { 0, 1, 2, 2, 1, 3 };
					for (v = 0; v < 6; v++)
						indices[j * 6 + v] = (UINT16)(j * 4 + quad[v]);
				}
				if (f / 8 / ((mesh_count - plain + rereach - 1) / rereach) & 1)
					std::swap(indices[0], indices[1]);
				transition(lists[0], mesh->ib, D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
				lists[0]->CopyBufferRegion(mesh->ib, 0, frame->ring, at, INDICES * 2);
				transition(lists[0], mesh->ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			}
		}

		// And buffers that come and go
		for (i = 0; i < churn; i++) {
			static ID3D12Resource *transient[8][BACK_BUFFERS];
			ID3D12Resource **slot = &transient[i % 8][index];

			if (*slot)
				(*slot)->Release();
			*slot = create_buffer(4096 + ((f + i) % 4) * 4096, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
		}

		transition(lists[0], back_buffers[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		lists[0]->ClearRenderTargetView(target, clear, 0, NULL);

		auto record_pass = [&](UINT pass) {
			ID3D12GraphicsCommandList *list = lists[pass ? 1 : 0];
			UINT m;

			list->OMSetRenderTargets(1, &target, FALSE, NULL);
			for (m = 0; m < mesh_count; m++) {
				Mesh *mesh = &meshes[m];
				if (appear_frames && m < scene_parts.size() && scene_parts[m].group != visible_group)
					continue;
				D3D12_VERTEX_BUFFER_VIEW views[3] = { mesh->views[0], mesh->views[1], mesh->views[2] };
				D3D12_CPU_DESCRIPTOR_HANDLE dest;
				D3D12_GPU_DESCRIPTOR_HANDLE table;
				void *data;
				float *constants;
				UINT64 at;
				UINT t;

				// A material per mesh: most draws come with a
				// pipeline state other than the one before
				if (m % 4 != 3)
					list->SetPipelineState(pipelines[(m * 5 + pass) % PIPELINES]);

				// Positions a buffer uploaded this frame, or handed
				// out from the upload buffer of the frame. Not a scene
				// part's with slot=vb0: its positions carry the hash.
				bool hashed_vb0 = scene_on_vb0 && m < scene_parts.size();
				if (m % 17 == 3 && DYNAMIC_UPLOADS && !hashed_vb0) {
					views[0].BufferLocation = dynamic_buffers[(m / 17) % DYNAMIC_UPLOADS]->GetGPUVirtualAddress();
				} else if (m % 12 == 5 && m / 12 < RING_VBS && !hashed_vb0) {
					at = ring_alloc(frame, VERTICES * 12, &data);
					float *positions = (float*)data;
					UINT v;

					for (v = 0; v < VERTICES; v++) {
						positions[v * 3 + 0] = ((v & 1) ? 0.2f : -0.2f);
						positions[v * 3 + 1] = ((v & 2) ? 0.2f : -0.2f) + (f % 8) * 0.01f;
						positions[v * 3 + 2] = 0.5f;
					}
					views[0].BufferLocation = frame->ring->GetGPUVirtualAddress() + at;
				}

				at = ring_alloc(frame, 256, &data);
				constants = (float*)data;
				constants[0] = -0.95f + (m % 25) * 0.078f;
				constants[1] = -0.9f + (m / 25) * 0.12f + pass * 0.02f;
				constants[2] = 0.035f;
				constants[3] = 0.05f;
				constants[4] = constants[5] = constants[6] = constants[7] = 1.0f;

				// The views of the material, copied to where the
				// shaders will find them
				UINT cursor = (gpu_cursor.fetch_add(1) % (GPU_HEAP_DESCRIPTORS / TABLE)) * TABLE;
				dest.ptr = gpu_cpu_start.ptr + (SIZE_T)cursor * view_size;
				table.ptr = gpu_start.ptr + (UINT64)cursor * view_size;
				device->CopyDescriptorsSimple(2, dest, { cpu_start.ptr + (SIZE_T)(mesh->texture % (TEXTURES - 1)) * view_size },
					D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				for (t = 2; t < TABLE; t++) {
					device->CopyDescriptorsSimple(1, { dest.ptr + (SIZE_T)t * view_size },
						{ cpu_start.ptr + (SIZE_T)((mesh->texture + t) % TEXTURES) * view_size },
						D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				}

				list->IASetVertexBuffers(0, 3, views);
				list->IASetIndexBuffer(&mesh->index_view);
				list->SetGraphicsRootConstantBufferView(0, frame->ring->GetGPUVirtualAddress() + at);
				list->SetGraphicsRootDescriptorTable(1, table);
				list->DrawIndexedInstanced(mesh->count, 1, mesh->first, 0, 0);
			}
		};

		if (threaded && passes > 1) {
			// The first pass here and the others on a thread of their own, each to its list
			std::thread other([&] {
				for (UINT later = 1; later < passes; later++)
					record_pass(later);
			});
			record_pass(0);
			other.join();
		} else {
			for (pass = 0; pass < passes; pass++)
				record_pass(pass);
		}

		transition(lists[1], back_buffers[index], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		CHECK(lists[0]->Close());
		CHECK(lists[1]->Close());
		if (evict_test) {
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&empty_list);
			CHECK(device->MakeResident((UINT)evicted_buffers.size(), evicted_buffers.data()));
		}
		if (move_test && f == 1700)
			CHECK(queue->Wait(queue2_fence, 1));
		queue->ExecuteCommandLists(1, (ID3D12CommandList**)&lists[0]);
		if (move_test && f == 1800) {
			CHECK(queue->Signal(queue2_fence, 2));
			CHECK(queue2->Wait(queue2_fence, 2));
			queue2->ExecuteCommandLists(1, (ID3D12CommandList**)&queue2_list);
			CHECK(queue2->Signal(queue2_fence, 3));
			CHECK(queue->Wait(queue2_fence, 3));
		}
		queue->ExecuteCommandLists(1, (ID3D12CommandList**)&lists[1]);
		if (move_test && f == 1700) {
			queue2->ExecuteCommandLists(1, (ID3D12CommandList**)&queue2_list);
			CHECK(queue2->Signal(queue2_fence, 1));
		}
		if (again_list && (f == 1600 || f == 1650))
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&again_list);
		if (move_list && (f == 1000 || f == 1100))
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&move_list);
		if (late_list && f == 1460)
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&late_list);
		if (move_test && f == 1950)
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&race_lists[1]);
		if (move_test && f == 1980)
			queue->ExecuteCommandLists(1, (ID3D12CommandList**)&race_lists[3]);

		if (dump && f == frame_count - 1) {
			wait_for(signal());
			dump_back_buffer(dump, back_buffers[index]);
		}

		QueryPerformanceCounter(&t1);
		if (profile_present && f >= 300)
			profiling = true;
		CHECK(swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING));
		frame->fence = signal();
		if (profile_present)
			profiling = false;
		QueryPerformanceCounter(&t2);
		if (in_window >= 0) {
			if (in_window == 0) {
				appearances.push_back({ visible_group });
				DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
				if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
					appearances.back().vram = info.CurrentUsage;
					vram_budget = info.Budget;
				}
			}
			int slot = min(in_window, 3);
			appearances.back().ms[slot] += (t2.QuadPart - previous_end.QuadPart) * 1000.0 / freq.QuadPart;
			appearances.back().record[slot] += (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
		}
		previous_end = t2;
		if (f >= 300) {
			record_ticks += t1.QuadPart - t0.QuadPart;
			present_ticks += t2.QuadPart - t1.QuadPart;
			timed_frames++;
		}

		if (f >= 300 && (f - 300) % 500 == 499) {
			QueryPerformanceCounter(&now);
			blocks.push_back((now.QuadPart - block_start.QuadPart) * 1000.0 / freq.QuadPart / 500);
			block_start = now;
		} else if (f == 299) {
			QueryPerformanceCounter(&block_start);
			profiling = profile && !profile_present && !appear_frames;
		}
	}

	profiling = false;
	stop_profile();
	wait_for(signal());
	if (profile) {
		report_profile();
	}

	if (appear_frames && !appearances.empty())
		report_appearances();
	else if (!blocks.empty()) {
		std::vector<double> sorted = blocks;
		double sum = 0;

		std::sort(sorted.begin(), sorted.end());
		for (double block : blocks)
			sum += block;
		printf("%u draws a frame, %s: median %.3f ms, best %.3f ms, mean %.3f ms a frame over %zu blocks of 500\n",
			mesh_count * passes, load_dll ? "with d3d11.dll" : "without", sorted[sorted.size() / 2], sorted[0], sum / blocks.size(), blocks.size());
		printf("  of that recording and executing %.3f ms, presenting %.3f ms\n", record_ticks * 1000.0 / freq.QuadPart / timed_frames,
			present_ticks * 1000.0 / freq.QuadPart / timed_frames);
	}
	if (contend)
		printf("  contend: %u rounds of making and releasing 80 MB\n", contend_rounds.load());
	if (scene_failures)
		printf("FAILED to give %u parts of the scene their hashes\n", scene_failures);
	fflush(stdout);
	// The shim has threads of its own that are not worth shutting down
	ExitProcess(scene_failures ? 1 : 0);
}
