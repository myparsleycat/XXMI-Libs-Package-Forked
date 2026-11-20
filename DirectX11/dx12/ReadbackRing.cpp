#include "ReadbackRing.h"

bool Dx12ReadbackRing::Create(ID3D12Device *device, UINT64 bytes)
{
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC desc = {};
	D3D12_RANGE range = { 0, (SIZE_T)bytes };

	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&resource)))) {
		resource = NULL;
		return false;
	}
	if (FAILED(resource->Map(0, &range, (void**)&data)) || !data) {
		resource->Release();
		resource = NULL;
		data = NULL;
		return false;
	}
	size = bytes;
	head = 0;
	starts.clear();
	return true;
}

void Dx12ReadbackRing::Release()
{
	if (resource) {
		resource->Unmap(0, NULL);
		resource->Release();
	}
	resource = NULL;
	data = NULL;
	size = 0;
	head = 0;
	starts.clear();
}

bool Dx12ReadbackRing::Alloc(UINT64 bytes, UINT64 *offset)
{
	UINT64 start = head, tail;

	if (!resource)
		return false;

	bytes = (bytes + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
	if (bytes > size)
		return false;

	if (starts.empty()) {
		start = 0;
	} else {
		tail = starts.front();
		// With copies pending, the head on the tail is a full ring
		if (start > tail) {
			if (start + bytes > size) {
				if (bytes > tail)
					return false;
				start = 0;
			}
		} else if (start == tail || start + bytes > tail) {
			return false;
		}
	}

	head = start + bytes;
	starts.push_back(start);
	last_used = GetTickCount64();
	*offset = start;
	return true;
}

void Dx12ReadbackRing::FreeOldest()
{
	if (!starts.empty())
		starts.pop_front();
}

void Dx12ReadbackRing::FreeNewest()
{
	if (starts.empty())
		return;
	head = starts.back();
	starts.pop_back();
}
