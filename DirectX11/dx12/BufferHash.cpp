#include "BufferHash.h"

#include <d3d11.h>

#include "util.h"

#include "Dx12CommandQueue.h"
#include "HashDump.h"
#include "../profiling.h"

Dx12BufferStats dx12_buffer_stats;

const UINT64 MAX_HASHED_BYTES = 256 << 20;

struct BufferRange {
	UINT64 bytes;
	uint32_t data_hash;
	bool valid;
	// Written by a copy from memory the CPU cannot read
	bool unmapped;
	UINT frame;
	UINT hot;
	// Different for each write that made the range, see Dx12BufferWrite
	UINT64 version;
	// Where the list of that write was executed, see Dx12QueueExecutes. 0
	// until then: a copy from the range copies what was there before.
	ID3D12CommandQueue *queue;
	UINT64 order;
};

static SRWLOCK lock = SRWLOCK_INIT;
// Ranges of one resource by their offset. They do not overlap.
static std::unordered_map<ID3D12Resource*, std::map<UINT64, BufferRange>> ranges;
static UINT64 range_versions;

// See Dx12BufferHashStamp. A counter per 64 KiB of a resource that is counted
// up by what is written there, shared with whatever else hashes to it, and
// one per resource for what covers too many of those to count them all up.
const UINT STAMP_SHIFT = 16;
const UINT64 STAMP_MAX_SPAN = 16;
static volatile LONG64 range_stamps[65536];
static volatile LONG64 resource_stamps[1024];

static volatile LONG64* range_stamp(ID3D12Resource *resource, UINT64 granule)
{
	UINT64 hash = ((UINT64)(uintptr_t)resource ^ (granule * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;

	return &range_stamps[hash >> 48];
}

static volatile LONG64* resource_stamp(ID3D12Resource *resource)
{
	return &resource_stamps[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 54];
}

// How many of the resources that hash to a slot have ranges, counted with the
// lock held: most of the buffers a draw is asked about were never written by a
// copy, which Dx12FindBufferHash can then say without the lock
static volatile LONG resources_with_ranges[4096];

static volatile LONG* with_ranges(ID3D12Resource *resource)
{
	return &resources_with_ranges[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 52];
}

// With the lock held, so that whoever finds the stamp unchanged afterwards
// has seen all of the change or none of it
static void stamp_write(ID3D12Resource *resource, UINT64 offset, UINT64 bytes)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;

	if (last - first >= STAMP_MAX_SPAN) {
		InterlockedIncrement64(resource_stamp(resource));
		return;
	}
	for (; first <= last; first++)
		InterlockedIncrement64(range_stamp(resource, first));
}

bool Dx12BufferHashStamp(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, UINT64 *stamp)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;

	if (last - first >= STAMP_MAX_SPAN)
		return false;

	// The counters only go up, so their sum is the same only if each is
	*stamp = (UINT64)*resource_stamp(resource);
	for (; first <= last; first++)
		*stamp += (UINT64)*range_stamp(resource, first);
	return true;
}

UINT Dx12BufferHashStampCounters(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, const volatile LONG64 **counters, UINT max)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;
	UINT count = 0;

	if (last - first >= STAMP_MAX_SPAN || last - first + 2 > max)
		return 0;

	counters[count++] = resource_stamp(resource);
	for (; first <= last; first++)
		counters[count++] = range_stamp(resource, first);
	return count;
}

uint32_t Dx12BufferDescHash(const Dx12BufferHash *hash, UINT bind_flags)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = (UINT)hash->bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;

	return crc32c_hw(hash->data_hash, &desc, sizeof(desc));
}

// With the lock held
static std::map<UINT64, BufferRange>& ranges_of(ID3D12Resource *resource)
{
	auto inserted = ranges.try_emplace(resource);

	if (inserted.second)
		InterlockedIncrement(with_ranges(resource));
	return inserted.first->second;
}

// Whatever a write overlaps is no longer what was hashed. A range of exactly
// those bytes may be kept, to tell how often they are written.
static void erase_overlaps(std::map<UINT64, BufferRange> &map, UINT64 offset, UINT64 bytes, bool keep_same)
{
	auto i = map.lower_bound(offset);

	if (i != map.begin()) {
		auto previous = std::prev(i);
		if (previous->first + previous->second.bytes > offset)
			map.erase(previous);
	}
	while (i != map.end() && i->first < offset + bytes) {
		if (keep_same && i->first == offset && i->second.bytes == bytes)
			++i;
		else
			i = map.erase(i);
	}
}

// With the lock held. Returns false if the range turned out to be dynamic.
static bool write_range(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, bool unmapped, UINT64 *version)
{
	std::map<UINT64, BufferRange> &map = ranges_of(resource);
	UINT frame = dx12_frame;
	bool hash_it;

	erase_overlaps(map, offset, bytes, true);

	BufferRange &range = map[offset];
	if (range.bytes == bytes && frame - range.frame < DX12_HOT_FRAMES) {
		if (range.hot < DX12_HOT_LIMIT)
			range.hot++;
	} else {
		range.hot = 0;
	}
	range.bytes = bytes;
	range.frame = frame;
	range.valid = false;
	range.unmapped = unmapped;
	range.version = *version = ++range_versions;
	range.queue = NULL;
	range.order = 0;
	hash_it = range.hot < DX12_HOT_LIMIT;

	// Whatever was erased overlaps the write, and so shares a counter
	// with it
	stamp_write(resource, offset, bytes);

	return hash_it;
}

// Returns false if the range turned out to be dynamic
static bool note_write(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, bool unmapped, UINT64 *version)
{
	bool hash_it;

	AcquireSRWLockExclusive(&lock);
	hash_it = write_range(resource, offset, bytes, unmapped, version);
	ReleaseSRWLockExclusive(&lock);

	return hash_it;
}

// With the lock held: notes the copy from the GPU's memory, with the source's
// hashed ranges it takes whole, and leaves the destination without a hash
static void note_move(Dx12BufferWrite *write)
{
	// Gathered before anything is erased, as the source may be the
	// destination's resource
	auto source = ranges.find(write->src);
	if (source != ranges.end()) {
		const std::map<UINT64, BufferRange> &map = source->second;
		for (auto i = map.lower_bound(write->src_offset); i != map.end() && i->first + i->second.bytes <= write->src_offset + write->bytes; ++i) {
			if (i->second.valid)
				write->ranges.push_back({ i->first, i->second.bytes, i->second.data_hash, i->second.version });
		}
	}

	write_range(write->dst, write->dst_offset, write->bytes, true, &write->version);
}

// With the lock held: whether the destination's bytes are all in ranges the
// write put there last, which nothing since has written over. Those are then
// marked executed there.
static bool write_placed(const Dx12BufferWrite *write, ID3D12CommandQueue *queue, UINT64 order)
{
	UINT64 at = write->dst_offset, end = write->dst_offset + write->bytes;
	auto map = ranges.find(write->dst);

	if (map == ranges.end())
		return false;
	auto first = map->second.lower_bound(at), i = first;
	for (; at < end; ++i) {
		if (i == map->second.end() || i->first != at || i->second.version != write->version)
			return false;
		at += i->second.bytes;
	}
	for (; first != i; ++first) {
		first->second.queue = queue;
		first->second.order = order;
	}
	return true;
}

// With the lock held: whether a range the write overlaps has a hash
static bool hash_under(const Dx12BufferWrite *write)
{
	auto map = ranges.find(write->dst);

	if (map == ranges.end())
		return false;
	auto i = map->second.lower_bound(write->dst_offset);
	if (i != map->second.begin()) {
		auto previous = std::prev(i);
		if (previous->first + previous->second.bytes > write->dst_offset && previous->second.valid)
			return true;
	}
	for (; i != map->second.end() && i->first < write->dst_offset + write->bytes; ++i) {
		if (i->second.valid)
			return true;
	}
	return false;
}

// With the lock held: whether the source of a copy from the GPU's memory has
// hashed ranges it takes whole, as they were when it was recorded, written by
// lists the GPU gets through before the copy executed on the queue now. Lists
// executed on another queue before it may still run after it.
static bool source_kept(const Dx12BufferWrite *write, ID3D12CommandQueue *queue)
{
	auto map = ranges.find(write->src);

	if (write->ranges.empty() || map == ranges.end())
		return false;
	for (const Dx12BufferWrite::Range &range : write->ranges) {
		auto i = map->second.find(range.offset);
		if (i == map->second.end() || !i->second.valid || !i->second.order || i->second.version != range.version)
			return false;
	}
	for (const Dx12BufferWrite::Range &range : write->ranges) {
		const BufferRange &kept = map->second.find(range.offset)->second;
		if (kept.queue != queue && !Dx12QueueAfter(queue, kept.queue, kept.order))
			return false;
	}
	return true;
}

// What the GPU copies is what the upload buffer holds when the list is
// executed. False if it cannot be read.
static bool hash_upload(Dx12BufferWrite *write)
{
	BYTE *data = NULL;

	Profiling::Spike::Scope spike(Profiling::Spike::DX12_BUFFER_HASH, L"bytes", write->bytes);

	if (FAILED(write->upload->Map(0, NULL, (void**)&data)) || !data)
		return false;
	write->data_hash = Dx12Crc32cWriteCombined(0, data + write->src_offset, (size_t)write->bytes);
	write->upload->Unmap(0, NULL);
	return true;
}

struct MovedHash {
	ID3D12Resource *resource;
	UINT64 offset;
	Dx12BufferHash hash;
};

// With the lock held: puts what the write writes in place, with hashes or
// without, all in ranges of one version, and adds the hashes a copy from the
// GPU's memory moved to moved
static void place_write(Dx12BufferWrite *write, bool hashed, ID3D12CommandQueue *queue, UINT64 order, std::vector<MovedHash> *moved)
{
	std::map<UINT64, BufferRange> &map = ranges_of(write->dst);
	UINT64 version = ++range_versions, at = write->dst_offset, end = write->dst_offset + write->bytes;
	UINT frame = dx12_frame;

	if (!write->src) {
		// As recorded, with the count of writes of these bytes kept,
		// see write_range
		auto same = map.find(write->dst_offset);
		UINT hot = same != map.end() && same->second.bytes == write->bytes ? same->second.hot : 0;

		erase_overlaps(map, write->dst_offset, write->bytes, false);
		map[write->dst_offset] = { write->bytes, write->data_hash, hashed, false, write->frame, hot, version, queue, order };
	} else {
		erase_overlaps(map, write->dst_offset, write->bytes, false);
		if (hashed) {
			for (const Dx12BufferWrite::Range &range : write->ranges) {
				UINT64 offset = range.offset - write->src_offset + write->dst_offset;

				// What is between the hashed ranges is told
				// apart for the report
				if (offset > at)
					map[at] = { offset - at, 0, false, true, frame, 0, version, queue, order };
				map[offset] = { range.bytes, range.data_hash, true, true, frame, 0, version, queue, order };
				moved->push_back({ write->dst, offset, { range.bytes, range.data_hash, frame } });
				at = offset + range.bytes;
			}
		}
		if (end > at)
			map[at] = { end - at, 0, false, true, frame, 0, version, queue, order };
	}
	stamp_write(write->dst, write->dst_offset, write->bytes);
	write->version = version;
	write->placed_hashed = hashed;
}

// Without the lock
static void rehash_upload(Dx12BufferWrite *write, std::vector<MovedHash> *rehashed)
{
	uint32_t recorded = write->data_hash;

	// If it cannot be read, what is in place with a hash is replaced by
	// the same without
	write->hashed = hash_upload(write);
	// What is in place has the hash from before
	if (write->hashed && write->data_hash != recorded) {
		write->placed_hashed = false;
		rehashed->push_back({ write->dst, write->dst_offset, { write->bytes, write->data_hash, dx12_frame } });
	}
}

void Dx12BufferWritesExecuted(Dx12ListBufferWrites *list, ID3D12CommandQueue *queue, UINT64 order)
{
	std::vector<Dx12BufferWrite> *writes = &list->writes;
	std::vector<MovedHash> moved, rehashed;

	if (writes->empty())
		return;

	// Without the lock, before any of them is put in place
	for (Dx12BufferWrite &write : *writes) {
		if (write.upload && write.hashed && write.executed)
			rehash_upload(&write, &rehashed);
	}

	AcquireSRWLockExclusive(&lock);
	for (Dx12BufferWrite &write : *writes) {
		bool hashed = write.src ? source_kept(&write, queue) : write.hashed;
		bool first = !write.executed;

		write.executed = true;
		if (write_placed(&write, queue, order) && write.placed_hashed == hashed)
			continue;
		// Another write was put there since this one was recorded, which
		// may have been from the same upload buffer written again since:
		// what the GPU copies is what it holds now
		if (first && write.upload && write.hashed) {
			ReleaseSRWLockExclusive(&lock);
			rehash_upload(&write, &rehashed);
			AcquireSRWLockExclusive(&lock);
			hashed = write.hashed;
		}
		// Where the write has no hash and nothing it overlaps has one,
		// there is nothing to take back
		if (!hashed && !hash_under(&write))
			continue;
		place_write(&write, hashed, queue, order, &moved);
	}
	ReleaseSRWLockExclusive(&lock);

	InterlockedAdd64(&dx12_buffer_stats.copy_propagated, (LONG64)moved.size());
	for (const MovedHash &hash : moved)
		Dx12DumpBuffer(hash.resource, hash.offset, &hash.hash);
	for (const MovedHash &hash : rehashed)
		Dx12DumpBuffer(hash.resource, hash.offset, &hash.hash);
}

// What the list's draws recorded after it read there, see Dx12ListBufferWrites
static void add_write(Dx12ListBufferWrites *list, Dx12BufferWrite *write)
{
	std::map<UINT64, Dx12ListBufferWrites::Recorded> &map = list->recorded[write->dst];
	UINT64 start = write->dst_offset, end = write->dst_offset + write->bytes;
	auto i = map.lower_bound(start);

	if (i != map.begin() && std::prev(i)->first + std::prev(i)->second.bytes > start)
		--i;
	while (i != map.end() && i->first < end) {
		UINT64 at = i->first, after = i->first + i->second.bytes;
		Dx12ListBufferWrites::Recorded earlier = i->second;

		i = map.erase(i);
		if (at < start)
			map[at] = { start - at, earlier.write, false };
		if (after > end)
			map[end] = { after - end, earlier.write, false };
	}
	map[start] = { write->bytes, list->writes.size(), true };
	list->writes.push_back(std::move(*write));
}

void Dx12CopyBufferRegion(Dx12ListBufferWrites *writes, ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src,
		UINT64 src_offset, UINT64 bytes)
{
	Dx12ResourceInfo dst_info, src_info;
	Dx12BufferWrite write = { dst, dst_offset, bytes, NULL, src_offset, dx12_frame };
	uint32_t hash;
	BYTE *data = NULL;
	bool placed = false;

	if (!dst || !src || !bytes)
		return;

	Profiling::Spike::Scope spike(Profiling::Spike::DX12_BUFFER_HASH, L"bytes", bytes);

	if (!Dx12GetResourceInfo(src, &src_info) || !Dx12GetResourceInfo(dst, &dst_info)) {
		InterlockedIncrement64(&dx12_buffer_stats.untracked);
		return;
	}
	// Copies between upload buffers and into readback buffers are not
	// uploads of something a draw will use
	if (dst_info.cpu_accessible)
		return;
	if (!src_info.cpu_accessible || src_info.heap_type == D3D12_HEAP_TYPE_READBACK) {
		write.src = src;
		AcquireSRWLockExclusive(&lock);
		note_move(&write);
		ReleaseSRWLockExclusive(&lock);
		InterlockedIncrement64(&dx12_buffer_stats.not_mappable);
		add_write(writes, &write);
		return;
	}
	if (src_offset > src_info.desc.Width || bytes > src_info.desc.Width - src_offset) {
		InterlockedIncrement64(&dx12_buffer_stats.out_of_bounds);
		return;
	}

	if (!note_write(dst, dst_offset, bytes, false, &write.version)) {
		InterlockedIncrement64(&dx12_buffer_stats.dynamic);
		add_write(writes, &write);
		return;
	}
	if (bytes > MAX_HASHED_BYTES) {
		add_write(writes, &write);
		return;
	}
	if (FAILED(src->Map(0, NULL, (void**)&data)) || !data) {
		InterlockedIncrement64(&dx12_buffer_stats.not_mappable);
		add_write(writes, &write);
		return;
	}
	hash = Dx12Crc32cWriteCombined(0, data + src_offset, (size_t)bytes);
	src->Unmap(0, NULL);

	AcquireSRWLockExclusive(&lock);
	auto resource = ranges.find(dst);
	if (resource != ranges.end()) {
		auto range = resource->second.find(dst_offset);
		if (range != resource->second.end() && range->second.version == write.version) {
			range->second.data_hash = hash;
			range->second.valid = true;
			stamp_write(dst, dst_offset, bytes);
			placed = true;
		}
	}
	ReleaseSRWLockExclusive(&lock);

	write.upload = src;
	write.data_hash = hash;
	write.hashed = true;
	write.placed_hashed = placed;
	add_write(writes, &write);

	InterlockedIncrement64(&dx12_buffer_stats.hashed);
	InterlockedAdd64(&dx12_buffer_stats.hashed_bytes, bytes);

	Dx12BufferHash result = { bytes, hash, dx12_frame };
	Dx12DumpBuffer(dst, dst_offset, &result);
}

void Dx12CopyBuffer(Dx12ListBufferWrites *writes, ID3D12Resource *dst, ID3D12Resource *src)
{
	Dx12ResourceInfo info;

	if (dst && src && Dx12GetResourceInfo(dst, &info) && info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		Dx12CopyBufferRegion(writes, dst, 0, src, 0, info.desc.Width);
}

bool Dx12FindBufferHash(ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash)
{
	bool found = false;

	if (!*with_ranges(resource))
		return false;

	AcquireSRWLockShared(&lock);
	auto i = ranges.find(resource);
	if (i != ranges.end()) {
		auto range = i->second.find(offset);
		if (range != i->second.end() && range->second.valid) {
			hash->bytes = range->second.bytes;
			hash->data_hash = range->second.data_hash;
			hash->frame = range->second.frame;
			found = true;
		}
	}
	ReleaseSRWLockShared(&lock);

	return found;
}

bool Dx12ListWroteBuffer(const Dx12ListBufferWrites *writes, ID3D12Resource *resource, UINT64 offset, UINT64 bytes)
{
	if (!writes || writes->recorded.empty())
		return false;
	auto map = writes->recorded.find(resource);
	if (map == writes->recorded.end())
		return false;
	auto i = map->second.lower_bound(offset);
	if (i != map->second.end() && i->first < offset + bytes)
		return true;
	return i != map->second.begin() && std::prev(i)->first + std::prev(i)->second.bytes > offset;
}

bool Dx12FindListBufferHash(const Dx12ListBufferWrites *writes, ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash, bool *own)
{
	*own = false;
	if (writes && !writes->recorded.empty()) {
		auto map = writes->recorded.find(resource);
		if (map != writes->recorded.end()) {
			auto i = map->second.upper_bound(offset);
			if (i != map->second.begin() && std::prev(i)->first + std::prev(i)->second.bytes > offset) {
				const Dx12ListBufferWrites::Recorded &recorded = std::prev(i)->second;
				const Dx12BufferWrite &write = writes->writes[recorded.write];

				// As the copy was recorded: one from the GPU's
				// memory has no hash until it is executed
				*own = true;
				if (std::prev(i)->first != offset || !recorded.whole || write.src || !write.hashed)
					return false;
				hash->bytes = write.bytes;
				hash->data_hash = write.data_hash;
				hash->frame = write.frame;
				return true;
			}
		}
	}
	if (!Dx12FindBufferHash(resource, offset, hash))
		return false;
	// Put there by another list since the list's copy over part of it
	// was recorded
	if (Dx12ListWroteBuffer(writes, resource, offset, hash->bytes)) {
		*own = true;
		return false;
	}
	return true;
}

void Dx12DescribeBufferRange(ID3D12Resource *resource, UINT64 offset, char *text, size_t size)
{
	AcquireSRWLockShared(&lock);
	auto i = ranges.find(resource);
	if (i == ranges.end()) {
		_snprintf_s(text, size, _TRUNCATE, "no copy into the buffer was seen");
	} else {
		auto next = i->second.upper_bound(offset);
		auto range = next;

		if (range != i->second.begin() && std::prev(range)->first + std::prev(range)->second.bytes > offset) {
			range = std::prev(range);
			const BufferRange &r = range->second;

			if (range->first != offset)
				_snprintf_s(text, size, _TRUNCATE, "a copy of %llu bytes to +%llu at frame %u covers it", r.bytes, range->first, r.frame);
			else if (r.valid && r.unmapped)
				_snprintf_s(text, size, _TRUNCATE, "hashed %08x, %llu bytes moved there by a copy between buffers of the GPU at frame %u",
					r.data_hash, r.bytes, r.frame);
			else if (r.valid)
				_snprintf_s(text, size, _TRUNCATE, "hashed %08x, %llu bytes copied at frame %u", r.data_hash, r.bytes, r.frame);
			else if (r.unmapped)
				_snprintf_s(text, size, _TRUNCATE, "%llu bytes copied at frame %u from memory that is not mappable", r.bytes, r.frame);
			else if (r.hot >= DX12_HOT_LIMIT)
				_snprintf_s(text, size, _TRUNCATE, "%llu bytes copied at frame %u, %u times in a row within %u frames: taken for dynamic",
					r.bytes, r.frame, r.hot + 1, DX12_HOT_FRAMES);
			else
				_snprintf_s(text, size, _TRUNCATE, "%llu bytes copied at frame %u, not hashed yet", r.bytes, r.frame);
		} else if (next != i->second.end()) {
			// What covered it may have been replaced by copies after
			// its start
			_snprintf_s(text, size, _TRUNCATE, "no copy covers it, the next is of %llu bytes to +%llu at frame %u",
				next->second.bytes, next->first, next->second.frame);
		} else {
			_snprintf_s(text, size, _TRUNCATE, "no copy covers it or follows it");
		}
	}
	ReleaseSRWLockShared(&lock);
}

void Dx12ForgetBufferHashes(ID3D12Resource *resource)
{
	AcquireSRWLockExclusive(&lock);
	if (ranges.erase(resource))
		InterlockedDecrement(with_ranges(resource));
	InterlockedIncrement64(resource_stamp(resource));
	ReleaseSRWLockExclusive(&lock);
}
