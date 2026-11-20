#pragma once

#include <deque>

#include "Dx12Common.h"

// A readback buffer, mapped for good, that GPU copies take turns in. The
// copies are gone through in the order they were made, so the space is handed
// out as a ring and given back oldest first.
struct Dx12ReadbackRing {
	static const UINT64 ALIGNMENT = 256;

	ID3D12Resource *resource = NULL;
	BYTE *data = NULL;
	UINT64 size = 0;
	UINT64 head = 0;
	// Where each copy that is pending starts, oldest first
	std::deque<UINT64> starts;
	// GetTickCount64 of the last Alloc
	ULONGLONG last_used = 0;

	bool Create(ID3D12Device *device, UINT64 bytes);
	void Release();
	// False if the copy does not fit until older ones are freed
	bool Alloc(UINT64 bytes, UINT64 *offset);
	// For the oldest copy, once what it read is gone through
	void FreeOldest();
	// For the copy last handed out, which is not going to be made
	void FreeNewest();
	bool Empty() const { return starts.empty(); }
};
