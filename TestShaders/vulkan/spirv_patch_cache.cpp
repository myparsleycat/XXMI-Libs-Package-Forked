#include <cstdio>
#include <cstdlib>

#include "spirv_patch_cache_fixture.h"
#include "DirectX11/vk/SpirvPatchCache.h"

static CacheTestGlobals settings = {};
CacheTestGlobals *G = &settings;
HINSTANCE migoto_handle;
const char *const dx12_stage_names[DX12_STAGE_COUNT] = { "vs", "ps", "gs", "hs", "ds", "cs" };

uint64_t SpirvToolsIdentity() { return 0x12345678; }
void VkDumpNote(const char *, ...) {}

static void require(bool condition, const char *message)
{
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s (Win32 error %lu)\n", message, GetLastError());
		std::exit(1);
	}
}

static void require_rejected(const SpirvPatchCacheKey &key)
{
	SpirvPatchOutcome outcome;
	SpirvPatched patched;
	std::string groups;
	if (SpirvPatchCacheLoad(key, &outcome, &patched, &groups)) {
		std::fputs("FAIL: Drop must reject a cache entry when DeleteFileW is denied\n", stderr);
		std::exit(2);
	}
}

static void require_round_trip(const SpirvPatchCacheKey &key, const SpirvPatched &expected)
{
	SpirvPatchOutcome outcome = SpirvPatchOutcome::FAILED;
	SpirvPatched patched;
	std::string groups;
	SpirvPatchCacheSave(key, SpirvPatchOutcome::PATCHED, expected, "fixture-group");
	require(SpirvPatchCacheLoad(key, &outcome, &patched, &groups), "saved cache must load");
	require(outcome == SpirvPatchOutcome::PATCHED && groups == "fixture-group",
		"outcome and group names must round-trip");
	require(patched.code == expected.code && patched.set_words == expected.set_words,
		"code and descriptor-set offsets must round-trip");
	require(patched.declarations.size() == 1
		&& patched.declarations[0].kind == expected.declarations[0].kind
		&& patched.declarations[0].reg == expected.declarations[0].reg,
		"declarations must round-trip");
}

static void invalid_cache_path(const wchar_t *, const wchar_t *, const wchar_t *, unsigned, uintptr_t)
{
	require(false, "long cache paths must not invoke the invalid-parameter handler");
}

static void require_long_paths(const SpirvPatchCacheKey &key, const SpirvPatched &expected, const wchar_t *cached_path)
{
	std::wstring original = settings.SHADER_CACHE_PATH;
	std::wstring directory = original + L"\\";
	directory.append(216 - directory.size(), L'x');
	std::wstring path = directory + wcsrchr(cached_path, L'\\');
	require(path.size() < MAX_PATH && path.size() + wcslen(L".1.1.tmp") >= MAX_PATH,
		"final cache path must fit while even the shortest temporary suffix overflows");
	require(CreateDirectoryW(directory.c_str(), NULL) != 0, "create long cache directory");
	require(CopyFileW(cached_path, path.c_str(), TRUE) != 0, "copy existing cache to long directory");
	require(wcscpy_s(settings.SHADER_CACHE_PATH, directory.c_str()) == 0, "set long cache directory");
	auto previous = _set_invalid_parameter_handler(invalid_cache_path);
	SpirvPatchCacheSave(key, SpirvPatchOutcome::NO_MATCH, SpirvPatched(), "replacement");
	SpirvPatchOutcome outcome;
	SpirvPatched patched;
	std::string groups;
	require(SpirvPatchCacheLoad(key, &outcome, &patched, &groups), "long cache path must remain readable");
	require(outcome == SpirvPatchOutcome::PATCHED && patched.code == expected.code && groups == "fixture-group",
		"skipping an oversized temporary path must preserve the existing cache");
	SpirvPatchCacheDrop(key);
	require(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES, "long cache path must remain deletable");
	require(RemoveDirectoryW(directory.c_str()) != 0, "skipped save must leave no temporary files");
	std::puts("PASS: oversized temporary path preserves the existing cache without terminating");

	directory.append(MAX_PATH - 1 - directory.size(), L'x');
	require(wcscpy_s(settings.SHADER_CACHE_PATH, directory.c_str()) == 0, "set oversized final cache path");
	require(!SpirvPatchCacheLoad(key, &outcome, &patched, &groups), "oversized final cache path must not load");
	SpirvPatchCacheSave(key, SpirvPatchOutcome::PATCHED, expected, "fixture-group");
	SpirvPatchCacheDrop(key);
	_set_invalid_parameter_handler(previous);
	require(wcscpy_s(settings.SHADER_CACHE_PATH, original.c_str()) == 0, "restore cache directory");
	require(SpirvPatchCacheLoad(key, &outcome, &patched, &groups), "normal cache must remain usable after long paths");
	std::puts("PASS: oversized final paths are rejected by Load, Save and Drop without terminating");
}

int wmain(int argc, wchar_t **argv)
{
	require(argc == 2, "supply a new isolated cache directory");
	require(wcslen(argv[1]) < MAX_PATH - 80, "cache directory must fit Win32 cache paths");
	require(CreateDirectoryW(argv[1], NULL) != 0, "cache directory must be new");
	require(wcscpy_s(settings.SHADER_CACHE_PATH, argv[1]) == 0, "copy cache directory");
	settings.vk_cache_patched_shaders = true;
	settings.IniParamsReg = 120;
	migoto_handle = GetModuleHandleW(NULL);
	require(migoto_handle != NULL && SpirvPatchCacheEnabled(), "initialize isolated settings");

	SpirvPatchCacheKey key = {};
	key.shader = 0x123456789abcdef0;
	key.code = 42;
	key.stage = DX12_STAGE_PS;
	key.entry = "main";
	key.groups = 17;
	key.declarations = 23;
	key.ini_params = 256;
	SpirvPatched expected;
	expected.code = { 0x07230203, 0x00010000, 0, 5, 0 };
	expected.set_words = { 3 };
	expected.declarations = { { SpirvDeclarationKind::TEXTURE2D, 116 } };
	require_round_trip(key, expected);
	std::puts("PASS: cache Save/Load round-trip");

	wchar_t pattern[MAX_PATH], path[MAX_PATH];
	require(swprintf_s(pattern, L"%ls\\*_spirv.bin", argv[1]) > 0, "form cache search path");
	WIN32_FIND_DATAW found;
	HANDLE search = FindFirstFileW(pattern, &found);
	require(search != INVALID_HANDLE_VALUE, "find the file written by Save");
	require(swprintf_s(path, L"%ls\\%ls", argv[1], found.cFileName) > 0, "form cache file path");
	require(!FindNextFileW(search, &found) && GetLastError() == ERROR_NO_MORE_FILES,
		"isolated directory must contain exactly one cache entry");
	FindClose(search);
	require_long_paths(key, expected, path);

	// Deny deletion through real Windows sharing rules while allowing cache reads.
	HANDLE blocker = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	require(blocker != INVALID_HANDLE_VALUE, "open cache without FILE_SHARE_DELETE");
	require(!DeleteFileW(path) && GetLastError() == ERROR_SHARING_VIOLATION,
		"real cache deletion must fail with a sharing violation");
	SpirvPatchOutcome outcome;
	SpirvPatched patched;
	std::string groups;
	require(SpirvPatchCacheLoad(key, &outcome, &patched, &groups), "deletion blocker must still allow cache reads");
	std::puts("PASS: real deletion is denied while cache reads remain usable");
	SpirvPatchCacheDrop(key);
	require(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, "Drop must leave the blocked file present");
	require_rejected(key);
	require(CloseHandle(blocker) != 0, "release deletion blocker");
	require(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, "file must remain after releasing blocker");
	require_rejected(key);
	std::puts("PASS: Drop rejects the remaining file before and after releasing the deletion blocker");
	require(DeleteFileW(path) != 0, "remove rejected fixture before saving again");
	SpirvPatchCacheSave(key, SpirvPatchOutcome::PATCHED, expected, "fixture-group");
	require(GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES, "Save must recreate the rejected cache file");
	require_rejected(key);
	std::puts("PASS: successful Save does not re-enable the rejected path in this process");

	SpirvPatchCacheKey other = key;
	other.shader++;
	require_round_trip(other, expected);
	std::puts("PASS: rejecting one path leaves another cache entry usable");
	require(DeleteFileW(path) != 0, "remove blocked fixture after verification");
	SpirvPatchCacheDrop(other);
	require(!SpirvPatchCacheLoad(other, &outcome, &patched, &groups), "successful Drop must remove the other entry");
	require(RemoveDirectoryW(argv[1]) != 0, "isolated cache directory must be empty");
	std::puts("PASS: SPIR-V patch cache deletion-denied regression");
	return 0;
}
