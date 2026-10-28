// A Direct3D 11 "game" for the DLL to sit in: it draws scenery nothing is
// for, and the meshes, buffers and textures the TextureOverride sections of
// the real mods of an importer look for (scene.py lists them, the data is
// given the hashes here), so the per-draw work of the DLL is that of the
// real configuration. Nothing of a game's pictures is needed for that: every
// triangle is degenerate.
//
//   harness.exe scene=<file> [frames=N] [warmup=N] [chars=N] [first=N] [rotate=N]
//               [scenery=N] [passes=N] [materials=N] [ui=N]
//               [check] [text=A-B] [keys=N] [nodll] [profile] [steady] [appear] [verbose]
//
// check: a clock that advances a 60th of a second a frame, no real input, and
// the hashes of what reached Direct3D (trace.h), a line a frame in frames.txt.
// Without it: the time a frame takes on this thread.
// appear: every object of the scene comes on screen `chars` at a time for
// `rotate` frames (at least 4); prints what its first frames cost over the rest.
// Exits with 1 when Direct3D fails, the DLL cannot be hooked or a buffer cannot
// be given its hash, since the run then does not do what the mods expect.
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dbghelp.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include "../../common/crc.h"
#include "../../common/window.h"
#include "trace.h"
#include "../../common/profile.inc"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IDXGISwapChain *swap;
static HWND window;

#define CHECK(hr, what) do { HRESULT hr_ = (hr); if (FAILED(hr_)) { printf("FAILED %s: %08x\n", what, (unsigned)hr_); exit(1); } } while (0)

// ------------------------------------------------- a clock and keys of ours

static uint64_t virtual_us = 1000ull * 1000000;
static LARGE_INTEGER qpf;
static bool key_down[256];

static BOOL WINAPI my_QueryPerformanceCounter(LARGE_INTEGER *out)
{
	out->QuadPart = (LONGLONG)(virtual_us * (uint64_t)qpf.QuadPart / 1000000);
	return TRUE;
}
static DWORD WINAPI my_GetTickCount() { return (DWORD)(virtual_us / 1000); }
static ULONGLONG WINAPI my_GetTickCount64() { return virtual_us / 1000; }
static SHORT WINAPI my_GetAsyncKeyState(int vk) { return key_down[vk & 0xff] ? (SHORT)0x8000 : 0; }
static HWND WINAPI my_GetForegroundWindow() { return window; }
static BOOL WINAPI my_GetCursorPos(POINT *p) { p->x = 200; p->y = 150; return TRUE; }
static BOOL WINAPI my_GetCursorInfo(CURSORINFO *info)
{
	info->flags = CURSOR_SHOWING;
	info->hCursor = NULL;
	info->ptScreenPos.x = 200;
	info->ptScreenPos.y = 150;
	return TRUE;
}

// What the DLL asks Direct3D for its device with: in a check, the device and
// context it gets are the listening ones of trace.cpp.
typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
	const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
	ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static CreateFn real_create_and_swap;
static CreateDeviceFn real_create_device;

static HRESULT WINAPI traced_create_and_swap(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk,
	const DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain, ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
	HRESULT hr = real_create_and_swap(adapter, type, software, flags, levels, count, sdk, desc, chain, device, level, context);
	if (SUCCEEDED(hr))
		trace_wrap((void**)device, (void**)context);
	return hr;
}
static HRESULT WINAPI traced_create_device(IDXGIAdapter *adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags, const D3D_FEATURE_LEVEL *levels, UINT count, UINT sdk,
	ID3D11Device **device, D3D_FEATURE_LEVEL *level, ID3D11DeviceContext **context)
{
	HRESULT hr = real_create_device(adapter, type, software, flags, levels, count, sdk, device, level, context);
	if (SUCCEEDED(hr))
		trace_wrap((void**)device, (void**)context);
	return hr;
}
// The DLL takes the creation functions of the real d3d11.dll from that
// module's export table (and hooks what it finds there), so for a check the
// table is pointed at a jump to the functions above: whatever way the DLL
// reaches Direct3D, it gets the listening device.
static DWORD *redirect_export(HMODULE module, const char *name, void *target, void **real)
{
	uint8_t *base = (uint8_t*)module;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_EXPORT_DIRECTORY *exports = (IMAGE_EXPORT_DIRECTORY*)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
	DWORD *names = (DWORD*)(base + exports->AddressOfNames), *functions = (DWORD*)(base + exports->AddressOfFunctions);
	WORD *ordinals = (WORD*)(base + exports->AddressOfNameOrdinals);

	for (DWORD i = 0; i < exports->NumberOfNames; i++) {
		if (strcmp((const char*)base + names[i], name))
			continue;
		DWORD *slot = &functions[ordinals[i]];
		*real = base + *slot;
		// The table holds offsets of 32 bits from the module, so the jump
		// has to be near it
		uint8_t *stub = NULL;
		for (uint8_t *at = base + nt->OptionalHeader.SizeOfImage + 0x10000; !stub && at < base + 0x70000000; at += 0x10000)
			stub = (uint8_t*)VirtualAlloc((void*)((ULONG_PTR)at & ~0xffffull), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!stub) {
			printf("no room near d3d11.dll for a jump\n");
			exit(1);
		}
		// mov rax, target; jmp rax
		uint8_t code[16] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0, 0xCC, 0xCC, 0xCC, 0xCC };
		memcpy(code + 2, &target, 8);
		memcpy(stub, code, sizeof(code));
		DWORD old;
		VirtualProtect(slot, 4, PAGE_READWRITE, &old);
		*slot = (DWORD)(stub - base);
		VirtualProtect(slot, 4, old, &old);
		return slot;
	}
	printf("d3d11.dll does not export %s\n", name);
	exit(1);
}

static void restore_export(HMODULE module, DWORD *slot, void *real)
{
	DWORD old;
	VirtualProtect(slot, 4, PAGE_READWRITE, &old);
	*slot = (DWORD)((uint8_t*)real - (uint8_t*)module);
	VirtualProtect(slot, 4, old, &old);
}

static void patch_imports(HMODULE module, bool clock)
{
	struct { const char *name; void *hook; bool clock; } hooks[] = {
		{ "QueryPerformanceCounter", my_QueryPerformanceCounter, true },
		{ "GetTickCount", my_GetTickCount, true },
		{ "GetTickCount64", my_GetTickCount64, true },
		{ "GetCursorPos", my_GetCursorPos, true },
		{ "GetCursorInfo", my_GetCursorInfo, true },
		{ "GetAsyncKeyState", my_GetAsyncKeyState, false },
		{ "GetForegroundWindow", my_GetForegroundWindow, false },
	};
	uint8_t *base = (uint8_t*)module;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
	IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	int patched = 0;

	for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; d++) {
		IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA*)(base + d->OriginalFirstThunk), *slots = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
		for (; names->u1.AddressOfData; names++, slots++) {
			if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)
				continue;
			const char *name = ((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name;
			for (auto &h : hooks) {
				if (strcmp(name, h.name) || (h.clock && !clock))
					continue;
				DWORD old;
				VirtualProtect(&slots->u1.Function, 8, PAGE_READWRITE, &old);
				slots->u1.Function = (ULONGLONG)h.hook;
				VirtualProtect(&slots->u1.Function, 8, old, &old);
				patched++;
			}
		}
	}
	if (patched < (clock ? 7 : 2)) {
		printf("FAILED: only %d imports of the DLL could be replaced\n", patched);
		exit(1);
	}
}

// ------------------------------------------------------------- the scene

struct Range { int first, count; UINT offset; };
struct Mesh {
	uint32_t hash;
	bool empty;
	std::vector<Range> ranges;
	ID3D11Buffer *buffer;
};
struct Hashed {
	uint32_t hash;
	UINT count;
	ID3D11Buffer *buffer;
	ID3D11ShaderResourceView *view;
};
struct Object {
	std::string name;
	std::vector<Mesh> meshes;
	std::vector<Hashed> vb[3], textures, cbs;
	std::vector<std::vector<int>> keys;
	ID3D11Buffer *pool;
	bool made;
	UINT variants;
};

static std::vector<Object> objects, scenery;
static bool mesh_on_vb0, region_hashes, instanced;
static int texture_hash_version;
static int config_reload = -1;

static uint64_t rng_state;
static uint32_t rnd()
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}
static void fill(std::vector<uint8_t> &data, uint32_t seed)
{
	uint64_t s = seed * 0x9E3779B97F4A7C15ull + 1;
	for (size_t i = 0; i + 8 <= data.size(); i += 8) {
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		memcpy(&data[i], &s, 8);
	}
}

static bool load_scene(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[1 << 16];
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ')
			p++;
		char word[64];
		int used = 0;
		if (sscanf(p, "%63s%n", word, &used) != 1)
			continue;
		p += used;
		if (!strcmp(word, "mesh_slot")) {
			mesh_on_vb0 = strstr(p, "vb0") != NULL;
		} else if (!strcmp(word, "region")) {
			region_hashes = atoi(p) != 0;
		} else if (!strcmp(word, "texture_hash")) {
			texture_hash_version = atoi(p);
		} else if (!strcmp(word, "config_reload")) {
			config_reload = atoi(p);
		} else if (!strcmp(word, "draw")) {
			instanced = strstr(p, "instanced") != NULL;
		} else if (!strcmp(word, "object")) {
			Object o = {};
			while (*p == ' ')
				p++;
			o.name = p;
			while (!o.name.empty() && (o.name.back() == '\n' || o.name.back() == '\r'))
				o.name.pop_back();
			objects.push_back(o);
		} else if (objects.empty()) {
			continue;
		} else if (!strcmp(word, "mesh")) {
			Mesh m = {};
			m.hash = strtoul(p, &p, 16);
			int first, count;
			while (sscanf(p, " %d:%d%n", &first, &count, &used) == 2) {
				m.ranges.push_back({ first, count, 0 });
				p += used;
			}
			m.empty = strstr(p, "empty") != NULL;
			objects.back().meshes.push_back(m);
		} else if (!strcmp(word, "key")) {
			std::vector<int> combo;
			int vk;
			while (sscanf(p, " %d%n", &vk, &used) == 1) {
				combo.push_back(vk);
				p += used;
			}
			if (!combo.empty())
				objects.back().keys.push_back(combo);
		} else {
			Hashed h = {};
			h.hash = strtoul(p, &p, 16);
			h.count = strtoul(p, &p, 10);
			if (!strcmp(word, "vb0")) objects.back().vb[0].push_back(h);
			else if (!strcmp(word, "vb1")) objects.back().vb[1].push_back(h);
			else if (!strcmp(word, "vb2") || !strcmp(word, "vb3")) objects.back().vb[2].push_back(h);
			else if (!strcmp(word, "tex")) objects.back().textures.push_back(h);
			else if (!strcmp(word, "cb")) objects.back().cbs.push_back(h);
		}
	}
	fclose(f);
	return true;
}

static const UINT strides[3] = { 40, 20, 32 };
static unsigned forge_failures;
static bool verbose;

static ID3D11Buffer *forged_buffer(uint32_t hash, UINT bytes, UINT bind, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0)
{
	bytes = (std::max(bytes, 16u) + 15) & ~15u;
	std::vector<uint8_t> data(bytes);
	fill(data, hash);
	D3D11_BUFFER_DESC desc = { bytes, usage, bind, cpu, 0, 0 };
	crc_forge(data.data(), bytes, &desc, sizeof(desc), hash);
	if (crc32c(crc32c(0, data.data(), bytes), &desc, sizeof(desc)) != hash)
		forge_failures++;
	D3D11_SUBRESOURCE_DATA init = { data.data() };
	ID3D11Buffer *buffer = NULL;
	if (verbose)
		printf("buffer %08x of %u bytes\n", hash, bytes);
	CHECK(dev->CreateBuffer(&desc, &init, &buffer), "CreateBuffer");
	return buffer;
}

// A buffer the game fills later has the hash of its description alone. The
// one field Direct3D does not look at for a plain buffer is solved for.
static ID3D11Buffer *forged_empty_buffer(uint32_t hash, UINT bytes, UINT bind)
{
	D3D11_BUFFER_DESC desc = { (std::max(bytes, 16u) + 15) & ~15u, D3D11_USAGE_DEFAULT, bind, 0, 0, 0 };
	crc_forge((uint8_t*)&desc, sizeof(desc), NULL, 0, hash);
	if (crc32c(0, &desc, sizeof(desc)) != hash)
		forge_failures++;
	ID3D11Buffer *buffer = NULL;
	CHECK(dev->CreateBuffer(&desc, NULL, &buffer), "CreateBuffer without data");
	return buffer;
}

static ID3D11ShaderResourceView *forged_texture(uint32_t hash)
{
	const UINT side = 8;
	std::vector<uint8_t> data(side * side * 4);
	fill(data, hash ^ 0x7465);
	D3D11_TEXTURE2D_DESC desc = { side, side, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
	// The legacy hash only takes the first width * height bytes
	size_t hashed = texture_hash_version ? data.size() : side * side;
	crc_forge(data.data(), hashed, &desc, sizeof(desc), hash);
	D3D11_SUBRESOURCE_DATA init = { data.data(), side * 4, 0 };
	ID3D11Texture2D *texture = NULL;
	ID3D11ShaderResourceView *view = NULL;
	CHECK(dev->CreateTexture2D(&desc, &init, &texture), "CreateTexture2D");
	CHECK(dev->CreateShaderResourceView(texture, NULL, &view), "CreateShaderResourceView");
	texture->Release();
	return view;
}

static void make_object(Object &o)
{
	if (o.made)
		return;
	o.made = true;
	if (region_hashes) {
		// One index buffer for the object, and in it the indices of every
		// range with the hash a section looks for
		std::vector<uint8_t> pool;
		for (Mesh &m : o.meshes) {
			for (Range &r : m.ranges) {
				if (r.count < 2) r.count = 300;
				if (r.first < 0) r.first = 0;
				size_t at = pool.size() + (size_t)r.first * 2;
				std::vector<uint8_t> region((size_t)r.count * 2);
				fill(region, m.hash + r.count);
				crc_forge(region.data(), region.size(), NULL, 0, m.hash);
				pool.resize(at);
				pool.insert(pool.end(), region.begin(), region.end());
				r.offset = (UINT)(at - (size_t)r.first * 2);
			}
		}
		if (!pool.empty()) {
			D3D11_BUFFER_DESC desc = { (UINT)pool.size(), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
			D3D11_SUBRESOURCE_DATA init = { pool.data() };
			CHECK(dev->CreateBuffer(&desc, &init, &o.pool), "CreateBuffer");
		}
	} else {
		for (Mesh &m : o.meshes) {
			UINT indices = 300;
			for (Range &r : m.ranges)
				if (r.first >= 0)
					indices = std::max(indices, (UINT)(r.first + std::max(r.count, 0)));
			UINT bytes = mesh_on_vb0 ? 1000 * strides[0] : indices * 2, bind = mesh_on_vb0 ? D3D11_BIND_VERTEX_BUFFER : D3D11_BIND_INDEX_BUFFER;
			m.buffer = m.empty ? forged_empty_buffer(m.hash, bytes, bind) : forged_buffer(m.hash, bytes, bind);
		}
	}
	for (int slot = 0; slot < 3; slot++)
		for (Hashed &h : o.vb[slot])
			// A section that overrides the vertex count is for a buffer the
			// game makes empty (the DLL makes it larger, which data of the
			// old size would not fill)
			h.buffer = h.count ? forged_empty_buffer(h.hash, 1000 * strides[slot], D3D11_BIND_VERTEX_BUFFER) :
				forged_buffer(h.hash, 1000 * strides[slot], D3D11_BIND_VERTEX_BUFFER);
	for (Hashed &h : o.textures)
		h.view = forged_texture(h.hash);
	for (Hashed &h : o.cbs)
		h.buffer = h.count ? forged_empty_buffer(h.hash, 4096, D3D11_BIND_CONSTANT_BUFFER) :
			forged_buffer(h.hash, 4096, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
	o.variants = (UINT)std::max<size_t>(1, std::max(o.vb[0].size(), std::max(o.vb[1].size(), o.vb[2].size())));
}

static void release_object(Object &o)
{
	if (!o.made)
		return;
	o.made = false;
	for (Mesh &m : o.meshes)
		if (m.buffer) { m.buffer->Release(); m.buffer = NULL; }
	if (o.pool) { o.pool->Release(); o.pool = NULL; }
	for (int slot = 0; slot < 3; slot++)
		for (Hashed &h : o.vb[slot])
			if (h.buffer) { h.buffer->Release(); h.buffer = NULL; }
	for (Hashed &h : o.textures)
		if (h.view) { h.view->Release(); h.view = NULL; }
	for (Hashed &h : o.cbs)
		if (h.buffer) { h.buffer->Release(); h.buffer = NULL; }
}

// ------------------------------------------------------------ the "game"

static const char *shader_source = R"(
cbuffer PerFrame : register(b0) { float4x4 view_proj; float4 frame_misc; };
cbuffer PerDraw : register(b1) { float4x4 world; float4 tint; };
Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);
SamplerState s0 : register(s0);
struct VIn {
	float3 pos : POSITION; float3 normal : NORMAL; float4 tangent : TANGENT;
	float4 color : COLOR; float2 uv : TEXCOORD0; float2 uv2 : TEXCOORD1;
	float4 weights : BLENDWEIGHT; uint4 bones : BLENDINDICES;
};
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR; };
VOut vs_mesh(VIn i)
{
	VOut o;
	// Every vertex in one spot: nothing reaches a pixel
	float4 p = mul(world, float4(i.pos + i.normal * i.weights.x + i.tangent.xyz * (float)i.bones.x, 1));
	o.pos = float4(p.xy * 1e-12 + frame_misc.xy * 0, 0.5, 1);
	o.uv = i.uv + i.uv2 * (VARIANT * 0.001);
	o.color = i.color * tint;
	return o;
}
float4 ps_mesh(VOut i) : SV_Target
{
	return (t0.Sample(s0, i.uv) + t1.Sample(s0, i.uv) * t2.Sample(s0, i.uv) + t3.Sample(s0, i.uv)) * i.color * (1 + VARIANT * 0.001);
}
VOut vs_screen(uint id : SV_VertexID)
{
	VOut o;
	o.uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.color = 1;
	return o;
}
float4 ps_screen(VOut i) : SV_Target { return t0.Sample(s0, i.uv) * 0.5 + 0.25; }
struct UIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };
VOut vs_ui(UIn i) { VOut o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.color = i.color; return o; }
float4 ps_ui(VOut i) : SV_Target { return t0.Sample(s0, i.uv) * i.color; }
)";

static ID3DBlob *compile(const char *entry, const char *target, int variant)
{
	char number[16];
	sprintf(number, "%d", variant);
	D3D_SHADER_MACRO macros[] = { { "VARIANT", number }, { NULL, NULL } };
	ID3DBlob *code = NULL, *errors = NULL;
	HRESULT hr = D3DCompile(shader_source, strlen(shader_source), NULL, macros, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
	if (FAILED(hr)) {
		printf("shader %s: %s\n", entry, errors ? (char*)errors->GetBufferPointer() : "?");
		exit(1);
	}
	return code;
}

struct Material { ID3D11VertexShader *vs; ID3D11PixelShader *ps; };
static std::vector<Material> materials;
static ID3D11InputLayout *mesh_layout, *ui_layout;
static ID3D11VertexShader *screen_vs, *ui_vs;
static ID3D11PixelShader *screen_ps, *ui_ps;
static ID3D11Buffer *frame_cb, *draw_cb, *pixel_cb, *default_vb[3], *default_ib, *ui_vb;
static ID3D11Texture2D *color_tex[2], *depth_tex, *shadow_tex;
static ID3D11RenderTargetView *color_rtv[2], *back_rtv;
static ID3D11ShaderResourceView *color_srv[2];
static ID3D11DepthStencilView *depth_dsv, *shadow_dsv;
static ID3D11BlendState *blend_opaque, *blend_alpha;
static ID3D11DepthStencilState *depth_on, *depth_off;
static ID3D11RasterizerState *raster_back, *raster_none;
static ID3D11SamplerState *sampler;
static std::vector<ID3D11ShaderResourceView*> scenery_textures;
static const UINT WIDTH = 1280, HEIGHT = 720;

static ID3D11Buffer *plain_buffer(UINT bytes, UINT bind, uint32_t seed, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpu = 0)
{
	std::vector<uint8_t> data(bytes);
	fill(data, seed);
	D3D11_BUFFER_DESC desc = { bytes, usage, bind, cpu, 0, 0 };
	D3D11_SUBRESOURCE_DATA init = { data.data() };
	ID3D11Buffer *buffer = NULL;
	CHECK(dev->CreateBuffer(&desc, &init, &buffer), "CreateBuffer");
	return buffer;
}

static void make_game(int material_count, int scenery_count)
{
	ID3DBlob *code;
	D3D11_INPUT_ELEMENT_DESC mesh_elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 1, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 2, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	D3D11_INPUT_ELEMENT_DESC ui_elements[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};

	for (int i = 0; i < material_count; i++) {
		Material m;
		code = compile("vs_mesh", "vs_5_0", i);
		CHECK(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &m.vs), "CreateVertexShader");
		if (!mesh_layout)
			CHECK(dev->CreateInputLayout(mesh_elements, 8, code->GetBufferPointer(), code->GetBufferSize(), &mesh_layout), "CreateInputLayout");
		code->Release();
		code = compile("ps_mesh", "ps_5_0", i);
		CHECK(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &m.ps), "CreatePixelShader");
		code->Release();
		materials.push_back(m);
	}
	code = compile("vs_screen", "vs_5_0", 0);
	CHECK(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &screen_vs), "CreateVertexShader");
	code->Release();
	code = compile("ps_screen", "ps_5_0", 0);
	CHECK(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &screen_ps), "CreatePixelShader");
	code->Release();
	code = compile("vs_ui", "vs_5_0", 0);
	CHECK(dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ui_vs), "CreateVertexShader");
	CHECK(dev->CreateInputLayout(ui_elements, 3, code->GetBufferPointer(), code->GetBufferSize(), &ui_layout), "CreateInputLayout");
	code->Release();
	code = compile("ps_ui", "ps_5_0", 0);
	CHECK(dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ui_ps), "CreatePixelShader");
	code->Release();

	frame_cb = plain_buffer(256, D3D11_BIND_CONSTANT_BUFFER, 1, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
	draw_cb = plain_buffer(256, D3D11_BIND_CONSTANT_BUFFER, 2, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
	pixel_cb = plain_buffer(256, D3D11_BIND_CONSTANT_BUFFER, 3, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
	for (int slot = 0; slot < 3; slot++)
		default_vb[slot] = plain_buffer(1000 * strides[slot], D3D11_BIND_VERTEX_BUFFER, 10 + slot);
	default_ib = plain_buffer(600000, D3D11_BIND_INDEX_BUFFER, 20);
	ui_vb = plain_buffer(65536, D3D11_BIND_VERTEX_BUFFER, 21, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);

	for (int i = 0; i < 2; i++) {
		D3D11_TEXTURE2D_DESC desc = { WIDTH, HEIGHT, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		CHECK(dev->CreateTexture2D(&desc, NULL, &color_tex[i]), "CreateTexture2D");
		CHECK(dev->CreateRenderTargetView(color_tex[i], NULL, &color_rtv[i]), "CreateRenderTargetView");
		CHECK(dev->CreateShaderResourceView(color_tex[i], NULL, &color_srv[i]), "CreateShaderResourceView");
	}
	{
		D3D11_TEXTURE2D_DESC desc = { WIDTH, HEIGHT, 1, 1, DXGI_FORMAT_D24_UNORM_S8_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0 };
		CHECK(dev->CreateTexture2D(&desc, NULL, &depth_tex), "CreateTexture2D");
		CHECK(dev->CreateDepthStencilView(depth_tex, NULL, &depth_dsv), "CreateDepthStencilView");
		desc.Width = desc.Height = 1024;
		CHECK(dev->CreateTexture2D(&desc, NULL, &shadow_tex), "CreateTexture2D");
		CHECK(dev->CreateDepthStencilView(shadow_tex, NULL, &shadow_dsv), "CreateDepthStencilView");
	}
	ID3D11Texture2D *back = NULL;
	CHECK(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back), "GetBuffer");
	CHECK(dev->CreateRenderTargetView(back, NULL, &back_rtv), "CreateRenderTargetView");
	back->Release();

	D3D11_BLEND_DESC blend = {};
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	CHECK(dev->CreateBlendState(&blend, &blend_opaque), "CreateBlendState");
	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	CHECK(dev->CreateBlendState(&blend, &blend_alpha), "CreateBlendState");
	D3D11_DEPTH_STENCIL_DESC depth = {};
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	CHECK(dev->CreateDepthStencilState(&depth, &depth_on), "CreateDepthStencilState");
	depth.DepthEnable = FALSE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	CHECK(dev->CreateDepthStencilState(&depth, &depth_off), "CreateDepthStencilState");
	D3D11_RASTERIZER_DESC raster = {};
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_BACK;
	raster.DepthClipEnable = TRUE;
	CHECK(dev->CreateRasterizerState(&raster, &raster_back), "CreateRasterizerState");
	raster.CullMode = D3D11_CULL_NONE;
	CHECK(dev->CreateRasterizerState(&raster, &raster_none), "CreateRasterizerState");
	D3D11_SAMPLER_DESC samp = {};
	samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	samp.MaxLOD = D3D11_FLOAT32_MAX;
	CHECK(dev->CreateSamplerState(&samp, &sampler), "CreateSamplerState");

	// Scenery: what a game draws that no mod is for
	rng_state = 0x5ce4e11;
	for (int i = 0; i < 256; i++) {
		std::vector<uint8_t> data(8 * 8 * 4);
		fill(data, 0x10000 + i);
		D3D11_TEXTURE2D_DESC desc = { 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
		D3D11_SUBRESOURCE_DATA init = { data.data(), 32, 0 };
		ID3D11Texture2D *texture = NULL;
		ID3D11ShaderResourceView *view = NULL;
		CHECK(dev->CreateTexture2D(&desc, &init, &texture), "CreateTexture2D");
		CHECK(dev->CreateShaderResourceView(texture, NULL, &view), "CreateShaderResourceView");
		texture->Release();
		scenery_textures.push_back(view);
	}
	for (int i = 0; i < scenery_count; i++) {
		Object o = {};
		Mesh m = {};
		m.ranges.push_back({ 0, 30 + (int)(rnd() % 300) * 3, 0 });
		if (rnd() % 4 == 0)
			m.ranges.push_back({ m.ranges[0].count, 60, 0 });
		m.buffer = plain_buffer(2400, D3D11_BIND_INDEX_BUFFER, 0x20000 + i);
		o.meshes.push_back(m);
		for (int slot = 0; slot < 3; slot++) {
			Hashed h = {};
			h.buffer = plain_buffer(300 * strides[slot], D3D11_BIND_VERTEX_BUFFER, 0x30000 + i * 4 + slot);
			o.vb[slot].push_back(h);
		}
		for (int t = 0; t < 4; t++) {
			Hashed h = {};
			h.view = scenery_textures[rnd() % scenery_textures.size()];
			o.textures.push_back(h);
		}
		o.variants = 1;
		o.made = true;
		scenery.push_back(o);
	}
}

static UINT frame_index;
static const Material *bound_material;
static unsigned draws_made;

static void update_cb(ID3D11Buffer *cb, uint32_t a, uint32_t b)
{
	D3D11_MAPPED_SUBRESOURCE map;
	if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
		return;
	uint32_t *words = (uint32_t*)map.pData;
	for (int i = 0; i < 64; i++)
		words[i] = a * 2654435761u + b * 40503u + i;
	ctx->Unmap(cb, 0);
}

static void draw_indexed(UINT count, UINT first)
{
	if (instanced)
		ctx->DrawIndexedInstanced(count, 1, first, 0, 0);
	else
		ctx->DrawIndexed(count, first, 0);
	draws_made++;
}

static void draw_plain(UINT count)
{
	if (instanced)
		ctx->DrawInstanced(count, 1, 0, 0);
	else
		ctx->Draw(count, 0);
	draws_made++;
}

static void bind_object(Object &o, UINT variant, UINT salt, bool is_mod)
{
	ID3D11Buffer *vbs[3];
	UINT offsets[3] = { 0, 0, 0 };
	for (int slot = 0; slot < 3; slot++)
		vbs[slot] = o.vb[slot].empty() ? default_vb[slot] : o.vb[slot][variant % o.vb[slot].size()].buffer;
	ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);

	ID3D11ShaderResourceView *views[8];
	size_t n = o.textures.size();
	for (int i = 0; i < 8; i++) {
		if (n)
			views[i] = (n <= 8 && (size_t)i >= n) ? scenery_textures[(salt + i) % scenery_textures.size()] : o.textures[((n > 8 ? salt * 8 : 0) + i) % n].view;
		else
			views[i] = scenery_textures[(salt * 5 + i) % scenery_textures.size()];
	}
	ctx->PSSetShaderResources(0, is_mod ? 8 : 4, views);

	if (!o.cbs.empty()) {
		ID3D11Buffer *cbs[2] = { o.cbs[variant % o.cbs.size()].buffer, o.cbs[(variant + 1) % o.cbs.size()].buffer };
		ctx->VSSetConstantBuffers(3, 2, cbs);
	}
}

static void draw_object(Object &o, UINT pass, UINT index, bool is_mod)
{
	const Material *m = &materials[(index * 7 + pass * 3) % materials.size()];
	if (m != bound_material) {
		ctx->VSSetShader(m->vs, NULL, 0);
		ctx->PSSetShader(m->ps, NULL, 0);
		bound_material = m;
	}
	update_cb(draw_cb, frame_index, index * 16 + pass);
	bind_object(o, pass, frame_index + pass + index, is_mod);

	if (o.meshes.empty()) {
		ctx->IASetIndexBuffer(default_ib, DXGI_FORMAT_R16_UINT, 0);
		draw_indexed(300, 0);
	}
	for (Mesh &mesh : o.meshes) {
		if (mesh_on_vb0 && is_mod) {
			UINT offset = 0;
			ctx->IASetVertexBuffers(0, 1, &mesh.buffer, &strides[0], &offset);
			ctx->IASetIndexBuffer(default_ib, DXGI_FORMAT_R16_UINT, 0);
		} else if (!region_hashes || !is_mod) {
			ctx->IASetIndexBuffer(mesh.buffer, DXGI_FORMAT_R16_UINT, 0);
		}
		bool ranged = false;
		for (Range &r : mesh.ranges)
			ranged |= r.first >= 0;
		for (Range &r : mesh.ranges) {
			if (r.first < 0 && ranged)
				continue;
			if (region_hashes && is_mod)
				ctx->IASetIndexBuffer(o.pool, DXGI_FORMAT_R16_UINT, r.offset);
			draw_indexed(r.count > 0 ? r.count : 300, r.first > 0 ? r.first : 0);
		}
	}
	if (!o.cbs.empty()) {
		ID3D11Buffer *none[2] = { NULL, NULL };
		ctx->VSSetConstantBuffers(3, 2, none);
	}
}

static void frame(const std::vector<int> &on_screen, int passes, int ui_draws)
{
	const float clear[4] = { 0.1f, 0.2f, 0.3f, 1 };
	D3D11_VIEWPORT viewport = { 0, 0, (float)WIDTH, (float)HEIGHT, 0, 1 }, shadow_viewport = { 0, 0, 1024, 1024, 0, 1 };
	ID3D11Buffer *vs_cbs[2] = { frame_cb, draw_cb };
	ID3D11ShaderResourceView *no_views[8] = {};

	update_cb(frame_cb, frame_index, 0);
	update_cb(pixel_cb, frame_index, 1);
	ctx->ClearRenderTargetView(color_rtv[0], clear);
	ctx->ClearDepthStencilView(depth_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
	ctx->ClearDepthStencilView(shadow_dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->IASetInputLayout(mesh_layout);
	ctx->VSSetConstantBuffers(0, 2, vs_cbs);
	ctx->PSSetConstantBuffers(0, 1, &pixel_cb);
	ctx->PSSetSamplers(0, 1, &sampler);
	bound_material = NULL;

	for (int pass = 0; pass < passes; pass++) {
		// Shadows first, then what is opaque, then outlines and the like
		if (pass == 0) {
			ctx->OMSetRenderTargets(0, NULL, shadow_dsv);
			ctx->RSSetViewports(1, &shadow_viewport);
		} else {
			ctx->OMSetRenderTargets(1, &color_rtv[0], depth_dsv);
			ctx->RSSetViewports(1, &viewport);
		}
		ctx->OMSetBlendState(pass < 2 ? blend_opaque : blend_alpha, NULL, 0xffffffff);
		ctx->OMSetDepthStencilState(depth_on, 0);
		ctx->RSSetState(pass % 2 ? raster_back : raster_none);

		size_t next_mod = 0, every = on_screen.empty() ? 0 : std::max<size_t>(1, scenery.size() / on_screen.size());
		for (size_t i = 0; i < scenery.size(); i++) {
			draw_object(scenery[i], pass, (UINT)i, false);
			if (every && i % every == every - 1 && next_mod < on_screen.size()) {
				draw_object(objects[on_screen[next_mod]], pass, 100000 + on_screen[next_mod], true);
				next_mod++;
			}
		}
		for (; next_mod < on_screen.size(); next_mod++)
			draw_object(objects[on_screen[next_mod]], pass, 100000 + on_screen[next_mod], true);
	}

	// The draws without indices some games skin their characters with, one
	// for each combination of vertex buffers a mod has sections for
	for (int index : on_screen) {
		Object &o = objects[index];
		for (UINT v = 0; v < o.variants; v++) {
			bind_object(o, v, frame_index + v, true);
			draw_plain(36);
		}
	}

	// What is drawn over the picture: small draws from a buffer written as it goes
	ctx->OMSetBlendState(blend_alpha, NULL, 0xffffffff);
	ctx->OMSetDepthStencilState(depth_off, 0);
	ctx->RSSetState(raster_none);
	ctx->IASetInputLayout(ui_layout);
	ctx->VSSetShader(ui_vs, NULL, 0);
	ctx->PSSetShader(ui_ps, NULL, 0);
	{
		UINT stride = 20, offset = 0;
		ctx->IASetVertexBuffers(0, 1, &ui_vb, &stride, &offset);
	}
	for (int i = 0; i < ui_draws; i++) {
		D3D11_MAPPED_SUBRESOURCE map;
		if (SUCCEEDED(ctx->Map(ui_vb, 0, i == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map))) {
			if (i == 0)
				memset(map.pData, 0, 65536);
			uint32_t *words = (uint32_t*)((uint8_t*)map.pData + i * 120);
			for (int k = 0; k < 30; k++)
				words[k] = frame_index * 31 + i * 7 + k;
			ctx->Unmap(ui_vb, 0);
		}
		ctx->PSSetShaderResources(0, 1, &scenery_textures[(frame_index + i) % scenery_textures.size()]);
		ctx->Draw(6, i * 6);
		draws_made++;
	}

	// And what is done to the whole picture afterwards
	ctx->IASetInputLayout(NULL);
	ctx->VSSetShader(screen_vs, NULL, 0);
	ctx->PSSetShader(screen_ps, NULL, 0);
	ctx->OMSetBlendState(blend_opaque, NULL, 0xffffffff);
	for (int i = 0; i < 4; i++) {
		ctx->PSSetShaderResources(0, 8, no_views);
		ctx->OMSetRenderTargets(1, &color_rtv[(i + 1) % 2], NULL);
		ctx->PSSetShaderResources(0, 1, &color_srv[i % 2]);
		ctx->Draw(3, 0);
		draws_made++;
	}
	ctx->PSSetShaderResources(0, 8, no_views);
	ctx->OMSetRenderTargets(1, &back_rtv, NULL);
	ctx->PSSetShaderResources(0, 1, &color_srv[0]);
	ctx->Draw(3, 0);
	draws_made++;
	ctx->PSSetShaderResources(0, 8, no_views);
}

// appear: what the first frames of objects coming on screen cost over the
// frames after them (ms[3] is the sum of the frames from the fourth on)
struct Appearance {
	std::vector<int> objects;
	double create;
	double ms[4];
	double present;
};
static std::vector<Appearance> appearances;

static void report_appearances(int rotate)
{
	std::vector<double> steady, first, second, third, created;
	std::vector<std::pair<double, size_t>> worst;
	double sum_first = 0, sum_second = 0, sum_third = 0, sum_created = 0, sum_present = 0;

	for (const Appearance &a : appearances)
		steady.push_back(a.ms[3] / (rotate - 3));
	std::vector<double> sorted = steady;
	std::sort(sorted.begin(), sorted.end());
	double base = sorted[sorted.size() / 2];

	for (size_t i = 0; i < appearances.size(); i++) {
		const Appearance &a = appearances[i];
		first.push_back(a.ms[0] - base);
		second.push_back(a.ms[1] - base);
		third.push_back(a.ms[2] - base);
		created.push_back(a.create);
		sum_first += a.ms[0] - base;
		sum_second += a.ms[1] - base;
		sum_third += a.ms[2] - base;
		sum_created += a.create;
		sum_present += a.present / rotate;
		worst.push_back({ a.ms[0] + a.ms[1] + a.ms[2] - 3 * base, i });
	}
	auto pct = [](std::vector<double> v, double p) { std::sort(v.begin(), v.end()); return v[(size_t)((v.size() - 1) * p)]; };
	printf("appear: %zu windows, steady frame %.3f ms (median)\n", appearances.size(), base);
	printf("  first frame over steady: total %.1f ms, mean %.2f, p50 %.2f, p90 %.2f, p99 %.2f, max %.2f\n",
		sum_first, sum_first / appearances.size(), pct(first, 0.5), pct(first, 0.9), pct(first, 0.99), pct(first, 1));
	printf("  second frame over steady: total %.1f ms, max %.2f; third: total %.1f ms, max %.2f\n",
		sum_second, pct(second, 1), sum_third, pct(third, 1));
	printf("  the game making its resources (outside the frame): total %.1f ms, max %.2f; present %.3f ms a frame\n",
		sum_created, pct(created, 1), sum_present / appearances.size());
	std::sort(worst.begin(), worst.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	for (size_t n = 0; n < worst.size() && n < 12; n++) {
		const Appearance &a = appearances[worst[n].second];
		printf("  %8.2f ms  (%.2f %.2f %.2f)", worst[n].first, a.ms[0] - base, a.ms[1] - base, a.ms[2] - base);
		for (int index : a.objects)
			printf("  %.60s", objects[index].name.c_str());
		printf("\n");
	}
	printf("APPEAR total %.1f ms over %zu windows (first three frames over steady)\n", sum_first + sum_second + sum_third, appearances.size());
}

// A frame that never ends (a mod's lists calling each other without end, for
// one) must not hang whatever script runs the harness
static volatile LONG frames_done;
static DWORD WINAPI watchdog(void *)
{
	LONG seen = -1;
	int quiet = 0;
	for (;;) {
		Sleep(1000);
		LONG now = frames_done;
		quiet = now == seen ? quiet + 1 : 0;
		seen = now;
		if (quiet >= 120) {
			printf("STUCK: frame %ld did not end in two minutes\n", now);
			TerminateProcess(GetCurrentProcess(), 3);
		}
	}
}

static LRESULT CALLBACK window_proc(HWND w, UINT message, WPARAM wp, LPARAM lp)
{
	return DefWindowProcW(w, message, wp, lp);
}

int main(int argc, char **argv)
{
	const char *scene = NULL;
	int frames = 600, warmup = 120, chars = 8, first = 0, rotate = 0, scenery_count = 400, passes = 3, material_count = 48, ui_draws = 60;
	int key_every = 6;
	bool check = false, nodll = false, profile = false, appear = false, profile_steady = false;
	unsigned text_first = 1, text_last = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strncmp(a, "scene=", 6)) scene = a + 6;
		else if (!strncmp(a, "frames=", 7)) frames = atoi(a + 7);
		else if (!strncmp(a, "warmup=", 7)) warmup = atoi(a + 7);
		else if (!strncmp(a, "chars=", 6)) chars = atoi(a + 6);
		else if (!strncmp(a, "first=", 6)) first = atoi(a + 6);
		else if (!strncmp(a, "rotate=", 7)) rotate = atoi(a + 7);
		else if (!strncmp(a, "scenery=", 8)) scenery_count = atoi(a + 8);
		else if (!strncmp(a, "passes=", 7)) passes = atoi(a + 7);
		else if (!strncmp(a, "materials=", 10)) material_count = atoi(a + 10);
		else if (!strncmp(a, "ui=", 3)) ui_draws = atoi(a + 3);
		else if (!strncmp(a, "keys=", 5)) key_every = atoi(a + 5);
		else if (!strncmp(a, "text=", 5)) sscanf(a + 5, "%u-%u", &text_first, &text_last);
		else if (!strcmp(a, "check")) check = true;
		else if (!strcmp(a, "verbose")) verbose = true;
		else if (!strcmp(a, "nodll")) nodll = true;
		else if (!strcmp(a, "profile")) profile = true;
		else if (!strcmp(a, "appear")) appear = true;
		else if (!strcmp(a, "steady")) profile_steady = true;
		else { printf("unknown argument %s\n", a); return 1; }
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	crc_init();
	QueryPerformanceFrequency(&qpf);
	if (scene && !load_scene(scene)) {
		printf("cannot read the scene %s\n", scene);
		return 1;
	}
	if (objects.empty())
		chars = 0;
	chars = std::min<int>(chars, (int)objects.size());
	// appear: nothing of the mods for the warmup, then every object of the
	// scene once, `chars` at a time for `rotate` frames: the first frames of
	// each are those where its mods are met for the first time
	if (appear) {
		if (rotate < 4)
			rotate = 12;
		int windows = chars ? ((int)objects.size() - first + chars - 1) / chars : 0;
		frames = warmup + windows * rotate;
	}
	if (frames < 1) {
		printf("nothing to draw: frames=%d\n", frames);
		return 1;
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = window_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"cpu-harness";
	RegisterClassW(&wc);
	RECT rect = { 0, 0, (LONG)WIDTH, (LONG)HEIGHT };
	AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
	window = CreateWindowW(wc.lpszClassName, L"cpu-harness", WS_OVERLAPPEDWINDOW, 40, 40, rect.right - rect.left, rect.bottom - rect.top, NULL, NULL, wc.hInstance, NULL);
	show_harness_window(window);

	wchar_t system_path[MAX_PATH], own_path[MAX_PATH];
	GetSystemDirectoryW(system_path, MAX_PATH);
	wcscat(system_path, L"\\d3d11.dll");
	HMODULE system_d3d11 = LoadLibraryW(system_path);
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

	if (check) {
		redirect_export(system_d3d11, "D3D11CreateDeviceAndSwapChain", traced_create_and_swap, (void**)&real_create_and_swap);
		redirect_export(system_d3d11, "D3D11CreateDevice", traced_create_device, (void**)&real_create_device);
	}
	HMODULE lib = system_d3d11;
	uint64_t dll_loaded_us = virtual_us;
	if (!nodll) {
		GetModuleFileNameW(NULL, own_path, MAX_PATH);
		*wcsrchr(own_path, L'\\') = 0;
		wcscat(own_path, L"\\d3d11.dll");
		// The DLL reads the clock while it loads, as the start of `time`,
		// before patch_imports gets to it, so for a check it binds to ours
		// from the start. What loads after it keeps the real clock.
		HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
		void *real_qpc = NULL;
		DWORD *qpc_slot = check ? redirect_export(kernel32, "QueryPerformanceCounter", my_QueryPerformanceCounter, &real_qpc) : NULL;
		lib = LoadLibraryW(own_path);
		if (qpc_slot)
			restore_export(kernel32, qpc_slot, real_qpc);
		if (!lib) {
			printf("no d3d11.dll next to the harness\n");
			return 1;
		}
		patch_imports(lib, check);
	}

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = WIDTH;
	scd.BufferDesc.Height = HEIGHT;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.OutputWindow = window;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	CreateFn create = (CreateFn)GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain");
	CHECK(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, &scd, &swap, &dev, NULL, &ctx), "D3D11CreateDeviceAndSwapChain");

	if (check)
		trace_enable(true);
	make_game(material_count, scenery_count);

	// The DLL loads the mods again at the first Present past
	// config_initialization_delay seconds of its `time`, and only then takes
	// the [Rendering] settings of the includes: what it hashes before that is
	// hashed with those of d3dx.ini alone. The objects of the mods, made from
	// the first frame on, come after it.
	if (config_reload >= 0 && !nodll) {
		LARGE_INTEGER start, now;
		QueryPerformanceCounter(&start);
		trace_note_frame(~0u);
		for (;;) {
			if (check)
				virtual_us += 16667;
			QueryPerformanceCounter(&now);
			double since = check ? (virtual_us - dll_loaded_us) / 1e6 : (now.QuadPart - start.QuadPart) / (double)qpf.QuadPart;
			swap->Present(0, 0);
			if (since > config_reload)
				break;
			Sleep(1);
		}
		if (check) {
			trace_frame_end();
			// As after the first frames below
			Sleep(700);
		}
	}

	FILE *frames_file = NULL, *text_file = NULL;
	if (check) {
		frames_file = fopen("frames.txt", "w");
		if (!frames_file) {
			printf("cannot write frames.txt\n");
			return 1;
		}
		if (text_last >= text_first) {
			text_file = fopen("trace.txt", "w");
			trace_text(text_file, text_first, text_last);
		}
	}
	if (profile)
		start_profile();

	CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
	std::vector<int> on_screen;
	std::vector<double> draw_ms, present_ms;
	uint64_t total_strict = 0, total_effective = 0;
	unsigned long long total_actions = 0, total_draws = 0;
	unsigned unknown = 0;
	int window_no = -1;
	size_t key_cursor = 0;

	for (int f = 0; f < frames; f++) {
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);

		// Who is on screen: the same mods all along, or others every so often,
		// which makes and releases what they are drawn from as a game does
		int now = rotate ? f / rotate : 0;
		if (appear)
			now = f < warmup ? -1 : (f - warmup) / rotate;
		double create_ms = 0;
		if (now != window_no && chars) {
			LARGE_INTEGER c0, c1;
			QueryPerformanceCounter(&c0);
			std::vector<int> next;
			for (int i = 0; i < chars && now >= 0; i++)
				if (!appear || first + now * chars + i < (int)objects.size())
					next.push_back((first + now * chars + i) % (int)objects.size());
			for (int index : on_screen)
				if (std::find(next.begin(), next.end(), index) == next.end())
					release_object(objects[index]);
			for (int index : next)
				make_object(objects[index]);
			on_screen = next;
			window_no = now;
			key_cursor = 0;
			QueryPerformanceCounter(&c1);
			create_ms = (c1.QuadPart - c0.QuadPart) * 1000.0 / qpf.QuadPart;
		}
		int in_window = appear && now >= 0 ? (f - warmup) % rotate : -1;

		if (check && key_every > 0) {
			// The keys the mods on screen bind, one after the other
			std::vector<const std::vector<int>*> combos;
			for (int index : on_screen)
				for (auto &combo : objects[index].keys)
					combos.push_back(&combo);
			if (f % key_every == 0 && !combos.empty()) {
				for (int vk : *combos[key_cursor++ % combos.size()])
					key_down[vk & 0xff] = true;
			} else if (f % key_every == 2) {
				memset(key_down, 0, sizeof(key_down));
			}
		}

		frame_index = (UINT)f;
		trace_note_frame((unsigned)f);
		profiling = profile && f >= warmup && (!appear || (profile_steady ? in_window == 5 || in_window == 6 : in_window == 0 || in_window == 1));
		LARGE_INTEGER t0, t1, t2;
		QueryPerformanceCounter(&t0);
		frame(on_screen, passes, ui_draws);
		QueryPerformanceCounter(&t1);
		profiling = false;
		swap->Present(0, 0);
		QueryPerformanceCounter(&t2);
		virtual_us += 16667;
		InterlockedIncrement(&frames_done);
		// The DLL looks at the shaders it has met on threads of its own once
		// its configuration is loaded (at a Present), and a shader's
		// draws run their lists from whenever that is done: for a check, it
		// has to be done before the next frame, not some frame later.
		if (check && f < 3)
			Sleep(700);

		if (f >= warmup) {
			draw_ms.push_back((t1.QuadPart - t0.QuadPart) * 1000.0 / qpf.QuadPart);
			present_ms.push_back((t2.QuadPart - t1.QuadPart) * 1000.0 / qpf.QuadPart);
		}
		if (in_window >= 0) {
			if (in_window == 0)
				appearances.push_back({ on_screen, create_ms });
			appearances.back().ms[std::min(in_window, 3)] += draw_ms.back();
			appearances.back().present += present_ms.back();
		}
		if (check) {
			TraceFrame t = trace_frame_end();
			fprintf(frames_file, "%d %016llx %016llx %u %u %u\n", f, (unsigned long long)t.strict, (unsigned long long)t.effective, t.calls, t.actions, t.draws);
			total_strict = total_strict * 0x100000001b3ull ^ t.strict;
			total_effective = total_effective * 0x100000001b3ull ^ t.effective;
			total_actions += t.actions;
			total_draws += t.draws;
			unknown = t.unknown;
		}
	}

	stop_profile();
	printf("%d frames, %u draws a frame by the harness, %zu mods on screen of %zu, %zu scenery objects\n",
		frames, draws_made / (unsigned)frames, on_screen.size(), objects.size(), scenery.size());
	if (forge_failures)
		printf("FAILED to give %u buffers their hashes\n", forge_failures);
	if (check) {
		printf("effective %016llx strict %016llx (%llu actions, %llu draws reached Direct3D, %u objects of unknown origin)\n",
			(unsigned long long)total_effective, (unsigned long long)total_strict, total_actions, total_draws, unknown);
		fclose(frames_file);
		if (text_file)
			fclose(text_file);
	} else if (appear && !appearances.empty()) {
		report_appearances(rotate);
	} else if (!draw_ms.empty()) {
		std::vector<double> sorted = draw_ms, sorted_present = present_ms;
		std::sort(sorted.begin(), sorted.end());
		std::sort(sorted_present.begin(), sorted_present.end());
		double sum = 0;
		for (double v : draw_ms)
			sum += v;
		printf("frame %.4f ms median, %.4f mean, %.4f best tenth, %.4f p99; present %.4f ms median\n",
			sorted[sorted.size() / 2], sum / draw_ms.size(), sorted[sorted.size() / 10], sorted[sorted.size() * 99 / 100], sorted_present[sorted_present.size() / 2]);
	}
	if (profile)
		report_profile();

	trace_enable(false);
	ctx->ClearState();
	ctx->Flush();
	for (Object &o : objects)
		release_object(o);
	swap->Release();
	ctx->Release();
	dev->Release();
	return forge_failures ? 1 : 0;
}
