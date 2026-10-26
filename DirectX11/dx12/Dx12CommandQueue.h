#pragma once

#include "Dx12Common.h"

class Dx12Device;

class Dx12CommandQueue final : public ID3D12CommandQueue
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12CommandQueue *mOrig;
	Dx12Device *mDevice;
	volatile LONG mRef;
	// Lives as long as the original, see Tie
	bool mTied;

	Dx12CommandQueue(Dx12Device *device, ID3D12CommandQueue *orig);
	// See Dx12KeepGone
	static void operator delete(void *storage);

	bool Usable(REFIID riid);
	void Abandon();
	void Tie();

#include "generated/Dx12CommandQueue.decl.inl"
};

// For telling whether a queue can be made to wait for a fence without two
// queues ending up waiting for each other. That takes knowing every signal and
// wait the queues were handed: the proxies note the game's, and whoever else
// hands a queue a wait, or a signal another queue is made to wait for, has to
// as well (a signal after the queue has it, a wait before).
void Dx12QueueSignalled(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);
void Dx12QueueWaits(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);
// Held from noting a signal, a wait or an execution with buffer writes until
// the queue has it, or the other way round, so that they are noted in the
// order the queue has them, which Dx12QueueAfter goes by. Taken after the
// shim's lock, before any other of ours.
void Dx12QueueLock(ID3D12CommandQueue *queue);
void Dx12QueueUnlock(ID3D12CommandQueue *queue);
// Whether the signal that takes the fence to the value has been handed to a
// queue that gets to it with what the queues have been handed so far. Then a
// wait for it handed to a queue now cannot be one of a circle.
bool Dx12QueueCanWait(ID3D12Fence *fence, UINT64 value);
// The place of an execution of lists among those signals and waits, taken
// before the lists are handed to the queue, with Dx12QueueLock held
UINT64 Dx12QueueExecutes(ID3D12CommandQueue *queue);
// Whether the GPU gets through what writer was handed up to the execution of
// that order before anything queue is handed now: writer is queue, or it was
// handed a signal after the execution that the fence has got to, or queue was
// handed a wait for one.
bool Dx12QueueAfter(ID3D12CommandQueue *queue, ID3D12CommandQueue *writer, UINT64 order);
