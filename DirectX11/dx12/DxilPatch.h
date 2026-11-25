#pragma once

#include <string>
#include <vector>

#include "PipelineRegistry.h"
#include "../ShaderRegex.h"

enum class DxilPatchOutcome {
	NO_MATCH,
	MATCH,
	PATCHED,
	FAILED,
};

struct DxilPatched {
	std::vector<BYTE> bytecode;
	UINT64 srv[2] = {};
	BYTE srv_dimension[128] = {};
	UINT16 srv_buffer[128] = {};
	UINT64 uav = 0;
	UINT samplers = 0;
	std::vector<const ShaderRegexGroup*> groups;
};

DxilPatchOutcome DxilPatchShader(const Dx12ShaderInfo *shader, const std::vector<const ShaderRegexGroup*> &groups,
		DxilPatched *patched, std::string *why);

// The patched shader for a game draw, if a matching DXIL group made one.
// With link_groups, its successful groups also add their command lists and
// filter_index to the original shader's ShaderOverride. added_srv gets the
// SRV registers t0-t127 the patch declared that the original shader has not.
const Dx12ShaderInfo* DxilGameShader(const Dx12ShaderInfo *shader, bool link_groups, const char **why,
		const UINT64 **added_srv = NULL);
// Frees the patches of earlier configurations. With the shim lock held and
// no draw scope open, which is all that holds them.
void DxilForgetOldPatches();
