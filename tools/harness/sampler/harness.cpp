// Checks the ps-s0 = ResourceSampler bindings of d3d11.dll without a game.
// Loads the d3d11.dll next to the run folder given, makes a device and swap
// chain through it, binds "game" state (a point/wrap sampler on every slot the
// ini touches, a 2x1 texture with texels 0 and 1, an 8x1 float render target and
// an 8 element RWBuffer), presents (which runs the ini's [Present]) and prints
// what the custom shaders sampled and which samplers are bound afterwards,
// then checks those of the last frame against what case/d3dx.ini asks for.
//
//   harness.exe <run folder> [frames]
//
// Exits with 2 when a check fails, 1 when the DLL or D3D11 fails.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include "../common/window.h"
#include <d3d11.h>
#include <stdio.h>
#include <float.h>

#pragma comment(lib, "user32.lib")

typedef HRESULT (WINAPI *CreateFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
		const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

#define CHECK(call) do { HRESULT hr_ = (call); if (FAILED(hr_)) { printf("FAILED %s = 0x%08x\n", #call, (unsigned)hr_); exit(1); } } while (0)

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static int failures;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static void expect(const char *what, bool ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", what);
	failures += !ok;
}

static bool close_to(float a, float b)
{
	return a > b - 0.01f && a < b + 0.01f;
}

typedef void (STDMETHODCALLTYPE ID3D11DeviceContext::*GetSamplersFn)(UINT, UINT, ID3D11SamplerState**);
typedef void (STDMETHODCALLTYPE ID3D11DeviceContext::*SetSamplersFn)(UINT, UINT, ID3D11SamplerState*const*);

static const struct {
	const char *name;
	GetSamplersFn get;
	SetSamplersFn set;
} stages[] = {
	{ "vs", &ID3D11DeviceContext::VSGetSamplers, &ID3D11DeviceContext::VSSetSamplers },
	{ "hs", &ID3D11DeviceContext::HSGetSamplers, &ID3D11DeviceContext::HSSetSamplers },
	{ "ds", &ID3D11DeviceContext::DSGetSamplers, &ID3D11DeviceContext::DSSetSamplers },
	{ "gs", &ID3D11DeviceContext::GSGetSamplers, &ID3D11DeviceContext::GSSetSamplers },
	{ "ps", &ID3D11DeviceContext::PSGetSamplers, &ID3D11DeviceContext::PSSetSamplers },
	{ "cs", &ID3D11DeviceContext::CSGetSamplers, &ID3D11DeviceContext::CSSetSamplers },
};

static ID3D11SamplerState *bound(int stage, UINT slot)
{
	ID3D11SamplerState *s = NULL;
	(ctx->*stages[stage].get)(slot, 1, &s);
	if (s)
		s->Release();
	return s;
}

static void describe(int stage, UINT slot, ID3D11SamplerState *game)
{
	ID3D11SamplerState *s = bound(stage, slot);
	D3D11_SAMPLER_DESC d;

	if (!s) {
		printf("  %s-s%u: null\n", stages[stage].name, slot);
		return;
	}
	if (s == game) {
		printf("  %s-s%u: game sampler\n", stages[stage].name, slot);
		return;
	}
	s->GetDesc(&d);
	printf("  %s-s%u: filter=0x%x address=%d/%d/%d bias=%g aniso=%u cmp=%d border=%g,%g,%g,%g lod=%g..%g\n",
		stages[stage].name, slot, d.Filter, d.AddressU, d.AddressV, d.AddressW, d.MipLODBias, d.MaxAnisotropy,
		d.ComparisonFunc, d.BorderColor[0], d.BorderColor[1], d.BorderColor[2], d.BorderColor[3], d.MinLOD, d.MaxLOD);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("harness.exe <run folder> [frames]\n");
		return 1;
	}
	int frames = argc > 2 ? atoi(argv[2]) : 2;
	setvbuf(stdout, NULL, _IONBF, 0);

	char dll[MAX_PATH];
	_snprintf(dll, MAX_PATH, "%s\\d3d11.dll", argv[1]);
	HMODULE lib = LoadLibraryA(dll);
	if (!lib) {
		printf("cannot load %s: %lu\n", dll, GetLastError());
		return 1;
	}
	CreateFn create = (CreateFn)GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain");
	if (!create) {
		printf("%s does not export D3D11CreateDeviceAndSwapChain\n", dll);
		return 1;
	}

	WNDCLASSW wc = {};
	wc.lpfnWndProc = window_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"sampler-harness";
	RegisterClassW(&wc);
	HWND window = CreateWindowW(wc.lpszClassName, L"sampler-harness", WS_OVERLAPPEDWINDOW, 40, 40, 320, 120, NULL, NULL, wc.hInstance, NULL);
	show_harness_window(window);

	DXGI_SWAP_CHAIN_DESC scd = {};
	scd.BufferDesc.Width = 64;
	scd.BufferDesc.Height = 16;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.OutputWindow = window;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	IDXGISwapChain *swap;
	CHECK(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, &scd, &swap, &dev, NULL, &ctx));

	// Texels 0 and 1: linear at u = 0.6 gives 0.7, point gives 1
	float texels[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = 2; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA init = { texels, sizeof(texels), 0 };
	ID3D11Texture2D *tex;
	ID3D11ShaderResourceView *srv;
	CHECK(dev->CreateTexture2D(&td, &init, &tex));
	CHECK(dev->CreateShaderResourceView(tex, NULL, &srv));

	td.Width = 8;
	td.BindFlags = D3D11_BIND_RENDER_TARGET;
	ID3D11Texture2D *rt, *rt_staging;
	ID3D11RenderTargetView *rtv;
	CHECK(dev->CreateTexture2D(&td, NULL, &rt));
	CHECK(dev->CreateRenderTargetView(rt, NULL, &rtv));
	td.BindFlags = 0;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CHECK(dev->CreateTexture2D(&td, NULL, &rt_staging));

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = 8 * 16;
	bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	ID3D11Buffer *buf, *buf_staging;
	CHECK(dev->CreateBuffer(&bd, NULL, &buf));
	bd.BindFlags = 0;
	bd.Usage = D3D11_USAGE_STAGING;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	CHECK(dev->CreateBuffer(&bd, NULL, &buf_staging));
	D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
	ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	ud.Buffer.NumElements = 8;
	ID3D11UnorderedAccessView *uav;
	CHECK(dev->CreateUnorderedAccessView(buf, &ud, &uav));

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sd.MaxAnisotropy = 1;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = 7.0f;  // Unlike anything the ini makes
	ID3D11SamplerState *game;
	CHECK(dev->CreateSamplerState(&sd, &game));

	for (int frame = 0; frame < frames; frame++) {
		ID3D11SamplerState *games[16];
		for (int i = 0; i < 16; i++)
			games[i] = game;
		for (int stage = 0; stage < 6; stage++)
			(ctx->*stages[stage].set)(0, 16, games);

		float clear[4] = { -1, -1, -1, -1 };
		UINT clear_uav[4] = { 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000 };
		ctx->ClearRenderTargetView(rtv, clear);
		ctx->ClearUnorderedAccessViewUint(uav, clear_uav);
		ctx->OMSetRenderTargets(1, &rtv, NULL);
		D3D11_VIEWPORT vp = { 0, 0, 8, 1, 0, 1 };
		ctx->RSSetViewports(1, &vp);
		ctx->PSSetShaderResources(0, 1, &srv);
		ctx->CSSetShaderResources(0, 1, &srv);
		ctx->CSSetUnorderedAccessViews(0, 1, &uav, NULL);

		CHECK(swap->Present(0, 0));

		ID3D11UnorderedAccessView *no_uav = NULL;
		ctx->CSSetUnorderedAccessViews(0, 1, &no_uav, NULL);
		ctx->CopyResource(rt_staging, rt);
		ctx->CopyResource(buf_staging, buf);
		D3D11_MAPPED_SUBRESOURCE map;
		float px[8][4], cs[8][4];
		CHECK(ctx->Map(rt_staging, 0, D3D11_MAP_READ, 0, &map));
		memcpy(px, map.pData, sizeof(px));
		ctx->Unmap(rt_staging, 0);
		CHECK(ctx->Map(buf_staging, 0, D3D11_MAP_READ, 0, &map));
		memcpy(cs, map.pData, sizeof(cs));
		ctx->Unmap(buf_staging, 0);

		printf("frame %d\n", frame);
		printf("  ps columns:");
		for (int i = 0; i < 8; i++)
			printf(" %.3f", px[i][0]);
		printf("\n  cs elements:");
		for (int i = 0; i < 8; i++)
			printf(" %.3f", cs[i][0]);
		printf("\n");
		describe(4, 0, game); describe(4, 1, game); describe(4, 2, game); describe(4, 3, game);
		describe(5, 0, game); describe(0, 1, game); describe(1, 0, game); describe(2, 0, game);
		describe(3, 15, game);

		if (frame == frames - 1) {
			expect("ps-s0 linear clamp samples 0.7", close_to(px[0][0], 0.7f));
			// The game's point/wrap sampler would give texel 0 there
			expect("ps-s1 point clamp samples 1.0 at u = 1.2", close_to(px[1][0], 1.0f));
			expect("legacy sampler = linear_filter alone samples 0.7", close_to(px[2][0], 0.7f));
			expect("legacy point_filter + ps-s0 linear: the new line wins (0.7)", close_to(px[3][0], 0.7f));
			expect("ps-s2 bound in an if block samples 0.7", close_to(px[4][0], 0.7f));
			expect("border colour 0.25 at u = 1.5", close_to(px[5][0], 0.25f));
			expect("ps-s0 = null in a custom shader samples with the default sampler (0.7)", close_to(px[6][0], 0.7f));
			expect("cs-s0 linear clamp samples 0.7", close_to(cs[0][0], 0.7f));
			expect("ps-s0..s2 restored after the custom shaders", bound(4, 0) == game && bound(4, 1) == game && bound(4, 2) == game);
			expect("cs-s0 restored after the compute custom shader", bound(5, 0) == game);
			expect("ps-s3 = null in [Present] leaves the slot empty", bound(4, 3) == NULL);
			expect("vs-s1, hs-s0, ds-s0 bound from [Present] stay bound",
				bound(0, 1) && bound(0, 1) != game && bound(1, 0) && bound(1, 0) != game && bound(2, 0) && bound(2, 0) != game);
			ID3D11SamplerState *all = bound(3, 15);
			D3D11_SAMPLER_DESC d = {};
			if (all)
				all->GetDesc(&d);
			expect("gs-s15 has every key of ResourceAll", all && d.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC
				&& d.AddressU == D3D11_TEXTURE_ADDRESS_WRAP && d.AddressV == D3D11_TEXTURE_ADDRESS_MIRROR
				&& d.AddressW == D3D11_TEXTURE_ADDRESS_BORDER && close_to(d.MipLODBias, -0.5f) && d.MaxAnisotropy == 4
				&& d.ComparisonFunc == D3D11_COMPARISON_LESS_EQUAL && close_to(d.BorderColor[2], 0.5f)
				&& close_to(d.MinLOD, 1.0f) && close_to(d.MaxLOD, 2.0f));
			ID3D11SamplerState *aniso = bound(0, 1);
			if (aniso)
				aniso->GetDesc(&d);
			expect("vs-s1 anisotropic defaults (16x, clamp, lod 0..FLT_MAX)", aniso && d.Filter == D3D11_FILTER_ANISOTROPIC
				&& d.MaxAnisotropy == 16 && d.AddressU == D3D11_TEXTURE_ADDRESS_CLAMP && d.MinLOD == 0.0f && d.MaxLOD == FLT_MAX);
		}
	}

	printf(failures ? "%d FAILED\n" : "done\n", failures);
	return failures ? 2 : 0;
}
