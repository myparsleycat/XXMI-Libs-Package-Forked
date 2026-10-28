#pragma once

#include <windows.h>
#include <stdint.h>

// Only the cache module's external dependencies are substituted. Its source,
// public header, SpirvPatch.h and SpirvTools.h are copied without changes.
enum class SpirvDeclarationKind {
	TEXTURE2D,
	INI_PARAMS,
	SAMPLER,
};

struct SpirvDeclaration {
	SpirvDeclarationKind kind;
	unsigned reg;
};

class ShaderRegexGroup;

enum Dx12ShaderStage {
	DX12_STAGE_VS,
	DX12_STAGE_PS,
	DX12_STAGE_GS,
	DX12_STAGE_HS,
	DX12_STAGE_DS,
	DX12_STAGE_CS,
	DX12_STAGE_COUNT
};

extern const char *const dx12_stage_names[DX12_STAGE_COUNT];

struct CacheTestGlobals {
	wchar_t SHADER_CACHE_PATH[MAX_PATH];
	bool vk_cache_patched_shaders;
	int IniParamsReg;
};

extern CacheTestGlobals *G;
extern HINSTANCE migoto_handle;

#define VER_FILE_VERSION_STR "cache-regression-fixture"
#define VER_COMPANYNAME_STR "cache-regression-fixture"

void VkDumpNote(const char *format, ...);
