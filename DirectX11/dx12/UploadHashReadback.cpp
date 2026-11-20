#include "UploadHashReadback.h"

#include <deque>
#include <utility>
#include <vector>

#include "Dx12CommandList.h"
#include "ReadbackRing.h"
#include "ResourceTracker.h"
#include "TextureHash.h"
#include "../profiling.h"

// {8B3E5C1A-2F47-4D6B-9A10-6C2E7F3D8B54}
static const GUID QueueGoneGuid = { 0x8b3e5c1a, 0x2f47, 0x4d6b, { 0x9a, 0x10, 0x6c, 0x2e, 0x7f, 0x3d, 0x8b, 0x54 } };

// A queue starts with one ring of this size and gets more while the copies in
// flight do not fit, up to RINGS_MAX_BYTES for all queues together (queues
// making one at the same time can go one ring past it). Only beyond that is an
// upload hashed on the CPU, which is what the deferral is there to avoid.
static const UINT64 RING_BYTES = 64 << 20;
static const UINT64 RINGS_MAX_BYTES = 512 << 20;
// Rings that went unused for this long are released, the last one of a queue
// after longer, as making one again holds up the execution that needs it
static const ULONGLONG SPARE_RING_IDLE_MS = 30000;
static const ULONGLONG LAST_RING_IDLE_MS = 300000;
// Hashing at a present stops once it took this long, after one upload at the
// least, so that a burst of uploads is spread over a few frames
static const double FINISH_BUDGET_MS = 1.0;

// A texture upload, or a buffer upload if buffer
struct PendingUpload {
	bool buffer;
	Dx12TextureUpload texture;
	Dx12BufferUpload buffer_upload;
	Dx12ReadbackRing *ring;
	UINT64 at;
	UINT64 fence;
};

static UINT64 upload_start(const PendingUpload *p)
{
	return p->buffer ? p->buffer_upload.src_offset + p->buffer_upload.hashed : Dx12TextureUploadStart(&p->texture);
}

static UINT64 upload_span(const PendingUpload *p)
{
	return p->buffer ? p->buffer_upload.bytes - p->buffer_upload.hashed : Dx12TextureUploadSpan(&p->texture);
}

static ID3D12Resource* upload_buffer(const PendingUpload *p)
{
	return p->buffer ? p->buffer_upload.upload : p->texture.upload;
}

// For one that will not be hashed
static void upload_abandoned(const PendingUpload *p)
{
	if (p->buffer)
		Dx12BufferHashDone(&p->buffer_upload, NULL);
	else
		Dx12TextureHashDone(p->texture.texture, p->texture.serial, p->texture.generation, NULL);
}

// One for each queue the game executes deferred uploads on. Its fence is only
// signalled on that queue, so the copies get through in the order they were
// handed out, and each ring is freed in the order it handed out space.
struct QueueHashes {
	// The original queue, only compared
	ID3D12CommandQueue *queue;
	D3D12_COMMAND_LIST_TYPE type;
	SRWLOCK lock;
	volatile LONG gone;
	bool failed;
	bool close_failure_logged;
	// The queue's, which it holds a reference to while it is alive
	ID3D12Device *device;

	ID3D12Fence *fence;
	UINT64 signalled;
	ID3D12GraphicsCommandList *list;
	// The allocator of the list while it is recorded, and the ones it was
	// executed with by the value the fence gets to after
	ID3D12CommandAllocator *recording;
	std::deque<std::pair<ID3D12CommandAllocator*, UINT64>> allocators;
	// Lists that failed to close, which cannot be reset again, by the value
	// the fence gets to after their earlier executions
	std::deque<std::pair<ID3D12GraphicsCommandList*, UINT64>> retired_lists;

	std::vector<Dx12ReadbackRing*> rings;

	std::deque<PendingUpload> pending;
	// The oldest of pending is being hashed without the lock, see finish,
	// and the executions waiting for that in ring_alloc
	bool finishing;
	CONDITION_VARIABLE finished;
	UINT waiting;
};

static SRWLOCK registry_lock = SRWLOCK_INIT;
static std::vector<QueueHashes*> registry;

static void queue_gone(void *context)
{
	InterlockedExchange(&((QueueHashes*)context)->gone, 1);
}

static QueueHashes* create_context(ID3D12CommandQueue *queue)
{
	QueueHashes *q = new QueueHashes();
	ID3D12Device *device = NULL;

	q->queue = queue;
	q->type = queue->GetDesc().Type;
	InitializeSRWLock(&q->lock);
	InitializeConditionVariable(&q->finished);

	if (q->type != D3D12_COMMAND_LIST_TYPE_DIRECT && q->type != D3D12_COMMAND_LIST_TYPE_COMPUTE && q->type != D3D12_COMMAND_LIST_TYPE_COPY) {
		q->failed = true;
	} else if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))
	           || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&q->fence)))) {
		Dx12LogInfo("Deferred upload hashing: no fence for queue %p, its uploads are hashed on the CPU\n", queue);
		q->failed = true;
	}
	if (device) {
		q->device = device;
		device->Release();
	}

	Dx12NotifyDestruction(queue, QueueGoneGuid, queue_gone, q);
	return q;
}

static QueueHashes* context_of(ID3D12CommandQueue *queue)
{
	QueueHashes *found = NULL;

	AcquireSRWLockShared(&registry_lock);
	for (QueueHashes *q : registry) {
		if (q->queue == queue && !q->gone)
			found = q;
	}
	ReleaseSRWLockShared(&registry_lock);
	if (found)
		return found;

	AcquireSRWLockExclusive(&registry_lock);
	for (QueueHashes *q : registry) {
		if (q->queue == queue && !q->gone)
			found = q;
	}
	if (!found) {
		found = create_context(queue);
		registry.push_back(found);
	}
	ReleaseSRWLockExclusive(&registry_lock);
	return found;
}

static void release_ring(Dx12ReadbackRing *ring)
{
	InterlockedAdd64(&dx12_texture_stats.readback_bytes, -(LONG64)ring->size);
	InterlockedDecrement64(&dx12_texture_stats.readback_rings);
	ring->Release();
	delete ring;
}

static void destroy_context(QueueHashes *q)
{
	for (auto &allocator : q->allocators)
		allocator.first->Release();
	if (q->recording)
		q->recording->Release();
	if (q->list)
		q->list->Release();
	for (auto &list : q->retired_lists)
		list.first->Release();
	for (Dx12ReadbackRing *ring : q->rings)
		release_ring(ring);
	if (q->fence)
		q->fence->Release();
	delete q;
}

// Takes the hash of a copy the GPU has got through
static void hash_copy(const PendingUpload *p)
{
	if (p->buffer) {
		Dx12BufferUpload upload = p->buffer_upload;
		UINT64 done = 0;

		Dx12HashUploadBytes(p->ring->data + p->at, upload.bytes - upload.hashed, &done, &upload.crc, false, false);
		Dx12BufferHashDone(&upload, &upload.crc);
		return;
	}

	Dx12TextureUpload upload = p->texture;

	Dx12HashTextureRows(p->ring->data + p->at, &upload, false, false);
	Dx12TextureHashDone(upload.texture, upload.serial, upload.generation, &upload.crc);
}

// With the context's lock held: gives back the space of the oldest copy once
// its hash is taken, and returns the bytes it read
static UINT64 free_oldest(QueueHashes *q)
{
	PendingUpload &p = q->pending.front();
	UINT64 span = upload_span(&p);

	p.ring->FreeOldest();
	q->pending.pop_front();

	return span;
}

static Dx12ReadbackRing* existing_ring_alloc(QueueHashes *q, UINT64 bytes, UINT64 *at)
{
	for (Dx12ReadbackRing *ring : q->rings) {
		if (ring->Alloc(bytes, at))
			return ring;
	}
	return NULL;
}

// With the context's lock held. Copies the GPU has got through are hashed to
// make room before another ring is made, which also covers a long time between
// presents, when nothing else takes their hashes. While finish hashes the
// oldest, whose space FreeOldest gives back, that one is waited for.
static Dx12ReadbackRing* ring_alloc(QueueHashes *q, UINT64 bytes, UINT64 *at)
{
	Dx12ReadbackRing *ring = existing_ring_alloc(q, bytes, at);
	UINT64 size, completed, done = 0;

	if (ring)
		return ring;

	if (q->finishing) {
		q->waiting++;
		while (q->finishing)
			SleepConditionVariableSRW(&q->finished, &q->lock, INFINITE, 0);
		q->waiting--;
		ring = existing_ring_alloc(q, bytes, at);
		if (ring)
			return ring;
	}

	completed = q->fence->GetCompletedValue();
	if (completed != UINT64_MAX && !q->pending.empty() && q->pending.front().fence <= completed) {
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_UPLOAD_HASH_FINISH, L"to make room, bytes");

		while (!ring && !q->pending.empty() && q->pending.front().fence <= completed) {
			hash_copy(&q->pending.front());
			done += free_oldest(q);
			ring = existing_ring_alloc(q, bytes, at);
		}
		spike.add_bytes(done);
		if (ring)
			return ring;
	}

	size = (bytes + Dx12ReadbackRing::ALIGNMENT - 1) & ~(Dx12ReadbackRing::ALIGNMENT - 1);
	if (size < RING_BYTES)
		size = RING_BYTES;
	if ((UINT64)dx12_texture_stats.readback_bytes + size > RINGS_MAX_BYTES)
		return NULL;

	ring = new Dx12ReadbackRing();
	{
		Profiling::Spike::Scope spike(Profiling::Spike::DX12_UPLOAD_HASH_RING, L"bytes", size);

		if (!ring->Create(q->device, size)) {
			Dx12LogInfo("Deferred upload hashing: no readback buffer of %llu MB for queue %p\n", size >> 20, q->queue);
			delete ring;
			return NULL;
		}
	}
	q->rings.push_back(ring);
	InterlockedIncrement64(&dx12_texture_stats.readback_rings);
	InterlockedAdd64(&dx12_texture_stats.readback_bytes, (LONG64)size);

	return ring->Alloc(bytes, at) ? ring : NULL;
}

// Hands out the allocator the list is recorded with, and opens the list
static bool open_list(QueueHashes *q)
{
	ID3D12Device *device = q->device;
	ID3D12CommandAllocator *allocator = NULL;
	UINT64 completed = q->fence->GetCompletedValue();

	while (!q->retired_lists.empty() && completed != UINT64_MAX && q->retired_lists.front().second <= completed) {
		q->retired_lists.front().first->Release();
		q->retired_lists.pop_front();
	}
	if (!q->allocators.empty() && completed != UINT64_MAX && q->allocators.front().second <= completed) {
		allocator = q->allocators.front().first;
		q->allocators.pop_front();
		if (FAILED(allocator->Reset())) {
			allocator->Release();
			allocator = NULL;
		}
	}
	if (!allocator && FAILED(device->CreateCommandAllocator(q->type, IID_PPV_ARGS(&allocator))))
		return false;

	if (!q->list) {
		if (FAILED(device->CreateCommandList(0, q->type, allocator, NULL, IID_PPV_ARGS(&q->list)))) {
			allocator->Release();
			return false;
		}
	} else if (FAILED(q->list->Reset(allocator, NULL))) {
		allocator->Release();
		return false;
	}

	q->recording = allocator;
	return true;
}

// With the context's lock held, which it is again on return: takes the hashes
// of the copies the GPU has got through until QueryPerformanceCounter reaches
// deadline, after the first upload of the present (*any). Each is hashed
// without the lock, so that an execution on the queue does not wait for it,
// and keeps its space in the ring until then. An execution waiting for the
// room hashes the rest itself. False if the device was removed, when none of
// the copies will be.
static bool finish(QueueHashes *q, LONG64 deadline, bool *any)
{
	LARGE_INTEGER now;
	UINT64 completed, done = 0;
	PendingUpload p;

	if (!q->fence)
		return true;
	completed = q->fence->GetCompletedValue();
	if (completed == UINT64_MAX)
		return false;
	if (q->pending.empty() || q->pending.front().fence > completed)
		return true;

	Profiling::Spike::Scope spike(Profiling::Spike::DX12_UPLOAD_HASH_FINISH, L"bytes");
	while (!q->pending.empty() && q->pending.front().fence <= completed && !q->waiting) {
		if (*any) {
			QueryPerformanceCounter(&now);
			if (now.QuadPart >= deadline)
				break;
		}
		p = q->pending.front();
		q->finishing = true;
		ReleaseSRWLockExclusive(&q->lock);
		hash_copy(&p);
		AcquireSRWLockExclusive(&q->lock);
		q->finishing = false;
		WakeAllConditionVariable(&q->finished);
		done += free_oldest(q);
		*any = true;
	}
	spike.add_bytes(done);

	return true;
}

// With the context's lock held and the list open: records the GPU copy of the
// upload, false if there is no room for it
static bool copy_upload(QueueHashes *q, PendingUpload *p)
{
	UINT64 span = upload_span(p);

	p->ring = ring_alloc(q, span, &p->at);
	if (!p->ring)
		return false;
	q->list->CopyBufferRegion(p->ring->resource, p->at, upload_buffer(p), upload_start(p), span);
	p->fence = q->signalled + 1;
	q->pending.push_back(*p);
	return true;
}

ID3D12CommandList* Dx12UploadHashesExecuting(ID3D12CommandQueue *queue, Dx12GraphicsCommandList *const *proxies, size_t count,
		const std::vector<Dx12BufferUpload> &buffers, std::vector<Dx12TextureUpload> *on_cpu_textures,
		std::vector<Dx12BufferUpload> *on_cpu_buffers, void **context)
{
	QueueHashes *q = context_of(queue);
	PendingUpload p = {};
	size_t i, added = 0;
	bool close_failed, tried = false;

	*context = NULL;

	AcquireSRWLockExclusive(&q->lock);

	auto open = [&]() {
		if (!q->failed && !q->recording && !tried) {
			tried = true;
			open_list(q);
		}
		return q->recording != NULL;
	};
	for (i = 0; i < count; i++) {
		for (const Dx12TextureUpload &upload : proxies[i]->mTextureUploads) {
			p.buffer = false;
			p.texture = upload;
			if (open() && copy_upload(q, &p))
				added++;
			else
				on_cpu_textures->push_back(upload);
		}
	}
	for (const Dx12BufferUpload &upload : buffers) {
		p.buffer = true;
		p.buffer_upload = upload;
		if (open() && copy_upload(q, &p))
			added++;
		else
			on_cpu_buffers->push_back(upload);
	}

	if (!q->recording) {
		ReleaseSRWLockExclusive(&q->lock);
		return NULL;
	}

	// The list is reset for the next execution, or replaced after a failure
	// to close. Without a copy in it, it is not executed.
	close_failed = FAILED(q->list->Close());
	if (close_failed || !added) {
		if (close_failed && !q->close_failure_logged) {
			Dx12LogInfo("Deferred upload hashing: our list for queue %p failed to close, its uploads are hashed on the CPU\n", queue);
			q->close_failure_logged = true;
		}
		for (; added; added--) {
			const PendingUpload &last = q->pending.back();

			if (last.buffer)
				on_cpu_buffers->push_back(last.buffer_upload);
			else
				on_cpu_textures->push_back(last.texture);
			last.ring->FreeNewest();
			q->pending.pop_back();
		}
		if (close_failed) {
			q->retired_lists.push_back({ q->list, q->signalled });
			q->list = NULL;
		}
		q->allocators.push_back({ q->recording, q->signalled });
		q->recording = NULL;
		ReleaseSRWLockExclusive(&q->lock);
		return NULL;
	}

	// Not held while the driver executes the lists, so that a present on
	// another thread does not wait for that. The caller's Dx12QueueLock keeps
	// other executions on the queue out until the fence is signalled, and
	// until then the copies are pending on a value the fence has not got to.
	ReleaseSRWLockExclusive(&q->lock);
	*context = q;
	return q->list;
}

void Dx12UploadHashesSubmitted(ID3D12CommandQueue *queue, void *context)
{
	QueueHashes *q = (QueueHashes*)context;

	AcquireSRWLockExclusive(&q->lock);
	q->signalled++;
	queue->Signal(q->fence, q->signalled);
	q->allocators.push_back({ q->recording, q->signalled });
	q->recording = NULL;
	ReleaseSRWLockExclusive(&q->lock);
}

// With the context's lock held
static void release_idle_rings(QueueHashes *q, ULONGLONG now)
{
	for (size_t i = 0; i < q->rings.size(); ) {
		Dx12ReadbackRing *ring = q->rings[i];
		ULONGLONG idle_ms = q->rings.size() == 1 ? LAST_RING_IDLE_MS : SPARE_RING_IDLE_MS;

		if (ring->Empty() && now - ring->last_used > idle_ms) {
			q->rings.erase(q->rings.begin() + i);
			release_ring(ring);
		} else {
			i++;
		}
	}
}

// With the context's lock held: whether the GPU is done with what the context
// handed it
static bool idle(QueueHashes *q)
{
	UINT64 completed = q->fence ? q->fence->GetCompletedValue() : 0;

	return q->pending.empty() && !q->recording
		&& (q->allocators.empty() || q->allocators.back().second <= completed);
}

void Dx12FinishUploadHashes()
{
	static SRWLOCK finish_lock = SRWLOCK_INIT;
	static LONG64 frequency;
	static size_t first_context;
	std::vector<QueueHashes*> contexts, finished;
	ULONGLONG now = GetTickCount64();
	LARGE_INTEGER start, ticks;
	LONG64 deadline;
	bool any = false;

	// One present at a time, which makes it the only thread that destroys
	// contexts, so that the registry need not be held while it hashes
	if (!TryAcquireSRWLockExclusive(&finish_lock))
		return;

	if (!frequency) {
		QueryPerformanceFrequency(&ticks);
		frequency = ticks.QuadPart;
	}
	QueryPerformanceCounter(&start);
	deadline = start.QuadPart + (LONG64)(FINISH_BUDGET_MS * frequency / 1000);

	AcquireSRWLockShared(&registry_lock);
	contexts = registry;
	ReleaseSRWLockShared(&registry_lock);

	// Each queue in turn goes first, so that one with more copies than the
	// budget does not keep another's from ever being hashed here. One that
	// is busy making room in ring_alloc is left for the next present.
	for (size_t i = 0; i < contexts.size(); i++) {
		QueueHashes *q = contexts[(first_context + i) % contexts.size()];

		if (!TryAcquireSRWLockExclusive(&q->lock))
			continue;
		if (!finish(q, deadline, &any)) {
			// Removed with the device: nothing of ours runs any more,
			// and what the copies were for keeps the hash it had
			while (!q->pending.empty()) {
				upload_abandoned(&q->pending.front());
				q->pending.pop_front();
			}
			for (Dx12ReadbackRing *ring : q->rings)
				ring->starts.clear();
			q->failed = true;
		}
		release_idle_rings(q, now);
		// A removed device has its fence at UINT64_MAX, which idle takes
		// for done
		if (q->gone && idle(q))
			finished.push_back(q);
		ReleaseSRWLockExclusive(&q->lock);
	}

	first_context++;

	// The queues are gone, so nothing else looks for these
	if (!finished.empty()) {
		AcquireSRWLockExclusive(&registry_lock);
		for (QueueHashes *q : finished) {
			for (auto i = registry.begin(); i != registry.end(); ++i) {
				if (*i == q) {
					registry.erase(i);
					break;
				}
			}
			destroy_context(q);
		}
		ReleaseSRWLockExclusive(&registry_lock);
	}

	ReleaseSRWLockExclusive(&finish_lock);
}
