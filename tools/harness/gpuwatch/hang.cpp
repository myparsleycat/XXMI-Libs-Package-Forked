// hang.exe <draws per frame> <frames> <frame of the hang, 0 = none>
// Runs through the d3d11.dll next to it. Prints the CPU time per frame, and at
// the given frame runs a compute shader long enough for the driver to reset the
// GPU (TDR), then goes on until Present reports the device as removed.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char vs_source[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4(id & 1, id >> 1, 0, 1) * 0.01; }";
static const char ps_source[] = "float4 main() : SV_Target { return 1; }";
static const char cs_source[] =
	"RWStructuredBuffer<uint> u : register(u0);\n"
	"[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {\n"
	"	uint x = id.x;\n"
	"	for (uint i = 0; i < 2000000000; i++) {\n"
	"		x = x * 2654435761u + i;\n"
	"	}\n"
	"	if (x == 0xdeadbeef) InterlockedAdd(u[0], x);\n"
	"}\n";

static ID3DBlob *compile(const char *source, const char *target)
{
	ID3DBlob *code = NULL, *errors = NULL;
	if (FAILED(D3DCompile(source, strlen(source), NULL, NULL, NULL, "main", target, 0, 0, &code, &errors))) {
		printf("compile: %s\n", errors ? (const char *)errors->GetBufferPointer() : "failed");
		exit(1);
	}
	return code;
}

int main(int argc, char **argv)
{
	int draws = argc > 1 ? atoi(argv[1]) : 2000;
	int frames = argc > 2 ? atoi(argv[2]) : 600;
	int hang_frame = argc > 3 ? atoi(argv[3]) : 0;

	wchar_t path[MAX_PATH];
	GetModuleFileNameW(NULL, path, MAX_PATH);
	wcscpy(wcsrchr(path, L'\\') + 1, L"d3d11.dll");
	HMODULE lib = LoadLibraryW(path);
	if (!lib) {
		printf("no d3d11.dll next to the program\n");
		return 1;
	}
	auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(lib, "D3D11CreateDeviceAndSwapChain");

	WNDCLASSW wc = {};
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = L"gpuwatch";
	RegisterClassW(&wc);
	HWND window = CreateWindowW(L"gpuwatch", L"gpuwatch", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);

	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferCount = 2;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = window;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	IDXGISwapChain *swap_chain = NULL;
	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (!create) {
		printf("no D3D11CreateDeviceAndSwapChain in the d3d11.dll next to this\n");
		return 1;
	}
	HRESULT hr = create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &level, 1, D3D11_SDK_VERSION, &desc, &swap_chain, &device, NULL, &context);
	if (FAILED(hr)) {
		printf("device: 0x%08lx\n", hr);
		return 1;
	}

	// hang.exe <draws> <frames> 0 <index>: what a mod binding a texture over a
	// structured buffer t1 does: the pixel shader reads t1 from that index on.
	// With one more argument of mod it leaves t1 for the ini to bind (guard.sh).
	static char wrong_ps[512];
	static const char big_vs[] = "float4 main(uint id : SV_VertexID) : SV_Position { return float4((id >> 1) * 4.0 - 1, (id & 1) * 4.0 - 1, 0, 1); }";
	ID3D11ShaderResourceView *wrong_view = NULL;
	if (argc > 4) {
		sprintf(wrong_ps, "StructuredBuffer<float4> b : register(t1);\n"
			"float4 main(float4 p : SV_Position) : SV_Target { return b[%sU + (uint)p.x + (uint)p.y * 320]; }\n", argv[4]);
		D3D11_TEXTURE2D_DESC texture_desc = {};
		texture_desc.Width = texture_desc.Height = 1024;
		texture_desc.MipLevels = texture_desc.ArraySize = 1;
		texture_desc.Format = DXGI_FORMAT_BC7_UNORM;
		texture_desc.SampleDesc.Count = 1;
		texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D *texture = NULL;
		if (FAILED(device->CreateTexture2D(&texture_desc, NULL, &texture))
				|| FAILED(device->CreateShaderResourceView(texture, NULL, &wrong_view))) {
			printf("cannot make the texture\n");
			return 1;
		}
	}

	ID3D11Texture2D *back_buffer = NULL;
	ID3D11RenderTargetView *rtv = NULL;
	swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
	device->CreateRenderTargetView(back_buffer, NULL, &rtv);

	ID3DBlob *code = compile(wrong_view ? big_vs : vs_source, "vs_5_0");
	ID3D11VertexShader *vs = NULL;
	device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &vs);
	code = compile(wrong_view ? wrong_ps : ps_source, "ps_5_0");
	ID3D11PixelShader *ps = NULL;
	device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &ps);
	code = compile(cs_source, "cs_5_0");
	ID3D11ComputeShader *cs = NULL;
	device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), NULL, &cs);

	D3D11_BUFFER_DESC buffer_desc = {};
	buffer_desc.ByteWidth = 16;
	buffer_desc.StructureByteStride = 4;
	buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ID3D11Buffer *buffer = NULL;
	ID3D11UnorderedAccessView *uav = NULL;
	device->CreateBuffer(&buffer_desc, NULL, &buffer);
	device->CreateUnorderedAccessView(buffer, NULL, &uav);

	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);
	double total_ms = 0;
	int measured = 0;

	D3D11_VIEWPORT viewport = { 0, 0, 320, 240, 0, 1 };
	for (int frame = 1; frame <= frames || hang_frame; frame++) {
		QueryPerformanceCounter(&start);
		context->OMSetRenderTargets(1, &rtv, NULL);
		context->RSSetViewports(1, &viewport);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(vs, NULL, 0);
		context->PSSetShader(ps, NULL, 0);
		if (wrong_view && argc <= 5)
			context->PSSetShaderResources(1, 1, &wrong_view);
		for (int i = 0; i < draws; i++) {
			context->Draw(3, 0);
			if (frame == hang_frame && i == draws / 2) {
				context->CSSetShader(cs, NULL, 0);
				context->CSSetUnorderedAccessViews(0, 1, &uav, NULL);
				context->Dispatch(1024, 1, 1);
				printf("hang dispatched in frame %d\n", frame);
			}
		}
		hr = swap_chain->Present(0, 0);
		QueryPerformanceCounter(&end);
		if (frame > 60) {
			total_ms += (end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
			measured++;
		}
		if (hr != S_OK && hr != DXGI_STATUS_OCCLUDED) {
			printf("frame %d: Present returned 0x%08lx, removed reason 0x%08lx\n", frame, hr, device->GetDeviceRemovedReason());
			break;
		}
		MSG msg;
		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
			DispatchMessageW(&msg);
		if (hang_frame && frame > hang_frame + 20000)
			break;
	}
	if (measured)
		printf("%.3f ms per frame over %d frames\n", total_ms / measured, measured);
	// Lets the watch thread see a removal the render thread got first
	Sleep(500);
	return 0;
}
