#pragma once

#include <span>
#include <vector>

#include "Dx12Common.h"

struct Dx12DxilCBuffer {
	UINT space;
	UINT reg;
	// More than one for an array of constant buffers
	UINT count;
	UINT bytes;
};

struct Dx12DxilStructuredBuffer {
	UINT space, reg, count, stride;
	bool uav;
};

struct Dx12DxilResources {
	std::vector<Dx12DxilCBuffer> cbuffers;
	std::vector<Dx12DxilStructuredBuffer> structured;
};

// Reads sizes and strides from the DXIL part of the container. PSV0 omits them.
bool Dx12ReadDxilResources(std::span<const BYTE> data, Dx12DxilResources *result);
