#include "Pick.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "Channel.h"
#include "Protocol.h"

#include "../CommandList.h"
#include "../ByteCodeReader.h"
#include "../cursor.h"
#include "../globals.h"
#include "../HackerContext.h"
#include "../HackerDevice.h"
#include "../IniHandler.h"
#include "../log.h"
#include "../ResourceHash.h"
#include "../dx12/PipelineRegistry.h"
#include "util.h"

using namespace DesktopProtocol;

volatile bool desktop_pick_capturing;

namespace {

const UINT HIT_CAPACITY = 256;
// A count, three words of padding and the hits, as the pixel shader writes them
const UINT HIT_BYTES = 16 + HIT_CAPACITY * 16;
const unsigned READBACK_FRAMES = 10;
const UINT PS_PARAMS_SLOT = 13;
// D3D_NAME_POSITION
const UINT SYSTEM_VALUE_POSITION = 1;

struct ShaderInfo {
	int position;
	bool replaced;
	bool replay_safe;
};

SRWLOCK shader_lock = SRWLOCK_INIT;
std::unordered_map<ID3D11DeviceChild*, ShaderInfo> shader_infos;

enum class State { IDLE, ARMED, CAPTURING, READBACK };

struct Request {
	uint64_t capture_id;
	uint32_t connection;
	uint8_t trigger;
	bool cursor_given;
	float x, y;
};

// The game's bindings when it issued a draw, before any command list changed them
struct Snapshot {
	bool has_ib;
	Buffer ib;
	std::vector<VertexBuffer> vbs;
	UINT64 shader_hashes[5];
	bool replaced[5];
	std::vector<Texture> srvs;
	std::vector<Target> rts;
	bool has_ds;
	Surface ds;
	std::vector<std::pair<uint8_t, std::wstring>> overrides;
};

struct PickRecord {
	uint8_t origin;
	unsigned draw_number;
	// Index into snapshots, -1 for none
	int snapshot;
	DesktopPickDraw draw;
	uint8_t topology;
	uint8_t depth_func;
	std::wstring section;
	std::wstring line;
};

// Why draws were not picked, for the detail of NO_ELIGIBLE_DRAW
struct Skipped {
	unsigned query, no_depth, viewport, stream_output, position, shader, shader_uav;
};

struct PsParams {
	UINT record;
	UINT flags;
	UINT pixel[2];
};

class UnconditionalCommands {
	ID3D11DeviceContext1 *context;
	ID3D11Predicate *predicate = NULL;
	BOOL value = FALSE;
public:
	explicit UnconditionalCommands(ID3D11DeviceContext1 *context) : context(context)
	{
		context->GetPredication(&predicate, &value);
		if (predicate)
			context->SetPredication(NULL, FALSE);
	}
	~UnconditionalCommands()
	{
		if (predicate) {
			context->SetPredication(predicate, value);
			predicate->Release();
		}
	}
};

State state = State::IDLE;
Request request;
uint64_t next_capture_id = 1;

HackerContext *capture_context;
ID3D11Device1 *resource_device;
ID3D11DeviceContext1 *capture_orig;
uint64_t capture_frame;
UINT window_width, window_height;
float cursor_x, cursor_y;
std::vector<Snapshot> snapshots;
std::vector<PickRecord> records;
int current_snapshot = -1;
bool in_game_draw;
unsigned current_draw_number;
bool deferred_skipped;
unsigned draws_seen;
Skipped skipped;
LARGE_INTEGER hook_ticks;
unsigned readback_waited;

ID3D11Buffer *hit_buffer, *hit_staging, *params_buffer;
ID3D11UnorderedAccessView *hit_uav;
ID3D11DepthStencilState *cover_state;
std::map<ID3D11DepthStencilState*, ID3D11DepthStencilState*> depth_states;
bool resources_failed;
bool constant_buffer_offsetting;
// By position register, depth_test pass and primitive ID input
std::map<std::tuple<int, bool, bool>, ID3D11PixelShader*> pixel_shaders;

void add_ticks(const LARGE_INTEGER &start)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	hook_ticks.QuadPart += now.QuadPart - start.QuadPart;
}

void release_capture()
{
	snapshots.clear();
	records.clear();
	current_snapshot = -1;
	in_game_draw = false;
	if (capture_context)
		capture_context->Release();
	capture_orig = NULL;
	capture_context = NULL;
}

void fail(const char *code, const std::string &detail)
{
	PickFailed m;
	m.capture_id = request.capture_id;
	m.code = code;
	m.detail = detail;
	std::vector<uint8_t> frame;
	if (EncodeFrame(MessageType::PickFailed, 0, m, &frame))
		DesktopChannelQueue(DesktopSend::RESULT, std::move(frame), request.connection);
	LogInfo("Desktop pick %llu failed: %s %s\n", request.capture_id, code, detail.c_str());
	release_capture();
	state = State::IDLE;
}

void release_resources()
{
	for (auto &p : depth_states) {
		if (p.first)
			p.first->Release();
		p.second->Release();
	}
	depth_states.clear();
	for (auto &p : pixel_shaders) {
		if (p.second)
			p.second->Release();
	}
	pixel_shaders.clear();
	for (IUnknown *resource : { (IUnknown*)hit_buffer, (IUnknown*)hit_staging, (IUnknown*)params_buffer,
			(IUnknown*)hit_uav, (IUnknown*)cover_state }) {
		if (resource)
			resource->Release();
	}
	hit_buffer = hit_staging = params_buffer = NULL;
	hit_uav = NULL;
	cover_state = NULL;
	if (resource_device)
		resource_device->Release();
	resource_device = NULL;
	resources_failed = false;
}

bool create_resources(ID3D11Device1 *device)
{
	if (resource_device != device) {
		release_resources();
		resource_device = device;
		resource_device->AddRef();
	}
	if (resources_failed)
		return false;
	if (hit_buffer)
		return true;
	resources_failed = true;
	if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
		return false;
	D3D11_FEATURE_DATA_D3D11_OPTIONS options = {};
	constant_buffer_offsetting = SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
		&& options.ConstantBufferOffsetting;

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = HIT_BYTES;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (FAILED(device->CreateBuffer(&bd, NULL, &hit_buffer)))
		return false;
	bd.Usage = D3D11_USAGE_STAGING;
	bd.BindFlags = 0;
	bd.MiscFlags = 0;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(device->CreateBuffer(&bd, NULL, &hit_staging)))
		return false;
	bd = {};
	bd.ByteWidth = sizeof(PsParams);
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(device->CreateBuffer(&bd, NULL, &params_buffer)))
		return false;

	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.Format = DXGI_FORMAT_R32_TYPELESS;
	ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ud.Buffer.NumElements = HIT_BYTES / 4;
	ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (FAILED(device->CreateUnorderedAccessView(hit_buffer, &ud, &hit_uav)))
		return false;

	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	if (FAILED(device->CreateDepthStencilState(&dd, &cover_state)))
		return false;
	resources_failed = false;
	return true;
}

// The input signature has K float4 registers ahead of SV_Position, for a
// stage before the rasterizer that writes SV_Position to register K: a driver
// that links by register finds it at K, one that links by semantic finds it by
// name. The values of the registers ahead of it are never read.
ID3D11PixelShader* pixel_shader(int position, bool depth_test, bool primitive)
{
	auto key = std::make_tuple(position, depth_test, primitive);
	auto found = pixel_shaders.find(key);
	if (found != pixel_shaders.end())
		return found->second;

	std::string src =
		"cbuffer DesktopPick : register(b13) { uint pick_record; uint pick_flags; uint2 pick_pixel; };\n"
		"RWByteAddressBuffer pick_hits : register(u0);\n"
		"struct Input {\n";
	for (int i = 0; i < position; i++)
		src += "\tfloat4 pad" + std::to_string(i) + " : TEXCOORD" + std::to_string(i) + ";\n";
	src += "\tfloat4 pos : SV_Position;\n";
	if (primitive)
		src += "\tuint primitive : SV_PrimitiveID;\n";
	src += "\tbool front : SV_IsFrontFace;\n};\n";
	if (depth_test)
		src += "[earlydepthstencil]\n";
	src +=
		"void main(Input i)\n{\n"
		"\tif (any(uint2(i.pos.xy) != pick_pixel))\n\t\treturn;\n"
		"\tuint slot;\n"
		"\tpick_hits.InterlockedAdd(0, 1, slot);\n"
		"\tif (slot < 256)\n";
	src += primitive ? "\t\tpick_hits.Store4(16 + slot * 16, uint4(pick_record, i.primitive, asuint(i.pos.z), pick_flags | (i.front ? 2 : 0)));\n"
	                 : "\t\tpick_hits.Store4(16 + slot * 16, uint4(pick_record, 0xffffffff, asuint(i.pos.z), pick_flags | (i.front ? 2 : 0)));\n";
	src += "}\n";

	ID3DBlob *blob = NULL, *errors = NULL;
	ID3D11PixelShader *ps = NULL;
	HRESULT hr = D3DCompile(src.data(), src.size(), "DesktopPick", NULL, NULL, "main", "ps_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
	if (SUCCEEDED(hr)) {
		std::vector<Dx12SignatureElement> inputs;
		bool linked = false;
		if (Dx12ReadSignature(blob->GetBufferPointer(), blob->GetBufferSize(), false, &inputs)) {
			for (const Dx12SignatureElement &e : inputs) {
				if (e.system_value == SYSTEM_VALUE_POSITION)
					linked = e.reg == (UINT)position;
			}
		}
		if (linked)
			resource_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &ps);
		else
			LogInfo("Desktop pick: compiled pixel shader has SV_Position away from v%d\n", position);
	} else {
		LogInfo("Desktop pick: pixel shader for v%d does not compile: %s\n", position,
			errors ? (const char*)errors->GetBufferPointer() : "");
	}
	if (blob)
		blob->Release();
	if (errors)
		errors->Release();
	pixel_shaders[key] = ps;
	return ps;
}

bool shader_info(ID3D11DeviceChild *shader, ShaderInfo *info)
{
	bool found = false;
	AcquireSRWLockShared(&shader_lock);
	auto i = shader_infos.find(shader);
	if (i != shader_infos.end()) {
		*info = i->second;
		found = true;
	}
	ReleaseSRWLockShared(&shader_lock);
	return found;
}

// The output register with SV_Position of the stage that feeds the
// rasterizer, and whether a geometry shader is that stage, which the pixel
// shader can then only have SV_PrimitiveID from if it writes it.
bool raster_position(ID3D11DeviceContext1 *c, int *position, bool *geometry, bool *replay_safe)
{
	ID3D11GeometryShader *gs = NULL;
	ID3D11DomainShader *ds = NULL;
	ID3D11HullShader *hs = NULL;
	ID3D11VertexShader *vs = NULL;
	ID3D11DeviceChild *last;
	ShaderInfo info;
	bool known;

	c->GSGetShader(&gs, NULL, NULL);
	c->DSGetShader(&ds, NULL, NULL);
	c->HSGetShader(&hs, NULL, NULL);
	c->VSGetShader(&vs, NULL, NULL);
	*geometry = !!gs;
	last = gs ? (ID3D11DeviceChild*)gs : ds ? (ID3D11DeviceChild*)ds : vs;
	known = last && shader_info(last, &info) && info.position >= 0 && info.position < 32;
	if (known)
		*position = info.position;
	*replay_safe = true;
	for (ID3D11DeviceChild *shader : { (ID3D11DeviceChild*)vs, (ID3D11DeviceChild*)hs,
			(ID3D11DeviceChild*)ds, (ID3D11DeviceChild*)gs }) {
		if (!shader)
			continue;
		if (!shader_info(shader, &info) || !info.replay_safe)
			*replay_safe = false;
		shader->Release();
	}
	return known;
}

bool viewport_eligible(const D3D11_VIEWPORT &vp)
{
	if (vp.Width < 1 || vp.Height < 1 || vp.Width * 2 < window_width)
		return false;
	float aspect = (vp.Width / vp.Height) / ((float)window_width / window_height);
	return aspect > 0.98f && aspect < 1.02f;
}

// The hash and size of the view's resource
void describe_view(ID3D11View *view, uint32_t *hash, uint32_t *orig_hash, uint32_t *width, uint32_t *height)
{
	ID3D11Resource *resource = NULL;
	D3D11_RESOURCE_DIMENSION dimension;

	*width = *height = 0;
	view->GetResource(&resource);
	if (!resource) {
		*hash = *orig_hash = 0;
		return;
	}
	*hash = GetResourceHash(resource);
	if (orig_hash)
		*orig_hash = GetOrigResourceHash(resource);
	resource->GetType(&dimension);
	switch (dimension) {
	case D3D11_RESOURCE_DIMENSION_BUFFER: {
		D3D11_BUFFER_DESC d;
		((ID3D11Buffer*)resource)->GetDesc(&d);
		*width = d.ByteWidth;
		*height = 1;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
		D3D11_TEXTURE1D_DESC d;
		((ID3D11Texture1D*)resource)->GetDesc(&d);
		*width = d.Width;
		*height = 1;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
		D3D11_TEXTURE2D_DESC d;
		((ID3D11Texture2D*)resource)->GetDesc(&d);
		*width = d.Width;
		*height = d.Height;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
		D3D11_TEXTURE3D_DESC d;
		((ID3D11Texture3D*)resource)->GetDesc(&d);
		*width = d.Width;
		*height = d.Height;
		break;
	}
	}
	resource->Release();
}

typedef void (STDMETHODCALLTYPE ID3D11DeviceContext::*GetResourcesFn)(UINT, UINT, ID3D11ShaderResourceView**);
const GetResourcesFn get_resources[5] = {
	&ID3D11DeviceContext::VSGetShaderResources,
	&ID3D11DeviceContext::HSGetShaderResources,
	&ID3D11DeviceContext::DSGetShaderResources,
	&ID3D11DeviceContext::GSGetShaderResources,
	&ID3D11DeviceContext::PSGetShaderResources,
};

UINT64 game_shader_hash(ID3D11DeviceChild *handle)
{
	UINT64 hash = 0;
	if (!handle)
		return 0;
	EnterCriticalSectionPretty(&G->mCriticalSection);
	ShaderMap::iterator i = lookup_shader_hash(handle);
	if (i != G->mShaders.end())
		hash = i->second;
	LeaveCriticalSection(&G->mCriticalSection);
	return hash;
}

Buffer buffer_of(HackerContext *context, ID3D11Buffer *buffer, UINT region_offset, UINT region_size,
		UINT offset, UINT format_or_stride)
{
	Buffer b;
	b.hash = GetResourceHash(buffer);
	b.orig_hash = GetOrigResourceHash(buffer);
	b.offset = offset;
	b.format_or_stride = format_or_stride;
	// Preserve the region matched by TextureOverride before the buffer can be updated.
	if (G->track_region_hashes && region_size) {
		b.region_hash = GetRegionHash(context, buffer, region_offset, region_size);
		b.has_region = b.region_hash != 0;
	}
	return b;
}

void take_snapshot(HackerContext *context, ID3D11DeviceContext1 *c, DrawContext &data)
{
	Snapshot s = {};
	ID3D11Buffer *ib = NULL;
	DXGI_FORMAT ib_format;
	UINT ib_offset;
	ID3D11Buffer *vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT], offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *dsv = NULL;

	c->IAGetIndexBuffer(&ib, &ib_format, &ib_offset);
	if (ib) {
		if (data.call_info.type == DrawCall::DrawIndexed || data.call_info.type == DrawCall::DrawIndexedInstanced
				|| data.call_info.type == DrawCall::DrawIndexedInstancedIndirect) {
			s.has_ib = true;
			s.ib = buffer_of(context, ib, GetIndexBufferRegionOffset(ib_format, &data.call_info, ib_offset),
				GetIndexBufferRegionSize(ib_format, &data.call_info), ib_offset, ib_format);
		}
		ib->Release();
	}

	c->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, vbs, strides, offsets);
	for (UINT i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
		if (!vbs[i])
			continue;
		VertexBuffer vb;
		vb.slot = (uint8_t)i;
		vb.buffer = buffer_of(context, vbs[i], GetVertexBufferRegionOffset(strides[i], &data.call_info, offsets[i]),
			GetVertexBufferRegionSize(strides[i], &data.call_info), offsets[i], strides[i]);
		s.vbs.push_back(vb);
		vbs[i]->Release();
	}

	ID3D11DeviceChild *handles[5] = {
		context->mCurrentVertexShaderHandle, context->mCurrentHullShaderHandle, context->mCurrentDomainShaderHandle,
		context->mCurrentGeometryShaderHandle, context->mCurrentPixelShaderHandle,
	};
	for (int stage = 0; stage < 5; stage++) {
		s.shader_hashes[stage] = game_shader_hash(handles[stage]);

		(c->*get_resources[stage])(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views);
		for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; slot++) {
			if (!views[slot])
				continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC vd;
			Texture t = {};
			views[slot]->GetDesc(&vd);
			t.stage = (uint8_t)stage;
			t.slot = (uint8_t)slot;
			t.dimension = (uint8_t)vd.ViewDimension;
			t.format = vd.Format;
			describe_view(views[slot], &t.hash, &t.orig_hash, &t.width, &t.height);
			s.srvs.push_back(t);
			views[slot]->Release();
		}
	}

	c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
	for (UINT slot = 0; slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; slot++) {
		if (!rtvs[slot])
			continue;
		D3D11_RENDER_TARGET_VIEW_DESC vd;
		Target t = {};
		rtvs[slot]->GetDesc(&vd);
		t.slot = (uint8_t)slot;
		t.surface.format = vd.Format;
		describe_view(rtvs[slot], &t.surface.hash, NULL, &t.surface.width, &t.surface.height);
		s.rts.push_back(t);
		rtvs[slot]->Release();
	}
	if (dsv) {
		D3D11_DEPTH_STENCIL_VIEW_DESC vd;
		dsv->GetDesc(&vd);
		s.has_ds = true;
		s.ds.format = vd.Format;
		describe_view(dsv, &s.ds.hash, NULL, &s.ds.width, &s.ds.height);
		dsv->Release();
	}

	current_snapshot = (int)snapshots.size();
	snapshots.push_back(std::move(s));
}

// The game's shaders are replaced when what is bound is not what the game
// set, or is what ShaderFixes made of it at creation
void note_replaced(HackerContext *context, ID3D11DeviceContext1 *c, Snapshot &s)
{
	ID3D11VertexShader *vs = NULL;
	ID3D11HullShader *hs = NULL;
	ID3D11DomainShader *ds = NULL;
	ID3D11GeometryShader *gs = NULL;
	ID3D11PixelShader *ps = NULL;
	ShaderInfo info;

	c->VSGetShader(&vs, NULL, NULL);
	c->HSGetShader(&hs, NULL, NULL);
	c->DSGetShader(&ds, NULL, NULL);
	c->GSGetShader(&gs, NULL, NULL);
	c->PSGetShader(&ps, NULL, NULL);
	ID3D11DeviceChild *bound[5] = { vs, hs, ds, gs, ps };
	ID3D11DeviceChild *handles[5] = {
		context->mCurrentVertexShaderHandle, context->mCurrentHullShaderHandle, context->mCurrentDomainShaderHandle,
		context->mCurrentGeometryShaderHandle, context->mCurrentPixelShaderHandle,
	};
	for (int stage = 0; stage < 5; stage++) {
		if (!s.shader_hashes[stage])
			continue;
		s.replaced[stage] = bound[stage] != handles[stage] || (bound[stage] && shader_info(bound[stage], &info) && info.replaced);
	}
	for (ID3D11DeviceChild *shader : bound) {
		if (shader)
			shader->Release();
	}
}

uint8_t depth_func(ID3D11DepthStencilState *dss)
{
	// Without a state D3D11 tests with LESS
	if (!dss)
		return D3D11_COMPARISON_LESS;
	D3D11_DEPTH_STENCIL_DESC d;
	dss->GetDesc(&d);
	return d.DepthEnable ? (uint8_t)d.DepthFunc : 0;
}

ID3D11DepthStencilState* read_only_depth_state(ID3D11DeviceContext1 *c)
{
	ID3D11DepthStencilState *source = NULL;
	UINT stencil_ref;
	c->OMGetDepthStencilState(&source, &stencil_ref);
	auto found = depth_states.find(source);
	if (found != depth_states.end()) {
		if (source)
			source->Release();
		return found->second;
	}
	D3D11_DEPTH_STENCIL_DESC desc = {};
	if (source)
		source->GetDesc(&desc);
	else {
		desc.DepthEnable = TRUE;
		desc.DepthFunc = D3D11_COMPARISON_LESS;
		desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
		desc.FrontFace.StencilFunc = desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	}
	desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	desc.StencilWriteMask = 0;
	desc.FrontFace.StencilFailOp = desc.FrontFace.StencilDepthFailOp = desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	desc.BackFace.StencilFailOp = desc.BackFace.StencilDepthFailOp = desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	ID3D11DepthStencilState *readonly = NULL;
	if (FAILED(resource_device->CreateDepthStencilState(&desc, &readonly))) {
		if (source)
			source->Release();
		return NULL;
	}
	// Keep the source alive so a reused COM address cannot select another state's tests.
	depth_states[source] = readonly;
	return readonly;
}

void issue(ID3D11DeviceContext1 *c, const DesktopPickDraw &d)
{
	switch (d.call) {
	case CALL_DRAW:
		c->Draw(d.count, d.start);
		break;
	case CALL_DRAW_INDEXED:
		c->DrawIndexed(d.count, d.start, d.base_vertex);
		break;
	case CALL_DRAW_INSTANCED:
		c->DrawInstanced(d.count, d.instances, d.start, d.start_instance);
		break;
	case CALL_DRAW_INDEXED_INSTANCED:
		c->DrawIndexedInstanced(d.count, d.instances, d.start, d.base_vertex, d.start_instance);
		break;
	case CALL_DRAW_AUTO:
		c->DrawAuto();
		break;
	case CALL_DRAW_INSTANCED_INDIRECT:
		c->DrawInstancedIndirect(d.args, d.args_offset);
		break;
	case CALL_DRAW_INDEXED_INSTANCED_INDIRECT:
		c->DrawIndexedInstancedIndirect(d.args, d.args_offset);
		break;
	}
}

// Probes the draw before it changes depth or stencil, with our pixel shader and no render
// targets, and restores the output merger, the pixel shader and its constant
// buffer and the depth stencil state after
void redraw(ID3D11DeviceContext1 *c, PickRecord &&r)
{
	ID3D11DepthStencilView *dsv = NULL;
	D3D11_VIEWPORT vp;
	UINT viewports = 1;
	ID3D11Buffer *so[D3D11_SO_BUFFER_SLOT_COUNT];
	int position;
	bool geometry, replay_safe, so_bound = false;

	if (capture_context->HasActiveDesktopQueries()) {
		skipped.query++;
		return;
	}
	c->OMGetRenderTargets(0, NULL, &dsv);
	if (!dsv) {
		skipped.no_depth++;
		return;
	}
	dsv->Release();
	c->RSGetViewports(&viewports, &vp);
	if (!viewports || !viewport_eligible(vp)) {
		skipped.viewport++;
		return;
	}
	c->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, so);
	for (ID3D11Buffer *b : so) {
		if (b) {
			so_bound = true;
			b->Release();
		}
	}
	if (so_bound) {
		skipped.stream_output++;
		return;
	}
	if (!raster_position(c, &position, &geometry, &replay_safe)) {
		skipped.position++;
		return;
	}
	// Graphics stages share OM UAVs. Replaying them can read different inputs
	// after the bindings change, or repeat writes and atomics to game data.
	if (!replay_safe) {
		skipped.shader_uav++;
		return;
	}
	ID3D11PixelShader *cover = pixel_shader(position, false, !geometry);
	ID3D11PixelShader *tested = pixel_shader(position, true, !geometry);
	ID3D11DepthStencilState *test_state = read_only_depth_state(c);
	if (!cover || !tested || !test_state) {
		skipped.shader++;
		return;
	}

	PsParams params;
	params.record = (UINT)records.size();
	params.flags = 0;
	params.pixel[0] = (UINT)max(0.0f, vp.TopLeftX + cursor_x * vp.Width);
	params.pixel[1] = (UINT)max(0.0f, vp.TopLeftY + cursor_y * vp.Height);

	D3D11_PRIMITIVE_TOPOLOGY topology;
	c->IAGetPrimitiveTopology(&topology);
	r.topology = (uint8_t)topology;

	ID3D11PixelShader *ps = NULL;
	ID3D11ClassInstance *instances[256];
	UINT instance_count = 256;
	ID3D11Buffer *cb = NULL;
	UINT cb_first = 0, cb_count = 0;
	ID3D11DepthStencilState *dss = NULL;
	UINT stencil_ref = 0;
	OMState om;
	static const UINT keep_counts[1] = { (UINT)-1 };

	c->PSGetShader(&ps, instances, &instance_count);
	if (constant_buffer_offsetting)
		c->PSGetConstantBuffers1(PS_PARAMS_SLOT, 1, &cb, &cb_first, &cb_count);
	else
		c->PSGetConstantBuffers(PS_PARAMS_SLOT, 1, &cb);
	c->OMGetDepthStencilState(&dss, &stencil_ref);
	save_om_state(c, &om);
	r.depth_func = depth_func(dss);

	c->PSSetConstantBuffers(PS_PARAMS_SLOT, 1, &params_buffer);
	c->OMSetRenderTargetsAndUnorderedAccessViews(0, NULL, om.dsv, 0, 1, &hit_uav, keep_counts);

	c->UpdateSubresource(params_buffer, 0, NULL, &params, 0, 0);
	c->PSSetShader(cover, NULL, 0);
	c->OMSetDepthStencilState(cover_state, 0);
	issue(c, r.draw);

	// Exclude passes whose depth test cannot distinguish scene geometry.
	if (r.depth_func != 0 && r.depth_func != D3D11_COMPARISON_NEVER && r.depth_func != D3D11_COMPARISON_ALWAYS) {
		params.flags = HIT_DEPTH_PASSED;
		c->UpdateSubresource(params_buffer, 0, NULL, &params, 0, 0);
		c->PSSetShader(tested, NULL, 0);
		c->OMSetDepthStencilState(test_state, stencil_ref);
		issue(c, r.draw);
	}

	restore_om_state(c, &om);
	c->OMSetDepthStencilState(dss, stencil_ref);
	if (dss)
		dss->Release();
	if (constant_buffer_offsetting)
		c->PSSetConstantBuffers1(PS_PARAMS_SLOT, 1, &cb, &cb_first, &cb_count);
	else
		c->PSSetConstantBuffers(PS_PARAMS_SLOT, 1, &cb);
	if (cb)
		cb->Release();
	c->PSSetShader(ps, instances, instance_count);
	if (ps)
		ps->Release();
	for (UINT i = 0; i < instance_count; i++) {
		if (instances[i])
			instances[i]->Release();
	}

	records.push_back(std::move(r));
}

bool cursor_position(HWND window, float *x, float *y)
{
	RECT client;
	if (!window || !CursorUpscalingBypass_GetClientRect(window, &client))
		return false;
	window_width = client.right - client.left;
	window_height = client.bottom - client.top;
	if (!window_width || !window_height)
		return false;
	if (request.cursor_given) {
		*x = request.x;
		*y = request.y;
		return true;
	}
	CURSORINFO info = { sizeof(info) };
	if (!CursorUpscalingBypass_GetCursorInfo(&info))
		return false;
	POINT p = info.ptScreenPos;
	if (!CursorUpscalingBypass_ScreenToClient(window, &p))
		return false;
	*x = (p.x + 0.5f) / window_width;
	*y = (p.y + 0.5f) / window_height;
	return true;
}

void begin_capture(HackerDevice *device, HackerContext *context, HWND window)
{
	ID3D11Device1 *d = device->GetPassThroughOrigDevice1();
	ID3D11DeviceContext1 *c = context->GetPassThroughOrigContext1();

	if (d->GetDeviceRemovedReason() != S_OK) {
		fail("DEVICE_LOST", "");
		return;
	}
	if (!create_resources(d)) {
		fail("NO_ELIGIBLE_DRAW", "the device cannot write to a buffer from a pixel shader");
		return;
	}
	if (!cursor_position(window, &cursor_x, &cursor_y)) {
		fail("NO_ELIGIBLE_DRAW", "no game window");
		return;
	}
	if (cursor_x < 0 || cursor_x >= 1 || cursor_y < 0 || cursor_y >= 1) {
		fail("NO_ELIGIBLE_DRAW", "the cursor is outside the game window");
		return;
	}

	static const UINT zero[4] = {};
	{
		UnconditionalCommands unconditional(c);
		c->ClearUnorderedAccessViewUint(hit_uav, zero);
	}

	release_capture();
	capture_context = context;
	capture_orig = c;
	capture_context->AddRef();
	capture_frame = G->frame_no;
	snapshots.reserve(4096);
	records.reserve(4096);
	deferred_skipped = false;
	draws_seen = 0;
	skipped = {};
	hook_ticks.QuadPart = 0;
	state = State::CAPTURING;
	desktop_pick_capturing = true;
}

void end_capture()
{
	desktop_pick_capturing = false;
	current_snapshot = -1;
	in_game_draw = false;

	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	LogInfo("Desktop pick %llu: %u draws, %zu snapshots, %zu redrawn, %.2f ms in the hooks\n",
		request.capture_id, draws_seen, snapshots.size(), records.size(),
		hook_ticks.QuadPart * 1000.0 / frequency.QuadPart);

	if (records.empty()) {
		char detail[256];
		_snprintf_s(detail, _TRUNCATE, "%u draws: %u active query, %u without depth, %u viewport, %u stream output, %u position unknown, %u shader, %u shader UAV or unknown bindings",
			draws_seen, skipped.query, skipped.no_depth, skipped.viewport, skipped.stream_output, skipped.position, skipped.shader, skipped.shader_uav);
		fail("NO_ELIGIBLE_DRAW", detail);
		return;
	}
	{
		UnconditionalCommands unconditional(capture_orig);
		capture_orig->CopyResource(hit_staging, hit_buffer);
	}
	readback_waited = 0;
	state = State::READBACK;
}

std::string bare_line(const std::wstring &line)
{
	size_t at = line.find(L"] ");
	return DesktopUtf8(at == line.npos ? line.c_str() : line.c_str() + at + 2);
}

void section_origin(const std::wstring &section, std::string *name, std::string *path)
{
	std::wstring bare, ini;
	GetSectionOrigin(section.c_str(), &bare, &ini);
	*name = DesktopUtf8(bare.c_str());
	*path = DesktopUtf8(ini.c_str());
}

void fill_record(Record *out, uint32_t id, const PickRecord &r)
{
	out->id = id;
	out->origin = r.origin;
	out->draw_number = r.draw_number;
	out->call = r.draw.call;
	out->count = r.draw.count;
	out->instances = r.draw.instances;
	out->start = r.draw.start;
	out->base_vertex = r.draw.base_vertex;
	out->start_instance = r.draw.start_instance;
	out->topology = r.topology;
	out->depth_func = r.depth_func;
	if (r.origin == ORIGIN_COMMAND_LIST) {
		section_origin(r.section, &out->command_list.section, &out->command_list.ini_path);
		out->command_list.line = bare_line(r.line);
	}
	if (r.snapshot < 0)
		return;

	Snapshot &s = snapshots[r.snapshot];
	out->has_ib = s.has_ib;
	out->ib = s.ib;
	out->vbs = s.vbs;
	for (int stage = 0; stage < 5; stage++) {
		out->shaders[stage].hash = s.shader_hashes[stage];
		out->shaders[stage].replaced = s.replaced[stage];
	}
	out->srvs = s.srvs;
	out->rts = s.rts;
	out->has_ds = s.has_ds;
	out->ds = s.ds;
	for (auto &o : s.overrides) {
		Override ov;
		ov.kind = o.first;
		section_origin(o.second, &ov.section, &ov.ini_path);
		out->overrides.push_back(ov);
	}
}

void poll_readback()
{
	D3D11_MAPPED_SUBRESOURCE map;
	HRESULT hr = capture_orig->Map(hit_staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
		if (++readback_waited >= READBACK_FRAMES)
			fail("READBACK_TIMEOUT", std::to_string(READBACK_FRAMES) + " frames");
		return;
	}
	if (FAILED(hr)) {
		if (resource_device->GetDeviceRemovedReason() != S_OK)
			fail("DEVICE_LOST", "");
		else
			fail("READBACK_TIMEOUT", "Map failed");
		return;
	}

	PickResult result;
	const UINT *words = (const UINT*)map.pData;
	UINT total = words[0];
	UINT count = min(total, HIT_CAPACITY);
	result.hits.resize(count);
	for (UINT i = 0; i < count; i++) {
		const UINT *hit = words + 4 + i * 4;
		result.hits[i].record = hit[0];
		result.hits[i].primitive = hit[1];
		memcpy(&result.hits[i].depth, &hit[2], 4);
		result.hits[i].flags = (uint8_t)hit[3];
	}
	capture_orig->Unmap(hit_staging, 0);

	result.capture_id = request.capture_id;
	result.frame = capture_frame;
	result.trigger = request.trigger;
	result.window_width = window_width;
	result.window_height = window_height;
	result.cursor_x = cursor_x;
	result.cursor_y = cursor_y;
	result.hit_total = total;
	if (total > HIT_CAPACITY)
		result.flags |= PICK_HITS_OVERFLOW;
	if (deferred_skipped)
		result.flags |= PICK_DEFERRED_SKIPPED;

	std::set<uint32_t> referenced;
	for (const Hit &hit : result.hits) {
		if (hit.record < records.size())
			referenced.insert(hit.record);
	}
	for (uint32_t id : referenced) {
		result.records.emplace_back();
		fill_record(&result.records.back(), id, records[id]);
	}

	std::vector<uint8_t> frame;
	if (EncodeFrame(MessageType::PickResult, 0, result, &frame)) {
		DesktopChannelQueue(DesktopSend::RESULT, std::move(frame), request.connection);
		LogInfo("Desktop pick %llu: %u hits, %zu records sent\n", request.capture_id, total, result.records.size());
		release_capture();
		state = State::IDLE;
	} else {
		fail("NO_ELIGIBLE_DRAW", "the result does not fit in a frame");
	}
}

} // namespace

DesktopPickDraw DesktopPickDrawOf(const DrawCallInfo &info)
{
	DesktopPickDraw d = {};
	switch (info.type) {
	case DrawCall::Draw:
		d = { CALL_DRAW, info.VertexCount, 1, info.FirstVertex, 0, 0, NULL, 0 };
		break;
	case DrawCall::DrawIndexed:
		d = { CALL_DRAW_INDEXED, info.IndexCount, 1, info.FirstIndex, (INT)info.FirstVertex, 0, NULL, 0 };
		break;
	case DrawCall::DrawInstanced:
		d = { CALL_DRAW_INSTANCED, info.VertexCount, info.InstanceCount, info.FirstVertex, 0, info.FirstInstance, NULL, 0 };
		break;
	case DrawCall::DrawIndexedInstanced:
		d = { CALL_DRAW_INDEXED_INSTANCED, info.IndexCount, info.InstanceCount, info.FirstIndex, (INT)info.FirstVertex,
			info.FirstInstance, NULL, 0 };
		break;
	case DrawCall::DrawAuto:
		d.call = CALL_DRAW_AUTO;
		break;
	case DrawCall::DrawInstancedIndirect:
		if (info.indirect_buffer)
			d = { CALL_DRAW_INSTANCED_INDIRECT, 0, 0, 0, 0, 0, *info.indirect_buffer, info.args_offset };
		break;
	case DrawCall::DrawIndexedInstancedIndirect:
		if (info.indirect_buffer)
			d = { CALL_DRAW_INDEXED_INSTANCED_INDIRECT, 0, 0, 0, 0, 0, *info.indirect_buffer, info.args_offset };
		break;
	}
	return d;
}

void DesktopNoteShader(ID3D11DeviceChild *shader, const void *bytecode, SIZE_T size, bool replaced)
{
	std::vector<Dx12SignatureElement> outputs;
	ShaderInfo info = { -1, replaced, false };

	if (!desktop_channel_enabled || !shader)
		return;
	ShaderBindings bindings;
	bool uses_uav = true;
	info.replay_safe = get_shader_bindings_from_bytecode(bytecode, size, &bindings, &uses_uav) && !uses_uav;
	if (Dx12ReadSignature(bytecode, size, true, &outputs)) {
		for (const Dx12SignatureElement &e : outputs) {
			if (e.system_value == SYSTEM_VALUE_POSITION)
				info.position = (int)e.reg;
		}
	}
	AcquireSRWLockExclusive(&shader_lock);
	shader_infos[shader] = info;
	ReleaseSRWLockExclusive(&shader_lock);
}

uint64_t DesktopPickArm(uint8_t trigger, bool cursor_given, float x, float y, uint32_t connection)
{
	if (state != State::IDLE)
		return 0;
	request.capture_id = next_capture_id++;
	request.connection = connection;
	request.trigger = trigger;
	request.cursor_given = cursor_given;
	request.x = x;
	request.y = y;
	state = State::ARMED;
	return request.capture_id;
}

void DesktopPickFrame(HackerDevice *device, HackerContext *context, HWND window)
{
	if (state == State::CAPTURING)
		end_capture();
	else if (state == State::READBACK)
		poll_readback();
	if (state == State::ARMED) {
		if (device && context)
			begin_capture(device, context, window);
		else
			fail("NOT_D3D11", "");
	}
}

void DesktopPickCancel()
{
	if (state == State::IDLE)
		return;
	desktop_pick_capturing = false;
	fail("NO_ELIGIBLE_DRAW", "the presenting swap chain was released");
}

void DesktopPickExecuteCommandList(HackerContext *context)
{
	if (context == capture_context)
		deferred_skipped = true;
}

void DesktopPickBeforeDraw(HackerContext *context, DrawContext &data)
{
	LARGE_INTEGER start;
	ID3D11DepthStencilView *dsv = NULL;
	D3D11_VIEWPORT vp;
	UINT viewports = 1;

	if (context != capture_context)
		return;
	QueryPerformanceCounter(&start);
	draws_seen++;
	in_game_draw = true;
	current_draw_number = context->GetDrawNumber();
	current_snapshot = -1;

	// Only for draws that can be picked, unless a command list rebinds
	// the depth target or the viewport
	capture_orig->OMGetRenderTargets(0, NULL, &dsv);
	if (dsv) {
		dsv->Release();
		capture_orig->RSGetViewports(&viewports, &vp);
		if (viewports && viewport_eligible(vp))
			take_snapshot(context, capture_orig, data);
	}
	add_ticks(start);
}

void DesktopPickGameDraw(HackerContext *context, DrawContext &data)
{
	LARGE_INTEGER start;

	if (context != capture_context)
		return;
	QueryPerformanceCounter(&start);
	if (current_snapshot >= 0)
		note_replaced(context, capture_orig, snapshots[current_snapshot]);
	if (!data.call_info.skip) {
		PickRecord r = {};
		r.origin = ORIGIN_GAME;
		r.draw_number = current_draw_number;
		r.snapshot = current_snapshot;
		r.draw = DesktopPickDrawOf(data.call_info);
		if (r.draw.call)
			redraw(capture_orig, std::move(r));
	}
	add_ticks(start);
}

void DesktopPickAfterDrawEnd(HackerContext *context)
{
	if (context != capture_context)
		return;
	in_game_draw = false;
	current_snapshot = -1;
	current_draw_number = 0;
}

void DesktopPickCommandDraw(CommandListState *state, const std::wstring &section, const std::wstring &ini_line,
		const DesktopPickDraw &draw)
{
	LARGE_INTEGER start;

	if (state->mHackerContext != capture_context)
		return;
	if (!draw.call)
		return;
	QueryPerformanceCounter(&start);
	PickRecord r = {};
	r.origin = ORIGIN_COMMAND_LIST;
	r.draw_number = in_game_draw ? current_draw_number : 0;
	r.snapshot = in_game_draw ? current_snapshot : -1;
	r.draw = draw;
	r.section = section;
	r.line = ini_line;
	redraw(capture_orig, std::move(r));
	add_ticks(start);
}

void DesktopPickNoteOverride(HackerContext *context, uint8_t kind, const std::wstring &section)
{
	if (context != capture_context || current_snapshot < 0 || section.empty())
		return;
	auto &overrides = snapshots[current_snapshot].overrides;
	for (auto &o : overrides) {
		if (o.first == kind && o.second == section)
			return;
	}
	overrides.emplace_back(kind, section);
}
