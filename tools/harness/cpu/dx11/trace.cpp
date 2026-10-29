#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <d3d11_3.h>
#include <unordered_map>
#include <map>
#include <vector>
#include "trace.h"

typedef ID3D11DeviceContext1 CTX;
typedef ID3D11Device3 DEV;

static ID3D11DeviceContext1Vtbl orig;
static ID3D11Device3Vtbl dorig;
static bool enabled;
static SRWLOCK lock = SRWLOCK_INIT;
static void *main_context;

struct Lock {
	Lock() { AcquireSRWLockExclusive(&lock); }
	~Lock() { ReleaseSRWLockExclusive(&lock); }
};

static const uint64_t K = 0x9E3779B97F4A7C15ull;

static inline uint64_t mix(uint64_t h, uint64_t v)
{
	h = (h ^ v) * K;
	return h ^ (h >> 29);
}

static uint64_t hash_bytes(const void *data, size_t bytes)
{
	const uint8_t *p = (const uint8_t*)data;
	uint64_t h = mix(K, bytes);
	while (bytes >= 8) {
		uint64_t v;
		memcpy(&v, p, 8);
		h = mix(h, v);
		p += 8;
		bytes -= 8;
	}
	uint64_t v = 0;
	memcpy(&v, p, bytes);
	return mix(h, v);
}

// ----------------------------------------------------------------- objects

struct Res {
	uint64_t content, shape;
	int kind; // 1 buffer, 2 texture 1D, 3 2D, 4 3D
	UINT bytes, width, height, depth, mips;
	DXGI_FORMAT format;
};
struct View {
	void *resource;
	uint64_t desc;
};
struct Mapped {
	void *data;
	UINT row_pitch, depth_pitch;
	D3D11_MAP type;
};

static std::unordered_map<void*, Res> resources;
static std::unordered_map<void*, View> views;
static std::unordered_map<void*, uint64_t> objects;
static std::map<std::pair<void*, UINT>, Mapped> mapped;
static unsigned unknown_count;

static void format_block(DXGI_FORMAT f, UINT *block, UINT *bytes)
{
	*block = 1;
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
		*bytes = 16; return;
	case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT: case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
		*bytes = 12; return;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		*bytes = 8; return;
	case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT: case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM: case DXGI_FORMAT_R8G8_SINT: case DXGI_FORMAT_B5G6R5_UNORM: case DXGI_FORMAT_B5G5R5A1_UNORM:
		*bytes = 2; return;
	case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT: case DXGI_FORMAT_A8_UNORM:
		*bytes = 1; return;
	case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB: case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
		*block = 4; *bytes = 8; return;
	case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM: case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
		*block = 4; *bytes = 16; return;
	case DXGI_FORMAT_UNKNOWN:
		*bytes = 0; return;
	default:
		// Every other format in use here is 32 bits a texel
		*bytes = 4; return;
	}
}

static FILE *text;

static Res *resource_of(void *p)
{
	if (!p)
		return NULL;
	auto i = resources.find(p);
	if (i != resources.end())
		return &i->second;
	// Made where nothing was listening (a back buffer of the swap chain):
	// known by what it is
	Res r = {};
	D3D11_RESOURCE_DIMENSION dim;
	ID3D11Resource *res = (ID3D11Resource*)p;
	ID3D11Resource_GetType(res, &dim);
	if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
		D3D11_BUFFER_DESC d;
		ID3D11Buffer_GetDesc((ID3D11Buffer*)p, &d);
		r.kind = 1; r.bytes = d.ByteWidth;
		r.shape = hash_bytes(&d, sizeof(d));
	} else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		D3D11_TEXTURE2D_DESC d;
		ID3D11Texture2D_GetDesc((ID3D11Texture2D*)p, &d);
		r.kind = 3; r.width = d.Width; r.height = d.Height; r.depth = 1; r.mips = d.MipLevels; r.format = d.Format;
		r.shape = hash_bytes(&d, sizeof(d));
	} else {
		r.kind = (int)dim;
		r.shape = mix(K, dim);
	}
	r.content = mix(r.shape, 0x756e6b6e);
	unknown_count++;
	if (text)
		fprintf(text, "unknown resource %p kind %d %u bytes %ux%u format %d -> %08x\n", p, r.kind, r.bytes, r.width, r.height, r.format, (unsigned)r.content);
	return &(resources[p] = r);
}

static uint64_t rid(void *p)
{
	Res *r = resource_of(p);
	return r ? r->content : 0;
}

static void *view_resource(void *v)
{
	if (!v)
		return NULL;
	auto i = views.find(v);
	if (i != views.end())
		return i->second.resource;
	ID3D11Resource *res = NULL;
	ID3D11View_GetResource((ID3D11View*)v, &res);
	if (res)
		ID3D11Resource_Release(res);
	views[v] = { res, 0x766965 };
	unknown_count++;
	if (text)
		fprintf(text, "unknown view %p of %p\n", v, res);
	return res;
}

static uint64_t vid(void *v)
{
	if (!v)
		return 0;
	void *res = view_resource(v);
	return mix(views[v].desc, rid(res));
}

static uint64_t oid(void *o)
{
	if (!o)
		return 0;
	auto i = objects.find(o);
	if (i != objects.end())
		return i->second;
	unknown_count++;
	return objects[o] = mix(0x6f626a, objects.size());
}

static void touch(void *resource, uint64_t by)
{
	Res *r = resource_of(resource);
	if (r)
		r->content = mix(r->content, by);
}

// ------------------------------------------------------------ accumulators

static uint64_t frame_strict = K, frame_effective = K;
static unsigned frame_calls, frame_actions, frame_draws, frame_no;
static unsigned text_first, text_last;

static void strict(uint64_t op, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0)
{
	frame_strict = mix(mix(mix(mix(frame_strict, op), a), b), c);
	frame_calls++;
}

static bool texting()
{
	return text && frame_no >= text_first && frame_no <= text_last;
}

static void effective(const char *what, uint64_t h)
{
	frame_effective = mix(frame_effective, h);
	frame_strict = mix(frame_strict, h);
	frame_calls++;
	frame_actions++;
	if (texting())
		fprintf(text, "%u %u %s %016llx\n", frame_no, frame_actions, what, (unsigned long long)h);
}

// ------------------------------------------------------------ shadow state

struct Stage {
	void *shader;
	void *cb[14];
	UINT cb_first[14], cb_num[14];
	void *srv[128];
	void *sampler[16];
	int srvs;
};
enum { VS, HS, DS, GS, PS, CS };
static struct State {
	Stage stage[6];
	void *layout, *vb[32], *ib;
	UINT vb_stride[32], vb_offset[32], ib_offset;
	DXGI_FORMAT ib_format;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	void *rtv[8], *dsv, *uav[8], *cs_uav[8];
	UINT uav_start;
	void *blend, *depth, *raster;
	float blend_factor[4];
	UINT sample_mask, stencil_ref;
	D3D11_VIEWPORT viewport[16];
	D3D11_RECT scissor[16];
	UINT viewports, scissors;
	void *so[4];
	UINT so_offset[4];
} S;

static uint64_t stage_hash(int i)
{
	Stage &s = S.stage[i];
	uint64_t h = mix(K + i, oid(s.shader));
	for (int k = 0; k < 14; k++)
		if (s.cb[k])
			h = mix(mix(mix(mix(h, k), rid(s.cb[k])), s.cb_first[k]), s.cb_num[k]);
	for (int k = 0; k < s.srvs; k++)
		if (s.srv[k])
			h = mix(mix(h, 100 + k), vid(s.srv[k]));
	for (int k = 0; k < 16; k++)
		if (s.sampler[k])
			h = mix(mix(h, 300 + k), oid(s.sampler[k]));
	return h;
}

static uint64_t ia_hash()
{
	uint64_t h = mix(mix(K, oid(S.layout)), S.topology);
	for (int k = 0; k < 32; k++)
		if (S.vb[k])
			h = mix(mix(mix(mix(h, k), rid(S.vb[k])), S.vb_stride[k]), S.vb_offset[k]);
	return mix(mix(mix(h, rid(S.ib)), S.ib_format), S.ib_offset);
}

static uint64_t om_hash()
{
	uint64_t h = K;
	for (int k = 0; k < 8; k++)
		h = mix(h, vid(S.rtv[k]));
	h = mix(h, vid(S.dsv));
	for (int k = 0; k < 8; k++)
		if (S.uav[k])
			h = mix(mix(h, 50 + k), vid(S.uav[k]));
	h = mix(mix(mix(h, oid(S.blend)), oid(S.depth)), oid(S.raster));
	h = mix(h, hash_bytes(S.blend_factor, sizeof(S.blend_factor)));
	h = mix(mix(h, S.sample_mask), S.stencil_ref);
	h = mix(h, hash_bytes(S.viewport, S.viewports * sizeof(D3D11_VIEWPORT)));
	h = mix(h, hash_bytes(S.scissor, S.scissors * sizeof(D3D11_RECT)));
	for (int k = 0; k < 4; k++)
		if (S.so[k])
			h = mix(mix(mix(h, 70 + k), rid(S.so[k])), S.so_offset[k]);
	return h;
}

static void draw_action(const char *what, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e)
{
	uint64_t parts[7];
	for (int i = VS; i <= PS; i++)
		parts[i] = stage_hash(i);
	parts[5] = ia_hash();
	parts[6] = om_hash();
	uint64_t h = hash_bytes(parts, sizeof(parts));
	h = mix(mix(mix(mix(mix(h, a), b), c), d), e);
	frame_draws++;
	if (texting()) {
		Stage &ps = S.stage[PS];
		fprintf(text, "  %s %llu %llu %lld %llu %llu | vs %08x hs %08x ds %08x gs %08x ps %08x ia %08x om %08x | vb %08x %08x %08x ib %08x+%u | t %08x %08x %08x %08x | rt %08x\n",
			what, a, b, (long long)c, d, e, (unsigned)parts[0], (unsigned)parts[1], (unsigned)parts[2], (unsigned)parts[3], (unsigned)parts[4],
			(unsigned)parts[5], (unsigned)parts[6], (unsigned)rid(S.vb[0]), (unsigned)rid(S.vb[1]), (unsigned)rid(S.vb[2]), (unsigned)rid(S.ib), S.ib_offset,
			(unsigned)vid(ps.srv[0]), (unsigned)vid(ps.srv[1]), (unsigned)vid(ps.srv[2]), (unsigned)vid(ps.srv[3]), (unsigned)vid(S.rtv[0]));
		fprintf(text, "    what is in t0..t3: %08x %08x %08x %08x\n", (unsigned)rid(view_resource(ps.srv[0])), (unsigned)rid(view_resource(ps.srv[1])),
			(unsigned)rid(view_resource(ps.srv[2])), (unsigned)rid(view_resource(ps.srv[3])));
	}
	effective(what, h);
	for (int k = 0; k < 8; k++)
		touch(view_resource(S.rtv[k]), mix(h, k));
	touch(view_resource(S.dsv), mix(h, 8));
	for (int k = 0; k < 8; k++)
		touch(view_resource(S.uav[k]), mix(h, 10 + k));
	for (int k = 0; k < 4; k++)
		touch(S.so[k], mix(h, 20 + k));
}

static inline bool on(CTX *c)
{
	if (!enabled)
		return false;
	if (!main_context)
		main_context = c;
	return c == main_context;
}

// ------------------------------------------------------------ the context

static void set_cbs(int stage, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *first, const UINT *num)
{
	Lock l;
	for (UINT i = 0; i < n && start + i < 14; i++) {
		Stage &s = S.stage[stage];
		s.cb[start + i] = b ? b[i] : NULL;
		s.cb_first[start + i] = first ? first[i] : 0;
		s.cb_num[start + i] = num ? num[i] : 0;
		strict(0x100 + stage, start + i, rid(s.cb[start + i]), mix(s.cb_first[start + i], s.cb_num[start + i]));
	}
}

static void set_srvs(int stage, UINT start, UINT n, ID3D11ShaderResourceView *const *v)
{
	Lock l;
	Stage &s = S.stage[stage];
	for (UINT i = 0; i < n && start + i < 128; i++) {
		s.srv[start + i] = v ? v[i] : NULL;
		if (s.srv[start + i] && (int)(start + i) >= s.srvs)
			s.srvs = start + i + 1;
		strict(0x110 + stage, start + i, vid(s.srv[start + i]));
	}
}

static void set_samplers(int stage, UINT start, UINT n, ID3D11SamplerState *const *v)
{
	Lock l;
	for (UINT i = 0; i < n && start + i < 16; i++) {
		S.stage[stage].sampler[start + i] = v ? v[i] : NULL;
		strict(0x120 + stage, start + i, oid(S.stage[stage].sampler[start + i]));
	}
}

static void set_shader(int stage, void *shader)
{
	Lock l;
	S.stage[stage].shader = shader;
	strict(0x130 + stage, oid(shader));
}

#define STAGE(P, Type, i) \
static void STDMETHODCALLTYPE H_##P##SetShader(CTX *c, ID3D11##Type##Shader *s, ID3D11ClassInstance *const *ci, UINT n) { \
	if (on(c)) set_shader(i, s); \
	orig.P##SetShader(c, s, ci, n); } \
static void STDMETHODCALLTYPE H_##P##SetConstantBuffers(CTX *c, UINT start, UINT n, ID3D11Buffer *const *b) { \
	if (on(c)) set_cbs(i, start, n, b, NULL, NULL); \
	orig.P##SetConstantBuffers(c, start, n, b); } \
static void STDMETHODCALLTYPE H_##P##SetConstantBuffers1(CTX *c, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *first, const UINT *num) { \
	if (on(c)) set_cbs(i, start, n, b, first, num); \
	orig.P##SetConstantBuffers1(c, start, n, b, first, num); } \
static void STDMETHODCALLTYPE H_##P##SetShaderResources(CTX *c, UINT start, UINT n, ID3D11ShaderResourceView *const *v) { \
	if (on(c)) set_srvs(i, start, n, v); \
	orig.P##SetShaderResources(c, start, n, v); } \
static void STDMETHODCALLTYPE H_##P##SetSamplers(CTX *c, UINT start, UINT n, ID3D11SamplerState *const *v) { \
	if (on(c)) set_samplers(i, start, n, v); \
	orig.P##SetSamplers(c, start, n, v); }

STAGE(VS, Vertex, VS)
STAGE(HS, Hull, HS)
STAGE(DS, Domain, DS)
STAGE(GS, Geometry, GS)
STAGE(PS, Pixel, PS)
STAGE(CS, Compute, CS)

static void STDMETHODCALLTYPE H_DrawIndexed(CTX *c, UINT count, UINT start, INT base)
{
	if (on(c)) { Lock l; draw_action("DrawIndexed", count, start, base, 0, 0); }
	orig.DrawIndexed(c, count, start, base);
}
static void STDMETHODCALLTYPE H_Draw(CTX *c, UINT count, UINT start)
{
	if (on(c)) { Lock l; draw_action("Draw", count, start, 0, 0, 1); }
	orig.Draw(c, count, start);
}
static void STDMETHODCALLTYPE H_DrawIndexedInstanced(CTX *c, UINT count, UINT instances, UINT start, INT base, UINT first_instance)
{
	if (on(c)) { Lock l; draw_action("DrawIndexedInstanced", count, start, base, instances, mix(2, first_instance)); }
	orig.DrawIndexedInstanced(c, count, instances, start, base, first_instance);
}
static void STDMETHODCALLTYPE H_DrawInstanced(CTX *c, UINT count, UINT instances, UINT start, UINT first_instance)
{
	if (on(c)) { Lock l; draw_action("DrawInstanced", count, start, 0, instances, mix(3, first_instance)); }
	orig.DrawInstanced(c, count, instances, start, first_instance);
}
static void STDMETHODCALLTYPE H_DrawAuto(CTX *c)
{
	if (on(c)) { Lock l; draw_action("DrawAuto", 0, 0, 0, 0, 4); }
	orig.DrawAuto(c);
}
static void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(CTX *c, ID3D11Buffer *args, UINT offset)
{
	if (on(c)) { Lock l; draw_action("DrawIndexedInstancedIndirect", rid(args), offset, 0, 0, 5); }
	orig.DrawIndexedInstancedIndirect(c, args, offset);
}
static void STDMETHODCALLTYPE H_DrawInstancedIndirect(CTX *c, ID3D11Buffer *args, UINT offset)
{
	if (on(c)) { Lock l; draw_action("DrawInstancedIndirect", rid(args), offset, 0, 0, 6); }
	orig.DrawInstancedIndirect(c, args, offset);
}

static void dispatch_action(const char *what, uint64_t a, uint64_t b, uint64_t c)
{
	uint64_t h = stage_hash(CS);
	for (int k = 0; k < 8; k++)
		h = mix(h, vid(S.cs_uav[k]));
	h = mix(mix(mix(h, a), b), c);
	if (texting())
		fprintf(text, "  %s %llu %llu %llu | cs %08x | u %08x %08x %08x %08x\n", what, a, b, c, (unsigned)oid(S.stage[CS].shader),
			(unsigned)vid(S.cs_uav[0]), (unsigned)vid(S.cs_uav[1]), (unsigned)vid(S.cs_uav[2]), (unsigned)vid(S.cs_uav[3]));
	effective(what, h);
	for (int k = 0; k < 8; k++)
		touch(view_resource(S.cs_uav[k]), mix(h, k));
}
static void STDMETHODCALLTYPE H_Dispatch(CTX *c, UINT x, UINT y, UINT z)
{
	if (on(c)) { Lock l; dispatch_action("Dispatch", x, y, z); }
	orig.Dispatch(c, x, y, z);
}
static void STDMETHODCALLTYPE H_DispatchIndirect(CTX *c, ID3D11Buffer *args, UINT offset)
{
	if (on(c)) { Lock l; dispatch_action("DispatchIndirect", rid(args), offset, 7); }
	orig.DispatchIndirect(c, args, offset);
}

static void subresource_size(Res *r, UINT sub, UINT row_pitch, UINT *rows, UINT *row_bytes, UINT *slices)
{
	UINT mip = r->mips ? sub % r->mips : 0, block, bytes;
	format_block(r->format, &block, &bytes);
	UINT w = max(1u, r->width >> mip), h = max(1u, r->height >> mip);
	*rows = (h + block - 1) / block;
	*row_bytes = ((w + block - 1) / block) * bytes;
	*slices = r->kind == 4 ? max(1u, r->depth >> mip) : 1;
}

static uint64_t hash_surface(Res *r, UINT sub, const void *data, UINT row_pitch, UINT depth_pitch)
{
	if (r->kind == 1)
		return hash_bytes(data, r->bytes);
	UINT rows, row_bytes, slices;
	subresource_size(r, sub, row_pitch, &rows, &row_bytes, &slices);
	uint64_t h = K;
	for (UINT z = 0; z < slices; z++)
		for (UINT y = 0; y < rows; y++)
			h = mix(h, hash_bytes((const uint8_t*)data + (size_t)z * depth_pitch + (size_t)y * row_pitch, row_bytes));
	return h;
}

static HRESULT STDMETHODCALLTYPE H_Map(CTX *c, ID3D11Resource *res, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
	HRESULT hr = orig.Map(c, res, sub, type, flags, out);
	if (on(c) && SUCCEEDED(hr) && out) {
		Lock l;
		mapped[{res, sub}] = { out->pData, out->RowPitch, out->DepthPitch, type };
	}
	return hr;
}
static void STDMETHODCALLTYPE H_Unmap(CTX *c, ID3D11Resource *res, UINT sub)
{
	if (on(c)) {
		Lock l;
		auto i = mapped.find({res, sub});
		if (i != mapped.end()) {
			if (i->second.type != D3D11_MAP_READ) {
				Res *r = resource_of(res);
				uint64_t data = hash_surface(r, sub, i->second.data, i->second.row_pitch, i->second.depth_pitch);
				uint64_t h = mix(mix(mix(0x4d6170, sub), i->second.type), data);
				// A discard leaves nothing of what was there
				if (i->second.type == D3D11_MAP_WRITE_DISCARD && r->kind == 1)
					r->content = mix(r->shape, h);
				else
					r->content = mix(r->content, h);
				if (texting())
					fprintf(text, "  Unmap sub %u type %d data %08x -> %08x\n", sub, i->second.type, (unsigned)data, (unsigned)r->content);
				effective("Unmap", mix(h, r->shape));
			}
			mapped.erase(i);
		}
	}
	orig.Unmap(c, res, sub);
}

static void update_action(const char *what, ID3D11Resource *res, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch, UINT flags)
{
	Lock l;
	Res *r = resource_of(res);
	uint64_t d = 0;
	if (data) {
		if (r->kind == 1) {
			d = hash_bytes(data, box ? box->right - box->left : r->bytes);
		} else if (box) {
			UINT block, bytes;
			format_block(r->format, &block, &bytes);
			UINT rows = (box->bottom - box->top + block - 1) / block, row_bytes = ((box->right - box->left + block - 1) / block) * bytes;
			d = K;
			for (UINT z = 0; z < box->back - box->front; z++)
				for (UINT y = 0; y < rows; y++)
					d = mix(d, hash_bytes((const uint8_t*)data + (size_t)z * depth_pitch + (size_t)y * row_pitch, row_bytes));
		} else {
			d = hash_surface(r, sub, data, row_pitch, depth_pitch);
		}
	}
	uint64_t h = mix(mix(mix(0x557064, sub), d), flags);
	if (box)
		h = mix(h, hash_bytes(box, sizeof(*box)));
	r->content = mix(r->content, h);
	if (texting())
		fprintf(text, "  %s sub %u data %08x -> %08x\n", what, sub, (unsigned)d, (unsigned)r->content);
	effective(what, mix(h, r->shape));
}
static void STDMETHODCALLTYPE H_UpdateSubresource(CTX *c, ID3D11Resource *res, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch)
{
	if (on(c)) update_action("UpdateSubresource", res, sub, box, data, row_pitch, depth_pitch, 0);
	orig.UpdateSubresource(c, res, sub, box, data, row_pitch, depth_pitch);
}
static void STDMETHODCALLTYPE H_UpdateSubresource1(CTX *c, ID3D11Resource *res, UINT sub, const D3D11_BOX *box, const void *data, UINT row_pitch, UINT depth_pitch, UINT flags)
{
	if (on(c)) update_action("UpdateSubresource1", res, sub, box, data, row_pitch, depth_pitch, flags);
	orig.UpdateSubresource1(c, res, sub, box, data, row_pitch, depth_pitch, flags);
}

static void copy_action(const char *what, void *dst, void *src, uint64_t args, bool whole)
{
	Lock l;
	Res *d = resource_of(dst);
	uint64_t s = rid(src);
	uint64_t h = mix(mix(0x437079, s), args);
	if (d)
		d->content = whole ? mix(d->shape, h) : mix(d->content, h);
	if (texting())
		fprintf(text, "  %s src %08x -> %08x\n", what, (unsigned)s, d ? (unsigned)d->content : 0);
	effective(what, mix(h, d ? d->shape : 0));
}
static void STDMETHODCALLTYPE H_CopyResource(CTX *c, ID3D11Resource *dst, ID3D11Resource *src)
{
	if (on(c)) copy_action("CopyResource", dst, src, 0, true);
	orig.CopyResource(c, dst, src);
}
static uint64_t region_args(UINT dsub, UINT x, UINT y, UINT z, UINT ssub, const D3D11_BOX *box, UINT flags)
{
	uint64_t h = mix(mix(mix(mix(mix(mix(K, dsub), x), y), z), ssub), flags);
	return box ? mix(h, hash_bytes(box, sizeof(*box))) : h;
}
static void STDMETHODCALLTYPE H_CopySubresourceRegion(CTX *c, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box)
{
	if (on(c)) copy_action("CopySubresourceRegion", dst, src, region_args(dsub, x, y, z, ssub, box, 0), false);
	orig.CopySubresourceRegion(c, dst, dsub, x, y, z, src, ssub, box);
}
static void STDMETHODCALLTYPE H_CopySubresourceRegion1(CTX *c, ID3D11Resource *dst, UINT dsub, UINT x, UINT y, UINT z, ID3D11Resource *src, UINT ssub, const D3D11_BOX *box, UINT flags)
{
	if (on(c)) copy_action("CopySubresourceRegion1", dst, src, region_args(dsub, x, y, z, ssub, box, flags), false);
	orig.CopySubresourceRegion1(c, dst, dsub, x, y, z, src, ssub, box, flags);
}
static void STDMETHODCALLTYPE H_CopyStructureCount(CTX *c, ID3D11Buffer *dst, UINT offset, ID3D11UnorderedAccessView *src)
{
	if (on(c)) {
		void *resource;
		{ Lock l; resource = view_resource(src); }
		copy_action("CopyStructureCount", dst, resource, offset, false);
	}
	orig.CopyStructureCount(c, dst, offset, src);
}
static void STDMETHODCALLTYPE H_ResolveSubresource(CTX *c, ID3D11Resource *dst, UINT dsub, ID3D11Resource *src, UINT ssub, DXGI_FORMAT format)
{
	if (on(c)) copy_action("ResolveSubresource", dst, src, mix(mix(dsub, ssub), format), false);
	orig.ResolveSubresource(c, dst, dsub, src, ssub, format);
}

static void clear_action(const char *what, void *view, uint64_t args, bool whole)
{
	Lock l;
	Res *r = resource_of(view_resource(view));
	uint64_t h = mix(mix(0x436c72, views[view].desc), args);
	if (r)
		r->content = whole ? mix(r->shape, h) : mix(r->content, h);
	effective(what, mix(h, r ? r->shape : 0));
}
static void STDMETHODCALLTYPE H_ClearRenderTargetView(CTX *c, ID3D11RenderTargetView *v, const FLOAT color[4])
{
	if (on(c)) clear_action("ClearRenderTargetView", v, hash_bytes(color, 16), true);
	orig.ClearRenderTargetView(c, v, color);
}
static void STDMETHODCALLTYPE H_ClearUnorderedAccessViewUint(CTX *c, ID3D11UnorderedAccessView *v, const UINT values[4])
{
	if (on(c)) clear_action("ClearUnorderedAccessViewUint", v, hash_bytes(values, 16), true);
	orig.ClearUnorderedAccessViewUint(c, v, values);
}
static void STDMETHODCALLTYPE H_ClearUnorderedAccessViewFloat(CTX *c, ID3D11UnorderedAccessView *v, const FLOAT values[4])
{
	if (on(c)) clear_action("ClearUnorderedAccessViewFloat", v, mix(1, hash_bytes(values, 16)), true);
	orig.ClearUnorderedAccessViewFloat(c, v, values);
}
static void STDMETHODCALLTYPE H_ClearDepthStencilView(CTX *c, ID3D11DepthStencilView *v, UINT flags, FLOAT depth, UINT8 stencil)
{
	if (on(c)) clear_action("ClearDepthStencilView", v, mix(mix(flags, hash_bytes(&depth, 4)), stencil), flags == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL));
	orig.ClearDepthStencilView(c, v, flags, depth, stencil);
}
static void STDMETHODCALLTYPE H_ClearView(CTX *c, ID3D11View *v, const FLOAT color[4], const D3D11_RECT *rects, UINT n)
{
	if (on(c)) clear_action("ClearView", v, mix(hash_bytes(color, 16), hash_bytes(rects, n * sizeof(D3D11_RECT))), false);
	orig.ClearView(c, v, color, rects, n);
}
static void STDMETHODCALLTYPE H_GenerateMips(CTX *c, ID3D11ShaderResourceView *v)
{
	if (on(c)) clear_action("GenerateMips", v, 0x6d6970, false);
	orig.GenerateMips(c, v);
}

static void STDMETHODCALLTYPE H_IASetInputLayout(CTX *c, ID3D11InputLayout *layout)
{
	if (on(c)) { Lock l; S.layout = layout; strict(0x200, oid(layout)); }
	orig.IASetInputLayout(c, layout);
}
static void STDMETHODCALLTYPE H_IASetVertexBuffers(CTX *c, UINT start, UINT n, ID3D11Buffer *const *b, const UINT *strides, const UINT *offsets)
{
	if (on(c)) {
		Lock l;
		for (UINT i = 0; i < n && start + i < 32; i++) {
			S.vb[start + i] = b ? b[i] : NULL;
			S.vb_stride[start + i] = strides ? strides[i] : 0;
			S.vb_offset[start + i] = offsets ? offsets[i] : 0;
			strict(0x201, start + i, rid(S.vb[start + i]), mix(S.vb_stride[start + i], S.vb_offset[start + i]));
		}
	}
	orig.IASetVertexBuffers(c, start, n, b, strides, offsets);
}
static void STDMETHODCALLTYPE H_IASetIndexBuffer(CTX *c, ID3D11Buffer *b, DXGI_FORMAT format, UINT offset)
{
	if (on(c)) { Lock l; S.ib = b; S.ib_format = format; S.ib_offset = offset; strict(0x202, rid(b), format, offset); }
	orig.IASetIndexBuffer(c, b, format, offset);
}
static void STDMETHODCALLTYPE H_IASetPrimitiveTopology(CTX *c, D3D11_PRIMITIVE_TOPOLOGY t)
{
	if (on(c)) { Lock l; S.topology = t; strict(0x203, t); }
	orig.IASetPrimitiveTopology(c, t);
}

static void set_targets(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
	for (UINT i = 0; i < 8; i++) {
		S.rtv[i] = (i < n && rtv) ? rtv[i] : NULL;
		strict(0x210, i, vid(S.rtv[i]));
	}
	S.dsv = dsv;
	strict(0x211, vid(dsv));
}
static void STDMETHODCALLTYPE H_OMSetRenderTargets(CTX *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv)
{
	if (on(c)) {
		Lock l;
		set_targets(n, rtv, dsv);
		memset(S.uav, 0, sizeof(S.uav));
	}
	orig.OMSetRenderTargets(c, n, rtv, dsv);
}
static void STDMETHODCALLTYPE H_OMSetRenderTargetsAndUnorderedAccessViews(CTX *c, UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv,
	UINT start, UINT nu, ID3D11UnorderedAccessView *const *uav, const UINT *counts)
{
	if (on(c)) {
		Lock l;
		if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
			set_targets(n, rtv, dsv);
		if (nu != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
			memset(S.uav, 0, sizeof(S.uav));
			for (UINT i = 0; i < nu && start + i < 8; i++) {
				S.uav[start + i] = uav ? uav[i] : NULL;
				strict(0x212, start + i, vid(S.uav[start + i]), counts ? counts[i] : ~0u);
			}
		}
	}
	orig.OMSetRenderTargetsAndUnorderedAccessViews(c, n, rtv, dsv, start, nu, uav, counts);
}
static void STDMETHODCALLTYPE H_OMSetBlendState(CTX *c, ID3D11BlendState *state, const FLOAT factor[4], UINT mask)
{
	if (on(c)) {
		Lock l;
		S.blend = state;
		for (int i = 0; i < 4; i++)
			S.blend_factor[i] = factor ? factor[i] : 1.0f;
		S.sample_mask = mask;
		strict(0x213, oid(state), hash_bytes(S.blend_factor, 16), mask);
	}
	orig.OMSetBlendState(c, state, factor, mask);
}
static void STDMETHODCALLTYPE H_OMSetDepthStencilState(CTX *c, ID3D11DepthStencilState *state, UINT ref)
{
	if (on(c)) { Lock l; S.depth = state; S.stencil_ref = ref; strict(0x214, oid(state), ref); }
	orig.OMSetDepthStencilState(c, state, ref);
}
static void STDMETHODCALLTYPE H_SOSetTargets(CTX *c, UINT n, ID3D11Buffer *const *b, const UINT *offsets)
{
	if (on(c)) {
		Lock l;
		for (UINT i = 0; i < 4; i++) {
			S.so[i] = (i < n && b) ? b[i] : NULL;
			S.so_offset[i] = (i < n && offsets) ? offsets[i] : 0;
			strict(0x215, i, rid(S.so[i]), S.so_offset[i]);
		}
	}
	orig.SOSetTargets(c, n, b, offsets);
}
static void STDMETHODCALLTYPE H_RSSetState(CTX *c, ID3D11RasterizerState *state)
{
	if (on(c)) { Lock l; S.raster = state; strict(0x220, oid(state)); }
	orig.RSSetState(c, state);
}
static void STDMETHODCALLTYPE H_RSSetViewports(CTX *c, UINT n, const D3D11_VIEWPORT *v)
{
	if (on(c)) {
		Lock l;
		S.viewports = min(n, 16u);
		if (v)
			memcpy(S.viewport, v, S.viewports * sizeof(*v));
		strict(0x221, hash_bytes(S.viewport, S.viewports * sizeof(*v)));
	}
	orig.RSSetViewports(c, n, v);
}
static void STDMETHODCALLTYPE H_RSSetScissorRects(CTX *c, UINT n, const D3D11_RECT *r)
{
	if (on(c)) {
		Lock l;
		S.scissors = min(n, 16u);
		if (r)
			memcpy(S.scissor, r, S.scissors * sizeof(*r));
		strict(0x222, hash_bytes(S.scissor, S.scissors * sizeof(*r)));
	}
	orig.RSSetScissorRects(c, n, r);
}
static void STDMETHODCALLTYPE H_CSSetUnorderedAccessViews(CTX *c, UINT start, UINT n, ID3D11UnorderedAccessView *const *uav, const UINT *counts)
{
	if (on(c)) {
		Lock l;
		for (UINT i = 0; i < n && start + i < 8; i++) {
			S.cs_uav[start + i] = uav ? uav[i] : NULL;
			strict(0x230, start + i, vid(S.cs_uav[start + i]), counts ? counts[i] : ~0u);
		}
	}
	orig.CSSetUnorderedAccessViews(c, start, n, uav, counts);
}
static void STDMETHODCALLTYPE H_ClearState(CTX *c)
{
	if (on(c)) { Lock l; memset(&S, 0, sizeof(S)); strict(0x240); }
	orig.ClearState(c);
}
static void STDMETHODCALLTYPE H_ExecuteCommandList(CTX *c, ID3D11CommandList *list, BOOL restore)
{
	if (on(c)) { Lock l; effective("ExecuteCommandList", mix(0x45786563, restore)); if (!restore) memset(&S, 0, sizeof(S)); }
	orig.ExecuteCommandList(c, list, restore);
}
static void STDMETHODCALLTYPE H_SwapDeviceContextState(CTX *c, ID3DDeviceContextState *state, ID3DDeviceContextState **previous)
{
	if (on(c)) { Lock l; effective("SwapDeviceContextState", 0x53776170); }
	orig.SwapDeviceContextState(c, state, previous);
}

// ------------------------------------------------------------- the device

static void made_resource(void *p, int kind, const void *desc, size_t desc_bytes, uint64_t data, UINT bytes, UINT w, UINT h, UINT d, UINT mips, DXGI_FORMAT format)
{
	Lock l;
	Res r = {};
	r.kind = kind; r.bytes = bytes; r.width = w; r.height = h; r.depth = d; r.mips = mips; r.format = format;
	r.shape = hash_bytes(desc, desc_bytes);
	r.content = mix(r.shape, data);
	resources[p] = r;
	if (text)
		fprintf(text, "made kind %d %u bytes %ux%ux%u mips %u format %d data %08x -> %08x thread %lu\n", kind, bytes, w, h, d, mips, format, (unsigned)data, (unsigned)r.content, GetCurrentThreadId());
}

static uint64_t initial_hash(Res &shape, const D3D11_SUBRESOURCE_DATA *init, UINT subresources)
{
	if (!init)
		return 0;
	uint64_t h = K;
	for (UINT i = 0; i < subresources; i++) {
		if (!init[i].pSysMem)
			continue;
		h = mix(h, hash_surface(&shape, i, init[i].pSysMem, init[i].SysMemPitch, init[i].SysMemSlicePitch));
	}
	return h;
}

static HRESULT STDMETHODCALLTYPE H_CreateBuffer(DEV *d, const D3D11_BUFFER_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Buffer **out)
{
	HRESULT hr = dorig.CreateBuffer(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out && desc)
		made_resource(*out, 1, desc, sizeof(*desc), (init && init->pSysMem) ? hash_bytes(init->pSysMem, desc->ByteWidth) : 0, desc->ByteWidth, 0, 0, 0, 0, DXGI_FORMAT_UNKNOWN);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateTexture1D(DEV *d, const D3D11_TEXTURE1D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture1D **out)
{
	HRESULT hr = dorig.CreateTexture1D(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out && desc) {
		Res shape = {}; shape.kind = 2; shape.width = desc->Width; shape.height = 1; shape.depth = 1; shape.mips = desc->MipLevels; shape.format = desc->Format;
		made_resource(*out, 2, desc, sizeof(*desc), initial_hash(shape, init, desc->MipLevels * desc->ArraySize), 0, desc->Width, 1, 1, desc->MipLevels, desc->Format);
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateTexture2D(DEV *d, const D3D11_TEXTURE2D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D **out)
{
	HRESULT hr = dorig.CreateTexture2D(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out && desc) {
		Res shape = {}; shape.kind = 3; shape.width = desc->Width; shape.height = desc->Height; shape.depth = 1; shape.mips = desc->MipLevels; shape.format = desc->Format;
		made_resource(*out, 3, desc, sizeof(*desc), initial_hash(shape, init, desc->MipLevels * desc->ArraySize), 0, desc->Width, desc->Height, 1, desc->MipLevels, desc->Format);
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateTexture3D(DEV *d, const D3D11_TEXTURE3D_DESC *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture3D **out)
{
	HRESULT hr = dorig.CreateTexture3D(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out && desc) {
		Res shape = {}; shape.kind = 4; shape.width = desc->Width; shape.height = desc->Height; shape.depth = desc->Depth; shape.mips = desc->MipLevels; shape.format = desc->Format;
		made_resource(*out, 4, desc, sizeof(*desc), initial_hash(shape, init, desc->MipLevels), 0, desc->Width, desc->Height, desc->Depth, desc->MipLevels, desc->Format);
	}
	return hr;
}

// A view's description is a union: only the members of its dimension mean
// anything, and callers leave whatever was on their stack in the rest.
static uint64_t view_desc_hash(int kind, const void *desc, size_t bytes)
{
	static const unsigned char used[5][12] = {
		{},
		{ 0, 8, 8, 16, 8, 16, 0, 8, 8, 8, 16, 12 },	// shader resource views
		{ 0, 12, 4, 12, 4, 12, 0, 0, 12 },		// unordered access views
		{ 0, 8, 4, 12, 4, 12, 0, 8, 12 },		// render target views
		{ 0, 4, 12, 4, 12, 0, 8 },			// depth stencil views
	};
	if (!desc)
		return mix(kind, 0);
	const UINT *words = (const UINT*)desc;
	UINT format = words[0], dimension = words[1];
	size_t header = kind == 4 ? 12 : 8;
	// The newer descriptions have members the table does not know
	bool newer = bytes != (kind == 1 ? sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC) : kind == 2 ? sizeof(D3D11_UNORDERED_ACCESS_VIEW_DESC) :
		kind == 3 ? sizeof(D3D11_RENDER_TARGET_VIEW_DESC) : sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));
	uint64_t h = mix(mix(mix(K, kind), format), dimension);
	if (kind == 4)
		h = mix(h, words[2]);
	if (newer || dimension >= 12)
		return mix(h, hash_bytes((const uint8_t*)desc + header, bytes - header));
	return mix(h, hash_bytes((const uint8_t*)desc + header, used[kind][dimension]));
}

static void made_view(void *view, void *resource, const void *desc, size_t bytes, int kind)
{
	Lock l;
	views[view] = { resource, view_desc_hash(kind, desc, bytes) };
}
static HRESULT STDMETHODCALLTYPE H_CreateShaderResourceView(DEV *d, ID3D11Resource *res, const D3D11_SHADER_RESOURCE_VIEW_DESC *desc, ID3D11ShaderResourceView **out)
{
	HRESULT hr = dorig.CreateShaderResourceView(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 1);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateUnorderedAccessView(DEV *d, ID3D11Resource *res, const D3D11_UNORDERED_ACCESS_VIEW_DESC *desc, ID3D11UnorderedAccessView **out)
{
	HRESULT hr = dorig.CreateUnorderedAccessView(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 2);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateRenderTargetView(DEV *d, ID3D11Resource *res, const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out)
{
	HRESULT hr = dorig.CreateRenderTargetView(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 3);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateDepthStencilView(DEV *d, ID3D11Resource *res, const D3D11_DEPTH_STENCIL_VIEW_DESC *desc, ID3D11DepthStencilView **out)
{
	HRESULT hr = dorig.CreateDepthStencilView(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 4);
	return hr;
}

static void made_object(void *p, uint64_t h)
{
	Lock l;
	objects[p] = h;
}
static HRESULT STDMETHODCALLTYPE H_CreateInputLayout(DEV *d, const D3D11_INPUT_ELEMENT_DESC *elements, UINT n, const void *code, SIZE_T bytes, ID3D11InputLayout **out)
{
	HRESULT hr = dorig.CreateInputLayout(d, elements, n, code, bytes, out);
	if (SUCCEEDED(hr) && out && *out) {
		uint64_t h = mix(0x4c61796f, n);
		for (UINT i = 0; i < n; i++) {
			h = mix(h, hash_bytes(elements[i].SemanticName, strlen(elements[i].SemanticName)));
			h = mix(h, hash_bytes(&elements[i].SemanticIndex, sizeof(elements[i]) - offsetof(D3D11_INPUT_ELEMENT_DESC, SemanticIndex)));
		}
		made_object(*out, h);
	}
	return hr;
}
#define SHADER(Name, Type) \
static HRESULT STDMETHODCALLTYPE H_Create##Name(DEV *d, const void *code, SIZE_T bytes, ID3D11ClassLinkage *linkage, Type **out) { \
	HRESULT hr = dorig.Create##Name(d, code, bytes, linkage, out); \
	if (SUCCEEDED(hr) && out && *out) made_object(*out, hash_bytes(code, bytes)); \
	return hr; }
SHADER(VertexShader, ID3D11VertexShader)
SHADER(GeometryShader, ID3D11GeometryShader)
SHADER(PixelShader, ID3D11PixelShader)
SHADER(HullShader, ID3D11HullShader)
SHADER(DomainShader, ID3D11DomainShader)
SHADER(ComputeShader, ID3D11ComputeShader)
static HRESULT STDMETHODCALLTYPE H_CreateGeometryShaderWithStreamOutput(DEV *d, const void *code, SIZE_T bytes, const D3D11_SO_DECLARATION_ENTRY *entries, UINT n,
	const UINT *strides, UINT ns, UINT rasterized, ID3D11ClassLinkage *linkage, ID3D11GeometryShader **out)
{
	HRESULT hr = dorig.CreateGeometryShaderWithStreamOutput(d, code, bytes, entries, n, strides, ns, rasterized, linkage, out);
	if (SUCCEEDED(hr) && out && *out) made_object(*out, mix(mix(hash_bytes(code, bytes), n), rasterized));
	return hr;
}
#define STATE(Name, Desc, Type) \
static HRESULT STDMETHODCALLTYPE H_Create##Name(DEV *d, const Desc *desc, Type **out) { \
	HRESULT hr = dorig.Create##Name(d, desc, out); \
	if (SUCCEEDED(hr) && out && *out && desc) made_object(*out, mix(sizeof(Desc), hash_bytes(desc, sizeof(Desc)))); \
	return hr; }
// These two have padding between their members, so they are hashed member by member
static HRESULT STDMETHODCALLTYPE H_CreateBlendState(DEV *d, const D3D11_BLEND_DESC *desc, ID3D11BlendState **out)
{
	HRESULT hr = dorig.CreateBlendState(d, desc, out);
	if (SUCCEEDED(hr) && out && *out && desc) {
		uint64_t h = mix(mix(0x426c656e, desc->AlphaToCoverageEnable != 0), desc->IndependentBlendEnable != 0);
		for (int i = 0; i < (desc->IndependentBlendEnable ? 8 : 1); i++) {
			const D3D11_RENDER_TARGET_BLEND_DESC &t = desc->RenderTarget[i];
			h = mix(mix(mix(mix(h, t.BlendEnable != 0), t.SrcBlend), t.DestBlend), t.BlendOp);
			h = mix(mix(mix(mix(h, t.SrcBlendAlpha), t.DestBlendAlpha), t.BlendOpAlpha), t.RenderTargetWriteMask);
		}
		made_object(*out, h);
	}
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateDepthStencilState(DEV *d, const D3D11_DEPTH_STENCIL_DESC *desc, ID3D11DepthStencilState **out)
{
	HRESULT hr = dorig.CreateDepthStencilState(d, desc, out);
	if (SUCCEEDED(hr) && out && *out && desc) {
		uint64_t h = mix(mix(mix(0x44657074, desc->DepthEnable != 0), desc->DepthWriteMask), desc->DepthFunc);
		h = mix(mix(mix(h, desc->StencilEnable != 0), desc->StencilReadMask), desc->StencilWriteMask);
		h = mix(h, hash_bytes(&desc->FrontFace, sizeof(desc->FrontFace)));
		h = mix(h, hash_bytes(&desc->BackFace, sizeof(desc->BackFace)));
		made_object(*out, h);
	}
	return hr;
}
STATE(RasterizerState, D3D11_RASTERIZER_DESC, ID3D11RasterizerState)
STATE(SamplerState, D3D11_SAMPLER_DESC, ID3D11SamplerState)
STATE(BlendState1, D3D11_BLEND_DESC1, ID3D11BlendState1)
STATE(RasterizerState1, D3D11_RASTERIZER_DESC1, ID3D11RasterizerState1)

// What comes out of the newer creation calls is looked at when it is first
// used; all that matters here is that an address is not taken for the object
// that was there before.
static void forget(void *p)
{
	Lock l;
	resources.erase(p);
	views.erase(p);
	objects.erase(p);
}
static HRESULT STDMETHODCALLTYPE H_CreateTexture2D1(DEV *d, const D3D11_TEXTURE2D_DESC1 *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture2D1 **out)
{
	HRESULT hr = dorig.CreateTexture2D1(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out) forget(*out);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateTexture3D1(DEV *d, const D3D11_TEXTURE3D_DESC1 *desc, const D3D11_SUBRESOURCE_DATA *init, ID3D11Texture3D1 **out)
{
	HRESULT hr = dorig.CreateTexture3D1(d, desc, init, out);
	if (SUCCEEDED(hr) && out && *out) forget(*out);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateShaderResourceView1(DEV *d, ID3D11Resource *res, const D3D11_SHADER_RESOURCE_VIEW_DESC1 *desc, ID3D11ShaderResourceView1 **out)
{
	HRESULT hr = dorig.CreateShaderResourceView1(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 1);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateUnorderedAccessView1(DEV *d, ID3D11Resource *res, const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *desc, ID3D11UnorderedAccessView1 **out)
{
	HRESULT hr = dorig.CreateUnorderedAccessView1(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 2);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateRenderTargetView1(DEV *d, ID3D11Resource *res, const D3D11_RENDER_TARGET_VIEW_DESC1 *desc, ID3D11RenderTargetView1 **out)
{
	HRESULT hr = dorig.CreateRenderTargetView1(d, res, desc, out);
	if (SUCCEEDED(hr) && out && *out) made_view(*out, res, desc, sizeof(*desc), 3);
	return hr;
}
static HRESULT STDMETHODCALLTYPE H_CreateRasterizerState2(DEV *d, const D3D11_RASTERIZER_DESC2 *desc, ID3D11RasterizerState2 **out)
{
	HRESULT hr = dorig.CreateRasterizerState2(d, desc, out);
	if (SUCCEEDED(hr) && out && *out && desc) made_object(*out, mix(sizeof(*desc), hash_bytes(desc, sizeof(*desc))));
	return hr;
}

// --------------------------------------------------------------- wrapping

// Direct3D 11 rewrites entries of a context's function table as it goes (the
// draw calls have several versions), so patching that table loses calls.
// Instead the DLL is handed objects of ours in place of the real device and
// context: every entry of their tables is a few bytes of code that pass the
// call on to the same entry of the real object, whatever its arguments, and
// the entries listened to are replaced by the functions above.
struct Wrapper {
	void **table;
	IUnknown *real;
};

static void *thunks;
static void *device_table[256], *context_table[256];
static Wrapper device_wrapper, context_wrapper;

static void *thunk(int slot)
{
	return (uint8_t*)thunks + slot * 16;
}

static void make_thunks()
{
	thunks = VirtualAlloc(NULL, 256 * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	for (int slot = 0; slot < 256; slot++) {
		// mov rcx, [rcx + 8]; mov rax, [rcx]; jmp [rax + slot * 8]
		uint8_t code[16] = { 0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0xA0, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC };
		int offset = slot * 8;
		memcpy(code + 9, &offset, 4);
		memcpy(thunk(slot), code, 16);
	}
}

// Whatever interface is the object itself stays ours; the others (DXGI's, the
// multithread switch) are the real ones.
static HRESULT STDMETHODCALLTYPE W_QueryInterface(IUnknown *self, REFIID iid, void **out)
{
	Wrapper *w = (Wrapper*)self;
	HRESULT hr = IUnknown_QueryInterface(w->real, iid, out);
	if (SUCCEEDED(hr) && out && *out == w->real)
		*out = w;
	return hr;
}

static Wrapper *wrap_context(void *real)
{
	if (!real)
		return NULL;
	if (!context_wrapper.real) {
		context_wrapper.real = (IUnknown*)real;
		main_context = &context_wrapper;
	}
	return context_wrapper.real == real ? &context_wrapper : (Wrapper*)real;
}

static void STDMETHODCALLTYPE H_GetImmediateContext(DEV *d, ID3D11DeviceContext **out)
{
	dorig.GetImmediateContext(d, out);
	if (out) *out = (ID3D11DeviceContext*)wrap_context(*out);
}
static void STDMETHODCALLTYPE H_GetImmediateContext1(DEV *d, ID3D11DeviceContext1 **out)
{
	dorig.GetImmediateContext1(d, out);
	if (out) *out = (ID3D11DeviceContext1*)wrap_context(*out);
}
static void STDMETHODCALLTYPE H_GetImmediateContext2(DEV *d, ID3D11DeviceContext2 **out)
{
	dorig.GetImmediateContext2(d, out);
	if (out) *out = (ID3D11DeviceContext2*)wrap_context(*out);
}
static void STDMETHODCALLTYPE H_GetImmediateContext3(DEV *d, ID3D11DeviceContext3 **out)
{
	dorig.GetImmediateContext3(d, out);
	if (out) *out = (ID3D11DeviceContext3*)wrap_context(*out);
}
static void STDMETHODCALLTYPE H_GetDevice(CTX *c, ID3D11Device **out)
{
	orig.GetDevice(c, out);
	if (out && *out == (ID3D11Device*)device_wrapper.real)
		*out = (ID3D11Device*)&device_wrapper;
}

static void make_tables()
{
	if (thunks)
		return;
	make_thunks();
	for (int slot = 0; slot < 256; slot++)
		device_table[slot] = context_table[slot] = thunk(slot);
	memcpy(&orig, context_table, sizeof(orig));
	memcpy(&dorig, device_table, sizeof(dorig));

	ID3D11DeviceContext1Vtbl *v = (ID3D11DeviceContext1Vtbl*)context_table;
#define C(name) *(void**)&v->name = (void*)H_##name;
#define CS5(P) C(P##SetShader) C(P##SetConstantBuffers) C(P##SetConstantBuffers1) C(P##SetShaderResources) C(P##SetSamplers)
	CS5(VS) CS5(HS) CS5(DS) CS5(GS) CS5(PS) CS5(CS)
	C(DrawIndexed) C(Draw) C(DrawIndexedInstanced) C(DrawInstanced) C(DrawAuto) C(DrawIndexedInstancedIndirect) C(DrawInstancedIndirect)
	C(Dispatch) C(DispatchIndirect) C(Map) C(Unmap) C(UpdateSubresource) C(UpdateSubresource1)
	C(CopyResource) C(CopySubresourceRegion) C(CopySubresourceRegion1) C(CopyStructureCount) C(ResolveSubresource)
	C(ClearRenderTargetView) C(ClearUnorderedAccessViewUint) C(ClearUnorderedAccessViewFloat) C(ClearDepthStencilView) C(ClearView) C(GenerateMips)
	C(IASetInputLayout) C(IASetVertexBuffers) C(IASetIndexBuffer) C(IASetPrimitiveTopology)
	C(OMSetRenderTargets) C(OMSetRenderTargetsAndUnorderedAccessViews) C(OMSetBlendState) C(OMSetDepthStencilState) C(SOSetTargets)
	C(RSSetState) C(RSSetViewports) C(RSSetScissorRects) C(CSSetUnorderedAccessViews) C(ClearState) C(ExecuteCommandList) C(SwapDeviceContextState)
	C(GetDevice)
	*(void**)&v->QueryInterface = (void*)W_QueryInterface;

	ID3D11Device3Vtbl *dv = (ID3D11Device3Vtbl*)device_table;
#define D(name) *(void**)&dv->name = (void*)H_##name;
	D(CreateTexture2D1) D(CreateTexture3D1) D(CreateShaderResourceView1) D(CreateUnorderedAccessView1) D(CreateRenderTargetView1) D(CreateRasterizerState2)
	D(CreateBuffer) D(CreateTexture1D) D(CreateTexture2D) D(CreateTexture3D)
	D(CreateShaderResourceView) D(CreateUnorderedAccessView) D(CreateRenderTargetView) D(CreateDepthStencilView)
	D(CreateInputLayout) D(CreateVertexShader) D(CreateGeometryShader) D(CreateGeometryShaderWithStreamOutput) D(CreatePixelShader)
	D(CreateHullShader) D(CreateDomainShader) D(CreateComputeShader)
	D(CreateBlendState) D(CreateDepthStencilState) D(CreateRasterizerState) D(CreateSamplerState) D(CreateBlendState1) D(CreateRasterizerState1)
	D(GetImmediateContext) D(GetImmediateContext1) D(GetImmediateContext2) D(GetImmediateContext3)
	*(void**)&dv->QueryInterface = (void*)W_QueryInterface;

	device_wrapper.table = device_table;
	context_wrapper.table = context_table;
}

// The first device made through here is the one listened to
void trace_wrap(void **device, void **context)
{
	make_tables();
	if (device && *device) {
		if (!device_wrapper.real)
			device_wrapper.real = (IUnknown*)*device;
		if (device_wrapper.real == *device)
			*device = &device_wrapper;
		else
			return;
	}
	if (context && *context)
		*context = wrap_context(*context);
}

void trace_enable(bool on_)
{
	enabled = on_;
}

void trace_note_frame(unsigned frame)
{
	frame_no = frame;
}

TraceFrame trace_frame_end()
{
	Lock l;
	TraceFrame f = { frame_strict, frame_effective, frame_calls, frame_actions, frame_draws, unknown_count };
	frame_strict = frame_effective = K;
	frame_calls = frame_actions = frame_draws = 0;
	return f;
}

void trace_text(FILE *file, unsigned first, unsigned last)
{
	text = file;
	text_first = first;
	text_last = last;
}
