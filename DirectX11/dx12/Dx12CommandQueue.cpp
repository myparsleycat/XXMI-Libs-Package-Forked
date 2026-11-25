#include "Dx12CommandQueue.h"

#include <algorithm>
#include <array>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Bridge.h"
#include "Dx12CommandList.h"
#include "Dx12Device.h"
#include "ResourceTracker.h"
#include "UploadHashReadback.h"
#include "../profiling.h"

// {1B67B403-353E-45FD-B468-760207CA7886}
const GUID Dx12CommandQueue::ProxyIID = { 0x1b67b403, 0x353e, 0x45fd, { 0xb4, 0x68, 0x76, 0x02, 0x07, 0xca, 0x78, 0x86 } };
void *Dx12CommandQueue::sVtbl;

Dx12CommandQueue::Dx12CommandQueue(Dx12Device *device, ID3D12CommandQueue *orig) :
	mOrig(orig),
	mDevice(device),
	mRef(1),
	mTied(false)
{
	mOrig->AddRef();
	mDevice->AddRef();
	sVtbl = *(void**)this;
}

void Dx12CommandQueue::operator delete(void *storage)
{
	Dx12KeepGone(storage);
}

bool Dx12CommandQueue::Usable(REFIID riid)
{
	return IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild))
	    || IsEqualIID(riid, __uuidof(ID3D12Pageable))
	    || IsEqualIID(riid, __uuidof(ID3D12CommandQueue));
}

void Dx12CommandQueue::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

// {7E3A61C4-2B9D-4F58-A0E7-5C14D8B2F396}
static const GUID queue_destroyed_guid = { 0x7e3a61c4, 0x2b9d, 0x4f58, { 0xa0, 0xe7, 0x5c, 0x14, 0xd8, 0xb2, 0xf3, 0x96 } };

static void queue_destroyed(void *context)
{
	Dx12CommandQueue *queue = (Dx12CommandQueue*)context;
	Dx12Device *device = queue->mDevice;

	Dx12LogInfo("Dx12CommandQueue %p destroyed\n", queue);
	Dx12ForgetQueueOrdering(queue->mOrig);
	delete queue;
	device->Release();
}

// A D3D12 object lives while any of its interfaces has a reference, and the
// proxy only counts those taken through it. Whoever holds the queue by an
// interface the proxy forwards to the original may still use the proxy after
// the game let go of it: RTSS hands it to D3D11On12CreateDevice. So the proxy
// goes when the original does.
void Dx12CommandQueue::Tie()
{
	mTied = Dx12NotifyDestruction(mOrig, queue_destroyed_guid, queue_destroyed, this);
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::QueryInterface(REFIID riid, void **ppvObject)
{
	HRESULT hr;

	if (!ppvObject)
		return E_POINTER;

	if (Usable(riid)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	hr = mOrig->QueryInterface(riid, ppvObject);
	Dx12LogForwardedQuery("Dx12CommandQueue", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12CommandQueue *orig = mOrig;
	Dx12Device *device = mDevice;
	bool tied = mTied;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref || tied) {
		orig->Release();
		return ref;
	}

	Dx12LogInfo("Dx12CommandQueue %p destroyed\n", this);
	delete this;
	orig->Release();
	device->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

// Whether the lists being executed can write the memory the upload is read from
static bool gpu_writable(ID3D12Resource *upload)
{
	D3D12_HEAP_PROPERTIES heap;
	Dx12ResourceInfo info;

	if (Dx12GetResourceInfo(upload, &info))
		return info.heap_type != D3D12_HEAP_TYPE_UPLOAD;
	return FAILED(upload->GetHeapProperties(&heap, NULL)) || heap.Type != D3D12_HEAP_TYPE_UPLOAD;
}

// The uploads no GPU copy was made of. Before the lists are submitted, only
// those the lists can write are hashed and the rest are kept. After, the rest
// are, so that the GPU and the queue do not wait for the slow reads: the game
// keeps upload heap memory as it is until the lists are done, which it learns
// from a signal it can only queue after the lists are submitted.
template <typename Upload>
static void hash_on_cpu(std::vector<Upload> *uploads, bool before_lists, volatile LONG64 *count, void (*hash)(const Upload*))
{
	size_t i, kept = 0;

	for (i = 0; i < uploads->size(); i++) {
		if (before_lists && !gpu_writable((*uploads)[i].upload)) {
			(*uploads)[kept++] = (*uploads)[i];
			continue;
		}
		InterlockedIncrement64(count);
		hash(&(*uploads)[i]);
	}
	uploads->resize(kept);
}

void STDMETHODCALLTYPE Dx12CommandQueue::ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists)
{
	ID3D12CommandList *stack[65], **lists = stack + 1;
	std::vector<ID3D12CommandList*> large;
	std::vector<IUnknown*> pins, first;
	std::vector<Dx12GraphicsCommandList*> texture_uploads;
	std::vector<Dx12TextureUpload> on_cpu;
	std::vector<Dx12BufferUpload> buffer_uploads, buffers_on_cpu;
	ID3D12CommandList *upload_list = NULL;
	void *upload_context = NULL;
	UINT64 needed = 0, order;
	INT64 hash_ticks = 0;
	size_t upload_count;
	bool buffer_writes = false, locked;
	UINT i;

	if (!ppCommandLists) {
		mOrig->ExecuteCommandLists(NumCommandLists, ppCommandLists);
		return;
	}

	LARGE_INTEGER start = {};
	if (Profiling::Spike::threshold)
		QueryPerformanceCounter(&start);

	// One more in front for a list of ours, see Dx12UploadHashesExecuting
	if (NumCommandLists > ARRAYSIZE(stack) - 1) {
		large.resize(NumCommandLists + 1);
		lists = large.data() + 1;
	}

	// What the mods had done for the draws of these lists comes first, and
	// was recorded with the states their resources had before them
	Dx12ShimFlush();

	for (i = 0; i < NumCommandLists; i++) {
		lists[i] = Dx12Unwrap<Dx12GraphicsCommandList>(ppCommandLists[i]);
		if (lists[i] == ppCommandLists[i])
			continue;

		// From here on the states the list was recorded to leave its
		// resources in are the ones they have, as far as anything
		// executed later is concerned
		Dx12GraphicsCommandList *proxy = static_cast<Dx12GraphicsCommandList*>(ppCommandLists[i]);
		for (const Dx12Transition &transition : proxy->mTransitions)
			Dx12ApplyTransition(transition.resource, transition.subresource, transition.state);
		if (!proxy->mBufferWrites.empty())
			buffer_writes = true;
		// The list keeps what it pinned, for being executed again:
		// this execution gets references of its own, together so
		// that one signal of the fence does for all of them
		for (IUnknown *object : proxy->mPins) {
			object->AddRef();
			pins.push_back(object);
		}
		if (!proxy->mPins.empty())
			needed = max(needed, proxy->mShimFence);
		// What was worked out for its draws went by the queue, see
		// Dx12DrawPipelineWanted
		if (proxy->mQueue != mOrig) {
			proxy->mQueue = mOrig;
			proxy->mWantedGeneration = 0;
		}
		if (!proxy->mExecuted) {
			proxy->mExecuted = true;
			first.insert(first.end(), proxy->mPins.begin(), proxy->mPins.end());
			if (!proxy->mTextureUploads.empty())
				texture_uploads.push_back(proxy);
		}
	}

	InterlockedIncrement64(&dx12_stats.execute_calls);
	InterlockedAdd64(&dx12_stats.lists_executed, NumCommandLists);

	if (!pins.empty() || Dx12ShimQueueUnordered(mOrig))
		Dx12ShimQueueWaits(mOrig, needed, !pins.empty());

	// A list can only reach a resource of the mods it holds a pin on, so
	// the game's lists that use none go ahead of what is being paged in
	if (!pins.empty())
		Dx12ShimResidencyWait(mOrig);

	locked = buffer_writes || !texture_uploads.empty();
	if (locked)
		Dx12QueueLock(mOrig);
	if (buffer_writes) {
		order = Dx12QueueExecutes(mOrig);
		for (i = 0; i < NumCommandLists; i++) {
			if (lists[i] == ppCommandLists[i])
				continue;
			Dx12GraphicsCommandList *proxy = static_cast<Dx12GraphicsCommandList*>(ppCommandLists[i]);
			if (!proxy->mBufferWrites.empty())
				Dx12BufferWritesExecuted(&proxy->mBufferWrites, mOrig, order, &buffer_uploads, &hash_ticks);
		}
	}
	if (start.QuadPart)
		Profiling::Spike::add(Profiling::Spike::DX12_EXECUTE, start.QuadPart + hash_ticks, L"before", 0);
	if (!texture_uploads.empty() || !buffer_uploads.empty()) {
		upload_count = buffer_uploads.size();
		for (Dx12GraphicsCommandList *proxy : texture_uploads)
			upload_count += proxy->mTextureUploads.size();
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_UPLOAD_HASH_COPY, L"uploads", upload_count);
		upload_list = Dx12UploadHashesExecuting(mOrig, texture_uploads.data(), texture_uploads.size(), buffer_uploads,
				&on_cpu, &buffers_on_cpu, &upload_context);
	}
	hash_on_cpu(&on_cpu, true, &dx12_texture_stats.deferred_on_cpu, Dx12HashTextureUploadNow);
	hash_on_cpu(&buffers_on_cpu, true, &dx12_buffer_stats.deferred_on_cpu, Dx12HashBufferUploadNow);
	{
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_EXECUTE_DRIVER, L"lists", NumCommandLists);
		if (upload_list) {
			lists[-1] = upload_list;
			mOrig->ExecuteCommandLists(NumCommandLists + 1, lists - 1);
		} else {
			mOrig->ExecuteCommandLists(NumCommandLists, lists);
		}
	}
	if (upload_context)
		Dx12UploadHashesSubmitted(mOrig, upload_context);
	if (locked)
		Dx12QueueUnlock(mOrig);

	hash_on_cpu(&on_cpu, false, &dx12_texture_stats.deferred_on_cpu, Dx12HashTextureUploadNow);
	hash_on_cpu(&buffers_on_cpu, false, &dx12_buffer_stats.deferred_on_cpu, Dx12HashBufferUploadNow);

	if (!pins.empty()) {
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_EXECUTE, L"after");
		Dx12ShimPinsExecuted(mOrig, &pins, &first);
	}
}

// The signals and waits the queues were handed, in order: the waits their
// fences have not got to yet, and for each fence the signals of values it has
// not got to yet. That tells whether a queue gets to a signal with what all of
// them have been handed so far, see Dx12QueueCanWait. A fence signalled other
// than by a queue of ours is not known to be, which errs on the side of not
// waiting.
struct QueuedWait {
	// Referenced
	ID3D12Fence *fence;
	UINT64 value;
	ID3D12CommandQueue *queue;
	UINT64 order;
};
struct QueuedSignal {
	UINT64 value;
	ID3D12CommandQueue *queue;
	UINT64 order;
};
static SRWLOCK fence_lock = SRWLOCK_INIT;
static UINT64 queue_order;
static std::vector<QueuedWait> queued_waits;
static std::unordered_map<ID3D12Fence*, std::deque<QueuedSignal>> queued_signals;

// The last signals handed to each queue, which tell how far the GPU has got
// with it, see Dx12QueueAfter. Each holds a reference to its fence: the fence
// is asked for its value, and the notice of its destruction only comes from
// its destructor. Released without fence_lock, as that may be the end of one.
struct RecentSignal {
	ID3D12Fence *fence;
	UINT64 value;
	UINT64 order;
};
const int RECENT_SIGNALS = 4;
static std::unordered_map<ID3D12CommandQueue*, std::array<RecentSignal, RECENT_SIGNALS>> recent_signals;

const size_t QUEUED_SIGNALS_KEPT = 4096;
const int QUEUED_WAIT_DEPTH = 8;

static void fence_destroyed(void *object)
{
	AcquireSRWLockExclusive(&fence_lock);
	queued_signals.erase((ID3D12Fence*)object);
	ReleaseSRWLockExclusive(&fence_lock);
}

void Dx12ForgetQueueOrdering(ID3D12CommandQueue *queue)
{
	std::array<RecentSignal, RECENT_SIGNALS> recent = {};
	std::vector<ID3D12Fence*> waits;

	AcquireSRWLockExclusive(&fence_lock);
	auto found = recent_signals.find(queue);
	if (found != recent_signals.end()) {
		recent = found->second;
		recent_signals.erase(found);
	}
	queued_waits.erase(std::remove_if(queued_waits.begin(), queued_waits.end(), [&](const QueuedWait &wait) {
		if (wait.queue != queue)
			return false;
		waits.push_back(wait.fence);
		return true;
	}), queued_waits.end());
	for (auto &entry : queued_signals) {
		auto &signals = entry.second;
		signals.erase(std::remove_if(signals.begin(), signals.end(),
				[&](const QueuedSignal &signal) { return signal.queue == queue; }), signals.end());
	}
	ReleaseSRWLockExclusive(&fence_lock);

	for (const RecentSignal &signal : recent) {
		if (signal.fence)
			signal.fence->Release();
	}
	for (ID3D12Fence *fence : waits)
		fence->Release();
}

// With fence_lock held. The fences in done are for releasing without it:
// that may be the end of one, which takes the lock.
static void forget_reached_waits(std::vector<ID3D12Fence*> *done)
{
	size_t i, kept = 0;

	for (i = 0; i < queued_waits.size(); i++) {
		if (queued_waits[i].fence->GetCompletedValue() >= queued_waits[i].value)
			done->push_back(queued_waits[i].fence);
		else
			queued_waits[kept++] = queued_waits[i];
	}
	queued_waits.resize(kept);
}

// The first signal handed to a queue that takes the fence to the value
static const QueuedSignal* queued_signal(ID3D12Fence *fence, UINT64 value)
{
	auto found = queued_signals.find(fence);

	if (found == queued_signals.end())
		return NULL;
	for (const QueuedSignal &signal : found->second) {
		if (signal.value >= value)
			return &signal;
	}
	return NULL;
}

// Whether the queue gets through what it was handed before order without
// anything that no queue has been handed yet
static bool queue_reaches(ID3D12CommandQueue *waiter, ID3D12CommandQueue *queue, UINT64 order, int depth)
{
	if (queue == waiter)
		return true;
	for (const QueuedWait &wait : queued_waits) {
		const QueuedSignal *signal;

		if (wait.queue != queue || wait.order >= order || wait.fence->GetCompletedValue() >= wait.value)
			continue;
		// Queues of a game that wait in a circle are not ours to wait for
		if (depth >= QUEUED_WAIT_DEPTH)
			return false;
		signal = queued_signal(wait.fence, wait.value);
		if (!signal || !queue_reaches(waiter, signal->queue, signal->order, depth + 1))
			return false;
	}
	return true;
}

void Dx12QueueSignalled(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	UINT64 completed = fence->GetCompletedValue();
	ID3D12Fence *dropped;
	bool added;

	fence->AddRef();
	AcquireSRWLockExclusive(&fence_lock);
	added = !queued_signals.count(fence);
	std::deque<QueuedSignal> &signals = queued_signals[fence];
	while (!signals.empty() && (signals.front().value <= completed || signals.size() >= QUEUED_SIGNALS_KEPT))
		signals.pop_front();
	signals.push_back({ value, queue, ++queue_order });
	std::array<RecentSignal, RECENT_SIGNALS> &recent = recent_signals[queue];
	dropped = recent.back().fence;
	std::move_backward(recent.begin(), recent.end() - 1, recent.end());
	recent[0] = { fence, value, queue_order };
	ReleaseSRWLockExclusive(&fence_lock);

	if (dropped)
		dropped->Release();

	// Or the next fence at its address would be taken to have been
	// signalled as this one was
	if (added)
		Dx12NotifyDestruction(fence, fence_destroyed);
}

HRESULT Dx12QueueWait(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	std::vector<ID3D12Fence*> done;
	UINT64 order = 0;

	Dx12QueueLock(queue);
	if (fence && fence->GetCompletedValue() < value) {
		fence->AddRef();
		AcquireSRWLockExclusive(&fence_lock);
		forget_reached_waits(&done);
		order = ++queue_order;
		queued_waits.push_back({ fence, value, queue, order });
		ReleaseSRWLockExclusive(&fence_lock);
	}
	HRESULT hr = queue->Wait(fence, value);
	if (FAILED(hr) && order) {
		AcquireSRWLockExclusive(&fence_lock);
		auto found = std::find_if(queued_waits.begin(), queued_waits.end(),
				[order](const QueuedWait &wait) { return wait.order == order; });
		if (found != queued_waits.end()) {
			done.push_back(found->fence);
			queued_waits.erase(found);
		}
		ReleaseSRWLockExclusive(&fence_lock);
	}
	Dx12QueueUnlock(queue);

	for (ID3D12Fence *reached : done)
		reached->Release();
	return hr;
}

// With fence_lock held: the first wait queue_reaches stops at. The queues are
// not referenced, so only their addresses are told.
static bool describe_block(ID3D12CommandQueue *waiter, ID3D12CommandQueue *queue, UINT64 order, int depth,
		char *text, size_t size)
{
	if (queue == waiter)
		return false;
	for (const QueuedWait &wait : queued_waits) {
		const QueuedSignal *signal;

		if (wait.queue != queue || wait.order >= order || wait.fence->GetCompletedValue() >= wait.value)
			continue;
		signal = queued_signal(wait.fence, wait.value);
		if (depth < QUEUED_WAIT_DEPTH && signal
		    && !describe_block(waiter, signal->queue, signal->order, depth + 1, text, size))
			continue;
		if (*text)
			return true;

		auto found = queued_signals.find(wait.fence);
		const QueuedSignal *last = found != queued_signals.end() && !found->second.empty() ? &found->second.back() : NULL;
		_snprintf_s(text, size, _TRUNCATE,
			"at depth %d, queue %p was handed a wait for fence %p to reach %llu, %llu orders back, "
			"which is at %llu, and %s; the last signal of it handed to a queue is %llu on %p",
			depth, queue, wait.fence, wait.value, queue_order - wait.order,
			wait.fence->GetCompletedValue(),
			depth >= QUEUED_WAIT_DEPTH ? "the waits go deeper than are followed"
			: signal ? "the queue to signal it is held up in turn" : "no queue has been handed a signal of it that far",
			last ? last->value : 0, last ? last->queue : NULL);
		return true;
	}
	return false;
}

bool Dx12QueueCanWait(ID3D12CommandQueue *waiter, ID3D12Fence *fence, UINT64 value, char *why, size_t why_size)
{
	std::vector<ID3D12Fence*> done;
	const QueuedSignal *signal;
	bool reached;

	if (fence->GetCompletedValue() >= value)
		return true;

	AcquireSRWLockExclusive(&fence_lock);
	forget_reached_waits(&done);
	signal = queued_signal(fence, value);
	reached = signal && queue_reaches(waiter, signal->queue, signal->order, 0);
	if (!reached && why) {
		*why = 0;
		if (!signal)
			_snprintf_s(why, why_size, _TRUNCATE, "no queue has been handed a signal of fence %p to %llu, it is at %llu",
				fence, value, fence->GetCompletedValue());
		else
			describe_block(waiter, signal->queue, signal->order, 0, why, why_size);
	}
	ReleaseSRWLockExclusive(&fence_lock);

	for (ID3D12Fence *reached_fence : done)
		reached_fence->Release();
	return reached;
}

// By the queue's address: a queue gone needs nothing undone, and queues that
// share one only wait for each other for as long as a call to the queue
static SRWLOCK queue_locks[64];

static SRWLOCK* queue_lock(ID3D12CommandQueue *queue)
{
	return &queue_locks[((UINT64)(uintptr_t)queue * 0x9e3779b97f4a7c15ull) >> 58];
}

void Dx12QueueLock(ID3D12CommandQueue *queue)
{
	AcquireSRWLockExclusive(queue_lock(queue));
}

void Dx12QueueUnlock(ID3D12CommandQueue *queue)
{
	ReleaseSRWLockExclusive(queue_lock(queue));
}

UINT64 Dx12QueueExecutes(ID3D12CommandQueue *queue)
{
	UINT64 order;

	AcquireSRWLockExclusive(&fence_lock);
	order = ++queue_order;
	ReleaseSRWLockExclusive(&fence_lock);
	return order;
}

// With fence_lock held: whether the queue was handed a wait, before order,
// that the GPU gets past only once it got through what writer was handed up
// to after
static bool waits_for(ID3D12CommandQueue *queue, UINT64 order, ID3D12CommandQueue *writer, UINT64 after, int depth)
{
	for (const QueuedWait &wait : queued_waits) {
		const QueuedSignal *signal;

		// The signal a reached wait was for may be forgotten, and the
		// one found instead be a later one
		if (wait.queue != queue || wait.order >= order || wait.fence->GetCompletedValue() >= wait.value)
			continue;
		signal = queued_signal(wait.fence, wait.value);
		if (!signal)
			continue;
		if (signal->queue == writer ? signal->order > after
		    : depth < QUEUED_WAIT_DEPTH && waits_for(signal->queue, signal->order, writer, after, depth + 1))
			return true;
	}
	return false;
}

bool Dx12QueueAfter(ID3D12CommandQueue *queue, ID3D12CommandQueue *writer, UINT64 order)
{
	bool after = false;

	if (queue == writer)
		return true;

	AcquireSRWLockShared(&fence_lock);
	auto recent = recent_signals.find(writer);
	if (recent != recent_signals.end()) {
		for (const RecentSignal &signal : recent->second) {
			if (signal.fence && signal.order > order && signal.fence->GetCompletedValue() >= signal.value) {
				after = true;
				break;
			}
		}
	}
	if (!after)
		after = waits_for(queue, MAXUINT64, writer, order, 0);
	ReleaseSRWLockShared(&fence_lock);

	return after;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::Signal(ID3D12Fence *pFence, UINT64 Value)
{
	HRESULT hr;

	Dx12QueueLock(mOrig);
	hr = mOrig->Signal(pFence, Value);
	// After the queue has it and not before, as a wait is noted before:
	// either way round, what is not known for a moment is that the queue
	// gets further than it is taken to
	if (SUCCEEDED(hr) && pFence)
		Dx12QueueSignalled(mOrig, pFence, Value);
	Dx12QueueUnlock(mOrig);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::Wait(ID3D12Fence *pFence, UINT64 Value)
{
	return Dx12QueueWait(mOrig, pFence, Value);
}

#include "generated/Dx12CommandQueue.fwd.inl"
