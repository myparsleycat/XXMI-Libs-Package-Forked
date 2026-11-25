#pragma once

// Loading the files of a mod's [Resource] sections on a loader thread instead
// of in the draw that first uses them.
//
// The [Resource] sections the [TextureOverride] sections of an ini namespace
// and the namespaces below it (one mod) can reach are collected once the
// config is loaded. When a draw first matches one of those sections, the files
// the mod needs for its current variable values are read and created on a
// loader thread. Until they are there, no [TextureOverride] of that namespace
// runs and the game's own draw is seen; a change of the variables its branches
// go by checks again. At present, staged D3D12 uploads can continue across
// frames before the finished loads are given to their sections. Nothing is
// loaded for a mod before one of its sections is drawn.
//
// A [Present] initialization guarded by a mod's activation variables can
// also start loading at the first match. Only its resource-dependent pre
// branch waits for the files.
//
// Sections only [ShaderOverride], ShaderRegex, other [Present] and [Constants]
// command lists reach (a [ShaderOverride] holds the commands of every mod with
// its hash), sections reached in no way known here, and those whose uses need
// flags no single resource can have load on use, as without this. The files of
// the last kind are read into the file cache once the mod is first drawn.

#include <d3d11_1.h>
#include <atomic>
#include <cstdint>
#include <cstring>

class CustomResource;
struct ModLoad;
struct PresentLoad;
struct D3D12_RESOURCE_DESC;
struct ID3D12Resource;

enum ModLoadState : LONG {
	// Being given its resource, by Substantiate() or at present
	MOD_LOAD_APPLYING = -1,
	MOD_LOAD_IDLE = 0,
	MOD_LOAD_QUEUED = 1,
	// MOD_LOAD_DONE + epoch: given to its section, for the draws of that
	// epoch on (see ModLoadEpoch)
	MOD_LOAD_DONE = 2,
};

struct ModLoad
{
	std::atomic<bool> present_pending{false};
	bool has_present_spares = false;
	// The first epoch whose draws no longer wait for the mod, or 0
	std::atomic<unsigned> complete_from{0};
	// The variables its branches go by, with their bits when it was last
	// found ready
	const float *const *vars = nullptr;
	std::atomic<uint32_t> *seen = nullptr;
	unsigned var_count = 0;
};

static inline bool ModLoadVariablesSeen(const ModLoad *load)
{
	for (unsigned i = 0; i < load->var_count; i++) {
		uint32_t bits;

		memcpy(&bits, load->vars[i], sizeof(bits));
		if (bits != load->seen[i].load(std::memory_order_relaxed))
			return false;
	}
	return true;
}

// The frame count draws compare against, taken when a draw starts so that its
// pre and post command lists agree. Never 0.
unsigned ModLoadEpoch();

bool ModLoadCheck(ModLoad *load, unsigned epoch, ID3D11Device *device);
void ModLoadQueuePresent(ModLoad *load, ID3D11Device *device);
void ModLoadRefreshPresentSpares(ModLoad *load, ID3D11Device *device);
bool ModLoadPresentReady(const PresentLoad *load, ID3D11Device *device);
ID3D12Resource* ModLoadDx12SpareBuffer(const D3D12_RESOURCE_DESC &desc, UINT64 allocated, bool take);
UINT64 ModLoadSetDx12SparePressure(bool pressured);

// Whether the command lists that wait for load may run in a draw of epoch.
// Starts loading what they need if they may not.
static inline bool ModLoadAllows(ModLoad *load, unsigned epoch, ID3D11Device *device)
{
	unsigned from;

	if (!load)
		return true;
	// Epoch 0: a command list run outside a draw, which goes by the latest
	from = load->complete_from.load(std::memory_order_acquire);
	if (from && (from <= epoch || !epoch) && ModLoadVariablesSeen(load))
		return true;
	return ModLoadCheck(load, epoch, device);
}

// By CustomResource: whether this thread may give it its resource now, false
// if another one is doing so or has done so
bool ModLoadClaim(CustomResource *resource);
// By CustomResource: its resource is there, or it loads on use
void ModLoadResourceDone(CustomResource *resource);
void ModLoadNoteLoadOnUse(CustomResource *resource);

// The game creating a resource at least this large holds back the loader
// thread's next load for a few milliseconds, as both would wait on the driver
const UINT64 MOD_LOAD_HEAVY_BYTES = 1 << 20;
static inline bool ModLoadIsHeavyTexture(UINT64 width, UINT height, UINT depth_or_array)
{
	return width * height * depth_or_array >= 512 * 512;
}
void ModLoadNoteGameCreation();

static inline UINT ModLoadTextureBlockHeight(DXGI_FORMAT format)
{
	return (format >= DXGI_FORMAT_BC1_TYPELESS && format <= DXGI_FORMAT_BC5_SNORM)
		|| (format >= DXGI_FORMAT_BC6H_TYPELESS && format <= DXGI_FORMAT_BC7_UNORM_SRGB) ? 4 : 1;
}

// At the start of loading the config, which frees what the loads refer to.
// A reload holds the ShaderRegex config lock exclusively; the first load runs
// before any loader thread exists.
void ModLoadsConfigCleared();
// Once the command lists are final: after optimising them, or after loading a
// config that is not optimised
void ModLoadsConfigLoaded();

// Once a frame on the presenting thread, before the [Present] command list
// (under the shim lock on D3D12 and Vulkan): gives finished loads to their
// sections. device: the one presenting, or NULL.
void ModLoadsFrame(ID3D11Device *device);

// For the profiling overlay
struct ModLoadStats {
	unsigned mods;
	unsigned waiting_mods;
	unsigned queued;
	unsigned loaded;
	unsigned loaded_on_use;
	unsigned held_draws;
	double load_ms;
};
void ModLoadGetStats(ModLoadStats *stats);
