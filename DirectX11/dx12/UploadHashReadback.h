#pragma once

#include <vector>

#include "BufferHash.h"
#include "Dx12Common.h"
#include "TextureHash.h"

class Dx12GraphicsCommandList;

// The GPU half of dx12_defer_upload_hash, see Dx12TextureUpload and
// Dx12BufferUpload. A list of ours goes first in the game's own
// ExecuteCommandLists on the game's queue and copies what the deferred uploads
// of the lists read out of their upload buffers into readback memory. Being on
// the same queue ahead of the game's lists, it runs while the game still keeps
// those buffers alive, resident and unchanged. The hashes are taken from that
// memory once our fence says so.

// With Dx12QueueLock of the queue held, for the proxies about to be executed
// for the first time that have deferred texture uploads and the deferred
// buffer uploads of all lists being executed: the list to execute ahead of the
// game's, or NULL. The uploads no copy was made of go to on_cpu_textures and
// on_cpu_buffers, for the caller to hash with Dx12HashTextureUploadNow and
// Dx12HashBufferUploadNow before the game's lists are executed. A non-NULL
// context is to be handed to Dx12UploadHashesSubmitted right after the queue
// has the lists, still with that lock held.
ID3D12CommandList* Dx12UploadHashesExecuting(ID3D12CommandQueue *queue, Dx12GraphicsCommandList *const *proxies, size_t count,
		const std::vector<Dx12BufferUpload> &buffers, std::vector<Dx12TextureUpload> *on_cpu_textures,
		std::vector<Dx12BufferUpload> *on_cpu_buffers, void **context);
void Dx12UploadHashesSubmitted(ID3D12CommandQueue *queue, void *context);

// At present: takes the hashes of the copies the GPU has got through
void Dx12FinishUploadHashes();
