#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

using Microsoft::WRL::ComPtr;

static unsigned failures;

static void require(HRESULT result, const char *operation)
{
	if (FAILED(result)) {
		std::fprintf(stderr, "%s failed: 0x%08x\n", operation, static_cast<unsigned>(result));
		std::exit(1);
	}
}

static void expect(bool result, const char *contract)
{
	std::printf("%s %s\n", result ? "PASS" : "FAIL", contract);
	if (!result)
		++failures;
}

static auto input_layout(ID3D11Device *device)
{
	constexpr char source[] = "float4 main(float4 position : POSITION) : SV_Position { return position; }";
	ComPtr<ID3DBlob> signature;
	require(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "vs_4_0",
			0, 0, signature.GetAddressOf(), nullptr), "compile input signature");

	const D3D11_INPUT_ELEMENT_DESC element =
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	ComPtr<ID3D11InputLayout> layout;
	require(device->CreateInputLayout(&element, 1, signature->GetBufferPointer(), signature->GetBufferSize(),
			layout.GetAddressOf()), "create input layout");
	return layout;
}

static auto current_layout(ID3D11DeviceContext *context)
{
	ComPtr<ID3D11InputLayout> layout;
	context->IAGetInputLayout(layout.GetAddressOf());
	return layout;
}

static void check_resets(ID3D11Device *device, ID3D11DeviceContext *context)
{
	const auto layout = input_layout(device);

	context->IASetInputLayout(layout.Get());
	context->ClearState();
	expect(!current_layout(context), "ClearState clears the tracked input layout");

	ComPtr<ID3D11DeviceContext> deferred;
	require(device->CreateDeferredContext(0, deferred.GetAddressOf()), "create deferred context");
	deferred->IASetInputLayout(layout.Get());
	ComPtr<ID3D11CommandList> commands;
	require(deferred->FinishCommandList(FALSE, commands.GetAddressOf()), "finish command list");
	expect(!current_layout(deferred.Get()), "FinishCommandList(FALSE) clears the tracked input layout");

	context->IASetInputLayout(layout.Get());
	context->ExecuteCommandList(commands.Get(), TRUE);
	expect(current_layout(context).Get() == layout.Get(), "ExecuteCommandList(TRUE) preserves the input layout");
	context->ExecuteCommandList(commands.Get(), FALSE);
	expect(!current_layout(context), "ExecuteCommandList(FALSE) clears the tracked input layout");
}

static void check_swap(ID3D11Device *device, ID3D11DeviceContext1 *context)
{
	ComPtr<ID3D11Device1> device1;
	require(device->QueryInterface(IID_PPV_ARGS(device1.GetAddressOf())), "query device1");

	const auto first = input_layout(device);
	const auto second = input_layout(device);

	constexpr D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	ComPtr<ID3DDeviceContextState> alternate;
	require(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
			nullptr, alternate.GetAddressOf()), "create alternate context state");

	context->IASetInputLayout(first.Get());
	ComPtr<ID3DDeviceContextState> original;
	context->SwapDeviceContextState(alternate.Get(), original.GetAddressOf());
	expect(!current_layout(context), "fresh context state has no input layout");
	if (!original)
		std::exit(1);

	context->IASetInputLayout(second.Get());
	context->SwapDeviceContextState(original.Get(), nullptr);
	expect(current_layout(context).Get() == first.Get(), "switching back restores the original layout");
	context->SwapDeviceContextState(alternate.Get(), nullptr);
	expect(current_layout(context).Get() == second.Get(), "a switch without an output pointer still saves the layout");
	context->SwapDeviceContextState(nullptr, nullptr);
	expect(current_layout(context).Get() == second.Get(), "a null context state leaves the layout bound");
}

static void check_context1(ID3D11Device *device, ID3D11DeviceContext1 *context)
{
	D3D11_BUFFER_DESC desc = {};
	desc.ByteWidth = 512;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ComPtr<ID3D11Buffer> buffer;
	require(device->CreateBuffer(&desc, nullptr, buffer.GetAddressOf()), "create constant buffer");

	constexpr UINT first = 16, count = 16;
	auto *binding = buffer.Get();
	context->VSSetConstantBuffers1(0, 1, &binding, &first, &count);
	ComPtr<ID3D11Buffer> bound;
	UINT bound_first = 0, bound_count = 0;
	context->VSGetConstantBuffers1(0, 1, bound.GetAddressOf(), &bound_first, &bound_count);
	expect(bound.Get() == buffer.Get() && bound_first == first && bound_count == count,
			"the context hook forwards D3D11.1 constant buffer ranges");

	check_swap(device, context);
}

int wmain(int argc, wchar_t **argv)
{
	if (argc != 3 || !SetCurrentDirectoryW(argv[1]))
		return 1;
	const HMODULE module = LoadLibraryW(L".\\d3d11.dll");
	if (!module)
		return 1;
	const auto create = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(module, "D3D11CreateDevice"));
	if (!create)
		return 1;

	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	require(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
			device.GetAddressOf(), nullptr, context.GetAddressOf()), "create WARP device");

	ComPtr<ID3D11DeviceContext1> context1;
	require(context->QueryInterface(IID_PPV_ARGS(context1.GetAddressOf())), "query context1");
	if (wcscmp(argv[2], L"hooked") == 0)
		check_context1(device.Get(), context1.Get());
	else {
		check_resets(device.Get(), context.Get());
		check_swap(device.Get(), context1.Get());
	}
	return failures ? 2 : 0;
}
