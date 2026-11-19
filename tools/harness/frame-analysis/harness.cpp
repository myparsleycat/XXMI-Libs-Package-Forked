// Synthetic DDS fixture; no game assets, keyboard input or visible window.
// Game-independent command-list scope reproduction. No game assets or hashes.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
static void check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT " + std::to_string((unsigned)hr)); }
static std::vector<char> read(const fs::path &p) { std::ifstream f(p, std::ios::binary); if (!f) throw std::runtime_error("read"); return {std::istreambuf_iterator<char>(f), {}}; }
static void write(const fs::path &p, const void *v, size_t n) { std::ofstream f(p, std::ios::binary); f.write((const char *)v, n); if (!f) throw std::runtime_error("write"); }
static const char *vs_source = "float4 main(uint id:SV_VertexID):SV_Position { float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)}; return float4(p[id],0.5,1); }";
static const char *ps_source = "Texture2D<float4> sourceTex : register(t0); float4 main():SV_Target { return sourceTex.Load(int3(0,0,0)); }";
static void compile(const char *s, const char *model, const fs::path &out) {
    ComPtr<ID3DBlob> b, error;
    HRESULT hr = D3DCompile(s, strlen(s), "synthetic", nullptr, nullptr, "main", model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &error);
    if (FAILED(hr) && error) std::cerr << (char *)error->GetBufferPointer();
    check(hr); write(out, b->GetBufferPointer(), b->GetBufferSize());
}
static HWND window() {
    WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ScopeReproHidden";
    if (!RegisterClassW(&wc)) throw std::runtime_error("RegisterClass");
    HWND w = CreateWindowW(wc.lpszClassName, L"Offline scope reproduction", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) throw std::runtime_error("CreateWindow"); return w;
}
static ComPtr<ID3D12Resource> buffer(ID3D12Device *d, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES h = {}; h.Type = type; h.CreationNodeMask = h.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC r = {}; r.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; r.Width = bytes; r.Height = 1; r.DepthOrArraySize = r.MipLevels = 1; r.SampleDesc.Count = 1; r.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> p; check(d->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &r, state, nullptr, IID_PPV_ARGS(&p))); return p;
}
static void upload(ID3D12Resource *r, const void *b, size_t n) { void *p; D3D12_RANGE empty = {}; check(r->Map(0, &empty, &p)); memcpy(p, b, n); r->Unmap(0, nullptr); }
static void transition(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.StateBefore = a; x.Transition.StateAfter = b; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; l->ResourceBarrier(1, &x);
}
static void wait(ID3D12CommandQueue *q, ID3D12Fence *f, UINT64 value, HANDLE e) {
    check(q->Signal(f, value)); check(f->SetEventOnCompletion(value, e));
    if (WaitForSingleObject(e, 30000) != WAIT_OBJECT_0) throw std::runtime_error("fence timeout");
}
static void (*trigger_capture)() = nullptr;
static HMODULE test_module = nullptr;
static BOOL (*capture_state)() = nullptr;
struct Pixels { unsigned red = 0, green = 0, black = 0, other = 0; };
static Pixels pixels(const unsigned char *data, UINT pitch) {
    Pixels p;
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++) {
        const auto *c = data + y * pitch + x * 4;
        if (c[0] == 255 && c[1] == 0 && c[2] == 0) p.red++;
        else if (c[0] == 0 && c[1] == 255 && c[2] == 0) p.green++;
        else if (!c[0] && !c[1] && !c[2]) p.black++;
        else p.other++;
    }
    return p;
}
static Pixels dx12(HWND w, const fs::path &dir) {
    ComPtr<IDXGIFactory4> f; check(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
    ComPtr<IDXGIAdapter> a; check(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));
    ComPtr<ID3D12Device> d; check(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d)));
    D3D12_COMMAND_QUEUE_DESC qd = {}; ComPtr<ID3D12CommandQueue> q; check(d->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ComPtr<ID3D12Fence> fence; check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); if (!event) throw std::runtime_error("event"); UINT64 tick = 0;
    DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.Width = sd.Height = 32; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap; check(f->CreateSwapChainForHwnd(q.Get(), w, &sd, nullptr, nullptr, &swap));
    check(swap->Present(0, 0)); wait(q.Get(), fence.Get(), ++tick, event);
    D3D12_ROOT_SIGNATURE_DESC rd = {}; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12_DESCRIPTOR_RANGE range0 = {}; range0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range0.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp = {}; rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL; rp.DescriptorTable = {1, &range0};
    rd.NumParameters = 1; rd.pParameters = &rp;
    ComPtr<ID3DBlob> sig, error; check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &error));
    ComPtr<ID3D12RootSignature> root; check(d->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)));
    auto vs = read(dir / "vs.bin"), ps = read(dir / "ps.bin");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = root.Get(); pd.VS = {vs.data(), vs.size()}; pd.PS = {ps.data(), ps.size()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso; check(d->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT; hp.CreationNodeMask = hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC tr = {}; tr.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; tr.Width = tr.Height = 16; tr.DepthOrArraySize = tr.MipLevels = 1; tr.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tr.SampleDesc.Count = 1; tr.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> target; check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &tr, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap; check(d->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap))); auto rtv = heap->GetCPUDescriptorHandleForHeapStart(); d->CreateRenderTargetView(target.Get(), nullptr, rtv);
    unsigned original[] = {0, 0, 0, 0, 1, 2};
    auto up = buffer(d.Get(), sizeof(original), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ); upload(up.Get(), original, sizeof(original));
    auto ib = buffer(d.Get(), sizeof(original), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12CommandAllocator> alloc; check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> l; check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), pso.Get(), IID_PPV_ARGS(&l)));
    l->CopyBufferRegion(ib.Get(), 0, up.Get(), 0, sizeof(original)); transition(l.Get(), ib.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    auto texture_desc = tr; texture_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> texture; check(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &texture_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT tex_foot = {}; UINT64 tex_bytes = 0;
    d->GetCopyableFootprints(&texture_desc,0,1,0,&tex_foot,nullptr,nullptr,&tex_bytes);
    auto tex_upload = buffer(d.Get(),tex_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    std::vector<unsigned char> tex_pixels((size_t)tex_bytes,0);
    for (UINT y=0; y<16; y++) for(UINT x=0; x<16; x++) { auto p=&tex_pixels[tex_foot.Offset+y*tex_foot.Footprint.RowPitch+4*x];p[0]=p[3]=255; }
    upload(tex_upload.Get(),tex_pixels.data(),tex_pixels.size());
    D3D12_TEXTURE_COPY_LOCATION tex_src={},tex_dst={};tex_src.pResource=tex_upload.Get();tex_src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;tex_src.PlacedFootprint=tex_foot;tex_dst.pResource=texture.Get();tex_dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    l->CopyTextureRegion(&tex_dst,0,0,0,&tex_src,nullptr);
    transition(l.Get(),texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    D3D12_DESCRIPTOR_HEAP_DESC views={};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;views.NumDescriptors=1;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> view_heap;check(d->CreateDescriptorHeap(&views,IID_PPV_ARGS(&view_heap)));
    D3D12_SHADER_RESOURCE_VIEW_DESC sd0={};sd0.Format=texture_desc.Format;sd0.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sd0.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sd0.Texture2D.MipLevels=1;
    d->CreateShaderResourceView(texture.Get(),&sd0,view_heap->GetCPUDescriptorHandleForHeapStart());
    ID3D12DescriptorHeap *heaps[]={view_heap.Get()};l->SetDescriptorHeaps(1,heaps);
    l->SetGraphicsRootSignature(root.Get()); l->SetGraphicsRootDescriptorTable(0,view_heap->GetGPUDescriptorHandleForHeapStart()); D3D12_VIEWPORT vp = {0, 0, 16, 16, 0, 1}; D3D12_RECT sc = {0, 0, 16, 16}; l->RSSetViewports(1, &vp); l->RSSetScissorRects(1, &sc); l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float clear[] = {0, 0, 0, 1}; l->ClearRenderTargetView(rtv, clear, 0, nullptr); l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); D3D12_INDEX_BUFFER_VIEW iv = {ib->GetGPUVirtualAddress(), sizeof(original), DXGI_FORMAT_R32_UINT}; l->IASetIndexBuffer(&iv);
    if (trigger_capture) trigger_capture();
    if(!capture_state())throw std::runtime_error("capture was not armed");

    wchar_t gap_env[8]={};GetEnvironmentVariableW(L"CAPTURE_GAP_PRESENTS",gap_env,8);unsigned gap=_wtoi(gap_env);
    if(gap>6)throw std::runtime_error("fixture gap outside bound");
    for(unsigned n=0;n<gap;n++){
        check(swap->Present(0,0));wait(q.Get(),fence.Get(),++tick,event);
        std::cerr<<"capture_state["<<n+1<<"]="<<capture_state()<<"\n";
    }
    wchar_t action_env[32]={};GetEnvironmentVariableW(L"CAPTURE_ACTION",action_env,32);
    if(!wcscmp(action_env,L"abort")){
        if(!capture_state())throw std::runtime_error("capture expired before abort");
        trigger_capture();
        if(capture_state())throw std::runtime_error("abort did not disarm");
    }else if(!wcscmp(action_env,L"retry")){
        if(capture_state())throw std::runtime_error("capture did not expire");
        Sleep(1100); // FrameAnalysis directory names are limited to one/second.
        trigger_capture();
        if(!capture_state())throw std::runtime_error("retry did not arm");
    }

    l->DrawIndexedInstanced(3, 1, 3, 0, 0);
    transition(l.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {}; UINT64 bytes = 0; d->GetCopyableFootprints(&tr, 0, 1, 0, &foot, nullptr, nullptr, &bytes);
    auto rb = buffer(d.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {}; src.pResource = target.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.pResource = rb.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = foot; l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(l->Close()); ID3D12CommandList *lists[] = {l.Get()}; q->ExecuteCommandLists(1, lists); wait(q.Get(), fence.Get(), ++tick, event); check(d->GetDeviceRemovedReason());
    void *mapped; D3D12_RANGE range = {0, (SIZE_T)bytes}; check(rb->Map(0, &range, &mapped)); Pixels result = pixels((unsigned char *)mapped + foot.Offset, foot.Footprint.RowPitch); D3D12_RANGE empty = {}; rb->Unmap(0, &empty);
    check(swap->Present(0, 0)); wait(q.Get(), fence.Get(), ++tick, event); CloseHandle(event); return result;
}
int wmain(int argc, wchar_t **argv) { try {
    if (argc == 3 && !wcscmp(argv[1], L"compile")) { fs::path out = argv[2]; compile(vs_source, "vs_5_0", out / "vs.bin"); compile(ps_source, "ps_5_0", out / "ps.bin"); return 0; }
    if (argc != 4) throw std::runtime_error("usage: harness dx12 <test-DLL> <shader-directory>");
    if(wcscmp(argv[1],L"dx12"))throw std::runtime_error("API must be dx12");
    if (wcscmp(argv[2], L"none") && !(test_module = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))) throw std::runtime_error("DLL load");
    trigger_capture = (void(*)())GetProcAddress(test_module, "FrameAnalysisTestRequest"); if (!trigger_capture) throw std::runtime_error("missing offline test export");
    capture_state=(BOOL(*)())GetProcAddress(test_module,"FrameAnalysisTestState");
    if(!capture_state)throw std::runtime_error("missing test state hook");
    HWND w = window(); Pixels p = dx12(w, argv[3]); DestroyWindow(w);
    std::cout << "{\"red\":" << p.red << ",\"green\":" << p.green << ",\"black\":" << p.black << ",\"other\":" << p.other << "}\n"; return 0;
} catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; } }
