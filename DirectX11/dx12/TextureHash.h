#pragma once

#include <d3d11.h>

#include <vector>

#include "ResourceTracker.h"

// Reproduces the hashes [TextureOverride] sections were written against under
// D3D11, where 3DMigoto hashes the top mip passed to CreateTexture2D and then
// the D3D11_TEXTURE2D_DESC.
//
// D3D12 textures are created empty and filled by copying from an upload
// buffer, so the data is hashed when that copy is recorded: the upload buffer
// is mappable and holds the same bytes the game would have handed to D3D11.
// Later writes and copies follow the track_texture_updates rules of
// HackerContext.
//
// The description is rebuilt from the D3D12 one. What D3D12 does not record
// (bind flags, typeless formats chosen by a D3D11 renderer) is a guess, so the
// format has variants that HashDump.cpp compares against the hashes in the
// mods to tell which one a game needs.

enum Dx12FormatRule {
	// The format of the D3D12 resource
	DX12_FORMAT_AS_IS,
	// Block compressed formats as their typeless family
	DX12_FORMAT_BC_TYPELESS,
	// The formats Unreal Engine creates typeless under D3D11:
	// BC1, BC2, BC3, BC7, R8G8B8A8 and B8G8R8A8
	DX12_FORMAT_UNREAL,
	DX12_FORMAT_RULE_COUNT
};
extern const char *const dx12_format_rule_names[DX12_FORMAT_RULE_COUNT];

// The hash 3DMigoto would have given the D3D11 counterpart of a 2D texture,
// and the description of that counterpart, which the hash is of along with
// the data hash
uint32_t Dx12TextureHash(const Dx12ResourceInfo *info, Dx12FormatRule rule);
void Dx12TextureDesc(const Dx12ResourceInfo *info, Dx12FormatRule rule, D3D11_TEXTURE2D_DESC *desc);

// Hashing an upload when the copy is recorded reads the upload heap on the
// recording thread, which is slow where that memory is write combined or in
// video memory. With dx12_defer_upload_hash, hashing at record time stops once
// the frame has spent its budget for that, and what is left of the top mip is
// kept by the list until it is executed, when a list of ours copies those bytes
// on the GPU, see UploadHashReadback.h, and the hash is set some frames later.
struct Dx12TextureUpload {
	ID3D12Resource *texture;
	UINT64 serial;
	UINT generation;
	ID3D12Resource *upload;
	UINT64 offset;
	UINT row_pitch;
	UINT rows;
	// Bytes of a row that are hashed, and of the whole
	size_t row_bytes;
	size_t limit;
	// The bytes hashed so far, and the CRC they came to, which the rest
	// carries on from
	size_t hashed;
	uint32_t crc;
};

// For the command list proxy, before it forwards the call. uploads is the
// list's, where deferred uploads go.
void Dx12CopyTextureRegion(std::vector<Dx12TextureUpload> *uploads, const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
		const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box);
void Dx12CopyResource(ID3D12Resource *dst, ID3D12Resource *src);

// Where in the upload buffer the row the hash carries on from starts, and the
// bytes from there to the end of what the hash reads
UINT64 Dx12TextureUploadStart(const Dx12TextureUpload *upload);
UINT64 Dx12TextureUploadSpan(const Dx12TextureUpload *upload);
// Carries on hashing the upload from data, which holds what the upload buffer
// does from Dx12TextureUploadStart on. write_combined for the upload heap
// itself, false for a copy in cached memory. budgeted stops once the frame has
// spent its budget for hashing at record time. True once all of it is hashed,
// when crc is the hash.
bool Dx12HashTextureRows(const BYTE *data, Dx12TextureUpload *upload, bool write_combined, bool budgeted);
// The same budget for buffer uploads (BufferHash.cpp): whether the frame has
// any of it left, and hashing total bytes from data on from *hashed, carrying
// on from *crc, which stops when budgeted and it runs out. True once all of it
// is hashed.
bool Dx12HashBudgetLeft();
bool Dx12HashUploadBytes(const BYTE *data, UINT64 total, UINT64 *hashed, uint32_t *crc, bool write_combined, bool budgeted);
// Hashes the upload from the upload buffer, which has to be alive and hold
// what the copy reads: when the list is executed and no GPU copy can be made
void Dx12HashTextureUploadNow(const Dx12TextureUpload *upload);
// Sets the hash a deferred upload came to, data_hash NULL for one that was
// never executed, and the same for the copies recorded of the texture while it
// was pending
void Dx12TextureHashDone(ID3D12Resource *texture, UINT64 serial, UINT generation, const uint32_t *data_hash);
// For a list reset or released without having been executed
void Dx12AbandonTextureUploads(std::vector<Dx12TextureUpload> *uploads);

// Why uploads were or were not hashed, since the last report
struct Dx12TextureStats {
	volatile LONG64 hashed;
	volatile LONG64 hashed_bytes;
	// Mips below the top one and array slices after the first, which the
	// D3D11 hash does not cover either
	volatile LONG64 other_subresource;
	volatile LONG64 partial;
	volatile LONG64 update_ignored;
	volatile LONG64 dynamic;
	volatile LONG64 not_2d;
	volatile LONG64 unknown_format;
	volatile LONG64 untracked;
	volatile LONG64 not_mappable;
	volatile LONG64 out_of_bounds;
	volatile LONG64 copy_propagated;
	volatile LONG64 copy_partial;
	// dx12_defer_upload_hash: uploads and copies that waited for one,
	// those hashed from the upload heap after all, results superseded
	// before they came, and uploads of lists never executed
	volatile LONG64 deferred;
	volatile LONG64 copy_deferred;
	volatile LONG64 deferred_on_cpu;
	volatile LONG64 deferred_superseded;
	volatile LONG64 deferred_abandoned;
	// The readback buffers the deferred uploads are copied to, now
	volatile LONG64 readback_rings;
	volatile LONG64 readback_bytes;
	// Uploads hashed in full when the copy was recorded although the option
	// is on, as that fit the frame's budget
	volatile LONG64 hashed_at_once;
};
extern Dx12TextureStats dx12_texture_stats;
