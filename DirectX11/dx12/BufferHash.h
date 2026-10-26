#pragma once

#include <map>
#include <unordered_map>
#include <vector>

#include "ResourceTracker.h"

// Under D3D11 a vertex or index buffer is one resource, hashed from the data
// and the D3D11_BUFFER_DESC passed to CreateBuffer. A D3D12 renderer carves
// its buffers out of large pooled resources and fills each with a copy from
// an upload buffer, so here a "buffer" is the range written by one such copy,
// hashed when the copy is recorded and looked up by resource and offset.
//
// This only identifies buffers whose upload is a single copy of the same
// bytes D3D11 would have been given. Where a game uploads differently, the
// range has to be hashed from the GPU copy instead, which needs command lists
// of our own and comes with the engine shim.

struct Dx12BufferHash {
	UINT64 bytes;
	uint32_t data_hash;
	// dx12_frame when the copy was recorded
	UINT frame;
};

// What a copy into a buffer of the GPU's memory writes, kept by the list it is
// recorded to. Its range gets that as soon as the copy is recorded, for the
// draws recorded after it, and again each time the list is executed if
// anything else was put there since: lists are executed in an order of their
// own, and may be executed more than once.
//
// An upload writes what the copy was recorded with, hashed or not, the first
// time the list is executed. The GPU copies what the upload buffer holds when
// the list is executed, which the game may have changed by the time it is
// executed again, so then the upload buffer is hashed again. A copy from
// another buffer of the GPU's memory, as a renderer compacting its pools
// makes, moves what was uploaded before, where D3D11 buffers never move. What
// it writes is only known once it is executed: it has no hash until then, and
// then the hashes of the source's ranges it copies whole if the source still
// has those ranges as recorded and the lists that wrote them were executed
// before it on the GPU (see Dx12QueueAfter), otherwise none.
struct Dx12BufferWrite {
	struct Range {
		UINT64 offset, bytes;
		uint32_t data_hash;
		UINT64 version;
	};
	ID3D12Resource *dst;
	UINT64 dst_offset, bytes;
	// The buffer of the GPU's memory copied from, NULL for an upload
	ID3D12Resource *src;
	UINT64 src_offset;
	// dx12_frame at recording
	UINT frame;
	// Of an upload. The upload buffer is alive whenever the list is
	// executed.
	ID3D12Resource *upload;
	uint32_t data_hash;
	bool hashed;
	// Whether the list has been executed with it before
	bool executed;
	// Of a copy from the GPU's memory: the source's hashed ranges it takes
	// whole, as recorded
	std::vector<Range> ranges;
	// Of the ranges the write last put in place, and whether with hashes
	UINT64 version;
	bool placed_hashed;
};

// The writes of a list. What the ranges of all resources have is as of the
// copies recorded and executed so far by every list, which is not what the
// list's own draws read where the list copied to before them: those read what
// its copies put there whatever was executed or recorded elsewhere since.
struct Dx12ListBufferWrites {
	std::vector<Dx12BufferWrite> writes;
	// By destination and offset, not overlapping: the write that put
	// there last of those recorded so far, and whether all of it is still
	// there from this offset on
	struct Recorded {
		UINT64 bytes;
		size_t write;
		bool whole;
	};
	std::unordered_map<ID3D12Resource*, std::map<UINT64, Recorded>> recorded;

	bool empty() const { return writes.empty(); }
	void clear() { writes.clear(); recorded.clear(); }
};

// For the command list proxy, before it forwards the call. Adds what it
// records to the list's writes.
void Dx12CopyBufferRegion(Dx12ListBufferWrites *writes, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src,
		UINT64 src_offset, UINT64 bytes);
// Likewise for CopyResource, which does nothing unless both are buffers
void Dx12CopyBuffer(Dx12ListBufferWrites *writes, ID3D12Resource *dst, ID3D12Resource *src);
// For the queue proxy, for each list it executes, in order, every time, with
// the order Dx12QueueExecutes gave the execution
void Dx12BufferWritesExecuted(Dx12ListBufferWrites *writes, ID3D12CommandQueue *queue, UINT64 order);

bool Dx12FindBufferHash(ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash);
// The same for a draw recorded to a list with these writes, or NULL. *own is
// set if the list's copies decided it, which then holds for the list alone.
bool Dx12FindListBufferHash(const Dx12ListBufferWrites *writes, ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash, bool *own);
// Whether a copy recorded to the list wrote to any of these bytes
bool Dx12ListWroteBuffer(const Dx12ListBufferWrites *writes, ID3D12Resource *resource, UINT64 offset, UINT64 bytes);
// For the report: what is known of the copies into the resource at the offset,
// which says why Dx12FindBufferHash has or has no hash for it
void Dx12DescribeBufferRange(ID3D12Resource *resource, UINT64 offset, char *text, size_t size);
// For remembering what Dx12FindBufferHash said about a range: a number that
// has changed if anything was written to these bytes of the resource, or
// forgotten about them, since. It may change without that as well. Taken
// before asking, or what was written in between goes unnoticed. False if the
// range is too large to be told apart from the rest of its resource.
bool Dx12BufferHashStamp(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, UINT64 *stamp);
// The counters that stamp is the sum of, like Dx12FindBufferStampCounters:
// how many of them, 0 where there is no stamp or they are more than max
UINT Dx12BufferHashStampCounters(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, const volatile LONG64 **counters, UINT max);
void Dx12ForgetBufferHashes(ID3D12Resource *resource);

// The hash HackerDevice::CreateBuffer would have calculated for a default
// usage buffer of this size with these D3D11_BIND_FLAGs
uint32_t Dx12BufferDescHash(const Dx12BufferHash *hash, UINT bind_flags);

struct Dx12BufferStats {
	volatile LONG64 hashed;
	volatile LONG64 hashed_bytes;
	// Ranges rewritten every few frames are not buffers a mod would match
	volatile LONG64 dynamic;
	volatile LONG64 not_mappable;
	volatile LONG64 untracked;
	volatile LONG64 out_of_bounds;
	// Hashed ranges moved by those not mappable, see Dx12BufferWrite
	volatile LONG64 copy_propagated;
};
extern Dx12BufferStats dx12_buffer_stats;
