#include "ResourceLoading.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "log.h"
#include "util.h"
#include "globals.h"
#include "profiling.h"

#pragma region FileContents

// Idle read buffers, kept for the next file. A file bigger than the largest
// kept size goes into the one large buffer, which is freed once it has gone
// unused for a while: such files come in bursts when a mod is first drawn,
// and a fresh buffer costs more in page faults than the read itself.
static std::mutex read_buffers_lock;
static std::vector<std::pair<uint8_t*, size_t>> read_buffers;
static const size_t kept_read_buffers = 2;
// A 64 MB texture is 64 MB and the DDS header
static const size_t kept_read_buffer_size = 65 << 20;
static uint8_t *large_read_buffer = nullptr;
static size_t large_read_buffer_capacity = 0;
static ULONGLONG large_read_buffer_idle_since = 0;
static std::atomic<bool> large_read_buffer_kept{false};
static const size_t kept_large_read_buffer_size = 512 << 20;
static const ULONGLONG large_read_buffer_idle_ms = 5000;

// A file of two chunks or more is read by the thread pool as well as by the
// loading thread, which waits for it anyway. Copying out of the file cache is
// bound by one core, and a cold file is read at a higher queue depth:
static const size_t read_chunk_size = 2 << 20;
static const unsigned max_read_helpers = 3;

struct SplitRead
{
	HANDLE file;
	uint8_t *buffer;
	size_t size;
	std::atomic<size_t> next{0};
	std::atomic<DWORD> error{0};
};

static void ReadChunks(SplitRead *read)
{
	HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);

	if (!event) {
		DWORD expected = 0;
		read->error.compare_exchange_strong(expected, GetLastError());
		return;
	}

	for (;;) {
		size_t at = read->next.fetch_add(read_chunk_size);
		if (at >= read->size || read->error)
			break;

		DWORD len = (DWORD)std::min<size_t>(read_chunk_size, read->size - at), got = 0, error = 0;
		OVERLAPPED overlapped = {};
		overlapped.Offset = (DWORD)at;
		overlapped.OffsetHigh = (DWORD)((uint64_t)at >> 32);
		overlapped.hEvent = event;

		BOOL ok = ReadFile(read->file, read->buffer + at, len, &got, &overlapped);
		if (!ok && GetLastError() == ERROR_IO_PENDING)
			ok = GetOverlappedResult(read->file, &overlapped, &got, TRUE);
		if (!ok)
			error = GetLastError();
		else if (got != len)
			error = ERROR_HANDLE_EOF;
		if (error) {
			DWORD expected = 0;
			read->error.compare_exchange_strong(expected, error);
			break;
		}
	}
	CloseHandle(event);
}

static void CALLBACK ReadChunksCallback(PTP_CALLBACK_INSTANCE instance, void *context, PTP_WORK work)
{
	ReadChunks((SplitRead*)context);
}

static unsigned ReadHelpers()
{
	static unsigned helpers = std::min<unsigned>(max_read_helpers, std::max<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1) - 1);
	return helpers;
}

// Returns 0 or the error of the first chunk that could not be read
static DWORD ReadWhole(HANDLE file, uint8_t *buffer, size_t size)
{
	SplitRead read;
	PTP_WORK work = NULL;
	// Below two chunks a helper would cost more than it reads
	unsigned helpers = (unsigned)std::min<size_t>(ReadHelpers(), std::max<size_t>(size / read_chunk_size, 1) - 1);

	read.file = file;
	read.buffer = buffer;
	read.size = size;

	if (helpers)
		work = CreateThreadpoolWork(ReadChunksCallback, &read, NULL);
	for (unsigned i = 0; work && i < helpers; i++)
		SubmitThreadpoolWork(work);

	ReadChunks(&read);

	if (work) {
		// Helpers still reading are waited for. One that has not
		// started is cancelled: nothing is left for it to read.
		WaitForThreadpoolWorkCallbacks(work, TRUE);
		CloseThreadpoolWork(work);
	}
	return read.error;
}

void ReleaseIdleReadBuffers()
{
	uint8_t *released = nullptr;

	if (!large_read_buffer_kept.load(std::memory_order_relaxed))
		return;

	{
		std::lock_guard<std::mutex> lock(read_buffers_lock);
		if (!large_read_buffer || GetTickCount64() - large_read_buffer_idle_since < large_read_buffer_idle_ms)
			return;
		released = large_read_buffer;
		large_read_buffer = nullptr;
		large_read_buffer_capacity = 0;
		large_read_buffer_kept = false;
	}
	VirtualFree(released, 0, MEM_RELEASE);
}

FileContents::FileContents(const wchar_t *path)
{
	LARGE_INTEGER file_size;
	HANDLE file;
	size_t want;

	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		error = GetLastError();
		return;
	}

	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0
			|| (uint64_t)file_size.QuadPart > (uint64_t)SIZE_MAX) {
		error = GetLastError();
		if (!error)
			error = ERROR_FILE_INVALID;
		CloseHandle(file);
		return;
	}
	want = (size_t)file_size.QuadPart;

	if (want <= kept_read_buffer_size) {
		std::lock_guard<std::mutex> lock(read_buffers_lock);
		auto best = read_buffers.end();
		for (auto i = read_buffers.begin(); i != read_buffers.end(); i++) {
			if (best == read_buffers.end() || i->second > best->second)
				best = i;
		}
		if (best != read_buffers.end()) {
			buffer = best->first;
			capacity = best->second;
			read_buffers.erase(best);
		}
	} else {
		std::lock_guard<std::mutex> lock(read_buffers_lock);
		if (large_read_buffer && large_read_buffer_capacity >= want) {
			buffer = large_read_buffer;
			capacity = large_read_buffer_capacity;
			large_read_buffer = nullptr;
			large_read_buffer_capacity = 0;
			large_read_buffer_kept = false;
		}
	}
	if (capacity < want) {
		if (buffer)
			VirtualFree(buffer, 0, MEM_RELEASE);
		capacity = (want + 0xfffff) & ~(size_t)0xfffff;
		buffer = (uint8_t*)VirtualAlloc(NULL, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!buffer) {
			capacity = 0;
			error = ERROR_NOT_ENOUGH_MEMORY;
			CloseHandle(file);
			return;
		}
	}

	{
		Profiling::Spike::Scope spike(Profiling::Spike::RESOURCE_FILE_READ, path, want);
		error = ReadWhole(file, buffer, want);
	}
	CloseHandle(file);
	if (error)
		return;

	data = buffer;
	size = want;
}

FileContents::~FileContents()
{
	if (!buffer)
		return;

	if (capacity <= kept_read_buffer_size) {
		std::lock_guard<std::mutex> lock(read_buffers_lock);
		if (read_buffers.size() < kept_read_buffers) {
			read_buffers.push_back({ buffer, capacity });
			return;
		}
	} else if (capacity <= kept_large_read_buffer_size) {
		std::lock_guard<std::mutex> lock(read_buffers_lock);
		// Of two, the bigger one is kept
		if (large_read_buffer_capacity < capacity) {
			std::swap(buffer, large_read_buffer);
			std::swap(capacity, large_read_buffer_capacity);
		}
		large_read_buffer_idle_since = GetTickCount64();
		large_read_buffer_kept = true;
	}
	if (buffer)
		VirtualFree(buffer, 0, MEM_RELEASE);
}

#pragma endregion FileContents

#pragma region MappedFile

struct MemoryRangeEntry {
	PVOID VirtualAddress;
	SIZE_T NumberOfBytes;
};
typedef BOOL (WINAPI *PrefetchVirtualMemory_t)(HANDLE, ULONG_PTR, MemoryRangeEntry*, ULONG);

// Windows 8+; resolved at runtime so the DLL still loads on Windows 7,
// where the prefetch thread reads the files through a scratch buffer:
static PrefetchVirtualMemory_t GetPrefetchVirtualMemory()
{
	static PrefetchVirtualMemory_t fn = (PrefetchVirtualMemory_t)
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory");
	return fn;
}

typedef BOOL (WINAPI *SetThreadInformation_t)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);

// Windows 8+ as well
static SetThreadInformation_t GetSetThreadInformation()
{
	static SetThreadInformation_t fn = (SetThreadInformation_t)
		GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation");
	return fn;
}

// Read-only view of a whole file, only there so that prefetch() can ask the
// OS to read the file into its file cache at the thread's low I/O priority
// without copying it anywhere. That is a hint: the OS may read only part of
// the file, or nothing.
class MappedFile
{
public:
	explicit MappedFile(const wchar_t *path);
	~MappedFile();

	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;

	bool valid() const { return data != nullptr; }
	void prefetch() const;

	const uint8_t *data = nullptr;
	size_t size = 0;

private:
	HANDLE file = INVALID_HANDLE_VALUE;
	HANDLE mapping = NULL;
};

MappedFile::MappedFile(const wchar_t *path)
{
	LARGE_INTEGER file_size;

	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;

	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0
			|| (uint64_t)file_size.QuadPart > (uint64_t)SIZE_MAX)
		return;

	mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
	if (!mapping)
		return;

	data = (const uint8_t*)MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (!data)
		return;

	size = (size_t)file_size.QuadPart;
}

MappedFile::~MappedFile()
{
	if (data)
		UnmapViewOfFile(data);
	if (mapping)
		CloseHandle(mapping);
	if (file != INVALID_HANDLE_VALUE)
		CloseHandle(file);
}

void MappedFile::prefetch() const
{
	PrefetchVirtualMemory_t fn = GetPrefetchVirtualMemory();
	MemoryRangeEntry range;

	if (!fn || !data)
		return;

	range.VirtualAddress = (PVOID)data;
	range.NumberOfBytes = size;
	fn(GetCurrentProcess(), 1, &range, 0);
}

#pragma endregion MappedFile

#pragma region SharedFileResources

bool SharedResourceKey::operator==(const SharedResourceKey &other) const
{
	return device == other.device
		&& loader == other.loader
		&& bind_flags == other.bind_flags
		&& misc_flags == other.misc_flags
		&& load_flags == other.load_flags
		&& byte_width == other.byte_width
		&& structure_stride == other.structure_stride
		&& size == other.size
		&& content == other.content;
}

struct SharedEntry
{
	SharedResourceKey key;
	std::wstring path;
	ID3D11Resource *resource;
};

static std::mutex shared_resources_lock;
// Bucketed by file size:
static std::unordered_map<uint64_t, std::vector<SharedEntry>> shared_resources;

static bool FileContentsEqual(const std::wstring &path, const void *data, size_t size)
{
	FileContents file(path.c_str());

	return file.valid() && file.size == size && !memcmp(file.data, data, size);
}

ID3D11Resource* FindSharedFileResource(SharedResourceKey &key, const void *data, size_t size)
{
	std::vector<SharedEntry> candidates;
	ID3D11Resource *match = nullptr;
	Profiling::Spike::Scope spike(Profiling::Spike::RESOURCE_FILE_SHARE, L"bytes", size);

	// Hashed from the data the caller is about to upload, never from the
	// file at some later point, so an entry always describes what its
	// resource was created from even if the file changes afterwards:
	key.size = size;
	key.content = crc32c_hw(0, data, size);

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		auto bucket = shared_resources.find(key.size);
		if (bucket == shared_resources.end())
			return nullptr;

		for (const SharedEntry &entry : bucket->second) {
			if (entry.key == key) {
				entry.resource->AddRef();
				candidates.push_back(entry);
			}
		}
	}

	// The hash only selects candidates. A candidate is shared once its
	// file still compares equal byte for byte; one whose file has since
	// changed or disappeared is simply not shared with:
	for (const SharedEntry &entry : candidates) {
		if (!match && FileContentsEqual(entry.path, data, size))
			match = entry.resource;
		else
			entry.resource->Release();
	}

	return match;
}

void ShareFileResource(const SharedResourceKey &key, const std::wstring &path, ID3D11Resource *resource)
{
	std::lock_guard<std::mutex> lock(shared_resources_lock);

	resource->AddRef();
	shared_resources[key.size].push_back(SharedEntry{key, path, resource});
}

void ReleaseSharedFileResources(ID3D11Device *device)
{
	std::vector<ID3D11Resource*> released;

	{
		std::lock_guard<std::mutex> lock(shared_resources_lock);

		for (auto bucket = shared_resources.begin(); bucket != shared_resources.end(); ) {
			std::vector<SharedEntry> &entries = bucket->second;
			for (auto entry = entries.begin(); entry != entries.end(); ) {
				if (device && entry->key.device != device) {
					entry++;
					continue;
				}
				released.push_back(entry->resource);
				entry = entries.erase(entry);
			}
			if (entries.empty())
				bucket = shared_resources.erase(bucket);
			else
				bucket++;
		}
	}

	// Released outside the lock: the final Release() calls into the driver.
	for (ID3D11Resource *resource : released)
		resource->Release();
}

#pragma endregion SharedFileResources

#pragma region ResourceFilePrefetch

// Entries not used for this many consecutive sessions are dropped:
static const unsigned max_unused_sessions = 3;
static const wchar_t *prefetch_log_name = L"d3dx_resource_prefetch.txt";

// Path and the number of sessions since it was last used:
typedef std::vector<std::pair<std::wstring, unsigned>> ResourceFileLog;

static std::mutex file_log_lock;
// Kept open for the session. Every newly used file is appended as it is
// loaded, which is a small buffered write next to the read of the file
// itself and leaves nothing to save when the game exits or is killed:
static HANDLE file_log = INVALID_HANDLE_VALUE;
static std::unordered_set<std::wstring> recorded_files;

// Reference that keeps this DLL loaded while the prefetch thread runs:
static HMODULE prefetch_module = NULL;
static std::atomic<bool> prefetch_stop{false};

static std::wstring PrefetchLogPath()
{
	wchar_t path[MAX_PATH];

	GetModuleFileNameW(migoto_handle, path, MAX_PATH);
	wcsrchr(path, L'\\')[1] = 0;
	return std::wstring(path) + prefetch_log_name;
}

static std::wstring FromUTF8(const char *str, int len)
{
	int wlen = MultiByteToWideChar(CP_UTF8, 0, str, len, NULL, 0);
	std::wstring ret(wlen, 0);
	MultiByteToWideChar(CP_UTF8, 0, str, len, &ret[0], wlen);
	return ret;
}

static bool AppendToFileLog(const std::wstring &path, unsigned age)
{
	std::string line = std::to_string(age) + "\t" + to_utf8(path) + "\n";
	LARGE_INTEGER start, zero = {};
	DWORD written = 0, error = ERROR_WRITE_FAULT;

	if (!SetFilePointerEx(file_log, zero, &start, FILE_CURRENT))
		return false;

	if (!WriteFile(file_log, line.data(), (DWORD)line.size(), &written, NULL))
		error = GetLastError();
	else if (written == line.size())
		return true;

	// Part of the line may have been written, which a later line would
	// otherwise be joined onto:
	if (SetFilePointerEx(file_log, start, NULL, FILE_BEGIN))
		SetEndOfFile(file_log);

	LogInfo("Failed to write %S: wrote %u of %u bytes, error %d\n",
			prefetch_log_name, written, (unsigned)line.size(), error);
	return false;
}

// Each line is "<sessions since last use>\t<path>". A file used again is
// appended a second time, so the lowest count of a path is the one that
// applies. Returns the most recently used files first, in the order they
// were needed:
static ResourceFileLog LoadResourceFileLog()
{
	std::wstring path = PrefetchLogPath();
	FileContents file(path.c_str());
	std::unordered_set<std::wstring> seen;
	ResourceFileLog lines, log;
	const char *p, *end, *line, *tab;

	if (!file.valid())
		return log;

	p = (const char*)file.data;
	end = p + file.size;
	while (p < end) {
		line = p;
		while (p < end && *p != '\n')
			p++;
		const char *line_end = p;
		if (p < end)
			p++;
		if (line_end > line && line_end[-1] == '\r')
			line_end--;

		tab = (const char*)memchr(line, '\t', line_end - line);
		if (!tab || tab + 1 >= line_end)
			continue;

		unsigned age = (unsigned)strtoul(line, NULL, 10);
		lines.emplace_back(FromUTF8(tab + 1, (int)(line_end - tab - 1)), age);
	}

	std::stable_sort(lines.begin(), lines.end(),
			[](const ResourceFileLog::value_type &a, const ResourceFileLog::value_type &b) {
				return a.second < b.second;
			});
	for (auto &entry : lines) {
		if (seen.insert(entry.first).second)
			log.push_back(std::move(entry));
	}
	return log;
}

// Starts this session's list with the previous one aged by a session:
static void OpenResourceFileLog(const ResourceFileLog &previous)
{
	std::lock_guard<std::mutex> lock(file_log_lock);

	file_log = CreateFileW(PrefetchLogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file_log == INVALID_HANDLE_VALUE) {
		LogInfo("Failed to write %S: %d\n", prefetch_log_name, GetLastError());
		return;
	}

	for (const auto &entry : previous) {
		if (entry.second < max_unused_sessions)
			AppendToFileLog(entry.first, entry.second + 1);
	}
}

void RecordResourceFileUse(const std::wstring &path)
{
	if (!G->prefetch_resource_files)
		return;

	std::lock_guard<std::mutex> lock(file_log_lock);

	if (file_log == INVALID_HANDLE_VALUE || recorded_files.count(path))
		return;

	// A file that could not be recorded is tried again on its next load:
	if (AppendToFileLog(path, 0))
		recorded_files.insert(path);
}

// Fallback for systems without PrefetchVirtualMemory:
static void ReadFileIntoCache(const wchar_t *path, std::vector<uint8_t> &scratch)
{
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	DWORD read;

	if (f == INVALID_HANDLE_VALUE)
		return;

	while (!prefetch_stop && ReadFile(f, scratch.data(), (DWORD)scratch.size(), &read, NULL) && read)
		;
	CloseHandle(f);
}

static void PrefetchFiles(const ResourceFileLog &log)
{
	std::vector<uint8_t> scratch;
	uint64_t bytes = 0;
	unsigned files = 0;
	ULONGLONG start = GetTickCount64();

	if (!GetPrefetchVirtualMemory())
		scratch.resize(1 << 20);

	for (const auto &entry : log) {
		if (prefetch_stop)
			break;

		if (scratch.empty()) {
			MappedFile file(entry.first.c_str());
			if (!file.valid())
				continue;
			file.prefetch();
			bytes += file.size;
		} else {
			ReadFileIntoCache(entry.first.c_str(), scratch);
		}
		files++;
	}

	LogInfo("Prefetched %u resource files (%llu MB) into the file cache in %llu ms\n",
			files, bytes >> 20, GetTickCount64() - start);
}

static DWORD WINAPI PrefetchThread(LPVOID param)
{
	ResourceFileLog *log = (ResourceFileLog*)param;

	// Background mode lowers the CPU, I/O and memory priority of this
	// thread so that it yields to the game's own loading. The memory
	// priority goes back up: pages read at the lowest one are the first
	// the system takes back when the game allocates, so they were gone
	// again by the time a mod was first drawn.
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
	if (SetThreadInformation_t set_info = GetSetThreadInformation()) {
		MEMORY_PRIORITY_INFORMATION normal = { MEMORY_PRIORITY_NORMAL };
		set_info(GetCurrentThread(), ThreadMemoryPriority, &normal, sizeof(normal));
	}
	PrefetchFiles(*log);
	SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
	delete log;

	// Releases the DLL reference taken in StartResourceFilePrefetch()
	// without returning into code that may be unmapped by that release.
	// It does not unwind either, so nothing here may still own memory:
	FreeLibraryAndExitThread(prefetch_module, 0);
}

void StartResourceFilePrefetch()
{
	// Only the first config load of the process: a reload would prefetch
	// the files this session has already used.
	static bool started = false;

	if (!G->prefetch_resource_files || started)
		return;
	started = true;

	ResourceFileLog *log = new ResourceFileLog(LoadResourceFileLog());
	OpenResourceFileLog(*log);

	// The thread holds a reference on this DLL until it exits, so an
	// unload cannot unmap the code it is running (the thread releases it
	// with FreeLibraryAndExitThread):
	if (log->empty() || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
				(LPCWSTR)&PrefetchThread, &prefetch_module)) {
		delete log;
		return;
	}

	HANDLE thread = CreateThread(NULL, 0, PrefetchThread, log, 0, NULL);
	if (!thread) {
		FreeLibrary(prefetch_module);
		prefetch_module = NULL;
		delete log;
		return;
	}
	CloseHandle(thread);
}

// Called from DLL_PROCESS_DETACH. At process exit every other thread is
// already gone, and an unload through FreeLibrary only happens once the
// prefetch thread has dropped its reference, so there is nothing to wait
// for here.
void StopResourceFilePrefetch()
{
	prefetch_stop = true;
	if (file_log != INVALID_HANDLE_VALUE) {
		CloseHandle(file_log);
		file_log = INVALID_HANDLE_VALUE;
	}
}

#pragma endregion ResourceFilePrefetch
