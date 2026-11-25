#include "SpirvPatchCache.h"

#include <windows.h>

#include <set>
#include <span>
#include <utility>
#include <vector>

#include "SpirvTools.h"
#include "VkDump.h"
#include "../dx12/PipelineRegistry.h"
#include "../globals.h"

namespace {

const uint32_t MAGIC = 'VPSZ';
const uint32_t FORMAT = 1;
const uint32_t SPIRV_MAGIC = 0x07230203;

// Accessed by the patch worker only. A rejected file may still exist if
// deletion failed or another process recreated it.
std::set<std::wstring> rejected_paths;

struct Header {
	uint32_t magic, format;
	// Of this DLL, SPIRV-Tools and the settings that change what the
	// patterns are given
	uint64_t environment;
	uint64_t shader, code, groups, declarations;
	uint32_t stage, ini_params;
	uint32_t outcome;
	uint32_t entry_bytes, code_words, set_words, declaration_count, groups_bytes;
	// Of everything after the header
	uint64_t checksum;
};

uint64_t environment()
{
	static uint64_t made;
	const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER*)migoto_handle;
	const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS*)((const BYTE*)migoto_handle + dos->e_lfanew);
	uint64_t hash, tools;

	if (made)
		return made;
	// The version and commit stay the same over builds of uncommitted
	// changes, the time the DLL was linked does not
	hash = SpirvPatchCacheHash(VER_FILE_VERSION_STR, sizeof(VER_FILE_VERSION_STR));
	hash = SpirvPatchCacheHash(VER_COMPANYNAME_STR, sizeof(VER_COMPANYNAME_STR), hash);
	hash = SpirvPatchCacheHash(&nt->FileHeader.TimeDateStamp, sizeof(nt->FileHeader.TimeDateStamp), hash);
	tools = SpirvToolsIdentity();
	hash = SpirvPatchCacheHash(&tools, sizeof(tools), hash);
	hash = SpirvPatchCacheHash(&G->IniParamsReg, sizeof(G->IniParamsReg), hash);
	made = hash ? hash : 1;
	return made;
}

bool path_of(const SpirvPatchCacheKey &key, wchar_t *path)
{
	return _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%016llx-%S-%08x_spirv.bin", G->SHADER_CACHE_PATH, key.shader,
		dx12_stage_names[key.stage], (uint32_t)SpirvPatchCacheHash(key.entry.data(), key.entry.size())) > 0;
}

template <typename T, size_t Extent>
void append(std::vector<BYTE> &out, std::span<T, Extent> values)
{
	if (!values.empty())
		out.append_range(std::span(reinterpret_cast<const BYTE*>(values.data()), values.size_bytes()));
}

template <typename T, size_t Extent>
bool take(std::span<const BYTE> &input, std::span<T, Extent> values)
{
	if (values.size() > input.size() / sizeof(T))
		return false;
	if (!values.empty()) {
		memcpy(values.data(), input.data(), values.size_bytes());
		input = input.subspan(values.size_bytes());
	}
	return true;
}

}

uint64_t SpirvPatchCacheHash(const void *data, size_t bytes, uint64_t seed)
{
	uint64_t hash = seed;

	for (size_t i = 0; i < bytes; i++)
		hash = (hash ^ ((const BYTE*)data)[i]) * 0x100000001b3;
	return hash;
}

bool SpirvPatchCacheEnabled()
{
	return G->vk_cache_patched_shaders && G->SHADER_CACHE_PATH[0];
}

bool SpirvPatchCacheLoad(const SpirvPatchCacheKey &key, SpirvPatchOutcome *outcome, SpirvPatched *patched, std::string *groups)
{
	std::vector<BYTE> data;
	wchar_t path[MAX_PATH];
	std::span<const BYTE> payload;
	std::string entry;
	Header header{};
	LARGE_INTEGER size;
	HANDLE file;
	DWORD read;
	bool usable = false, valid = false;

	if (!path_of(key, path) || rejected_paths.contains(path))
		return false;
	// Sharing deletion for another process to write over it meanwhile
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	if (GetFileSizeEx(file, &size) && size.QuadPart >= (LONGLONG)sizeof(Header) && size.QuadPart < 64 << 20) {
		data.resize((size_t)size.QuadPart);
		valid = ReadFile(file, data.data(), (DWORD)data.size(), &read, NULL) && read == data.size();
	}
	CloseHandle(file);

	if (valid) {
		memcpy(&header, data.data(), sizeof(header));
		payload = std::span(data).subspan(sizeof(header));
		valid = header.magic == MAGIC && header.format == FORMAT
			&& header.checksum == SpirvPatchCacheHash(payload.data(), payload.size());
	}
	// One for another configuration is written over once patched, not
	// deleted: it would be of no use either way
	valid = valid && header.entry_bytes + (uint64_t)header.code_words * 4 + (uint64_t)header.set_words * 4
		+ (uint64_t)header.declaration_count * 8 + header.groups_bytes == payload.size();
	if (valid && header.environment == environment() && header.shader == key.shader && header.code == key.code
	    && header.groups == key.groups && header.declarations == key.declarations && header.stage == (uint32_t)key.stage
	    && header.ini_params == key.ini_params) {
		SpirvPatched made;
		std::string names;

		entry.resize(header.entry_bytes);
		made.code.resize(header.code_words);
		made.set_words.resize(header.set_words);
		made.declarations.resize(header.declaration_count);
		names.resize(header.groups_bytes);
		valid = take(payload, std::span(entry)) && take(payload, std::span(made.code));
		for (size_t &word : made.set_words) {
			uint32_t taken;
			if (!valid || !take(payload, std::span(&taken, 1)) || taken >= made.code.size()) {
				valid = false;
				break;
			}
			word = taken;
		}
		for (SpirvDeclaration &declaration : made.declarations) {
			uint32_t taken[2];
			if (!valid || !take(payload, std::span(taken))) {
				valid = false;
				break;
			}
			declaration.kind = static_cast<SpirvDeclarationKind>(taken[0]);
			declaration.reg = taken[1];
		}
		valid = valid && take(payload, std::span(names)) && payload.empty();
		if (header.outcome == static_cast<uint32_t>(std::to_underlying(SpirvPatchOutcome::PATCHED)))
			valid = valid && !made.code.empty() && made.code[0] == SPIRV_MAGIC;
		else
			valid = valid && header.outcome == static_cast<uint32_t>(std::to_underlying(SpirvPatchOutcome::NO_MATCH)) && made.code.empty();
		usable = valid && entry == key.entry;
		if (usable) {
			*outcome = (SpirvPatchOutcome)header.outcome;
			*patched = std::move(made);
			*groups = std::move(names);
		}
	}
	if (!valid) {
		VkDumpNote("*** ShaderRegex: %S in the cache is damaged and is deleted\n", wcsrchr(path, L'\\') + 1);
		DeleteFileW(path);
	}
	return usable;
}

void SpirvPatchCacheSave(const SpirvPatchCacheKey &key, SpirvPatchOutcome outcome, const SpirvPatched &patched, const std::string &groups)
{
	std::vector<BYTE> data(sizeof(Header));
	wchar_t path[MAX_PATH], temp[MAX_PATH];
	Header header = {};
	HANDLE file;
	DWORD written;
	bool done;

	if (outcome == SpirvPatchOutcome::FAILED || !path_of(key, path))
		return;
	header.magic = MAGIC;
	header.format = FORMAT;
	header.environment = environment();
	header.shader = key.shader;
	header.code = key.code;
	header.groups = key.groups;
	header.declarations = key.declarations;
	header.stage = key.stage;
	header.ini_params = key.ini_params;
	header.outcome = std::to_underlying(outcome);
	header.entry_bytes = (uint32_t)key.entry.size();
	header.groups_bytes = (uint32_t)groups.size();
	append(data, std::span(key.entry));
	if (outcome == SpirvPatchOutcome::PATCHED) {
		header.code_words = (uint32_t)patched.code.size();
		header.set_words = (uint32_t)patched.set_words.size();
		header.declaration_count = (uint32_t)patched.declarations.size();
		append(data, std::span(patched.code));
		for (size_t word : patched.set_words) {
			uint32_t written_word = (uint32_t)word;
			append(data, std::span(&written_word, 1));
		}
		for (const SpirvDeclaration &declaration : patched.declarations) {
			uint32_t fields[2] = { (uint32_t)declaration.kind, declaration.reg };
			append(data, std::span(fields));
		}
	}
	append(data, std::span(groups));
	header.checksum = SpirvPatchCacheHash(data.data() + sizeof(Header), data.size() - sizeof(Header));
	memcpy(data.data(), &header, sizeof(header));

	// Whole or not at all, also with another process writing the same one
	if (_snwprintf_s(temp, MAX_PATH, _TRUNCATE, L"%ls.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId()) <= 0)
		return;
	file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	done = WriteFile(file, data.data(), (DWORD)data.size(), &written, NULL) && written == data.size();
	CloseHandle(file);
	if (!done || !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
		DeleteFileW(temp);
}

void SpirvPatchCacheDrop(const SpirvPatchCacheKey &key)
{
	wchar_t path[MAX_PATH];

	if (path_of(key, path)) {
		rejected_paths.insert(path);
		DeleteFileW(path);
	}
}
