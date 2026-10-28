#pragma once

#include <stdint.h>

#include <string>

#include "SpirvPatch.h"

// Shaders of a Vulkan game as the ShaderRegex sections with SpirvPattern
// patched them, kept in cache_directory with vk_cache_patched_shaders, so that
// the next start uses them as they are. A file is of one shader, stage and
// entry point, and is used only if all of what it was patched with is as it
// is now, which includes this DLL and SPIRV-Tools; otherwise the shader is
// patched again and the file written over.

struct SpirvPatchCacheKey {
	// The hash the game's shader goes by, and of all of its code
	uint64_t shader, code;
	int stage;
	std::string entry;
	// SpirvPatchGroupsKey
	uint64_t groups;
	// What the shader declares were it a D3D11 one, as the patterns are
	// given it, and how many IniParams there are
	uint64_t declarations;
	unsigned ini_params;
};

uint64_t SpirvPatchCacheHash(const void *data, size_t bytes, uint64_t seed = 0xcbf29ce484222325);

bool SpirvPatchCacheEnabled();

// PATCHED or NO_MATCH. groups: the sections that patched it, for the log.
bool SpirvPatchCacheLoad(const SpirvPatchCacheKey &key, SpirvPatchOutcome *outcome, SpirvPatched *patched, std::string *groups);
// Not on the thread that draws: writes a file
void SpirvPatchCacheSave(const SpirvPatchCacheKey &key, SpirvPatchOutcome outcome, const SpirvPatched &patched, const std::string &groups);
// For one that loaded but gave no shader module or pipeline. The patch worker
// rejects the path for this process even if deleting its file fails.
void SpirvPatchCacheDrop(const SpirvPatchCacheKey &key);
