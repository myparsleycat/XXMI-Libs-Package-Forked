#define NOMINMAX
#include <Windows.h>
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
	void AddRef() { refs++; }
	void Release() { refs--; }
};
namespace Profiling { namespace Spike {
	enum Category { VK_SRV_LOOKUP };
	struct Scope { explicit Scope(Category) {} };
} }
#include "bin/bindings_types.inc"

struct ShaderLayout { std::vector<SpirvBinding> views; };
struct Pipeline { const ShaderLayout *layouts[DX12_STAGE_COUNT] = {}; };
struct CmdState { const Pipeline *pipeline; VkDescriptorSet sets[MAX_SETS] = {}; };
struct Scope {
	CmdState state;
	UINT srvs_asked[DX12_STAGE_COUNT] = {};
	ID3D11ShaderResourceView *srvs[DX12_STAGE_COUNT][SCOPE_SRVS] = {};
};
static Scope *scope;
enum { TIMED_ANSWERS, TIMED_PARTS };
static struct { unsigned textures_shown = 0; UINT64 part_ticks[TIMED_PARTS] = {}; } stats;
static struct { bool vk_dump_hashes = false; } settings;
static auto *G = &settings;
static UINT64 fixture_ticks = 1;
static UINT64 ticks() { return fixture_ticks; }
static SRWLOCK descriptor_lock = SRWLOCK_INIT;
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
	locks++;
	held++;
}
static void release(SRWLOCK *lock)
{
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
static ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot)
{
	if (!scope || stage != DX12_STAGE_PS || slot >= SCOPE_SRVS)
		return nullptr;
	if (scope->srvs_asked[stage] & (1u << slot))
		return scope->srvs[stage][slot];
	scope->srvs_asked[stage] |= 1u << slot;
	scope->srvs[stage][slot] = nullptr;
	const auto *shader = scope->state.pipeline->layouts[stage];
	if (!shader || slot >= shader->views.size())
		return nullptr;
	const auto &view = shader->views[slot];
	if ((view.kind != SPIRV_TEXTURE && view.kind != SPIRV_TEXTURE_SAMPLER) || view.set >= MAX_SETS || !scope->state.sets[view.set])
		return nullptr;
	VkImageView image = VK_NULL_HANDLE;
	acquire(&descriptor_lock);
	auto *set = find_set(scope->state.sets[view.set]);
	if (set && view.binding < set->layout->bindings.size()) {
		const auto &binding = set->layout->bindings[view.binding];
		if (binding.image && binding.count)
			image = set->images[binding.first_image];
	}
	release(&descriptor_lock);
	return scope->srvs[stage][slot] = shown_texture(image);
}
struct ShimStageState {
	ID3D11ShaderResourceView *srvs[SHIM_SRV_SLOTS] = {};
	UINT srv_unbound[4] = {};
};
struct ShimState { ShimStageState stages[DX12_STAGE_COUNT]; };
struct ShimContext {
	ShimState *mState;
	void GetShaderResources(Dx12ShaderStage, UINT, UINT, ID3D11ShaderResourceView**);
};
template <class Object> static void get(Object *object, Object **out)
{
	if (object)
		object->AddRef();
	*out = object;
}

// Compile the production getter and prefetch; all descriptors and COM references are per-test fixtures.
#define AcquireSRWLockShared acquire
#define ReleaseSRWLockShared release
#include "bin/bindings_impl.inc"
#undef AcquireSRWLockShared
#undef ReleaseSRWLockShared

int main(int argc, char **argv)
{
	try {
		bool batched = false, timed = false;
		for (int i = 1; i < argc; i++) {
			batched |= !strcmp(argv[i], "--batched");
			timed |= !strcmp(argv[i], "--timed");
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
		scope = nullptr;
		locks = lookups = 0;
		context.GetShaderResources(DX12_STAGE_PS, 0, 6, out);
		require(!out[0] && out[1] == &override_view && !out[4] && !locks, "outside a Vulkan draw");
		out[1]->Release();
		require(a.refs == 1 && b.refs == 1 && override_view.refs == 1, "getter COM reference counts");
		printf("PASS: mixed bindings, repeated reads, descriptor updates, handle reuse, slot bounds and references\n");
		return 0;
	} catch (const std::exception &error) {
		fprintf(stderr, "FAILED: %s\n", error.what());
		return 1;
	}
}
