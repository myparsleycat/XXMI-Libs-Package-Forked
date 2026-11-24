#include "MipCache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <unordered_set>

#include "MipGen.h"
#include "ResourceLoading.h"
#include "log.h"
#include "util.h"
#include "globals.h"
#include "profiling.h"

namespace {

const uint32_t MAGIC = 'XMIP';
const uint32_t FORMAT = 1;
const size_t MAX_SOURCE_BYTES = (size_t)1 << 30;
const auto WORKER_IDLE = std::chrono::seconds(30);

const MipGenSettings settings;

struct Header {
	uint32_t magic, format;
	uint32_t encoder;
	uint32_t source_crc;
	uint64_t source_size;
	uint32_t width, height, dxgi_format, srgb;
	// Lower levels stored; 0 records that the generator cannot make them
	uint32_t levels;
	uint32_t data_crc;
	uint64_t data_bytes;
};

struct Job {
	std::wstring path;
	// The mips folder as it was when queued; a config reload may change it
	std::wstring folder;
	uint32_t crc = 0;
	uint64_t size = 0;
	bool srgb = false;
	// Only the path is known: contents and need are found out by the worker
	bool from_last_session = false;
};

std::mutex queue_lock;
std::condition_variable queue_wake;
std::deque<Job> jobs;
// Every job of the session, so that none is made twice
std::unordered_set<std::wstring> seen;
// Files a load has looked up: their job from the last session's list is not needed
std::unordered_set<std::wstring> loaded_paths;
unsigned workers;
std::atomic<bool> stopping;
HMODULE worker_module;
std::once_flag tidy_once;

uint32_t read32(const uint8_t *data, size_t at)
{
	uint32_t value;
	memcpy(&value, data + at, 4);
	return value;
}

// data holds at least the header; file_size is the whole file's
bool parse_header(const uint8_t *data, size_t size, uint64_t file_size, DdsTexture2D *info)
{
	const uint32_t DDSD_DEPTH = 0x800000, DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_VOLUME = 0x200000;
	const uint32_t DDPF_FOURCC = 0x4, DDPF_RGB = 0x40;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	size_t offset = 128;

	if (size < 128 || memcmp(data, "DDS ", 4))
		return false;

	uint32_t flags = read32(data, 8), height = read32(data, 12), width = read32(data, 16);
	uint32_t mips = read32(data, 28), pf_flags = read32(data, 80), fourcc = read32(data, 84);
	uint32_t bits = read32(data, 88), r = read32(data, 92), g = read32(data, 96), b = read32(data, 100);
	uint32_t a = read32(data, 104), caps2 = read32(data, 112);

	if ((flags & DDSD_DEPTH) || (caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME)))
		return false;

	if ((pf_flags & DDPF_FOURCC) && fourcc == MAKEFOURCC('D', 'X', '1', '0')) {
		const uint32_t TEXTURE2D = 3, TEXTURECUBE = 0x4;
		if (size < 148)
			return false;
		format = (DXGI_FORMAT)read32(data, 128);
		if (read32(data, 132) != TEXTURE2D || (read32(data, 136) & TEXTURECUBE) || read32(data, 140) != 1)
			return false;
		offset = 148;
	} else if (pf_flags & DDPF_FOURCC) {
		if (fourcc == MAKEFOURCC('D', 'X', 'T', '1'))
			format = DXGI_FORMAT_BC1_UNORM;
		else if (fourcc == MAKEFOURCC('D', 'X', 'T', '5'))
			format = DXGI_FORMAT_BC3_UNORM;
		else if (fourcc == MAKEFOURCC('A', 'T', 'I', '1') || fourcc == MAKEFOURCC('B', 'C', '4', 'U'))
			format = DXGI_FORMAT_BC4_UNORM;
		else if (fourcc == MAKEFOURCC('A', 'T', 'I', '2') || fourcc == MAKEFOURCC('B', 'C', '5', 'U'))
			format = DXGI_FORMAT_BC5_UNORM;
	} else if ((pf_flags & DDPF_RGB) && bits == 32) {
		if (r == 0xff && g == 0xff00 && b == 0xff0000 && a == 0xff000000)
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
		else if (r == 0xff0000 && g == 0xff00 && b == 0xff && a == 0xff000000)
			format = DXGI_FORMAT_B8G8R8A8_UNORM;
	}

	if (!MipGenSupportsFormat(format) || !width || !height)
		return false;
	if (file_size < offset + MipGenLevelBytes(format, width, height))
		return false;

	info->width = width;
	info->height = height;
	info->mips = mips ? mips : 1;
	info->format = format;
	info->data_offset = offset;
	return true;
}

size_t lower_level_bytes(const DdsTexture2D &info)
{
	uint32_t levels = MipGenFullChainLevels(info.width, info.height);
	size_t bytes = 0;

	for (uint32_t level = 1; level < levels; level++) {
		uint32_t w = info.width >> level, h = info.height >> level;
		bytes += MipGenLevelBytes(info.format, w ? w : 1, h ? h : 1);
	}
	return bytes;
}

std::wstring current_folder()
{
	return std::wstring(G->SHADER_CACHE_PATH) + L"\\mips";
}

bool path_of(const std::wstring &folder, uint32_t crc, uint64_t size, bool srgb, wchar_t *path)
{
	return _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%ls\\%08x-%016llx-%ls_mips.bin",
			folder.c_str(), crc, size, srgb ? L"srgb" : L"linear") > 0;
}

bool read_exactly(HANDLE file, void *data, size_t bytes)
{
	uint8_t *at = (uint8_t*)data;
	DWORD read;

	while (bytes) {
		DWORD chunk = (DWORD)min(bytes, (size_t)64 << 20);
		if (!ReadFile(file, at, chunk, &read, NULL) || read != chunk)
			return false;
		at += chunk;
		bytes -= chunk;
	}
	return true;
}

bool header_matches(const Header &header, uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb)
{
	return header.encoder == MipGenEncoderVersion(settings) && header.source_crc == crc
		&& header.source_size == size && header.width == info.width && header.height == info.height
		&& header.dxgi_format == (uint32_t)info.format && header.srgb == (uint32_t)srgb;
}

// Of the right size for its own fields; contents not checked
bool header_sound(const Header &header, uint64_t file_size)
{
	return header.magic == MAGIC && header.format == FORMAT && header.levels < 32
		&& header.data_bytes == file_size - sizeof(Header);
}

// What the file for these contents holds, from its header alone
MipCacheLookup probe(const wchar_t *path, uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb)
{
	LARGE_INTEGER file_size;
	Header header;
	MipCacheLookup lookup = MipCacheLookup::MISS;

	HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return lookup;
	if (GetFileSizeEx(file, &file_size) && (uint64_t)file_size.QuadPart >= sizeof(Header)
	    && read_exactly(file, &header, sizeof(header))
	    && header_sound(header, file_size.QuadPart) && header_matches(header, crc, size, info, srgb))
		lookup = header.levels ? MipCacheLookup::HIT : MipCacheLookup::NEVER;
	CloseHandle(file);
	return lookup;
}

void save(const std::wstring &folder, const wchar_t *path, const Header &header, const std::vector<uint8_t> &levels)
{
	wchar_t temp[MAX_PATH];
	DWORD written;
	bool done;

	// Whole or not at all, also with another process writing the same one
	if (_snwprintf_s(temp, MAX_PATH, _TRUNCATE, L"%ls.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId()) <= 0)
		return;
	HANDLE file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	// The folder may have been removed, or be another since a config reload
	if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND) {
		CreateDirectoryEnsuringAccess(folder.c_str());
		file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	if (file == INVALID_HANDLE_VALUE)
		return;
	done = WriteFile(file, &header, sizeof(header), &written, NULL) && written == sizeof(header);
	for (size_t at = 0; done && at < levels.size(); at += written) {
		DWORD chunk = (DWORD)min(levels.size() - at, (size_t)64 << 20);
		done = WriteFile(file, levels.data() + at, chunk, &written, NULL) && written == chunk;
	}
	CloseHandle(file);
	if (!done || !MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING))
		DeleteFileW(temp);
}

struct CacheFile {
	std::wstring path;
	uint64_t bytes;
	uint64_t used;
};

// Deletes the files used longest ago once the folder holds more than
// generate_texture_mips_cache_mb, down to 90% of it. Temporary files a
// writer left behind more than an hour ago go as well.
void trim_cache(const std::wstring &folder)
{
	std::vector<CacheFile> files;
	WIN32_FIND_DATAW found;
	uint64_t total = 0, limit = (uint64_t)G->generate_texture_mips_cache_mb << 20;
	FILETIME now_time;

	GetSystemTimeAsFileTime(&now_time);
	uint64_t now = ((uint64_t)now_time.dwHighDateTime << 32) | now_time.dwLowDateTime;
	const uint64_t HOUR = 36000000000ull;

	HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do {
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		std::wstring name = found.cFileName;
		std::wstring path = folder + L"\\" + name;
		uint64_t written = ((uint64_t)found.ftLastWriteTime.dwHighDateTime << 32) | found.ftLastWriteTime.dwLowDateTime;
		uint64_t bytes = ((uint64_t)found.nFileSizeHigh << 32) | found.nFileSizeLow;
		if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".tmp")) {
			if (now > written && now - written > HOUR)
				DeleteFileW(path.c_str());
		} else if (name.size() > 9 && !_wcsicmp(name.c_str() + name.size() - 9, L"_mips.bin")) {
			files.push_back({ path, bytes, written });
			total += bytes;
		}
	} while (FindNextFileW(find, &found));
	FindClose(find);

	if (!limit || total <= limit)
		return;
	std::sort(files.begin(), files.end(), [](const CacheFile &a, const CacheFile &b) { return a.used < b.used; });
	for (const CacheFile &file : files) {
		if (total <= limit / 10 * 9)
			break;
		if (DeleteFileW(file.path.c_str()))
			total -= file.bytes;
	}
}

bool read_file(const std::wstring &path, std::vector<uint8_t> *data, bool header_first)
{
	LARGE_INTEGER size;
	uint8_t head[148];
	DdsTexture2D info;
	bool done = false;

	HANDLE file = CreateFileW(long_path_for_open(path).c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return false;
	if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (uint64_t)size.QuadPart > MAX_SOURCE_BYTES)
		goto out;
	// A file from last session's list is read whole only if it needs levels
	if (header_first) {
		size_t head_bytes = (size_t)min((LONGLONG)sizeof(head), size.QuadPart);
		if (!read_exactly(file, head, head_bytes) || !parse_header(head, head_bytes, size.QuadPart, &info)
		    || !MipCacheWanted(info, false))
			goto out;
		SetFilePointer(file, 0, NULL, FILE_BEGIN);
	}
	data->resize((size_t)size.QuadPart);
	done = read_exactly(file, data->data(), data->size());
out:
	CloseHandle(file);
	return done;
}

// A file of the last session's list with either variant made already needs
// nothing; the one there is read into the file cache for its load
bool made_already(const Job &job, uint32_t crc, uint64_t size, const DdsTexture2D &info)
{
	wchar_t path[MAX_PATH];

	for (bool srgb : { job.srgb, !job.srgb }) {
		if (!path_of(job.folder, crc, size, srgb, path))
			return true;
		MipCacheLookup lookup = probe(path, crc, size, info, srgb);
		if (lookup == MipCacheLookup::HIT)
			PrefetchFileIntoCache(path);
		if (lookup != MipCacheLookup::MISS)
			return true;
		if (!job.from_last_session)
			break;
	}
	return false;
}

void run(Job &job)
{
	std::vector<uint8_t> file, levels;
	wchar_t path[MAX_PATH];
	DdsTexture2D info;
	Header header = {};

	if (job.from_last_session) {
		std::lock_guard<std::mutex> lock(queue_lock);
		if (loaded_paths.count(job.path))
			return;
	}
	if (!read_file(job.path, &file, job.from_last_session))
		return;
	uint32_t crc = crc32c_hw(0, file.data(), file.size());
	// Changed since it was loaded: the next load queues it again
	if (!job.from_last_session && (file.size() != job.size || crc != job.crc))
		return;
	if (!ParseDdsTexture2D(file.data(), file.size(), &info))
		return;
	if (job.from_last_session)
		job.srgb = MipCacheIsSrgb(info.format);
	if (made_already(job, crc, file.size(), info) || !path_of(job.folder, crc, file.size(), job.srgb, path))
		return;

	ULONGLONG start = GetTickCount64();
	MipGenResult result = GenerateLowerMips(file.data() + info.data_offset, info.width, info.height,
			info.format, job.srgb, settings, &stopping, &levels);
	if (result == MipGenResult::STOPPED)
		return;

	header.magic = MAGIC;
	header.format = FORMAT;
	header.encoder = MipGenEncoderVersion(settings);
	header.source_crc = crc;
	header.source_size = file.size();
	header.width = info.width;
	header.height = info.height;
	header.dxgi_format = info.format;
	header.srgb = job.srgb;
	if (result == MipGenResult::OK) {
		header.levels = MipGenFullChainLevels(info.width, info.height) - 1;
	} else {
		header.levels = 0;
		levels.clear();
	}
	header.data_bytes = levels.size();
	header.data_crc = crc32c_hw(0, levels.data(), levels.size());
	save(job.folder, path, header, levels);

	if (header.levels) {
		LogInfoW(L"Generated %u mip levels for %ls (%ux%u, format %u) in %llu ms\n", header.levels,
				job.path.c_str(), info.width, info.height, info.format, GetTickCount64() - start);
	} else {
		LogInfoW(L"Cannot generate mip levels for %ls: %ls\n", job.path.c_str(),
				result == MipGenResult::REFUSED ? L"BC1 with transparent texels" : L"unsupported");
	}
	if (G->generate_texture_mips_cache_mb)
		trim_cache(job.folder);
}

DWORD WINAPI worker_thread(LPVOID)
{
	BeginBackgroundWork();

	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(queue_lock);
			queue_wake.wait_for(lock, WORKER_IDLE, [] { return stopping || !jobs.empty(); });
			if (stopping || jobs.empty()) {
				workers--;
				break;
			}
			job = std::move(jobs.front());
			jobs.pop_front();
		}
		std::call_once(tidy_once, [&job] {
			CreateDirectoryEnsuringAccess(job.folder.c_str());
			trim_cache(job.folder);
		});
		run(job);
	}

	EndBackgroundWork();
	// Releases the DLL reference taken when the thread was started, without
	// returning into code that release may unmap. Nothing may own memory here.
	FreeLibraryAndExitThread(worker_module, 0);
}

// A job of a load goes before those of the last session's list
bool enqueue(Job &&job, const std::wstring &key)
{
	if (!MipCacheEnabled() || stopping)
		return false;

	job.folder = current_folder();
	std::lock_guard<std::mutex> lock(queue_lock);
	if (!seen.insert(key).second)
		return false;
	if (job.from_last_session)
		jobs.push_back(std::move(job));
	else
		jobs.push_front(std::move(job));

	if (workers >= G->generate_texture_mips_threads || workers >= jobs.size()) {
		queue_wake.notify_one();
		return true;
	}
	// The thread holds a reference on this DLL until it exits, so an unload
	// cannot unmap the code it is running
	HMODULE module;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&worker_thread, &module))
		return true;
	worker_module = module;
	HANDLE thread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
	if (!thread) {
		FreeLibrary(module);
		return true;
	}
	CloseHandle(thread);
	workers++;
	return true;
}

} // namespace

bool ParseDdsTexture2D(const uint8_t *data, size_t size, DdsTexture2D *info)
{
	return parse_header(data, size, size, info);
}

bool MipCacheEnabled()
{
	return G->generate_texture_mips && G->SHADER_CACHE_PATH[0];
}

bool MipCacheWanted(const DdsTexture2D &info, bool explicit_request)
{
	if (info.mips > 1 || !MipGenSupportsFormat(info.format)
	    || info.width < 4 || info.height < 4 || info.width % 4 || info.height % 4)
		return false;
	if (explicit_request)
		return true;
	// Uncompressed textures without levels are often data, not pictures
	switch (info.format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return false;
	default:
		return max(info.width, info.height) >= G->generate_texture_mips_min_size;
	}
}

bool MipCacheIsSrgb(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

DXGI_FORMAT MipCacheSrgbFormat(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
	case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
	case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;
	case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	default: return format;
	}
}

size_t MipCacheRowPitch(DXGI_FORMAT format, uint32_t width)
{
	// One row of blocks, or of texels for the uncompressed formats
	return MipGenLevelBytes(format, width, 1);
}

MipCacheLookup MipCacheProbe(uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb_filter)
{
	wchar_t path[MAX_PATH];

	if (!path_of(current_folder(), crc, size, srgb_filter, path))
		return MipCacheLookup::MISS;
	return probe(path, crc, size, info, srgb_filter);
}

MipCacheLookup MipCacheLoad(uint32_t crc, uint64_t size, const DdsTexture2D &info, bool srgb_filter,
		std::vector<uint8_t> *levels)
{
	Profiling::Spike::Scope spike(Profiling::Spike::RESOURCE_MIP_CACHE);
	wchar_t path[MAX_PATH];
	LARGE_INTEGER file_size;
	MipCacheLookup lookup = MipCacheLookup::MISS;
	Header header;
	bool damaged = false;

	if (!path_of(current_folder(), crc, size, srgb_filter, path))
		return MipCacheLookup::MISS;
	// Write attributes to record the use for trim_cache(); sharing deletion
	// for another process to write over it meanwhile
	HANDLE file = CreateFileW(path, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_DELETE,
			NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return MipCacheLookup::MISS;

	if (!GetFileSizeEx(file, &file_size) || (uint64_t)file_size.QuadPart < sizeof(Header)
	    || !read_exactly(file, &header, sizeof(header)) || !header_sound(header, file_size.QuadPart)) {
		damaged = true;
	} else if (header_matches(header, crc, size, info, srgb_filter)) {
		// One for other settings is written over once generated again
		if (!header.levels) {
			lookup = MipCacheLookup::NEVER;
		} else if (header.levels != MipGenFullChainLevels(info.width, info.height) - 1
			   || header.data_bytes != lower_level_bytes(info)) {
			damaged = true;
		} else {
			levels->resize((size_t)header.data_bytes);
			if (read_exactly(file, levels->data(), levels->size())
			    && crc32c_hw(0, levels->data(), levels->size()) == header.data_crc) {
				lookup = MipCacheLookup::HIT;
				spike.add_bytes(levels->size());
			} else {
				levels->clear();
				damaged = true;
			}
		}
		if (lookup != MipCacheLookup::MISS) {
			FILETIME now;
			GetSystemTimeAsFileTime(&now);
			SetFileTime(file, NULL, NULL, &now);
		}
	}
	CloseHandle(file);

	if (damaged) {
		LogInfoW(L"Generated mip levels %ls are damaged and are deleted\n", wcsrchr(path, L'\\') + 1);
		DeleteFileW(path);
	}
	return lookup;
}

void MipCacheQueue(const std::wstring &path, uint32_t crc, uint64_t size, bool srgb_filter)
{
	wchar_t key[64];
	Job job;

	_snwprintf_s(key, _TRUNCATE, L"%08x-%016llx-%d", crc, size, (int)srgb_filter);
	// As the last session's list has it
	job.path = strip_long_path_prefix(path);
	job.crc = crc;
	job.size = size;
	job.srgb = srgb_filter;
	if (enqueue(std::move(job), key))
		Profiling::mip_jobs_queued++;
}

void MipCacheNoteLoaded(const std::wstring &path)
{
	if (!MipCacheEnabled())
		return;
	std::lock_guard<std::mutex> lock(queue_lock);
	loaded_paths.insert(strip_long_path_prefix(path));
}

void MipCacheQueueFromLastSession(const std::wstring &path)
{
	Job job;

	if (path.size() < 4 || _wcsicmp(path.c_str() + path.size() - 4, L".dds"))
		return;
	job.path = path;
	job.from_last_session = true;
	enqueue(std::move(job), L"path:" + path);
}

void MipCacheStop()
{
	stopping = true;
	queue_wake.notify_all();
}
