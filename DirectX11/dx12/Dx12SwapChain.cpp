#include "Dx12SwapChain.h"

#include <algorithm>

#include "Bridge.h"
#include "Dx12CommandQueue.h"
#include "Dx12Device.h"
#include "DrawPath.h"
#include "PresentPass.h"
#include "../profiling.h"

// {6216F1D1-6BC6-4B1C-8CB5-7D5F8A18C381}
const GUID Dx12SwapChain::ProxyIID = { 0x6216f1d1, 0x6bc6, 0x4b1c, { 0x8c, 0xb5, 0x7d, 0x5f, 0x8a, 0x18, 0xc3, 0x81 } };
void *Dx12SwapChain::sVtbl;

static const IID *const swap_chain_iids[] = {
	&__uuidof(IDXGISwapChain), &__uuidof(IDXGISwapChain1), &__uuidof(IDXGISwapChain2),
	&__uuidof(IDXGISwapChain3), &__uuidof(IDXGISwapChain4),
};

Dx12SwapChain::Dx12SwapChain(Dx12CommandQueue *queue, IDXGISwapChain *orig) :
	mRef(1),
	mQueue(queue)
{
	mVersion = Dx12QueryNewest(orig, swap_chain_iids, ARRAYSIZE(swap_chain_iids), (void**)&mOrig);
	mQueue->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12SwapChain::Supports(REFIID riid)
{
	int version;

	if (IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(IDXGIObject))
	    || IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)))
		return true;

	version = Dx12FindIID(swap_chain_iids, ARRAYSIZE(swap_chain_iids), riid);
	return version >= 0 && version <= mVersion;
}

void Dx12WrapSwapChain(Dx12CommandQueue *queue, IDXGISwapChain **ppSwapChain)
{
	IDXGISwapChain *orig = *ppSwapChain;
	Dx12SwapChain *proxy = new Dx12SwapChain(queue, orig);

	// The proxy took its own reference with the newest interface.
	orig->Release();
	*ppSwapChain = proxy;

	Dx12LogInfo("Dx12SwapChain %p created to wrap IDXGISwapChain%d %p presenting on Dx12CommandQueue %p\n",
		proxy, proxy->mVersion, proxy->mOrig, queue);
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::QueryInterface(REFIID riid, void **ppvObject)
{
	HRESULT hr;

	if (!ppvObject)
		return E_POINTER;

	if (Supports(riid)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	hr = mOrig->QueryInterface(riid, ppvObject);
	Dx12LogForwardedQuery("Dx12SwapChain", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12SwapChain::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12SwapChain::Release()
{
	// Read first: once our reference is gone, so may be this object.
	IDXGISwapChain4 *orig = mOrig;
	Dx12CommandQueue *queue = mQueue;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	Dx12LogInfo("Dx12SwapChain %p destroyed\n", this);
	Dx12SwapChainBuffersIdle(this);
	Dx12SwapChainBuffersReleased(this);
	delete this;
	orig->Release();
	queue->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::GetDevice(REFIID riid, void **ppDevice)
{
	// For a D3D12 swap chain DXGI answers this with the device of the
	// queue, not with the queue it was created on. Overlays rely on it.
	HRESULT hr = mOrig->GetDevice(riid, ppDevice);


	if (SUCCEEDED(hr) && ppDevice && *ppDevice && mQueue->mDevice->Supports(riid)) {
		((IUnknown*)*ppDevice)->Release();
		mQueue->mDevice->AddRef();
		*ppDevice = mQueue->mDevice;
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	HRESULT hr;
	bool active;

	if (Flags & DXGI_PRESENT_TEST)
		return mOrig->Present(SyncInterval, Flags);

	Profiling::Spike::frame(dx12_mods_disabled);
	{
		Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_PRE);
		active = Dx12PresentBegin(this);
		Dx12FramePresented();
	}
	{
		Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_CALL);
		hr = mOrig->Present(SyncInterval, Flags);
	}
	Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_POST);
	if (active)
		Dx12PresentEnd(this);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
	HRESULT hr;
	bool active;

	if (PresentFlags & DXGI_PRESENT_TEST)
		return mOrig->Present1(SyncInterval, PresentFlags, pPresentParameters);

	Profiling::Spike::frame(dx12_mods_disabled);
	{
		Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_PRE);
		active = Dx12PresentBegin(this);
		Dx12FramePresented();
	}
	{
		Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_CALL);
		hr = mOrig->Present1(SyncInterval, PresentFlags, pPresentParameters);
	}
	Profiling::Spike::Scope spike(Profiling::Spike::PRESENT_POST);
	if (active)
		Dx12PresentEnd(this);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	HRESULT hr = BeginResize();
	if (FAILED(hr))
		return hr;
	hr = mOrig->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
	// DXGI assigns every buffer to the creation queue on ResizeBuffers.
	FinishResize(hr, {}, true);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue)
{
	using Microsoft::WRL::ComPtr;
	std::vector<ComPtr<ID3D12CommandQueue>> queues;
	std::vector<IUnknown*> originals;
	HRESULT hr = BeginResize();
	if (FAILED(hr))
		return hr;

	UINT count = BufferCount;
	if (ppPresentQueue && !count) {
		DXGI_SWAP_CHAIN_DESC1 desc;
		hr = mOrig->GetDesc1(&desc);
		if (SUCCEEDED(hr))
			count = desc.BufferCount;
	}
	if (SUCCEEDED(hr) && ppPresentQueue && (!count || count > DXGI_MAX_SWAP_CHAIN_BUFFERS))
		hr = E_INVALIDARG;
	if (FAILED(hr)) {
		FinishResize(hr, {}, false);
		return hr;
	}

	bool queues_known = true;
	if (ppPresentQueue) {
		UINT node = max(1u, mQueue->mOrig->GetDesc().NodeMask);
		for (UINT i = 0; i < count; i++) {
			IUnknown *unknown = ppPresentQueue[i];
			ComPtr<Dx12CommandQueue> proxy;
			proxy.Attach(Dx12QueryProxy<Dx12CommandQueue>(unknown));
			ComPtr<ID3D12CommandQueue> queue;
			if (proxy)
				queue = proxy->mOrig;
			else if (unknown)
				unknown->QueryInterface(IID_PPV_ARGS(&queue));
			originals.push_back(queue ? queue.Get() : unknown);
			ComPtr<ID3D12Device> device;
			if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device.Get() != mQueue->mDevice->mOrig
			    || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT || max(1u, queue->GetDesc().NodeMask) != node
			    || (pCreationNodeMask && max(1u, pCreationNodeMask[i]) != node))
				queues_known = false;
			queues.push_back(std::move(queue));
		}
	}
	hr = mOrig->ResizeBuffers1(count, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
			ppPresentQueue ? originals.data() : nullptr);
	FinishResize(hr, std::move(queues), queues_known);
	return hr;
}

HRESULT Dx12SwapChain::BeginResize()
{
	Dx12ShimLock();
	HRESULT hr = mResizing ? DXGI_ERROR_WAS_STILL_DRAWING : Dx12SwapChainBuffersIdle(this);
	if (SUCCEEDED(hr)) {
		mResizing = true;
		Dx12SwapChainBuffersSuspended(this);
	}
	Dx12ShimUnlock();
	return hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : hr;
}

void Dx12SwapChain::FinishResize(HRESULT hr, std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> queues,
		bool queues_known)
{
	Dx12ShimLock();
	Dx12SwapChainBuffersResized(this, hr);
	if (SUCCEEDED(hr)) {
		mPresentQueuesKnown = queues_known;
		if (!mPresentQueuesKnown || std::all_of(queues.begin(), queues.end(),
				[&](const auto &queue) { return queue.Get() == mQueue->mOrig; }))
			queues.clear();
		mPresentQueues.swap(queues);
	}
	mResizing = false;
	Dx12ShimUnlock();
}

ID3D12CommandQueue *Dx12SwapChain::PresentQueue(UINT buffer_index) const
{
	if (mResizing || !mPresentQueuesKnown)
		return nullptr;
	if (mPresentQueues.empty())
		return mQueue->mOrig;
	return buffer_index < mPresentQueues.size() ? mPresentQueues[buffer_index].Get() : nullptr;
}

#include "generated/Dx12SwapChain.fwd.inl"
