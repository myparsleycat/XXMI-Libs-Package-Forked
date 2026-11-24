#pragma once

#include <d3d11_1.h>
#include <cstdint>
#include <string>

// The file a path leads to, as it was when opened: two paths to the same
// unchanged file give the same identity
struct FileIdentity
{
	uint32_t volume = 0;
	uint64_t index = 0;
	uint64_t size = 0;
	uint64_t write_time = 0;

	bool operator==(const FileIdentity &other) const;
};

// A whole file read into memory. Reading into a buffer kept for the next
// file is much faster than faulting the pages of a mapped view in one by one
// while a D3D create call copies from it, and the buffer is warm in the
// cache for that copy. A file of several MB is read by thread pool threads
// as well, before read() returns.
class FileContents
{
public:
	FileContents() {}
	// open() and read()
	explicit FileContents(const wchar_t *path);
	~FileContents();

	FileContents(const FileContents&) = delete;
	FileContents& operator=(const FileContents&) = delete;
	FileContents(FileContents &&other) noexcept :
		data(other.data), size(other.size), error(other.error), identity(other.identity),
		file(other.file), path(other.path), buffer(other.buffer), capacity(other.capacity)
	{
		other.data = nullptr;
		other.size = 0;
		other.file = INVALID_HANDLE_VALUE;
		other.buffer = nullptr;
		other.capacity = 0;
	}

	// Opens the file and fills in identity, false with error set on failure
	bool open(const wchar_t *path);
	// Reads all of the opened file and closes it
	bool read();

	bool valid() const { return data != nullptr; }

	const uint8_t *data = nullptr;
	size_t size = 0;
	DWORD error = 0;
	FileIdentity identity;

private:
	HANDLE file = INVALID_HANDLE_VALUE;
	const wchar_t *path = nullptr;
	uint8_t *buffer = nullptr;
	size_t capacity = 0;
};

// Once a frame: frees the buffer of files too big for the kept ones once it
// has gone unused for some seconds
void ReleaseIdleReadBuffers();

enum class SharedResourceLoader {
	DDS,
	WIC,
	BUFFER,
};

// Identifies a GPU resource created from a file by the file's contents and
// everything that affected its creation, so that two [Resource] sections
// pointing at identical files end up with the same ID3D11Resource:
struct SharedResourceKey
{
	ID3D11Device *device = nullptr;
	SharedResourceLoader loader = SharedResourceLoader::DDS;
	uint32_t bind_flags = 0;
	uint32_t misc_flags = 0;
	// DDS forceSRGB or the WIC loader flags:
	uint32_t load_flags = 0;
	// Buffers only, as created after the ini overrides:
	uint32_t byte_width = 0;
	uint32_t structure_stride = 0;

	// Filled in by FindSharedFileResource() from the data the resource is
	// created from. The crc32c only narrows down the candidates:
	uint64_t size = 0;
	uint32_t content = 0;

	bool operator==(const SharedResourceKey &other) const;
};

// The registry holds a reference of its own on every shared resource until
// ReleaseSharedFileResources() is called, so that a hit is never a dangling
// pointer. A hit means the data is byte for byte identical to the file an
// earlier resource with the same key was created from, and the returned
// resource is AddRef()ed for the caller. On a miss the key is complete and
// can be passed to ShareFileResource(), together with the path of the file,
// once the resource has been created from the same data.
ID3D11Resource* FindSharedFileResource(SharedResourceKey &key, const void *data, size_t size);
// Before the file is read: a resource made from this very file, unchanged
// since, with the same key except for the content, which is left unset.
// AddRef()ed for the caller.
ID3D11Resource* FindSharedFileResourceByIdentity(const SharedResourceKey &key, const FileIdentity &identity);
// A provisional entry is for a resource that may not be used yet (made on
// the loader thread of ModLoading.h): only threads inside a
// SharedProvisionalScope find it. Sharing the same resource again without
// provisional makes the entry a normal one.
void ShareFileResource(const SharedResourceKey &key, const std::wstring &path, const FileIdentity &identity,
		ID3D11Resource *resource, bool provisional = false);
// Removes the provisional entry of a resource that was never given to its
// section, if there is one
void DropProvisionalShare(ID3D11Resource *resource);
class SharedProvisionalScope
{
public:
	SharedProvisionalScope();
	~SharedProvisionalScope();
};
// Drops the registry's references, all of them or those on one device:
void ReleaseSharedFileResources(ID3D11Device *device = nullptr);

// Appends a resource file to the list the next session reads into the OS
// file cache ahead of the file's first use:
void RecordResourceFileUse(const std::wstring &path);
void StartResourceFilePrefetch();
void StopResourceFilePrefetch();
