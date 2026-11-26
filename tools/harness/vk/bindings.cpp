#define NOMINMAX
#include <Windows.h>
#include <dxgiformat.h>
#include <intrin.h>
#include <memory>
#include <stdio.h>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "DirectX11/vk/Spirv.h"

const UINT MAX_SETS = 8, SCOPE_SRVS = 32, SHIM_SRV_SLOTS = 128;
enum Dx12ShaderStage { DX12_STAGE_VS, DX12_STAGE_PS, DX12_STAGE_GS, DX12_STAGE_HS, DX12_STAGE_DS, DX12_STAGE_CS, DX12_STAGE_COUNT };
struct ID3D11ShaderResourceView {
	unsigned refs = 1;
	struct { DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN; } mDesc;
	void AddRef() { refs++; }
	void Release() { refs--; }
};
using ShimShaderResourceView = ID3D11ShaderResourceView;
using IUnknownFixture = ID3D11ShaderResourceView;
namespace Profiling { namespace Spike {
	enum Category { VK_SRV_LOOKUP, CL_VK_SRV };
	struct GameImage;
struct Scope { explicit Scope(Category) {} };
	struct SelfTime { explicit SelfTime(Category) {} };
} }
#include "bin/bindings_types.inc"

struct ShaderLayout { std::vector<SpirvBinding> views; };
struct Pipeline { const ShaderLayout *layouts[DX12_STAGE_COUNT] = {}; };
struct CmdState { const Pipeline *pipeline; VkDescriptorSet sets[MAX_SETS] = {}; };
struct GameImage;
struct Scope {
	CmdState state;
	UINT srvs_asked[DX12_STAGE_COUNT] = {};
	ID3D11ShaderResourceView *srvs[DX12_STAGE_COUNT][SCOPE_SRVS] = {};
	UINT original_formats_asked = 0;
	DXGI_FORMAT original_formats[SCOPE_SRVS];
#ifdef SCOPED_GAME_IMAGES
	GameImage *original_images[SCOPE_SRVS];
#else
	VkImageView original_images[SCOPE_SRVS];
#endif
};
static Scope *scope;
enum { TIMED_ANSWERS, TIMED_PARTS };
static struct { unsigned textures_shown = 0; UINT64 part_ticks[TIMED_PARTS] = {}; } stats;
static struct { bool vk_dump_hashes = false; } settings;
static auto *G = &settings;
static UINT64 fixture_ticks = 1;
static UINT64 ticks() { return fixture_ticks; }
static SRWLOCK descriptor_lock = SRWLOCK_INIT;
static SRWLOCK image_lock = SRWLOCK_INIT;
static volatile LONG image_epoch;
struct GameFormat { DXGI_FORMAT dxgi; };
struct GameImage {
	const GameFormat *format;
	ID3D11ShaderResourceView *srv = nullptr;
	ID3D11ShaderResourceView *object = nullptr;
#ifdef SCOPED_GAME_IMAGES
	ID3D11ShaderResourceView *fixture_view = nullptr;
	LONG shown_epoch = 0;
	bool used_by_format = false;
#endif
};
static std::unordered_map<VkImageView, VkImage> game_views;
#ifdef SCOPED_GAME_IMAGES
static std::unordered_map<VkImage, std::unique_ptr<GameImage>> game_images;
static std::vector<std::unique_ptr<GameImage>> retired_images;
#else
static std::unordered_map<VkImage, GameImage> game_images;
static std::vector<IUnknownFixture*> retired_images;
#endif
static unsigned locks, lookups, held;
static std::unordered_map<VkDescriptorSet, DescriptorSet> sets;
static std::unordered_map<VkImageView, ID3D11ShaderResourceView*> images;

static void require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
static void acquire(SRWLOCK *lock)
{
	AcquireSRWLockShared(lock);
	if (lock == &descriptor_lock) {
		locks++;
		held++;
	}
}
static void release(SRWLOCK *lock)
{
	if (lock == &descriptor_lock)
		held--;
	ReleaseSRWLockShared(lock);
}
static DescriptorSet* find_set(VkDescriptorSet handle)
{
	require(held == 1, "descriptor access requires the shared lock");
	lookups++;
	fixture_ticks += 10;
	auto found = sets.find(handle);
	return found != sets.end() && found->second.layout ? &found->second : nullptr;
}
static ID3D11ShaderResourceView* shown_texture(VkImageView image)
{
	require(!held, "image resolution must release the descriptor lock first");
	fixture_ticks += 100;
	auto found = images.find(image);
	return found == images.end() ? nullptr : found->second;
}
static bool VkInDrawScope() { return scope != nullptr; }
static bool Dx12InDrawScope() { return VkInDrawScope(); }
static ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot);
#ifdef SCOPED_GAME_IMAGES
static void publish_game_texture(GameImage *image)
{
	require(!held, "image resolution must release the descriptor lock first");
	fixture_ticks += 100;
	if (!image->srv) {
		image->srv = image->fixture_view;
		image->srv->AddRef();
	}
	image->shown_epoch = image_epoch;
}
#endif
struct ShimStageState {
	ID3D11ShaderResourceView *srvs[SHIM_SRV_SLOTS] = {};
	UINT srv_mask[4] = {};
	UINT srv_unbound[4] = {};
};
struct ShimState { ShimStageState stages[DX12_STAGE_COUNT]; };
struct ShimContext {
	ShimState *mState;
	bool mChanged = false;
	ID3D11ShaderResourceView *output = nullptr;
	bool IsOutput(const ID3D11ShaderResourceView *view) const { return view == output; }
	void SetShaderResources(Dx12ShaderStage, UINT, UINT, ID3D11ShaderResourceView *const *);
	void GetShaderResources(Dx12ShaderStage, UINT, UINT, ID3D11ShaderResourceView**);
};
static ID3D11ShaderResourceView* ShimViewOf(ID3D11ShaderResourceView *view)
{
	return view;
}

// The production bindings operate on per-test descriptors and COM references.
#define AcquireSRWLockShared acquire
#define ReleaseSRWLockShared release
#define IUnknown IUnknownFixture
#include "bin/bindings_impl.inc"
#undef IUnknown
#undef AcquireSRWLockShared
#undef ReleaseSRWLockShared

static ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot)
{
	return VkScopeShaderResource(stage, slot);
}

static void track_fixture_image(VkImage handle, VkImageView view, const GameFormat *format,
		ID3D11ShaderResourceView *fixture_view)
{
#ifdef SCOPED_GAME_IMAGES
	game_images[handle] = std::make_unique<GameImage>();
	game_images[handle]->format = format;
	game_images[handle]->fixture_view = fixture_view;
#else
	game_images[handle] = { format };
#endif
	VkImageViewCreateInfo info = {};
	info.image = handle;
	VkModsImageView(view, &info);
	images[view] = fixture_view;
}

#ifndef ORIGINAL_FORMATS
static DXGI_FORMAT VkScopeOriginalPixelFormat(UINT slot)
{
	auto view = Dx12ScopeShaderResource(DX12_STAGE_PS, slot);
	return view ? view->mDesc.Format : DXGI_FORMAT_UNKNOWN;
}
#endif

static void check_srv_writes(ID3D11ShaderResourceView *game_view, bool check_changes)
{
	ShimState state;
	ShimContext context = { &state };
	ID3D11ShaderResourceView replacement;
	ID3D11ShaderResourceView *view = nullptr;
	const auto stage = DX12_STAGE_PS;

	context.GetShaderResources(stage, 0, 1, &view);
	require(view == game_view, "an untouched slot inherits the game's binding");
	view->Release();
	context.SetShaderResources(stage, 0, 1, nullptr);
	context.GetShaderResources(stage, 0, 1, &view);
	require(!view, "explicit null must hide the inherited view");
	require(!check_changes || context.mChanged, "explicitly clearing an inherited slot must mark the scope changed");

	context.mChanged = false;
	context.SetShaderResources(stage, 0, 1, nullptr);
	require(!check_changes || !context.mChanged, "repeated explicit null must not trigger a scope reset");

	view = &replacement;
	context.SetShaderResources(stage, 0, 1, &view);
	require(replacement.refs == 2, "new binding holds a reference");
	require(!check_changes || context.mChanged, "a new binding must mark the scope changed");
	context.mChanged = false;
	context.SetShaderResources(stage, 0, 1, &view);
	require(replacement.refs == 2, "rebinding the same view must preserve the reference count");
	require(!check_changes || !context.mChanged, "the same view must not trigger a scope reset");

	context.output = &replacement;
	context.SetShaderResources(stage, 0, 1, &view);
	context.GetShaderResources(stage, 0, 1, &view);
	require(!view && replacement.refs == 1, "output conflict must still clear the binding");
	require(!check_changes || context.mChanged, "clearing a conflicting binding must mark the scope changed");
}

int main(int argc, char **argv)
{
	try {
		bool batched = false, timed = false, srv_changes = false, original_formats = false;
		for (int i = 1; i < argc; i++) {
			batched |= !strcmp(argv[i], "--batched");
			timed |= !strcmp(argv[i], "--timed");
			srv_changes |= !strcmp(argv[i], "--srv-changes");
			original_formats |= !strcmp(argv[i], "--original-formats");
		}
		ID3D11ShaderResourceView a, b, override_view;
		auto first = (VkDescriptorSet)(uintptr_t)1, second = (VkDescriptorSet)(uintptr_t)2;
		auto image_a = (VkImageView)(uintptr_t)11, image_b = (VkImageView)(uintptr_t)12;
		images[image_a] = &a;
		images[image_b] = &b;
		auto layout = std::make_shared<DescriptorLayout>();
		layout->bindings.resize(4);
		layout->bindings[2].image = true;
		layout->bindings[2].count = 1;
		layout->bindings[2].first_image = 0;
		sets[first].layout = sets[second].layout = layout;
		sets[first].images = { image_a };
		sets[second].images = { image_b };
		ShaderLayout shader;
		shader.views.resize(32, SpirvBinding{});
		for (UINT slot : { 0u, 1u, 3u, 4u, 5u, 31u })
			shader.views[slot] = { slot == 5 || slot == 31 ? 2u : 1u, 2, SPIRV_TEXTURE };
		shader.views[1].kind = SPIRV_TEXTURE_SAMPLER;
		shader.views[3].binding = 255;
		Pipeline pipeline;
		pipeline.layouts[DX12_STAGE_PS] = &shader;
		Scope opened;
		opened.state.pipeline = &pipeline;
		opened.state.sets[1] = first;
		opened.state.sets[2] = second;
		scope = &opened;
		ShimState state;
		state.stages[DX12_STAGE_PS].srvs[1] = &override_view;
		state.stages[DX12_STAGE_PS].srv_unbound[0] = 1u << 4;
		ShimContext context = { &state };
		ID3D11ShaderResourceView *out[6];
		auto fetch = [&]() {
			context.GetShaderResources(DX12_STAGE_PS, 0, 6, out);
			require(out[0] == &a && out[1] == &override_view && !out[2] && !out[3] && !out[4] && out[5] == &b,
				"mixed original, override, invalid and explicit null slots");
			for (auto *view : out)
				if (view) view->Release();
		};
		fetch();
		printf("mixed SRV batch: %u descriptor locks, %u set lookups\n", locks, lookups);
		if (batched)
			require(locks == 1 && lookups == 2, "one descriptor lock and one lookup per set");
		require(!stats.part_ticks[TIMED_ANSWERS], "disabled hash statistics must not accumulate lookup time");
		if (timed) {
			settings.vk_dump_hashes = true;
			opened.srvs_asked[DX12_STAGE_PS] = 0;
			const UINT64 before = ticks();
			fetch();
			require(stats.part_ticks[TIMED_ANSWERS] == ticks() - before && stats.part_ticks[TIMED_ANSWERS] == 320,
				"hash statistics must include batch descriptor and image lookup time");
			const UINT64 recorded = stats.part_ticks[TIMED_ANSWERS];
			fetch();
			require(stats.part_ticks[TIMED_ANSWERS] == recorded, "cached getter must not add lookup time");
			settings.vk_dump_hashes = false;
			opened.srvs_asked[DX12_STAGE_PS] = 0;
			fetch();
			require(stats.part_ticks[TIMED_ANSWERS] == recorded, "disabling hash statistics must stop lookup timing");
			printf("PASS: batch timing includes descriptor and image lookup, excludes cache hits and disabled statistics\n");
		}
		locks = lookups = 0;
		fetch();
		require(!locks && !lookups, "repeated getter must reuse the draw's answers");
		state.stages[DX12_STAGE_PS].srvs[1] = nullptr;
		state.stages[DX12_STAGE_PS].srv_unbound[0] = 0;
		ID3D11ShaderResourceView *restored[4];
		context.GetShaderResources(DX12_STAGE_PS, 1, 4, restored);
		require(restored[0] == &a && !restored[1] && !restored[2] && restored[3] == &a,
			"cleared mod and null bindings must reveal the game's bindings");
		restored[0]->Release();
		restored[3]->Release();
		state.stages[DX12_STAGE_PS].srvs[1] = &override_view;
		state.stages[DX12_STAGE_PS].srv_unbound[0] = 1u << 4;
		sets[first].images[0] = image_b;
		sets[first].generation++;
		opened.srvs_asked[DX12_STAGE_PS] = 0;
		context.GetShaderResources(DX12_STAGE_PS, 0, 6, out);
		require(out[0] == &b && out[1] == &override_view && !out[4], "next draw observes descriptor updates without overriding mod bindings");
		for (auto *view : out)
			if (view) view->Release();
		sets[first] = DescriptorSet{};
		sets[first].layout = layout;
		sets[first].images = { image_a };
		opened.srvs_asked[DX12_STAGE_PS] = 0;
		fetch();
		ID3D11ShaderResourceView *edge[2];
		context.GetShaderResources(DX12_STAGE_PS, 31, 2, edge);
		require(edge[0] == &b && !edge[1], "32-slot prefetch boundary");
		edge[0]->Release();
		check_srv_writes(&a, srv_changes);
		if (original_formats) {
			const GameFormat rgba { DXGI_FORMAT_R8G8B8A8_UNORM }, bc5 { DXGI_FORMAT_BC5_UNORM }, bc7 { DXGI_FORMAT_BC7_UNORM };
			auto handle = (VkImage)(uintptr_t)21;
			auto replacement_handle = (VkImage)(uintptr_t)22;
			ID3D11ShaderResourceView recycled;
			a.mDesc.Format = rgba.dxgi;
			b.mDesc.Format = bc5.dxgi;
			recycled.mDesc.Format = bc7.dxgi;
			track_fixture_image(handle, image_a, &rgba, &a);
			track_fixture_image(replacement_handle, image_b, &bc5, &b);
			opened.srvs_asked[DX12_STAGE_PS] = 0;
			locks = lookups = 0;
			require(VkScopeOriginalPixelFormat(0) == rgba.dxgi && VkScopeOriginalPixelFormat(1) == rgba.dxgi,
				"format must describe the game's texture even for a modded slot");
			require(!opened.srvs_asked[DX12_STAGE_PS] && a.refs == 1,
				"format-only queries must leave SRV materialization deferred");
			require(VkScopeOriginalPixelFormat(0) == rgba.dxgi && locks == 2 && lookups == 2,
				"repeated format query must reuse the original answer");
			require(VkScopeOriginalPixelFormat(4) == rgba.dxgi, "third slot must see the original texture before reuse");

			sets[first].images[0] = image_b;
			sets[first].generation++;
			VkModsForgetImageView(image_a);
			VkModsForgetImage(handle);
			track_fixture_image(handle, image_a, &bc7, &recycled);

			auto single = Dx12ScopeShaderResource(DX12_STAGE_PS, 4);
			VkScopePrefetchShaderResources(DX12_STAGE_PS, 3);
			const bool batch_matches = opened.srvs[DX12_STAGE_PS][0] == &a && opened.srvs[DX12_STAGE_PS][1] == &a;
			printf("original SRVs after handle reuse: single=%s batch=%s\n",
				single == &a ? "preserved" : "changed", batch_matches ? "preserved" : "changed");
			require(single == &a && batch_matches, "single and batched SRV reads must preserve the image seen by the format query");
			require(locks == 3 && lookups == 3, "SRV lookups after a format query must reuse its descriptor result");

			scope = nullptr;
			release_retired_images();
			require(a.refs == 1, "retiring the original image must release its published SRV");
			opened.srvs_asked[DX12_STAGE_PS] = 0;
			opened.original_formats_asked = 0;
			sets[second].images[0] = image_a;
			scope = &opened;
			require(VkScopeOriginalPixelFormat(0) == bc5.dxgi && VkScopeOriginalPixelFormat(5) == bc7.dxgi,
				"the next draw must see the updated descriptor and recycled handles");
			require(VkScopeOriginalPixelFormat(2) == DXGI_FORMAT_UNKNOWN && VkScopeOriginalPixelFormat(32) == DXGI_FORMAT_UNKNOWN,
				"non-texture and out-of-range slots have no format");
			printf("PASS: original formats, deferred SRVs, descriptor reuse and image metadata invalidation\n");
		}
		scope = nullptr;
		locks = lookups = 0;
		context.GetShaderResources(DX12_STAGE_PS, 0, 6, out);
		require(!out[0] && out[1] == &override_view && !out[4] && !locks, "outside a Vulkan draw");
		out[1]->Release();
		require(a.refs == 1 && b.refs == 1 && override_view.refs == 1, "getter COM reference counts");
		printf("PASS: mixed bindings, repeated reads and writes, descriptor updates, handle reuse, slot bounds and references\n");
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
