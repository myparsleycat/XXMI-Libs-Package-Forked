#include "TextureHash.h"

#include "util.h"
#include "../globals.h"
#include "../ResourceHash.h"

#include "HashDump.h"
#include "../profiling.h"

#include <atomic>
#include <map>

// For GetSurfaceInfo, which is what ResourceHash.cpp sizes a texture with
#include "DirectXTK/Src/pch.h"
#include "DirectXTK/Src/PlatformHelpers.h"
#include "DirectXTK/Src/LoaderHelpers.h"

Dx12TextureStats dx12_texture_stats;

const char *const dx12_format_rule_names[DX12_FORMAT_RULE_COUNT] = { "as is", "BC typeless", "Unreal typeless" };

static DXGI_FORMAT apply_format_rule(DXGI_FORMAT format, Dx12FormatRule rule)
{
	bool unreal = rule == DX12_FORMAT_UNREAL;

	if (rule == DX12_FORMAT_AS_IS)
		return format;

	switch (format) {
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			return DXGI_FORMAT_BC1_TYPELESS;
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			return DXGI_FORMAT_BC2_TYPELESS;
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			return DXGI_FORMAT_BC3_TYPELESS;
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return DXGI_FORMAT_BC7_TYPELESS;
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return unreal ? format : DXGI_FORMAT_BC4_TYPELESS;
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
			return unreal ? format : DXGI_FORMAT_BC5_TYPELESS;
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
			return unreal ? format : DXGI_FORMAT_BC6H_TYPELESS;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return unreal ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return unreal ? DXGI_FORMAT_B8G8R8A8_TYPELESS : format;
	}

	return format;
}

void Dx12TextureDesc(const Dx12ResourceInfo *info, Dx12FormatRule rule, D3D11_TEXTURE2D_DESC *desc)
{
	desc->Width = (UINT)info->desc.Width;
	desc->Height = info->desc.Height;
	desc->MipLevels = info->desc.MipLevels;
	desc->ArraySize = info->desc.DepthOrArraySize;
	desc->Format = apply_format_rule(info->desc.Format, rule);
	desc->SampleDesc = info->desc.SampleDesc;
	desc->Usage = D3D11_USAGE_DEFAULT;
	desc->BindFlags = 0;
	if (!(info->desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		desc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		desc->BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		desc->BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		desc->BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	desc->CPUAccessFlags = 0;
	desc->MiscFlags = info->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
}

uint32_t Dx12TextureHash(const Dx12ResourceInfo *info, Dx12FormatRule rule)
{
	D3D11_TEXTURE2D_DESC desc;

	Dx12TextureDesc(info, rule, &desc);
	return CalcTexture2DDescHash(info->has_data_hash ? info->data_hash : 0, &desc);
}

static bool is_mappable(ID3D12Resource *resource, UINT64 *size)
{
	D3D12_HEAP_PROPERTIES heap;
	Dx12ResourceInfo info;

	if (Dx12GetResourceInfo(resource, &info)) {
		*size = info.desc.Width;
		return info.cpu_accessible && info.heap_type != D3D12_HEAP_TYPE_READBACK;
	}

	// A buffer that was not created through the device proxy
	if (FAILED(resource->GetHeapProperties(&heap, NULL)))
		return false;
	*size = resource->GetDesc().Width;
	return heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD;
}

// dx12_defer_upload_hash still hashes uploads when the copy is recorded until
// that took this long in the frame, so that where upload memory reads fast a
// texture or buffer matches from its first draw. The time is measured every
// AT_ONCE_STEP_BYTES rather than estimated from earlier uploads.
static const double AT_ONCE_BUDGET_MS = 0.5;
static const size_t AT_ONCE_STEP_BYTES = 4 << 10;

// The frame in the upper half and the ticks hashing at record time took in it
// in the lower, so that a new frame starts from 0 without losing what another
// thread added for it meanwhile
static std::atomic<UINT64> at_once_state;

static LONG64 ticks_per_ms()
{
	static const LONG64 ticks = [] {
		LARGE_INTEGER frequency;

		QueryPerformanceFrequency(&frequency);
		return frequency.QuadPart / 1000;
	}();

	return ticks;
}

bool Dx12HashBudgetLeft()
{
	UINT64 state = at_once_state.load();

	if (G->dx12_defer_upload_hash == 2)
		return false;
	return (UINT)(state >> 32) != dx12_frame || (UINT32)state < AT_ONCE_BUDGET_MS * ticks_per_ms();
}

// Returns the ticks the frame has spent with these. Ticks of a frame that
// another thread has already moved on from go to that newer frame.
static UINT32 at_once_spend(UINT frame, LONG64 ticks)
{
	UINT64 state = at_once_state.load(), next;
	UINT64 spent;

	do {
		if ((INT32)((UINT)(state >> 32) - frame) > 0)
			frame = (UINT)(state >> 32);
		spent = (UINT)(state >> 32) == frame ? (UINT32)state : 0;
		spent = min(spent + (UINT64)max(ticks, 0LL), (UINT64)UINT32_MAX);
		next = (UINT64)frame << 32 | spent;
	} while (!at_once_state.compare_exchange_weak(state, next));

	return (UINT32)spent;
}

// Measures hashing at record time against the frame's budget, if budgeted
struct AtOnceTimer {
	bool budgeted;
	UINT frame;
	LONG64 budget;
	LARGE_INTEGER last;

	AtOnceTimer(bool budgeted) : budgeted(budgeted), frame(0), budget(0), last()
	{
		if (!budgeted)
			return;
		frame = dx12_frame;
		budget = (LONG64)(AT_ONCE_BUDGET_MS * ticks_per_ms());
		QueryPerformanceCounter(&last);
	}

	// Adds the time since the last call as it goes, so that hashing on
	// other threads stops on it as well. True once the budget is spent.
	bool spent()
	{
		LARGE_INTEGER now;
		UINT32 frame_spent;

		if (!budgeted)
			return false;
		QueryPerformanceCounter(&now);
		frame_spent = at_once_spend(frame, now.QuadPart - last.QuadPart);
		last = now;
		return frame_spent >= budget;
	}
};

static uint32_t crc_step(uint32_t crc, const BYTE *data, size_t count, bool write_combined)
{
	return write_combined ? Dx12Crc32cWriteCombined(crc, data, count) : crc32c_hw(crc, data, count);
}

// texture_hash = 1 covers the whole top mip without row padding. The default
// hash stops after as many bytes as the mip has texels, see
// CalcTexture2DDataHash for the history of that.
static size_t hash_bytes(const Dx12TextureUpload *upload)
{
	return min(upload->limit, upload->row_bytes * upload->rows);
}

UINT64 Dx12TextureUploadStart(const Dx12TextureUpload *upload)
{
	return upload->offset + (UINT64)upload->row_pitch * (upload->hashed / upload->row_bytes);
}

UINT64 Dx12TextureUploadSpan(const Dx12TextureUpload *upload)
{
	size_t total = hash_bytes(upload);
	size_t first = upload->hashed / upload->row_bytes, last = (total - 1) / upload->row_bytes;

	return (UINT64)upload->row_pitch * (last - first) + (total - last * upload->row_bytes);
}

bool Dx12HashTextureRows(const BYTE *data, Dx12TextureUpload *upload, bool write_combined, bool budgeted)
{
	size_t total = hash_bytes(upload), column = upload->hashed % upload->row_bytes, count, done = 0;
	AtOnceTimer timer(budgeted);
	bool complete = true;

	while (upload->hashed < total) {
		count = min(upload->row_bytes - column, total - upload->hashed);
		if (budgeted && count > AT_ONCE_STEP_BYTES)
			count = AT_ONCE_STEP_BYTES;
		upload->crc = crc_step(upload->crc, data + column, count, write_combined);
		upload->hashed += count;
		done += count;
		column += count;
		if (column == upload->row_bytes) {
			data += upload->row_pitch;
			column = 0;
		}

		if (timer.spent() && upload->hashed < total) {
			complete = false;
			break;
		}
	}
	InterlockedAdd64(&dx12_texture_stats.hashed_bytes, done);
	if (complete)
		InterlockedIncrement64(&dx12_texture_stats.hashed);

	return complete;
}

bool Dx12HashUploadBytes(const BYTE *data, UINT64 total, UINT64 *hashed, uint32_t *crc, bool write_combined, bool budgeted)
{
	AtOnceTimer timer(budgeted);
	size_t count;

	while (*hashed < total) {
		count = (size_t)min(total - *hashed, budgeted ? (UINT64)AT_ONCE_STEP_BYTES : (UINT64)SIZE_MAX);
		*crc = crc_step(*crc, data + *hashed, count, write_combined);
		*hashed += count;
		if (timer.spent() && *hashed < total)
			return false;
	}
	return true;
}

// With the hash of a pending texture, copies recorded of it go by what it
// comes to: the texture, its serial and generation are those of the write
// whose hash it is to be
struct PendingKey {
	ID3D12Resource *texture;
	UINT64 serial;
	UINT generation;

	bool operator<(const PendingKey &other) const
	{
		if (texture != other.texture)
			return texture < other.texture;
		if (serial != other.serial)
			return serial < other.serial;
		return generation < other.generation;
	}
};
// A copy from a texture whose hash is pending, with the hash the source had
// before that write: what the copy gets if the write's hash never comes, as
// the write then never reached the GPU or could not be read back
struct PendingCopy {
	PendingKey key;
	bool had_data_hash;
	uint32_t had_hash;
};
static SRWLOCK copies_lock = SRWLOCK_INIT;
static std::multimap<PendingKey, PendingCopy> pending_copies;

void Dx12TextureHashDone(ID3D12Resource *texture, UINT64 serial, UINT generation, const uint32_t *data_hash)
{
	struct Work {
		PendingKey key;
		bool known, has_data_hash;
		uint32_t data_hash;
	};
	std::vector<Work> work = { { { texture, serial, generation }, data_hash != NULL, data_hash != NULL, data_hash ? *data_hash : 0 } };
	bool copy = false;

	while (!work.empty()) {
		Work at = work.back();

		work.pop_back();
		if (Dx12FinishTextureHash(at.key.texture, at.key.serial, at.key.generation, at.known, at.has_data_hash, at.data_hash)) {
			if (at.known && at.has_data_hash)
				Dx12DumpTexture(at.key.texture, copy ? "copy" : "upload");
		} else {
			InterlockedIncrement64(&dx12_texture_stats.deferred_superseded);
		}

		// What was copied from the texture has the content of the
		// write the hash is of, even when the texture has been
		// written again since
		AcquireSRWLockExclusive(&copies_lock);
		auto range = pending_copies.equal_range(at.key);
		for (auto i = range.first; i != range.second; ++i) {
			if (at.known)
				work.push_back({ i->second.key, true, at.has_data_hash, at.data_hash });
			else
				work.push_back({ i->second.key, true, i->second.had_data_hash, i->second.had_hash });
		}
		pending_copies.erase(range.first, range.second);
		ReleaseSRWLockExclusive(&copies_lock);
		copy = true;
	}
}

// From the upload heap: -1 if the upload buffer cannot be mapped, else whether
// all of it is hashed
static int hash_mapped(Dx12TextureUpload *upload, bool budgeted, const wchar_t *what)
{
	Profiling::Spike::Scope spike(Profiling::Spike::DX12_TEXTURE_HASH, what);
	BYTE *data = NULL;
	bool complete;

	if (FAILED(upload->upload->Map(0, NULL, (void**)&data)) || !data) {
		InterlockedIncrement64(&dx12_texture_stats.not_mappable);
		return -1;
	}
	complete = Dx12HashTextureRows(data + Dx12TextureUploadStart(upload), upload, true, budgeted);
	upload->upload->Unmap(0, NULL);

	return complete;
}

void Dx12HashTextureUploadNow(const Dx12TextureUpload *upload)
{
	Dx12TextureUpload rest = *upload;

	if (hash_mapped(&rest, false, L"after all") < 0)
		Dx12TextureHashDone(rest.texture, rest.serial, rest.generation, NULL);
	else
		Dx12TextureHashDone(rest.texture, rest.serial, rest.generation, &rest.crc);
}

void Dx12AbandonTextureUploads(std::vector<Dx12TextureUpload> *uploads)
{
	for (const Dx12TextureUpload &upload : *uploads) {
		InterlockedIncrement64(&dx12_texture_stats.deferred_abandoned);
		Dx12TextureHashDone(upload.texture, upload.serial, upload.generation, NULL);
	}
	uploads->clear();
}

static void texture_uploaded(std::vector<Dx12TextureUpload> *uploads, ID3D12Resource *texture, UINT subresource, UINT x, UINT y, UINT z,
		ID3D12Resource *buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint, const D3D12_BOX *box)
{
	size_t row_bytes = 0, rows = 0;
	Dx12TextureUpload upload = {};
	Dx12ResourceInfo info;
	UINT64 buffer_size;
	UINT width, height;

	if (!Dx12GetResourceInfo(texture, &info)) {
		InterlockedIncrement64(&dx12_texture_stats.untracked);
		return;
	}
	if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		InterlockedIncrement64(&dx12_texture_stats.not_2d);
		return;
	}
	if (subresource) {
		InterlockedIncrement64(&dx12_texture_stats.other_subresource);
		return;
	}

	width = (UINT)info.desc.Width;
	height = info.desc.Height;
	if (x || y || z || footprint->Footprint.Width < width || footprint->Footprint.Height < height
	    || (box && (box->left || box->top || box->front || box->right < width || box->bottom < height))) {
		InterlockedIncrement64(&dx12_texture_stats.partial);
		return;
	}

	// Under D3D11 the first write is the initial data, which is always
	// hashed, and anything after it an update, which only is with
	// track_texture_updates.
	if ((info.has_data_hash || info.hash_pending) && G->track_texture_updates != 1) {
		InterlockedIncrement64(&dx12_texture_stats.update_ignored);
		return;
	}

	// Hashing a texture the game writes every frame, such as one it plays
	// a video into, reads all of it back out of the write combined upload
	// heap each time, which on some machines takes longer than the frame.
	// The hash of whichever write came last would be of no use to a mod,
	// where the description alone is the same from one run to the next.
	if (!Dx12NoteTopMipUpload(texture)) {
		InterlockedIncrement64(&dx12_texture_stats.dynamic);
		if (info.has_data_hash || info.hash_pending)
			Dx12SetTextureDataHash(texture, false, 0);
		if (info.has_data_hash)
			Dx12DumpTexture(texture, "dynamic");
		return;
	}

	DirectX::LoaderHelpers::GetSurfaceInfo(width, height, info.desc.Format, NULL, &row_bytes, &rows);
	if (!row_bytes || !rows) {
		InterlockedIncrement64(&dx12_texture_stats.unknown_format);
		return;
	}
	if (row_bytes > footprint->Footprint.RowPitch)
		row_bytes = footprint->Footprint.RowPitch;

	if (!is_mappable(buffer, &buffer_size)) {
		InterlockedIncrement64(&dx12_texture_stats.not_mappable);
		return;
	}
	if (footprint->Offset > buffer_size
	    || (UINT64)footprint->Footprint.RowPitch * (rows - 1) + row_bytes > buffer_size - footprint->Offset) {
		InterlockedIncrement64(&dx12_texture_stats.out_of_bounds);
		return;
	}

	upload.texture = texture;
	upload.upload = buffer;
	upload.offset = footprint->Offset;
	upload.row_pitch = footprint->Footprint.RowPitch;
	upload.rows = (UINT)rows;
	upload.row_bytes = row_bytes;
	upload.limit = G->texture_hash_version ? SIZE_MAX : (size_t)width * height * info.desc.DepthOrArraySize;

	wchar_t what[48] = L"";
	if (Profiling::Spike::threshold)
		swprintf_s(what, L"%ux%u format %d", width, height, (int)info.desc.Format);

	if (G->dx12_defer_upload_hash && uploads) {
		int complete = 0;

		if (Dx12HashBudgetLeft())
			complete = hash_mapped(&upload, true, what);
		if (complete < 0)
			return;
		if (complete) {
			InterlockedIncrement64(&dx12_texture_stats.hashed_at_once);
			Dx12SetTextureDataHash(texture, true, upload.crc);
			Dx12DumpTexture(texture, "upload");
			return;
		}
		upload.generation = Dx12BeginTextureHash(texture, &upload.serial);
		if (upload.generation) {
			InterlockedIncrement64(&dx12_texture_stats.deferred);
			uploads->push_back(upload);
			return;
		}
	}

	if (hash_mapped(&upload, false, what) < 0)
		return;
	Dx12SetTextureDataHash(texture, true, upload.crc);
	Dx12DumpTexture(texture, "upload");
}

// What HackerContext does with track_texture_updates for CopyResource and
// for a CopySubresourceRegion that replaces the top mip
static void propagate_hash(ID3D12Resource *dst, ID3D12Resource *src)
{
	Dx12ResourceInfo dst_info, src_info;

	if (!Dx12GetResourceInfo(dst, &dst_info) || dst_info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
		return;
	// Under the lock, so that the source's hash cannot come in between
	// being found pending and this being noted for it
	AcquireSRWLockExclusive(&copies_lock);
	if (!Dx12GetResourceInfo(src, &src_info) || src_info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		ReleaseSRWLockExclusive(&copies_lock);
		return;
	}
	if (src_info.hash_pending) {
		PendingKey copy = { dst };

		copy.generation = Dx12BeginTextureHash(dst, &copy.serial);
		if (copy.generation) {
			pending_copies.insert({ { src, src_info.serial, src_info.hash_generation },
				{ copy, src_info.has_data_hash, src_info.data_hash } });
			InterlockedIncrement64(&dx12_texture_stats.copy_deferred);
		}
		ReleaseSRWLockExclusive(&copies_lock);
		return;
	}
	ReleaseSRWLockExclusive(&copies_lock);

	if (!dst_info.hash_pending && src_info.has_data_hash == dst_info.has_data_hash && src_info.data_hash == dst_info.data_hash)
		return;

	InterlockedIncrement64(&dx12_texture_stats.copy_propagated);
	Dx12SetTextureDataHash(dst, src_info.has_data_hash, src_info.data_hash);
	Dx12DumpTexture(dst, "copy");
}

void Dx12CopyTextureRegion(std::vector<Dx12TextureUpload> *uploads, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
		const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
	if (!dst || !src || !dst->pResource || !src->pResource || dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
		return;

	if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
		texture_uploaded(uploads, dst->pResource, dst->SubresourceIndex, x, y, z, src->pResource, &src->PlacedFootprint, box);
		return;
	}

	if (G->track_texture_updates != 1)
		return;
	if (dst->SubresourceIndex || x || y || z || box) {
		InterlockedIncrement64(&dx12_texture_stats.copy_partial);
		return;
	}

	propagate_hash(dst->pResource, src->pResource);
}

void Dx12CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
{
	if (G->track_texture_updates != 1 || !dst || !src)
		return;

	propagate_hash(dst, src);
}
