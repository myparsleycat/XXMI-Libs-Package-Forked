#pragma once

#include <d3d11_1.h>

struct ModLoad;

// These values can now be exposed through draw_type and their values should
// not be changed. Any additions should be added to the end of the list.
enum class DrawCall {
	Invalid                      = 0,
	Draw                         = 1,
	DrawIndexed                  = 2,
	DrawInstanced                = 3,
	DrawIndexedInstanced         = 4,
	DrawInstancedIndirect        = 5,
	DrawIndexedInstancedIndirect = 6,
	DrawAuto                     = 7,
	Dispatch                     = 8,
	DispatchIndirect             = 9,
};

struct DrawCallInfo
{
	DrawCall type;

	UINT VertexCount, IndexCount, InstanceCount;
	UINT FirstVertex, FirstIndex, FirstInstance;
	UINT ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ;

	ID3D11Buffer **indirect_buffer;
	UINT args_offset;

	bool skip, hunting_skip;

	// ModLoadEpoch() when the draw started, 0 for none
	unsigned load_epoch = 0;
	// Whether the [TextureOverride] sections of a mod run in this draw, as
	// decided the first time it was asked, so that the post command lists
	// follow the pre ones even if a variable changed in between
	struct LoadVerdict {
		ModLoad *load;
		bool ready;
	} load_verdicts[8];
	unsigned char load_verdict_count = 0;

	DrawCallInfo() :
		type(DrawCall::Invalid),
		VertexCount(0),
		IndexCount(0),
		InstanceCount(0),
		FirstVertex(0),
		FirstIndex(0),
		FirstInstance(0),
		ThreadGroupCountX(0),
		ThreadGroupCountY(0),
		ThreadGroupCountZ(0),
		indirect_buffer(NULL),
		args_offset(0),
		skip(false),
		hunting_skip(false)
	{}

	DrawCallInfo(DrawCall type,
			UINT VertexCount, UINT IndexCount, UINT InstanceCount,
			UINT FirstVertex, UINT FirstIndex, UINT FirstInstance,
			ID3D11Buffer **indirect_buffer, UINT args_offset,
			UINT ThreadGroupCountX = 0, UINT ThreadGroupCountY = 0, UINT ThreadGroupCountZ = 0) :
		type(type),
		VertexCount(VertexCount),
		IndexCount(IndexCount),
		InstanceCount(InstanceCount),
		FirstVertex(FirstVertex),
		FirstIndex(FirstIndex),
		FirstInstance(FirstInstance),
		ThreadGroupCountX(ThreadGroupCountX),
		ThreadGroupCountY(ThreadGroupCountY),
		ThreadGroupCountZ(ThreadGroupCountZ),
		indirect_buffer(indirect_buffer),
		args_offset(args_offset),
		skip(false),
		hunting_skip(false)
	{}
};

