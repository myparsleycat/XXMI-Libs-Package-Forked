#include "DirectX11/dx12/PipelineRegistry.h"

struct ShimInterfaces {
    ID3D12Device *native_device;
    ID3D12CommandQueue *native_queue;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
};

class ShimSymbols {
    HANDLE process = GetCurrentProcess();
public:
    explicit ShimSymbols(const fs::path &dll) {
        if (!SymInitializeW(process, dll.parent_path().c_str(), FALSE))
            throw std::runtime_error("SymInitialize");
        HMODULE module = GetModuleHandleW(dll.c_str());
        if (!module || !SymLoadModuleExW(process, nullptr, dll.c_str(), L"xxmishim",
                reinterpret_cast<DWORD64>(module), 0, nullptr, 0)) {
            SymCleanup(process);
            throw std::runtime_error("load shim PDB");
        }
    }
    ~ShimSymbols() { SymCleanup(process); }

    template <class T>
    T find(const char *name) const {
        alignas(SYMBOL_INFO) BYTE bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(bytes);
        symbol->SizeOfStruct = sizeof(*symbol);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromName(process, name, symbol))
            throw std::runtime_error(std::string("missing shim symbol: ") + name);
        return reinterpret_cast<T>(symbol->Address);
    }
};

static void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

static ComPtr<ID3D11Buffer> shim_buffer(const ShimInterfaces &shim, UINT bytes, UINT flags,
        const void *data = nullptr, bool staging = false) {
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = bytes;
    desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags;
    desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    D3D11_SUBRESOURCE_DATA initial = {data, 0, 0};
    ComPtr<ID3D11Buffer> buffer;
    check(shim.device->CreateBuffer(&desc, data ? &initial : nullptr, &buffer));
    return buffer;
}

static void expect_buffer(const ShimInterfaces &shim, ID3D11Buffer *buffer,
        const UINT *expected, UINT bytes, const char *message) {
    auto staging = shim_buffer(shim, bytes, 0, nullptr, true);
    shim.context->CopyResource(staging.Get(), buffer);
    D3D11_MAPPED_SUBRESOURCE map = {};
    check(shim.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    bool same = !memcmp(map.pData, expected, bytes);
    shim.context->Unmap(staging.Get(), 0);
    expect(same, message);
}

static void shim_copy_probe(const ShimInterfaces &shim) {
    const UINT expected[] = {1, 2, 3, 4};
    const UINT other[] = {51, 52, 53, 54};
    auto src = shim_buffer(shim, sizeof(other), D3D11_BIND_VERTEX_BUFFER, other);
    auto dst = shim_buffer(shim, sizeof(expected), D3D11_BIND_VERTEX_BUFFER, expected);
    const D3D11_BOX reversed = {8, 0, 0, 4, 1, 1};
    const D3D11_BOX empty_height = {0, 0, 0, 8, 0, 1};
    shim.context->UpdateSubresource(dst.Get(), 0, &reversed, other, 0, 0);
    expect_buffer(shim, dst.Get(), expected, sizeof(expected), "a reversed box changed buffer contents");
    shim.context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, src.Get(), 0, &empty_height);
    shim.context->CopySubresourceRegion(dst.Get(), 1, 0, 0, 0, src.Get(), 0, nullptr);
    expect_buffer(shim, dst.Get(), expected, sizeof(expected), "an empty or out-of-range copy changed buffer contents");
    shim.context->CopySubresourceRegion(dst.Get(), 0, 0, 0, 0, src.Get(), 0, nullptr);
    expect_buffer(shim, dst.Get(), other, sizeof(other), "a valid copy failed after an invalid copy");
}

static void shim_depth_probe(const ShimInterfaces &shim) {
    for (bool stencil : {false, true}) {
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = texture.Height = 4;
        texture.ArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
        texture.Format = stencil ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> resource;
        check(shim.device->CreateTexture2D(&texture, nullptr, &resource));
        D3D11_DEPTH_STENCIL_VIEW_DESC depth = {};
        depth.Format = stencil ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
        depth.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        depth.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        ComPtr<ID3D11DepthStencilView> dsv;
        check(shim.device->CreateDepthStencilView(resource.Get(), &depth, &dsv));

        D3D11_SHADER_RESOURCE_VIEW_DESC input = {};
        input.Format = stencil ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        input.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        input.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> srv, stencil_srv;
        check(shim.device->CreateShaderResourceView(resource.Get(), &input, &srv));
        if (stencil) {
            input.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
            check(shim.device->CreateShaderResourceView(resource.Get(), &input, &stencil_srv));
        }
        ID3D11ShaderResourceView *views[] = {srv.Get(), stencil_srv.Get()};
        shim.context->PSSetShaderResources(0, 2, views);
        shim.context->OMSetRenderTargets(0, nullptr, dsv.Get());
        for (int attempt = 0; attempt < 2; attempt++) {
            ID3D11ShaderResourceView *bound[2] = {};
            shim.context->PSGetShaderResources(0, 2, bound);
            bool valid = bound[0] == srv.Get() && !bound[1];
            for (auto *view : bound)
                if (view) view->Release();
            expect(valid, "depth SRV was cleared or writable stencil SRV was retained");
            shim.context->PSSetShaderResources(0, 2, views);
        }
        shim.context->ClearState();
    }

    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = 4;
    texture.ArraySize = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> resource;
    check(shim.device->CreateTexture2D(&texture, nullptr, &resource));
    D3D11_RENDER_TARGET_VIEW_DESC desc = {};
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> view;
    check(shim.device->CreateRenderTargetView(resource.Get(), &desc, &view));
    view->GetDesc(&desc);
    expect(desc.Format == texture.Format, "RTV did not inherit its texture format");
}

static void shim_bindings_probe(const ShimInterfaces &shim, const fs::path &shader_path) {
    const UINT initial[] = {17, 17, 17, 17};
    const D3D11_SUBRESOURCE_DATA data[] = {{initial, 8, 0}, {initial, 4, 0}, {initial, 8, 0}, {initial, 4, 0}};
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = texture.ArraySize = texture.MipLevels = 2;
    texture.SampleDesc.Count = 1;
    texture.Format = DXGI_FORMAT_R32_UINT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> resource;
    check(shim.device->CreateTexture2D(&texture, data, &resource));

    D3D11_SHADER_RESOURCE_VIEW_DESC input = {};
    input.Format = texture.Format;
    input.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    input.Texture2DArray.MipLevels = input.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> same, disjoint;
    check(shim.device->CreateShaderResourceView(resource.Get(), &input, &same));
    input.Texture2DArray.MostDetailedMip = input.Texture2DArray.FirstArraySlice = 1;
    check(shim.device->CreateShaderResourceView(resource.Get(), &input, &disjoint));
    D3D11_UNORDERED_ACCESS_VIEW_DESC output = {};
    output.Format = texture.Format;
    output.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
    output.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11UnorderedAccessView> target, result_view;
    check(shim.device->CreateUnorderedAccessView(resource.Get(), &output, &target));
    auto result = shim_buffer(shim, sizeof(UINT), D3D11_BIND_UNORDERED_ACCESS);
    output.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    output.Buffer = {0, 1, 0};
    check(shim.device->CreateUnorderedAccessView(result.Get(), &output, &result_view));

    ID3D11ShaderResourceView *views[] = {same.Get(), nullptr, nullptr};
    ID3D11UnorderedAccessView *outputs[] = {target.Get(), result_view.Get(), nullptr, nullptr};
    shim.context->CSSetShaderResources(0, 3, views);
    shim.context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
    for (int attempt = 0; attempt < 2; attempt++) {
        ComPtr<ID3D11ShaderResourceView> bound;
        shim.context->CSGetShaderResources(0, 1, &bound);
        expect(!bound, "an overlapping SRV survived UAV binding");
        shim.context->CSSetShaderResources(0, 1, views);
    }
    views[0] = disjoint.Get();
    shim.context->CSSetShaderResources(0, 1, views);
    ComPtr<ID3D11ShaderResourceView> bound;
    shim.context->CSGetShaderResources(0, 1, &bound);
    expect(bound.Get() == disjoint.Get(), "a disjoint SRV was cleared");

    auto bytecode = read(shader_path);
    ComPtr<ID3D11ComputeShader> shader;
    check(shim.device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader));
    shim.context->CSSetShader(shader.Get(), nullptr, 0);
    shim.context->Dispatch(1, 1, 1);
    const UINT expected = 17;
    expect_buffer(shim, result.Get(), &expected, sizeof(expected), "dispatch did not read the disjoint mip with null buffers");
    shim.context->ClearState();

    views[0] = same.Get();
    shim.context->PSSetShaderResources(0, 1, views);
    shim.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 1, outputs, nullptr);
    shim.context->PSGetShaderResources(0, 1, &bound);
    expect(!bound, "an overlapping pixel SRV survived OM UAV binding");
    views[0] = disjoint.Get();
    shim.context->PSSetShaderResources(0, 1, views);
    shim.context->PSGetShaderResources(0, 1, &bound);
    expect(bound.Get() == disjoint.Get(), "OM UAV binding cleared a disjoint pixel SRV");
    shim.context->ClearState();
}

class ReleaseNotice : public IUnknown {
    std::atomic<unsigned long> references{1};
    std::shared_ptr<std::atomic_bool> released;
public:
    explicit ReleaseNotice(std::shared_ptr<std::atomic_bool> released) : released(std::move(released)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        *object = nullptr;
        if (iid != __uuidof(IUnknown))
            return E_NOINTERFACE;
        *object = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG remaining = --references;
        if (!remaining) {
            *released = true;
            delete this;
        }
        return remaining;
    }
};

static void shim_queue_lifetime_probe(const ShimInterfaces &shim, const ShimSymbols &symbols) {
    using QueueWaits = void (*)(ID3D12CommandQueue *, UINT64, bool);
    auto queue_waits = symbols.find<QueueWaits>("xxmishim!Dx12ShimQueueWaits");
    auto queue_released = std::make_shared<std::atomic_bool>(false);
    auto pin_released = std::make_shared<std::atomic_bool>(false);
    D3D12_COMMAND_QUEUE_DESC desc = {};
    ComPtr<ID3D12CommandQueue> queue;
    check(shim.native_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    const GUID notice_id = {0x53aac87d, 0x0db0, 0x499f, {0x8a, 0xc3, 0x88, 0xda, 0xa2, 0x7e, 0x32, 0x16}};
    ComPtr<IUnknown> notice;
    notice.Attach(new ReleaseNotice(queue_released));
    check(queue->SetPrivateDataInterface(notice_id, notice.Get()));
    notice.Reset();
    queue_waits(queue.Get(), 0, false);
    queue.Reset();
    expect(queue_released->load(), "retirement retained a destroyed foreign queue");

    using OnExecute = void (*)(ID3D12CommandQueue *, std::vector<IUnknown *> *, std::vector<IUnknown *> *);
    auto pins_executed = symbols.find<OnExecute>("xxmishim!Dx12ShimPinsExecuted");
    auto collect = symbols.find<void (*)()>("xxmishim!Dx12ShimSubmit");
    check(shim.native_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));

    ComPtr<ID3D12Fence> gate, finished;
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)));
    auto close_event = [](HANDLE handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_event)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), close_event);
    expect(!!event, "create foreign queue event");
    check(queue->Wait(gate.Get(), 1));
    std::vector<IUnknown *> pins = {new ReleaseNotice(pin_released)}, first;
    pins_executed(queue.Get(), &pins, &first);
    check(queue->Signal(finished.Get(), 1));
    bool held = !pin_released->load();
    check(gate->Signal(1));
    check(finished->SetEventOnCompletion(1, event.get()));
    expect(WaitForSingleObject(event.get(), 30000) == WAIT_OBJECT_0, "foreign queue did not finish");
    queue.Reset();
    collect();
    expect(held && pin_released->load(), "foreign queue pin did not live through GPU completion");
}

static void shim_queue_budget_probe(const ShimInterfaces &shim, const ShimSymbols &symbols, ID3D12CommandQueue *main_queue) {
    using QueueWaits = void (*)(ID3D12CommandQueue *, UINT64, bool);
    auto queue_waits = symbols.find<QueueWaits>("xxmishim!Dx12ShimQueueWaits");
    auto submit = symbols.find<void (*)()>("xxmishim!Dx12ShimSubmit");
    ComPtr<ID3D12Fence> gate;
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    D3D12_COMMAND_QUEUE_DESC desc = {};
    ComPtr<ID3D12CommandQueue> queues[6];
    for (auto &queue : queues)
        check(shim.native_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
    const UINT data = 1;
    auto buffer = shim_buffer(shim, sizeof(data), D3D11_BIND_VERTEX_BUFFER, &data);
    check(main_queue->Wait(gate.Get(), 1));
    submit();
    for (auto &queue : queues)
        queue_waits(queue.Get(), 0, false);
    check(gate->Signal(1));
}

static void shim_registry_probe(const ShimSymbols &symbols, ID3D12Device *device, const fs::path &shader_path) {
    auto find_pipeline = symbols.find<decltype(&Dx12FindPipeline)>("xxmishim!Dx12FindPipeline");
    auto shader = read(shader_path);
    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
    };
    D3D12_ROOT_PARAMETER table = {};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable = {ARRAYSIZE(ranges), ranges};
    D3D12_ROOT_SIGNATURE_DESC layout = {1, &table};
    ComPtr<ID3DBlob> blob;
    check(D3D12SerializeRootSignature(&layout, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr));
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root.Get();
    desc.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
    const Dx12PipelineInfo *registered = find_pipeline(pipeline.Get());
    expect(registered && registered->shaders[DX12_STAGE_CS] && registered->shaders[DX12_STAGE_CS]->reflected,
            "stripped DXIL resources were not reflected");
    const auto &info = *registered->shaders[DX12_STAGE_CS];
    expect(info.srv_dimension[2] == D3D12_SRV_DIMENSION_BUFFER && info.srv_buffer[2] == 8,
            "stripped DXIL structured SRV has no element stride");
    expect(info.uav_dimension[3] == D3D12_UAV_DIMENSION_BUFFER && info.uav_buffer[3] == 8,
            "stripped DXIL structured UAV has no element stride");
    expect(info.srv_buffer[1] == DX12_BUFFER_RAW && info.uav_buffer[2] == DX12_BUFFER_RAW,
            "stripped DXIL raw buffers lost their descriptor kind");
}

static void shim_removed_resize_probe(const ShimInterfaces &shim, IDXGISwapChain1 *swap) {
    ComPtr<ID3D12Fence> fence;
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    ComPtr<ID3D12Device5> removable;
    check(shim.native_device->QueryInterface(IID_PPV_ARGS(&removable)));
    ComPtr<IDXGISwapChain3> chain;
    check(swap->QueryInterface(IID_PPV_ARGS(&chain)));

    const UINT data = 1;
    auto source = shim_buffer(shim, sizeof(data), D3D11_BIND_VERTEX_BUFFER, &data);
    auto staging = shim_buffer(shim, sizeof(data), 0, nullptr, true);
    shim.context->CopyResource(staging.Get(), source.Get());
    D3D11_MAPPED_SUBRESOURCE map = {};
    expect(shim.context->Map(staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) == DXGI_ERROR_WAS_STILL_DRAWING,
            "staging copy was not pending before device removal");

    removable->RemoveDevice();
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (fence->GetCompletedValue() != UINT64_MAX && GetTickCount64() < deadline)
        Sleep(1);
    expect(fence->GetCompletedValue() == UINT64_MAX, "WARP shim fence did not report device removal");
    expect(shim.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map) == DXGI_ERROR_DEVICE_REMOVED && !map.pData,
            "staging Map did not propagate device removal");
    expect(chain->ResizeBuffers(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0) == DXGI_ERROR_DEVICE_REMOVED,
            "ResizeBuffers hid device removal");
    expect(chain->ResizeBuffers1(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nullptr, nullptr) == DXGI_ERROR_DEVICE_REMOVED,
            "ResizeBuffers1 hid device removal");
}

static void shim_present_queues_probe(const ShimInterfaces &shim, const ShimSymbols &symbols, IDXGISwapChain1 *swap, IDXGIFactory4 *factory,
        const DXGI_SWAP_CHAIN_DESC1 &desc) {
    ComPtr<IDXGISwapChain3> chain;
    check(swap->QueryInterface(IID_PPV_ARGS(&chain)));
    ID3D12CommandQueue *original = shim.native_queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ComPtr<ID3D12CommandQueue> queues[2];
    for (auto &queue : queues)
        check(shim.native_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    IUnknown *present_queues[] = {queues[0].Get(), queues[1].Get()};
    const UINT nodes[] = {1, 1};
    auto get_back_buffer = symbols.find<ID3D11Resource *(*)()>("xxmishim!Dx12ShimBackBuffer");
    ComPtr<ID3D11Resource> old_buffer;
    old_buffer.Attach(get_back_buffer());
    expect(!!old_buffer, "shim has no back buffer before resize");
    ComPtr<ID3D11RenderTargetView> view;
    check(shim.device->CreateRenderTargetView(old_buffer.Get(), nullptr, &view));
    expect(FAILED(chain->ResizeBuffers(1, 32, 32, DXGI_FORMAT_UNKNOWN, 0)), "DXGI accepted a one-buffer flip chain");
    ComPtr<ID3D11Resource> restored;
    restored.Attach(get_back_buffer());
    expect(restored.Get() == old_buffer.Get(), "failed resize lost the existing back-buffer wrapper");
    check(shim.device->CreateRenderTargetView(restored.Get(), nullptr, view.ReleaseAndGetAddressOf()));
    auto close_window = [](HWND handle) { DestroyWindow(handle); };
    std::unique_ptr<HWND__, decltype(close_window)> native_window(window(), close_window);
    ComPtr<ID3D12CommandQueue> native_original;
    check(shim.native_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&native_original)));
    ComPtr<IDXGISwapChain1> native_base;
    check(factory->CreateSwapChainForHwnd(native_original.Get(), native_window.get(), &desc, nullptr, nullptr, &native_base));
    ComPtr<IDXGISwapChain3> native;
    check(native_base.As(&native));
    auto references = [](ID3D12CommandQueue *queue) {
        queue->AddRef();
        return queue->Release();
    };
    ULONG initial_refs[] = {references(queues[0].Get()), references(queues[1].Get())};
    HRESULT native_resize = native->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nodes, present_queues);
    HRESULT native_without_queues = E_FAIL;
    if (SUCCEEDED(native_resize)) {
        ULONG explicit_refs = references(native_original.Get());
        ULONG explicit_queue_refs[] = {references(queues[0].Get()), references(queues[1].Get())};
        expect(explicit_queue_refs[0] > initial_refs[0] && explicit_queue_refs[1] > initial_refs[1],
                "native ResizeBuffers1 did not retain both present queues");
        auto restored_creation_queue = [&]() {
            return references(native_original.Get()) > explicit_refs && references(queues[0].Get()) == initial_refs[0]
                    && references(queues[1].Get()) == initial_refs[1];
        };
        native_without_queues = native->ResizeBuffers1(0, 36, 36, DXGI_FORMAT_UNKNOWN, 0, nullptr, nullptr);
        if (SUCCEEDED(native_without_queues)) {
            expect(restored_creation_queue(), "native ResizeBuffers1 without queues did not restore the creation queue");
            check(native->ResizeBuffers1(2, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nodes, present_queues));
            explicit_refs = references(native_original.Get());
        } else {
            expect(references(native_original.Get()) == explicit_refs && references(queues[0].Get()) == explicit_queue_refs[0]
                    && references(queues[1].Get()) == explicit_queue_refs[1],
                    "rejected native ResizeBuffers1 without queues changed the queue layout");
        }
        check(native->ResizeBuffers(0, 40, 40, DXGI_FORMAT_UNKNOWN, 0));
        expect(restored_creation_queue(), "native ResizeBuffers did not restore the creation queue");
    }
    HRESULT resized = chain->ResizeBuffers1(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nodes, present_queues);
    if (FAILED(native_resize)) {
        expect(FAILED(resized), "shim accepted a queue layout the native swap chain rejected");
        check(chain->Present(0, 0));
        expect(shim.native_queue == original, "failed resize changed the original queue");
        std::cout << "DXGI rejected the explicit WARP queue layout; verified the rejected resize\n";
        return;
    }
    expect(SUCCEEDED(resized), "shim rejected a queue layout accepted by the native swap chain");
    expect(FAILED(shim.device->CreateRenderTargetView(old_buffer.Get(), nullptr, view.ReleaseAndGetAddressOf())),
            "successful resize left the old back-buffer wrapper usable");
    UINT seen_buffers = 0;
    for (int frame = 0; frame < 4; frame++) {
        DXGI_PRESENT_PARAMETERS parameters = {};
        check(frame % 2 ? chain->Present1(0, 0, &parameters) : chain->Present(0, 0));
        UINT index = chain->GetCurrentBackBufferIndex();
        expect(index < 2 && shim.native_queue == queues[index].Get(),
                "shim queue did not follow the current back buffer");
        seen_buffers |= 1u << index;
    }
    expect(seen_buffers == 3, "Present did not exercise both back-buffer queues");
    expect(FAILED(chain->ResizeBuffers1(1, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nodes, present_queues)),
            "DXGI accepted a one-buffer flip chain");
    check(chain->Present(0, 0));
    expect(shim.native_queue == queues[chain->GetCurrentBackBufferIndex()].Get(),
            "failed resize lost the known queue layout");

    HRESULT without_queues = chain->ResizeBuffers1(0, 36, 36, DXGI_FORMAT_UNKNOWN, 0, nullptr, nullptr);
    expect(without_queues == native_without_queues, "ResizeBuffers1 without queues returned a different HRESULT from native DXGI");
    check(chain->Present(0, 0));
    UINT index = chain->GetCurrentBackBufferIndex();
    expect(index < 2 && shim.native_queue == (SUCCEEDED(without_queues) ? original : queues[index].Get()),
            "shim queue after ResizeBuffers1 without queues did not match native DXGI");
    if (SUCCEEDED(without_queues)) {
        check(chain->ResizeBuffers1(0, 32, 32, DXGI_FORMAT_UNKNOWN, 0, nodes, present_queues));
        check(chain->Present(0, 0));
    } else {
        std::cout << "DXGI rejected ResizeBuffers1 without queues; verified the retained queue layout\n";
    }
    check(chain->ResizeBuffers(0, 40, 40, DXGI_FORMAT_UNKNOWN, 0));
    check(chain->Present(0, 0));
    expect(shim.native_queue == original, "shim ResizeBuffers did not restore the creation queue");
}

static void check_messages(ID3D12InfoQueue *messages) {
    if (!messages)
        return;
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); i++) {
        SIZE_T size = 0;
        check(messages->GetMessage(i, nullptr, &size));
        std::vector<BYTE> bytes(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(bytes.data());
        check(messages->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            throw std::runtime_error(message->pDescription);
    }
}

static void shim_probe(const wchar_t *mode, const fs::path &dll, const fs::path &shader_path) {
    if (!LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
        throw std::runtime_error("DLL load");
    ShimSymbols symbols(dll);
    bool present_queues = !wcscmp(mode, L"present-queues");
    bool removed_resize = !wcscmp(mode, L"removed-resize");
    if (!present_queues && !removed_resize) {
        ComPtr<ID3D12Debug1> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(TRUE);
    }
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    if (!wcscmp(mode, L"registry")) {
        shim_registry_probe(symbols, device.Get(), shader_path);
        return;
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = desc.Height = 32;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HWND hwnd = window();
    ComPtr<IDXGISwapChain1> swap;
    check(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &desc, nullptr, nullptr, &swap));
    check(swap->Present(0, 0));

    // Use the built DLL's PDB to reach the same COM interfaces used by mods.
    auto *shim = *symbols.find<ShimInterfaces **>("xxmishim!dx12_shim");
    expect(shim && shim->device && shim->context, "Present did not initialize the shim");
    ComPtr<ID3D12InfoQueue> messages;
    if (!present_queues && !removed_resize) {
        check(shim->native_device->QueryInterface(IID_PPV_ARGS(&messages)));
        messages->ClearStoredMessages();
    }
    try {
        if (!wcscmp(mode, L"copy"))
            shim_copy_probe(*shim);
        else if (!wcscmp(mode, L"depth"))
            shim_depth_probe(*shim);
        else if (!wcscmp(mode, L"bindings"))
            shim_bindings_probe(*shim, shader_path);
        else if (!wcscmp(mode, L"queue-lifetime"))
            shim_queue_lifetime_probe(*shim, symbols);
        else if (!wcscmp(mode, L"queue-budget"))
            shim_queue_budget_probe(*shim, symbols, queue.Get());
        else if (present_queues)
            shim_present_queues_probe(*shim, symbols, swap.Get(), factory.Get(), desc);
        else if (removed_resize) {
            shim_removed_resize_probe(*shim, swap.Get());
            swap.Reset();
            DestroyWindow(hwnd);
            return;
        }
        else
            throw std::runtime_error("unknown shim probe");
    } catch (...) {
        check_messages(messages.Get());
        throw;
    }
    check(swap->Present(0, 0));
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    auto close_event = [](HANDLE handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_event)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), close_event);
    expect(!!event, "create fence event");
    wait(shim->native_queue, fence.Get(), 1, event.get());
    check(device->GetDeviceRemovedReason());
    check_messages(messages.Get());
    swap.Reset();
    DestroyWindow(hwnd);
}
