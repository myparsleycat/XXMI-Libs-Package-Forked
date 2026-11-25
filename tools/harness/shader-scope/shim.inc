#include <initializer_list>

#include "DirectX11/dx12/PipelineRegistry.h"
#include "DirectX11/dx12/ShimObjects.h"

struct ShimInterfaces {
    ID3D12Device *native_device;
    ID3D12CommandQueue *native_queue;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
};

class ShimSymbols {
    HANDLE process = GetCurrentProcess();
    struct Symbol {
        alignas(SYMBOL_INFO) BYTE bytes[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    };

    SYMBOL_INFO *lookup(Symbol &storage, const char *name) const {
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage.bytes);
        symbol->SizeOfStruct = sizeof(*symbol);
        symbol->MaxNameLen = MAX_SYM_NAME;
        if (!SymFromName(process, name, symbol))
            throw std::runtime_error(std::string("missing shim symbol: ") + name);
        return symbol;
    }

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
        Symbol storage;
        return reinterpret_cast<T>(lookup(storage, name)->Address);
    }

    template <class T>
    const T *field(const char *name, std::initializer_list<const wchar_t *> members) const {
        Symbol storage;
        auto *symbol = lookup(storage, name);
        DWORD type = symbol->TypeIndex;
        DWORD64 address = symbol->Address;
        for (const wchar_t *member : members) {
            ULONG count = 0;
            if (!SymGetTypeInfo(process, symbol->ModBase, type, TI_GET_CHILDRENCOUNT, &count) || !count)
                throw std::runtime_error("missing shim type members");
            std::vector<ULONG> buffer(count + 2);
            auto *params = reinterpret_cast<TI_FINDCHILDREN_PARAMS *>(buffer.data());
            params->Count = count;
            if (!SymGetTypeInfo(process, symbol->ModBase, type, TI_FINDCHILDREN, params))
                throw std::runtime_error("cannot read shim type members");
            bool found = false;
            for (ULONG index = 0; index < count; index++) {
                ULONG child = params->ChildId[index];
                WCHAR *child_name = nullptr;
                if (!SymGetTypeInfo(process, symbol->ModBase, child, TI_GET_SYMNAME, &child_name))
                    continue;
                bool matches = !wcscmp(member, child_name);
                LocalFree(child_name);
                if (!matches)
                    continue;
                DWORD offset;
                if (!SymGetTypeInfo(process, symbol->ModBase, child, TI_GET_OFFSET, &offset)
                    || !SymGetTypeInfo(process, symbol->ModBase, child, TI_GET_TYPEID, &type))
                    throw std::runtime_error("cannot locate shim type member");
                address += offset;
                found = true;
                break;
            }
            if (!found)
                throw std::runtime_error("missing shim type member");
        }
        return reinterpret_cast<const T *>(address);
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

static void shim_texture_replay_probe(const ShimInterfaces &shim, const ShimSymbols &symbols) {
    D3D11_TEXTURE3D_DESC desc = {};
    desc.Width = desc.Height = desc.Depth = 2;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> texture;
    check(shim.device->CreateTexture3D(&desc, nullptr, &texture));
    auto *resource = ShimResourceOf(texture.Get());
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    ComPtr<ID3D12CommandQueue> other_queue;
    check(shim.native_device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&other_queue)));

    {
        const UINT source[2][3][4] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
        resource->present_queue = other_queue.Get();
        shim.context->UpdateSubresource(texture.Get(), 0, nullptr, source, sizeof(source[0][0]), sizeof(source[0]));
        resource->present_queue = nullptr;
    }
    expect(resource->deferred_writes.size() == 1, "the blocked texture write was not deferred");
    const size_t deferred_bytes = resource->deferred_writes.front().data.size();

    auto transition = symbols.find<decltype(&Dx12ShimTransition)>("xxmishim!Dx12ShimTransition");
    auto list = symbols.find<decltype(&Dx12ShimList)>("xxmishim!Dx12ShimList");
    auto submit = symbols.find<decltype(&Dx12ShimSubmitAndWait)>("xxmishim!Dx12ShimSubmitAndWait");
    expect(transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, false), "the deferred texture write did not replay");

    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = resource->resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    UINT64 bytes;
    shim.native_device->GetCopyableFootprints(&resource->desc, 0, 1, 0, &dst.PlacedFootprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC readback_desc = {};
    readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readback_desc.Width = bytes;
    readback_desc.Height = readback_desc.DepthOrArraySize = readback_desc.MipLevels = 1;
    readback_desc.SampleDesc.Count = 1;
    readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(shim.native_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    dst.pResource = readback.Get();
    list()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submit();

    const UINT expected[4][2] = {{1, 2}, {3, 4}, {5, 6}, {7, 8}};
    void *mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped));
    bool same = true;
    for (size_t row = 0; row < ARRAYSIZE(expected); row++)
        same &= !memcmp(static_cast<const BYTE*>(mapped) + row * dst.PlacedFootprint.Footprint.RowPitch,
            expected[row], sizeof(expected[row]));
    readback->Unmap(0, nullptr);
    expect(same, "deferred texture replay lost data from padded rows or slices");
    std::cout << deferred_bytes << " bytes held for the deferred texture\n";
    expect(deferred_bytes == sizeof(expected), "the deferred write retained upload-page padding");
}

static void shim_game_upload_probe(const ShimInterfaces &shim, const ShimSymbols &symbols, ID3D12Device *device) {
    auto address = symbols.find<decltype(&Dx12ShimGameBufferAddress)>("xxmishim!Dx12ShimGameBufferAddress");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    constexpr UINT slice_bytes = 256 * 1024;
    const GUID marker = {0x845384ef, 0x32ce, 0x4ae5, {0xbb, 0x94, 0x61, 0xb9, 0x46, 0xa4, 0xb0, 0x23}};
    std::vector<ComPtr<ID3D11Buffer>> persistent;
    unsigned recycled = 0;
    for (unsigned page = 0; page < 13; page++) {
        ComPtr<ID3D12GraphicsCommandList> list;
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        auto *proxy = reinterpret_cast<Dx12GraphicsCommandList*>(list.Get());
        for (unsigned slice = 0; slice < 4; slice++) {
            auto buffer = shim_buffer(shim, slice_bytes, D3D11_BIND_CONSTANT_BUFFER);
            UINT bytes;
            expect(address(buffer.Get(), proxy, &bytes) && bytes == slice_bytes, "game upload slice allocation failed");
            if (slice != 0)
                continue;
            if (page < 8) {
                persistent.push_back(std::move(buffer));
            } else {
                ID3D12Resource *upload = ShimResourceOf(buffer.Get())->game_upload;
                unsigned previous;
                UINT size = sizeof(previous);
                if (SUCCEEDED(upload->GetPrivateData(marker, &size, &previous)))
                    recycled++;
                check(upload->SetPrivateData(marker, sizeof(page), &page));
            }
        }
        check(list->Close());
        list.Reset();
        check(allocator->Reset());
    }
    std::cout << recycled << " recycled game upload pages out of five\n";
    expect(recycled >= 3, "pinned pages displaced all game upload reuse");
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

static void shim_bindings_probe(const ShimInterfaces &shim, const ShimSymbols &symbols, const fs::path &shader_path) {
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
    shim.context->CSSetShaderResources(120, 1, views);
    shim.context->CSSetUnorderedAccessViews(0, 4, outputs, nullptr);
    for (int attempt = 0; attempt < 2; attempt++) {
        ComPtr<ID3D11ShaderResourceView> bound;
        shim.context->CSGetShaderResources(120, 1, &bound);
        expect(!bound, "an overlapping SRV survived UAV binding");
        shim.context->CSSetShaderResources(120, 1, views);
    }
    views[0] = disjoint.Get();
    shim.context->CSSetShaderResources(120, 1, views);
    ComPtr<ID3D11ShaderResourceView> bound;
    shim.context->CSGetShaderResources(120, 1, &bound);
    expect(bound.Get() == disjoint.Get(), "a disjoint SRV was cleared");

    auto bytecode = read(shader_path);
    ComPtr<ID3D11ComputeShader> shader;
    check(shim.device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader));
    shim.context->CSSetShader(shader.Get(), nullptr, 0);
    auto heap = symbols.field<ID3D12DescriptorHeap *>("xxmishim!view_pages", {L"current", L"heap"});
    auto used = symbols.field<UINT>("xxmishim!view_pages", {L"current", L"used"});
    auto previous = *heap;
    const UINT before = *used;
    UINT slots = 0;
    unsigned page_changes = 0;
    for (int dispatch = 0; dispatch < 40; dispatch++) {
        shim.context->Dispatch(1, 1, 1);
        if (!dispatch)
            slots = *heap == previous ? *used - before : *used;
        if (previous && *heap != previous)
            page_changes++;
        previous = *heap;
    }
    const UINT expected = 17;
    expect_buffer(shim, result.Get(), &expected, sizeof(expected), "dispatch did not read the disjoint mip with null buffers");
    std::cout << slots << " view slots per dispatch, " << page_changes << " page changes\n";
    expect(slots == 7 && !page_changes, "three sparse SRVs and four UAVs did not fit in seven slots");
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

static void shim_indexed_bindings_probe(const ShimInterfaces &shim, const fs::path &shader_path) {
    const UINT expected[] = {17, 37};
    const D3D11_SUBRESOURCE_DATA data[] = {{&expected[0], sizeof(UINT), 0}, {&expected[1], sizeof(UINT), 0}};
    D3D11_TEXTURE2D_DESC texture = {};
    texture.Width = texture.Height = texture.MipLevels = texture.SampleDesc.Count = 1;
    texture.ArraySize = ARRAYSIZE(data);
    texture.Format = DXGI_FORMAT_R32_UINT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> resource;
    check(shim.device->CreateTexture2D(&texture, data, &resource));

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = texture.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MipLevels = srv.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> first, second;
    check(shim.device->CreateShaderResourceView(resource.Get(), &srv, &first));
    srv.Texture2DArray.FirstArraySlice = 1;
    check(shim.device->CreateShaderResourceView(resource.Get(), &srv, &second));
    ID3D11ShaderResourceView *views[] = {first.Get(), second.Get()};
    shim.context->CSSetShaderResources(63, ARRAYSIZE(views), views);

    auto result = shim_buffer(shim, sizeof(expected), D3D11_BIND_UNORDERED_ACCESS);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = texture.Format;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = ARRAYSIZE(expected);
    ComPtr<ID3D11UnorderedAccessView> result_view;
    check(shim.device->CreateUnorderedAccessView(result.Get(), &uav, &result_view));
    shim.context->CSSetUnorderedAccessViews(1, 1, result_view.GetAddressOf(), nullptr);

    auto bytecode = read(shader_path);
    ComPtr<ID3D11ComputeShader> shader;
    check(shim.device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader));
    shim.context->CSSetShader(shader.Get(), nullptr, 0);
    shim.context->Dispatch(1, 1, 1);
    expect_buffer(shim, result.Get(), expected, sizeof(expected), "a dynamically indexed array across t63 and t64 was not contiguous");
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

static void shim_unsupported_bindings_probe(const ShimInterfaces &shim, const fs::path &shader_path, bool ray_query) {
    auto bytecode = read(shader_path);
    ComPtr<ID3D11ComputeShader> shader;
    check(shim.device->CreateComputeShader(bytecode.data(), bytecode.size(), nullptr, &shader));
    if (ray_query) {
        const auto *reflected = ShimShaderOf(shader.Get());
        expect(reflected->srv_used[0] == 3 && reflected->srv_dimension[0] == D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE,
            "the acceleration structure was missing from the reflected SRV table");
    }
    const UINT initial[] = {13, 29};
    auto result = shim_buffer(shim, sizeof(initial), D3D11_BIND_UNORDERED_ACCESS, initial);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R32_UINT;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = ARRAYSIZE(initial);
    ComPtr<ID3D11UnorderedAccessView> result_view;
    check(shim.device->CreateUnorderedAccessView(result.Get(), &uav, &result_view));
    shim.context->CSSetUnorderedAccessViews(0, 1, result_view.GetAddressOf(), nullptr);
    shim.context->CSSetShader(shader.Get(), nullptr, 0);
    shim.context->Dispatch(1, 1, 1);
    expect_buffer(shim, result.Get(), initial, sizeof(initial), "a shader with an unsupported SRV binding was dispatched");
    shim.context->ClearState();
}

static void shim_queue_lifetime_probe(const ShimInterfaces &shim, const ShimSymbols &symbols, ID3D12Device *device) {
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
    const UINT initial = 13, replacement = 29;
    auto busy = shim_buffer(shim, sizeof(UINT), D3D11_BIND_VERTEX_BUFFER, &initial);
    auto complete = shim_buffer(shim, sizeof(UINT), D3D11_BIND_VERTEX_BUFFER, &initial);
    auto recording = shim_buffer(shim, sizeof(UINT), D3D11_BIND_VERTEX_BUFFER, &initial);
    ID3D12Resource *busy_before = ShimResourceOf(busy.Get())->resource;
    ID3D12Resource *complete_before = ShimResourceOf(complete.Get())->resource;
    ID3D12Resource *recording_before = ShimResourceOf(recording.Get())->resource;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    auto use = symbols.find<decltype(&Dx12ShimUseOnGameList)>("xxmishim!Dx12ShimUseOnGameList");
    expect(use(ShimResourceOf(recording.Get()), reinterpret_cast<Dx12GraphicsCommandList*>(list.Get())), "could not pin the recording buffer");
    collect();

    ComPtr<ID3D12Fence> gate, finished;
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    check(shim.native_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&finished)));
    auto close_event = [](HANDLE handle) { CloseHandle(handle); };
    std::unique_ptr<void, decltype(close_event)> event(CreateEventW(nullptr, FALSE, FALSE, nullptr), close_event);
    expect(!!event, "create foreign queue event");
    check(queue->Wait(gate.Get(), 1));
    busy_before->AddRef();
    complete_before->AddRef();
    std::vector<IUnknown *> pins = {new ReleaseNotice(pin_released), busy_before, complete_before}, first;
    pins_executed(queue.Get(), &pins, &first);
    check(queue->Signal(finished.Get(), 1));
    shim.context->UpdateSubresource(busy.Get(), 0, nullptr, &replacement, 0, 0);
    bool held = !pin_released->load();
    check(gate->Signal(1));
    check(finished->SetEventOnCompletion(1, event.get()));
    expect(WaitForSingleObject(event.get(), 30000) == WAIT_OBJECT_0, "foreign queue did not finish");
    shim.context->UpdateSubresource(recording.Get(), 0, nullptr, &replacement, 0, 0);
    expect(!pin_released->load() && ShimResourceOf(recording.Get())->resource != recording_before,
        "a recording pin did not protect its buffer without polling the foreign queue");
    shim.context->UpdateSubresource(complete.Get(), 0, nullptr, &replacement, 0, 0);
    expect(held && ShimResourceOf(busy.Get())->resource != busy_before, "an unfinished foreign queue pin did not protect the buffer");
    expect(pin_released->load() && ShimResourceOf(complete.Get())->resource == complete_before,
        "a completed foreign queue pin forced a rename before the next submission");
    expect_buffer(shim, complete.Get(), &replacement, sizeof(replacement), "a write after foreign completion was lost");
    check(list->Close());
    queue.Reset();
    collect();
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
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 120, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
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
    expect(info.srv_dimension[1] == D3D12_SRV_DIMENSION_BUFFER && info.srv_buffer[1] == 8,
            "stripped DXIL structured SRV has no element stride");
    expect(info.uav_dimension[3] == D3D12_UAV_DIMENSION_BUFFER && info.uav_buffer[3] == 8,
            "stripped DXIL structured UAV has no element stride");
    expect(info.srv_buffer[0] == DX12_BUFFER_RAW && info.uav_buffer[2] == DX12_BUFFER_RAW,
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
        else if (!wcscmp(mode, L"texture-replay"))
            shim_texture_replay_probe(*shim, symbols);
        else if (!wcscmp(mode, L"game-upload"))
            shim_game_upload_probe(*shim, symbols, device.Get());
        else if (!wcscmp(mode, L"depth"))
            shim_depth_probe(*shim);
        else if (!wcscmp(mode, L"bindings"))
            shim_bindings_probe(*shim, symbols, shader_path);
        else if (!wcscmp(mode, L"bindings-indexed"))
            shim_indexed_bindings_probe(*shim, shader_path);
        else if (!wcscmp(mode, L"bindings-ray-query") || !wcscmp(mode, L"bindings-unsupported"))
            shim_unsupported_bindings_probe(*shim, shader_path, !wcscmp(mode, L"bindings-ray-query"));
        else if (!wcscmp(mode, L"queue-lifetime"))
            shim_queue_lifetime_probe(*shim, symbols, device.Get());
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
