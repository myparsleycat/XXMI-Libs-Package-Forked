#pragma once

#include <vector>
#include <wrl/client.h>

#include "Dx12Common.h"

class Dx12CommandQueue;

class Dx12SwapChain final : public IDXGISwapChain4
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	IDXGISwapChain4 *mOrig;
	// Highest N of the IDXGISwapChainN the original implements
	int mVersion;
	volatile LONG mRef;

	// The creation queue anchors the swap chain's device queries.
	Dx12CommandQueue *mQueue;

	Dx12SwapChain(Dx12CommandQueue *queue, IDXGISwapChain *orig);

	bool Supports(REFIID riid);
	// Borrowed with the shim lock held.
	ID3D12CommandQueue *PresentQueue(UINT buffer_index) const;
	bool Resizing() const { return mResizing; }

#include "generated/Dx12SwapChain.decl.inl"

private:
	std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> mPresentQueues;
	bool mPresentQueuesKnown = true;
	bool mResizing = false;

	HRESULT BeginResize();
	void FinishResize(HRESULT hr, std::vector<Microsoft::WRL::ComPtr<ID3D12CommandQueue>> queues,
			bool queues_known);
};

// Replaces a swap chain DXGI just created on the original of queue with its
// proxy.
void Dx12WrapSwapChain(Dx12CommandQueue *queue, IDXGISwapChain **ppSwapChain);
