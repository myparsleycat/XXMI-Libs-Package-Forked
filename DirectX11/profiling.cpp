#include "profiling.h"
#include "globals.h"

#include <algorithm>
#include <atomic>

void Profiling::Overhead::clear()
{
	cpu.QuadPart = 0;
	count = 0;
	hits = 0;
}

namespace Profiling {
	Mode mode;
	Overhead present_overhead;
	Overhead overlay_overhead;
	Overhead draw_overhead;
	Overhead map_overhead;
	Overhead hash_tracking_overhead;
	Overhead region_tracking_overhead;
	Overhead stat_overhead;
	Overhead shaderregex_overhead;
	Overhead cursor_overhead;
	wstring text;
	wstring cto_warning;
	INT64 interval;
	bool freeze;

	Overhead shader_hash_lookup_overhead;
	Overhead shader_reload_lookup_overhead;
	Overhead shader_original_lookup_overhead;
	Overhead shaderoverride_lookup_overhead;
	Overhead texture_handle_info_lookup_overhead;
	Overhead textureoverride_lookup_overhead;
	Overhead resource_pool_lookup_overhead;
	Overhead texture_override_fuzzy_match_overhead;
	Overhead texture_override_candidates_lookup_overhead;

	unsigned resource_full_copies;
	unsigned resource_reference_copies;
	unsigned inter_device_copies;
	unsigned msaa_resolutions;
	unsigned buffer_region_copies;
	unsigned views_cleared;
	unsigned resources_created;
	unsigned resources_shared;
	unsigned resource_pool_swaps;
	unsigned max_copies_per_frame_exceeded;
	unsigned injected_draw_calls;
	unsigned skipped_draw_calls;
	unsigned max_executions_per_frame_exceeded;
	unsigned iniparams_updates;
}

static LARGE_INTEGER profiling_start_time;
static unsigned start_frame_no;

static const struct D3D11_QUERY_DESC query_timestamp = {
	D3D11_QUERY_TIMESTAMP,
	0,
};

static std::unordered_set<CommandList*> warned_cto_command_lists;

static void cto_warn_post_commands(CommandList *command_list)
{
	IfCommand *if_command;
	RunExplicitCommandList *run_command;

	// Only visit each command list at most once both to keep the list
	// shorter and make sure we can't enter an infinite recursion if a list
	// calls itself:
	if (warned_cto_command_lists.count(command_list))
		return;
	warned_cto_command_lists.insert(command_list);

	for (auto &command : command_list->commands) {
		if_command = dynamic_cast<IfCommand*>(command.get());
		if (if_command) {
			// If commands aren't the culprits - it's whatever
			// stopped their post phases from being optimised out
			// we should warn about:
			cto_warn_post_commands(if_command->true_commands_post.get());
			cto_warn_post_commands(if_command->false_commands_post.get());
		} else
			Profiling::cto_warning += command->ini_line + L"\n";

		run_command = dynamic_cast<RunExplicitCommandList*>(command.get());
		if (run_command) {
			// Run commands can be the culprits themselves, or
			// what they contain, so list the run command and
			// recurse into the command lists it calls:
			if (run_command->run_pre_and_post_together)
				cto_warn_post_commands(&run_command->command_list_section->command_list);
			cto_warn_post_commands(&run_command->command_list_section->post_command_list);
		}
	}
}

void Profiling::update_cto_warning(bool warn)
{
	Profiling::cto_warning.clear();

	if (!warn)
		return;

	Profiling::cto_warning = L"\nThe following commands prevented optimising out all implicit post checktextureoverrides:\n";
	warned_cto_command_lists.clear();

	for (auto &tolkv : G->mTextureOverrideMap) {
		for (TextureOverride &to : tolkv.second)
			cto_warn_post_commands(&to.post_command_list);
	}
	for (auto &tof : G->mFuzzyTextureOverrides)
		cto_warn_post_commands(&tof->texture_override->post_command_list);

	LogInfoNoNL("%S", Profiling::cto_warning.c_str());
}

static void update_txt_cto_warning()
{
	Profiling::text += L" (post [TextureOverride] commands):\n" + Profiling::cto_warning;
}

static void update_txt_summary(LARGE_INTEGER collection_duration, LARGE_INTEGER freq, unsigned frames)
{
	LARGE_INTEGER present_overhead = {0};
	LARGE_INTEGER command_list_overhead = {0};
	LARGE_INTEGER overlay_overhead;
	LARGE_INTEGER draw_overhead;
	LARGE_INTEGER map_overhead;
	LARGE_INTEGER hash_tracking_overhead;
	LARGE_INTEGER region_tracking_overhead;
	LARGE_INTEGER stat_overhead;
	LARGE_INTEGER shaderregex_overhead;
	LARGE_INTEGER cursor_overhead;
	LARGE_INTEGER shader_hash_lookup_overhead;
	LARGE_INTEGER shader_reload_lookup_overhead;
	LARGE_INTEGER shader_original_lookup_overhead;
	LARGE_INTEGER shaderoverride_lookup_overhead;
	LARGE_INTEGER texture_handle_info_lookup_overhead;
	LARGE_INTEGER textureoverride_lookup_overhead;
	LARGE_INTEGER resource_pool_lookup_overhead;
	LARGE_INTEGER texture_override_fuzzy_match_overhead;
	LARGE_INTEGER texture_override_candidates_lookup_overhead;
	wchar_t buf[1024];

	// The overlay overhead should be a subset of the present overhead, but
	// given that it includes the overhead of drawing the profiling HUD we
	// want it counted separately. The > check is to stop the case where
	// the profiling overlay was only just turned on and the first frame
	// won't have counted any present overhead yet, but will have counted
	// overlay overhead:
	if (Profiling::present_overhead.cpu.QuadPart > Profiling::overlay_overhead.cpu.QuadPart)
		present_overhead.QuadPart = Profiling::present_overhead.cpu.QuadPart - Profiling::overlay_overhead.cpu.QuadPart;

	for (CommandList *command_list : command_lists_profiling)
		command_list_overhead.QuadPart += command_list->time_spent_exclusive.QuadPart;

	present_overhead.QuadPart = present_overhead.QuadPart * 1000000 / freq.QuadPart;
	command_list_overhead.QuadPart = command_list_overhead.QuadPart * 1000000 / freq.QuadPart;
	overlay_overhead.QuadPart = Profiling::overlay_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	draw_overhead.QuadPart = Profiling::draw_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	map_overhead.QuadPart = Profiling::map_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	hash_tracking_overhead.QuadPart = Profiling::hash_tracking_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	region_tracking_overhead.QuadPart = Profiling::region_tracking_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	stat_overhead.QuadPart = Profiling::stat_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	shaderregex_overhead.QuadPart = Profiling::shaderregex_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	cursor_overhead.QuadPart = Profiling::cursor_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;

	shader_hash_lookup_overhead.QuadPart = Profiling::shader_hash_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	shader_reload_lookup_overhead.QuadPart = Profiling::shader_reload_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	shader_original_lookup_overhead.QuadPart = Profiling::shader_original_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	shaderoverride_lookup_overhead.QuadPart = Profiling::shaderoverride_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	texture_handle_info_lookup_overhead.QuadPart = Profiling::texture_handle_info_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	textureoverride_lookup_overhead.QuadPart = Profiling::textureoverride_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	resource_pool_lookup_overhead.QuadPart = Profiling::resource_pool_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	texture_override_fuzzy_match_overhead.QuadPart = Profiling::texture_override_fuzzy_match_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;
	texture_override_candidates_lookup_overhead.QuadPart = Profiling::texture_override_candidates_lookup_overhead.cpu.QuadPart * 1000000 / freq.QuadPart;

	Profiling::text += L" (CPU Performance Summary):\n";
	_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"     Present overhead: %7.2fus/frame ~%ffps\n"
			    L"     Overlay overhead: %7.2fus/frame ~%ffps\n"
			    L"   Draw call overhead: %7.2fus/frame ~%ffps\n"
			    L"  Command lists total: %7.2fus/frame ~%ffps\n"
			    L"   Map/Unmap overhead: %7.2fus/frame ~%ffps\n"
			    L"track_texture_updates: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
		        L"  track_region_hashes: %7.2fus/frame ~%ffps\n"
			    L"  dump_usage overhead: %7.2fus/frame ~%ffps\n"
			    L" ShaderRegex overhead: %7.2fus/frame ~%ffps\n"
			    L"Mouse cursor overhead: %7.2fus/frame ~%ffps\n"
			    ,
			    (float)present_overhead.QuadPart / frames,
			    60.0 * present_overhead.QuadPart / collection_duration.QuadPart,

			    (float)overlay_overhead.QuadPart / frames,
			    60.0 * overlay_overhead.QuadPart / collection_duration.QuadPart,

			    (float)draw_overhead.QuadPart / frames,
			    60.0 * draw_overhead.QuadPart / collection_duration.QuadPart,

			    (float)command_list_overhead.QuadPart / frames,
			    60.0 * command_list_overhead.QuadPart / collection_duration.QuadPart,

			    (float)map_overhead.QuadPart / frames,
			    60.0 * map_overhead.QuadPart / collection_duration.QuadPart,

			    (float)hash_tracking_overhead.QuadPart / frames,
			    60.0 * hash_tracking_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::hash_tracking_overhead.hits / frames,
			    Profiling::hash_tracking_overhead.count / frames,

				(float)region_tracking_overhead.QuadPart / frames,
				60.0 * region_tracking_overhead.QuadPart / collection_duration.QuadPart,

			    (float)stat_overhead.QuadPart / frames,
			    60.0 * stat_overhead.QuadPart / collection_duration.QuadPart,

			    (float)shaderregex_overhead.QuadPart / frames,
			    60.0 * shaderregex_overhead.QuadPart / collection_duration.QuadPart,

			    (float)cursor_overhead.QuadPart / frames,
			    60.0 * cursor_overhead.QuadPart / collection_duration.QuadPart
	);
	Profiling::text += buf;

	_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"\n"
			    L"Map Lookups (CPU):\n"
			    L"          Shader hash: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"Live reloaded shaders: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"     Original shaders: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"       ShaderOverride: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"  Texture hash / info: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"      TextureOverride: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"       Resource pools: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L"TO candidates rebuild: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    L" TO candidates lookup: %7.2fus/frame ~%ffps (%u/%u hits/frame)\n"
			    ,
			    (float)shader_hash_lookup_overhead.QuadPart / frames,
			    60.0 * shader_hash_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::shader_hash_lookup_overhead.hits / frames,
			    Profiling::shader_hash_lookup_overhead.count / frames,

			    (float)shader_reload_lookup_overhead.QuadPart / frames,
			    60.0 * shader_reload_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::shader_reload_lookup_overhead.hits / frames,
			    Profiling::shader_reload_lookup_overhead.count / frames,

			    (float)shader_original_lookup_overhead.QuadPart / frames,
			    60.0 * shader_original_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::shader_original_lookup_overhead.hits / frames,
			    Profiling::shader_original_lookup_overhead.count / frames,

			    (float)shaderoverride_lookup_overhead.QuadPart / frames,
			    60.0 * shaderoverride_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::shaderoverride_lookup_overhead.hits / frames,
			    Profiling::shaderoverride_lookup_overhead.count / frames,

			    (float)texture_handle_info_lookup_overhead.QuadPart / frames,
			    60.0 * texture_handle_info_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::texture_handle_info_lookup_overhead.hits / frames,
			    Profiling::texture_handle_info_lookup_overhead.count / frames,

			    (float)textureoverride_lookup_overhead.QuadPart / frames,
			    60.0 * textureoverride_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::textureoverride_lookup_overhead.hits / frames,
			    Profiling::textureoverride_lookup_overhead.count / frames,

			    (float)resource_pool_lookup_overhead.QuadPart / frames,
			    60.0 * resource_pool_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::resource_pool_lookup_overhead.hits / frames,
			    Profiling::resource_pool_lookup_overhead.count / frames,

			    (float)texture_override_fuzzy_match_overhead.QuadPart / frames,
			    60.0 * texture_override_fuzzy_match_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::texture_override_fuzzy_match_overhead.hits / frames,
			    Profiling::texture_override_fuzzy_match_overhead.count / frames,

			    (float)texture_override_candidates_lookup_overhead.QuadPart / frames,
			    60.0 * texture_override_candidates_lookup_overhead.QuadPart / collection_duration.QuadPart,
			    Profiling::texture_override_candidates_lookup_overhead.hits / frames,
			    Profiling::texture_override_candidates_lookup_overhead.count / frames
	);
	Profiling::text += buf;

	_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
			    L"\n"
			    L"GPU Performance Impacting Stats (costs are guidelines only):\n"
			    L"   IniParams GPU resource updates: %4u/frame (%Iu bytes)\n"
			    L"             Full resource copies: %4u/frame (High cost)\n"
			    L"     By-Reference resource copies: %4u/frame (Low cost)\n"
			    L"     Inter-device resource copies: %4u/frame (Extremely high cost)\n"
			    L"          MSAA resources resolved: %4u/frame (High cost)\n"
			    L"             Region buffer copies: %4u/frame\n"
			    L"                Resources cleared: %4u/frame (Cost saving in some circumstances, e.g. SLI)\n"
			    L"            Resources [re]created: %4u       (High cost)\n"
			    L"   Resources shared between files: %4u       (Cost saving)\n"
			    L"              Resource pool swaps: %4u/frame (Low cost)\n"
			    L"    max_copies_per_frame exceeded: %4u/frame (Cost saving)\n"
			    L"     Injected draw/dispatch calls: %4u/frame\n"
			    L"               Skipped draw calls: %4u/frame (Cost saving)\n"
			    L"max_executions_per_frame exceeded: %4u/frame (Cost saving)\n"
			    ,
			    Profiling::iniparams_updates / frames, G->iniParams.size() * sizeof(DirectX::XMFLOAT4),
			    Profiling::resource_full_copies / frames,
			    Profiling::resource_reference_copies / frames,
			    Profiling::inter_device_copies / frames,
			    Profiling::msaa_resolutions / frames,
			    Profiling::buffer_region_copies / frames,
			    Profiling::views_cleared / frames,
			    Profiling::resources_created,
			    Profiling::resources_shared,
			    Profiling::resource_pool_swaps / frames,
			    Profiling::max_copies_per_frame_exceeded / frames,
			    Profiling::injected_draw_calls / frames,
			    Profiling::skipped_draw_calls / frames,
			    Profiling::max_executions_per_frame_exceeded / frames
	);
	Profiling::text += buf;

	if (G->implicit_post_checktextureoverride_used && !Profiling::cto_warning.empty())
		Profiling::text += L"\nImplicit post checktextureoverrides were not optimised out\n";
}

static void update_txt_command_lists(LARGE_INTEGER collection_duration, LARGE_INTEGER freq, unsigned frames)
{
	LARGE_INTEGER inclusive, exclusive;
	double inclusive_fps, exclusive_fps;
	wchar_t buf[256];

	vector<CommandList*> sorted(command_lists_profiling.begin(), command_lists_profiling.end());
	std::sort(sorted.begin(), sorted.end(), [](const CommandList *lhs, const CommandList *rhs) {
		return lhs->time_spent_inclusive.QuadPart > rhs->time_spent_inclusive.QuadPart;
	});

	Profiling::text += L" (Top Command Lists):\n"
			    L"      | Including sub-lists | Excluding sub-lists |\n"
			    L"count | CPU/frame ~fps cost | CPU/frame ~fps cost |\n"
			    L"----- | --------- --------- | --------- --------- |\n";
	for (CommandList *command_list : sorted) {
		inclusive.QuadPart = command_list->time_spent_inclusive.QuadPart * 1000000 / freq.QuadPart;
		exclusive.QuadPart = command_list->time_spent_exclusive.QuadPart * 1000000 / freq.QuadPart;

		// fps estimate based on the assumption that if we took 100%
		// CPU time it would cost all 60fps:
		inclusive_fps = 60.0 * inclusive.QuadPart / collection_duration.QuadPart;
		exclusive_fps = 60.0 * exclusive.QuadPart / collection_duration.QuadPart;

		_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
				L"%5.0f | %7.2fus %9f | %7.2fus %9f | %4s [%s]\n",
				ceil((float)command_list->executions / frames),
				(float)inclusive.QuadPart / frames,
				inclusive_fps,
				(float)exclusive.QuadPart / frames,
				exclusive_fps,
				command_list->post ? L"post" : L"pre",
				command_list->ini_section.c_str()
		);
		Profiling::text += buf;
		// TODO: GPU time spent
	}
}

static void update_txt_commands(LARGE_INTEGER collection_duration, LARGE_INTEGER freq, unsigned frames)
{
	LARGE_INTEGER pre_time_spent, post_time_spent;
	double pre_fps_cost, post_fps_cost;
	wchar_t buf[256];

	vector<CommandListCommand*> sorted(command_lists_cmd_profiling.begin(), command_lists_cmd_profiling.end());

	std::sort(sorted.begin(), sorted.end(), [](const CommandListCommand *lhs, const CommandListCommand *rhs) {
		return (lhs->pre_time_spent.QuadPart + lhs->post_time_spent.QuadPart) >
		       (rhs->pre_time_spent.QuadPart + rhs->post_time_spent.QuadPart);
	});

	Profiling::text += L" (Top Commands):\n"
			    L"         pre              |         post\n"
			    L"count CPU/frame ~fps cost | count CPU/frame ~fps cost\n"
			    L"----- --------- --------- | ----- --------- ---------\n";
	for (CommandListCommand *cmd : sorted) {
		pre_time_spent.QuadPart = cmd->pre_time_spent.QuadPart * 1000000 / freq.QuadPart;
		post_time_spent.QuadPart = cmd->post_time_spent.QuadPart * 1000000 / freq.QuadPart;

		// fps estimate based on the assumption that if we took 100%
		// CPU time it would cost all 60fps:
		pre_fps_cost = 60.0 * pre_time_spent.QuadPart / collection_duration.QuadPart;
		post_fps_cost = 60.0 * post_time_spent.QuadPart / collection_duration.QuadPart;

		if (cmd->pre_executions) {
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
					L"%5.0f %7.2fus %9f | ",
					ceil((float)cmd->pre_executions / frames),
					(float)pre_time_spent.QuadPart / frames,
					pre_fps_cost);
			Profiling::text += buf;
		} else
			Profiling::text += L"                          | ";
		if (cmd->post_executions) {
			_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
					L"%5.0f %7.2fus %9f ",
					ceil((float)cmd->post_executions / frames),
					(float)post_time_spent.QuadPart / frames,
					post_fps_cost);
			Profiling::text += buf;
		} else
			Profiling::text += L"                          ";
		Profiling::text += cmd->ini_line;
		Profiling::text += L"\n";
		// TODO: GPU time spent
	}
}

void Profiling::update_txt()
{
	static LARGE_INTEGER freq = {0};
	LARGE_INTEGER end_time, collection_duration;
	unsigned frames = G->frame_no - start_frame_no;
	wchar_t buf[256];

	if (freeze)
		return;

	QueryPerformanceCounter(&end_time);
	if (!freq.QuadPart)
		QueryPerformanceFrequency(&freq);

	// Safety - in case of zero frequency avoid divide by zero:
	if (!freq.QuadPart)
		return;

	collection_duration.QuadPart = (end_time.QuadPart - profiling_start_time.QuadPart) * 1000000 / freq.QuadPart;
	if (collection_duration.QuadPart < interval && !Profiling::text.empty())
		return;

	if (frames && collection_duration.QuadPart) {
		_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE,
				    L"Performance Monitor %.1ffps", frames * 1000000.0 / collection_duration.QuadPart);
		Profiling::text = buf;

		switch (Profiling::mode) {
			case Profiling::Mode::SUMMARY:
				update_txt_summary(collection_duration, freq, frames);
				break;
			case Profiling::Mode::TOP_COMMAND_LISTS:
				update_txt_command_lists(collection_duration, freq, frames);
				break;
			case Profiling::Mode::TOP_COMMANDS:
				update_txt_commands(collection_duration, freq, frames);
				break;
			case Profiling::Mode::CTO_WARNING:
				update_txt_cto_warning();
				break;
		}
	}

	// Restart profiling for the next time interval:
	clear();
}

void Profiling::clear()
{
	clear_command_list_profiling();
	present_overhead.clear();
	overlay_overhead.clear();
	draw_overhead.clear();
	map_overhead.clear();
	hash_tracking_overhead.clear();
	region_tracking_overhead.clear();
	stat_overhead.clear();
	shaderregex_overhead.clear();
	cursor_overhead.clear();
	freeze = false;

	shader_hash_lookup_overhead.clear();
	shader_reload_lookup_overhead.clear();
	shader_original_lookup_overhead.clear();
	shaderoverride_lookup_overhead.clear();
	texture_handle_info_lookup_overhead.clear();
	textureoverride_lookup_overhead.clear();
	resource_pool_lookup_overhead.clear();
	texture_override_fuzzy_match_overhead.clear();
	texture_override_candidates_lookup_overhead.clear();

	resource_full_copies = 0;
	resource_reference_copies = 0;
	inter_device_copies = 0;
	msaa_resolutions = 0;
	buffer_region_copies = 0;
	views_cleared = 0;
	resources_created = 0;
	resources_shared = 0;
	resource_pool_swaps = 0;
	max_copies_per_frame_exceeded = 0;
	injected_draw_calls = 0;
	skipped_draw_calls = 0;
	max_executions_per_frame_exceeded = 0;
	iniparams_updates = 0;

	start_frame_no = G->frame_no;
	QueryPerformanceCounter(&profiling_start_time);
}

namespace Profiling {
	namespace Spike {
		INT64 threshold;
		const wchar_t *reason_names[NUM_REASONS];
		volatile LONG reason_counts[NUM_REASONS];
	}
}

using Profiling::Spike::Category;
using Profiling::Spike::NUM_CATEGORIES;

static const wchar_t *spike_category_names[NUM_CATEGORIES] = {
	L"frame actions before Present (input, [Present], overlay)",
	L"the original Present call (driver, GPU, vsync)",
	L"post [Present] command list",
	L"3DMigoto work before draw calls",
	L"3DMigoto work after draw calls",
	L"Map/Unmap tracking",
	L"shader creation requested by the game",
	L"resource creation requested by the game",
	L"D3D12: applying what the mods set to a draw, and the draw",
	L"D3D12: ExecuteCommandLists, our part",
	L"D3D12: ExecuteCommandLists, the driver's part",
	L"D3D12: looking at a pipeline the game created",
	L"D3D12: pipeline creation requested by the game (driver)",
	L"D3D12: hashing a texture the game uploads",
	L"D3D12: hashing a buffer the game uploads",
	L"custom resource creation ([Resource] files, copies)",
	L"shader creation: the driver's part",
	L"ShaderRegex",
	L"ShaderRegex: reading the cache files",
	L"ShaderRegex: analysis (disassemble, match, assemble)",
	L"ShaderRegex: creating the patched shader (driver)",
	L"config reload",
	L"waiting for a 3DMigoto lock",
	L"D3D12: draws the mods are asked about, in all",
	L"D3D12: dispatches the mods are asked about, in all",
	L"D3D12: submitting our command list",
	L"D3D12: waiting for the GPU",
	L"D3D12: creating a pipeline for a mod (driver)",
	L"D3D12: creating a resource for a mod (driver)",
	L"D3D12: copying a mod's resource for a write while the game's lists use it",
	L"D3D12: upkeep of the draw path at present",
	L"D3D12: working out how far an index buffer reaches",
	L"D3D12: deciding without the lock whether a draw or dispatch concerns the mods",
	L"D3D12: noting what the game writes to a descriptor",
	L"D3D12: writing a descriptor of the game to the mirror as well (driver)",
	L"[Resource] files: reading the file",
	L"[Resource] files: comparing with identical files already loaded",
	L"D3D12: copying what a mod uploads to upload memory",
};

static const struct {
	const wchar_t *name;
	unsigned *counter;
} spike_counters[] = {
	{L"resources [re]created", &Profiling::resources_created},
	{L"resources shared between files", &Profiling::resources_shared},
	{L"full resource copies", &Profiling::resource_full_copies},
	{L"inter-device resource copies", &Profiling::inter_device_copies},
	{L"MSAA resolves", &Profiling::msaa_resolutions},
	{L"region buffer copies", &Profiling::buffer_region_copies},
	{L"by-reference copies", &Profiling::resource_reference_copies},
	{L"resource pool swaps", &Profiling::resource_pool_swaps},
	{L"injected draw calls", &Profiling::injected_draw_calls},
	{L"skipped draw calls", &Profiling::skipped_draw_calls},
	{L"IniParams updates", &Profiling::iniparams_updates},
};
static unsigned spike_counters_prev[ARRAYSIZE(spike_counters)];

struct SpikeTotal {
	std::atomic<INT64> ticks;
	std::atomic<unsigned> count;
};
// Games create shaders and resources on worker threads, which costs the frame
// nothing unless the render thread ends up waiting on them, so the thread that
// calls Present [0] is kept apart from all the others [1]:
static SpikeTotal spike_totals[NUM_CATEGORIES][2];
static std::atomic<DWORD> spike_present_thread;

struct SpikeEvent {
	Category category;
	bool other_thread;
	INT64 ticks;
	wstring name;
};
static const size_t spike_max_events = 64;
static const size_t spike_max_events_logged = 16;
static vector<SpikeEvent> spike_events;
static unsigned spike_events_dropped;
static SRWLOCK spike_events_lock = SRWLOCK_INIT;

static FILE *spike_log;
static INT64 spike_freq;
static INT64 spike_event_min;
static INT64 spike_last_present;
static double spike_average_ms;
static double spike_relative;

// What a run with the mods is compared against one without by: every few
// seconds, and whenever the mods are switched, the frame times since and where
// the time went on average
static const double spike_window_ms = 5000;
static vector<float> spike_window_frames;
static double spike_window_total_ms;
static unsigned spike_window_spikes;
static INT64 spike_window_ticks[NUM_CATEGORIES][2];
static unsigned spike_window_count[NUM_CATEGORIES][2];
static UINT64 spike_window_reasons[Profiling::Spike::NUM_REASONS];
static bool spike_mods_disabled;

void Profiling::Spike::init(float threshold_ms, float relative, const wchar_t *log_dir)
{
	LARGE_INTEGER freq;
	wchar_t path[MAX_PATH];
	bool was_enabled = !!threshold;

	threshold = 0;

	if (threshold_ms < 0)
		threshold_ms = 0;
	if (relative <= 1)
		relative = 0;
	if ((!threshold_ms && !relative) || !QueryPerformanceFrequency(&freq) || !freq.QuadPart)
		return;

	if (!spike_log) {
		_snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%lsd3d11_spike_log.txt", log_dir);
		// Mod paths are frequently not ASCII. Started afresh with every
		// run of the game, like d3d11_log.txt:
		spike_log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYNO);
		if (!spike_log) {
			LogInfo("  *** Unable to create %S\n", path);
			return;
		}
	}

	spike_freq = freq.QuadPart;
	spike_event_min = freq.QuadPart / 1000;
	if (!was_enabled) {
		// The time since the last Present we saw is not a frame time:
		spike_last_present = 0;
		spike_average_ms = 0;
	}

	// The config is loaded more than once while the game starts
	static float header_threshold_ms = -1, header_relative = -1;
	if (threshold_ms == header_threshold_ms && relative == header_relative)
		goto enable;
	header_threshold_ms = threshold_ms;
	header_relative = relative;

	fwprintf(spike_log, L"Logging frames that take longer than");
	if (threshold_ms)
		fwprintf(spike_log, L" %.1fms", threshold_ms);
	if (relative)
		fwprintf(spike_log, L"%ls %.2f times the recent average", threshold_ms ? L" or" : L"", relative);
	fwprintf(spike_log, L". Times listed per frame:\n"
			L"  Render thread: where the thread that calls Present spent the frame\n"
			L"  Included in the above: parts of those times worth singling out\n"
			L"  Other threads: ran concurrently, only matters if the render thread waited on it\n"
			L"  Slowest operations: individual operations that took at least 1ms\n"
			L"Every %.0f seconds a summary of the frames since is written as well.\n\n",
			spike_window_ms / 1000);
	fflush(spike_log);

enable:
	spike_relative = relative;
	threshold = threshold_ms ? (INT64)((double)threshold_ms * freq.QuadPart / 1000.0) : INT64_MAX;
}

void Profiling::Spike::add(Category category, INT64 start, const wchar_t *name, UINT64 id)
{
	LARGE_INTEGER end;
	wchar_t buf[32];

	QueryPerformanceCounter(&end);
	INT64 ticks = end.QuadPart - start;
	bool other_thread = GetCurrentThreadId() != spike_present_thread.load(std::memory_order_relaxed);

	SpikeTotal &total = spike_totals[category][other_thread];
	total.ticks.fetch_add(ticks, std::memory_order_relaxed);
	total.count.fetch_add(1, std::memory_order_relaxed);

	if (!name || ticks < spike_event_min)
		return;

	SpikeEvent event = {category, other_thread, ticks, name};
	if (id) {
		_snwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, L" %016I64x", id);
		event.name += buf;
	}

	AcquireSRWLockExclusive(&spike_events_lock);
	if (spike_events.size() < spike_max_events)
		spike_events.push_back(std::move(event));
	else
		spike_events_dropped++;
	ReleaseSRWLockExclusive(&spike_events_lock);
}

static double spike_ms(INT64 ticks)
{
	return ticks * 1000.0 / spike_freq;
}

static void spike_log_totals(const wchar_t *heading, INT64 ticks[][2], unsigned count[][2],
		int first, int last, int thread)
{
	bool heading_logged = !heading;

	for (int i = first; i < last; i++) {
		if (!count[i][thread])
			continue;
		if (!heading_logged) {
			fwprintf(spike_log, L"  %ls:\n", heading);
			heading_logged = true;
		}
		fwprintf(spike_log, L"  %10.2fms  %ls (x%u)\n",
				spike_ms(ticks[i][thread]), spike_category_names[i], count[i][thread]);
	}
}

static void spike_log_window()
{
	size_t frames = spike_window_frames.size();
	bool heading_logged = false;
	SYSTEMTIME time;

	if (!frames)
		return;

	// The slowest 1% of the frames start here
	size_t nth = frames - 1 - frames / 100;
	std::nth_element(spike_window_frames.begin(), spike_window_frames.begin() + nth, spike_window_frames.end());
	float percentile = spike_window_frames[nth];
	float worst = *std::max_element(spike_window_frames.begin(), spike_window_frames.end());

	GetLocalTime(&time);
	fwprintf(spike_log, L"[%02u:%02u:%02u.%03u] summary of %.1fs with mods %ls: %Iu frames, average %.2fms (%.1f fps), "
			L"99th percentile %.2fms, worst %.2fms, %u logged as slow\n",
			time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, spike_window_total_ms / 1000,
			spike_mods_disabled ? L"OFF" : L"on", frames, spike_window_total_ms / frames,
			frames * 1000.0 / spike_window_total_ms, percentile, worst, spike_window_spikes);
	for (int i = 0; i < NUM_CATEGORIES; i++) {
		if (!spike_window_count[i][0] && !spike_window_count[i][1])
			continue;
		if (!heading_logged)
			fwprintf(spike_log, L"  Per frame, on the render thread and on other threads:\n");
		heading_logged = true;
		fwprintf(spike_log, L"  %8.3fms (x%-7.1f) %8.3fms (x%-7.1f)  %ls\n",
				spike_ms(spike_window_ticks[i][0]) / frames, (double)spike_window_count[i][0] / frames,
				spike_ms(spike_window_ticks[i][1]) / frames, (double)spike_window_count[i][1] / frames,
				spike_category_names[i]);
	}
	heading_logged = false;
	for (unsigned i = 0; i < Profiling::Spike::NUM_REASONS; i++) {
		if (!spike_window_reasons[i])
			continue;
		fwprintf(spike_log, L"%ls%ls %.1f", heading_logged ? L", " : L"  Per frame: ",
				Profiling::Spike::reason_names[i], (double)spike_window_reasons[i] / frames);
		heading_logged = true;
	}
	fwprintf(spike_log, heading_logged ? L"\n\n" : L"\n");
	fflush(spike_log);

	memset(spike_window_reasons, 0, sizeof(spike_window_reasons));
	spike_window_frames.clear();
	spike_window_total_ms = 0;
	spike_window_spikes = 0;
	memset(spike_window_ticks, 0, sizeof(spike_window_ticks));
	memset(spike_window_count, 0, sizeof(spike_window_count));
}

void Profiling::Spike::frame(bool mods_disabled)
{
	INT64 ticks[NUM_CATEGORIES][2];
	unsigned count[NUM_CATEGORIES][2];
	unsigned counters[ARRAYSIZE(spike_counters)];
	LONG reasons[NUM_REASONS];
	vector<SpikeEvent> events;
	unsigned events_dropped;
	LARGE_INTEGER now;
	SYSTEMTIME time;

	if (!threshold)
		return;

	QueryPerformanceCounter(&now);
	INT64 frame_ticks = now.QuadPart - spike_last_present;
	bool first_frame = !spike_last_present;
	spike_last_present = now.QuadPart;
	spike_present_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);

	for (int i = 0; i < NUM_CATEGORIES; i++) {
		for (int thread = 0; thread < 2; thread++) {
			ticks[i][thread] = spike_totals[i][thread].ticks.exchange(0, std::memory_order_relaxed);
			count[i][thread] = spike_totals[i][thread].count.exchange(0, std::memory_order_relaxed);
		}
	}

	for (size_t i = 0; i < ARRAYSIZE(spike_counters); i++) {
		unsigned current = *spike_counters[i].counter;
		// The performance monitor zeroes these whenever it starts a new interval:
		counters[i] = current >= spike_counters_prev[i] ? current - spike_counters_prev[i] : current;
		spike_counters_prev[i] = current;
	}

	for (unsigned i = 0; i < NUM_REASONS; i++)
		reasons[i] = InterlockedExchange(&reason_counts[i], 0);

	AcquireSRWLockExclusive(&spike_events_lock);
	events.swap(spike_events);
	events_dropped = spike_events_dropped;
	spike_events_dropped = 0;
	ReleaseSRWLockExclusive(&spike_events_lock);

	if (first_frame)
		return;

	double frame_ms = spike_ms(frame_ticks);

	if (mods_disabled != spike_mods_disabled) {
		spike_log_window();
		spike_mods_disabled = mods_disabled;
	}
	spike_window_frames.push_back((float)frame_ms);
	spike_window_total_ms += frame_ms;
	for (unsigned i = 0; i < NUM_REASONS; i++)
		spike_window_reasons[i] += reasons[i];
	for (int i = 0; i < NUM_CATEGORIES; i++) {
		for (int thread = 0; thread < 2; thread++) {
			spike_window_ticks[i][thread] += ticks[i][thread];
			spike_window_count[i][thread] += count[i][thread];
		}
	}

	bool slow = frame_ticks >= threshold
		|| (spike_relative && spike_average_ms && frame_ms >= spike_average_ms * spike_relative);
	if (slow)
		spike_window_spikes++;
	if (spike_window_total_ms >= spike_window_ms)
		spike_log_window();

	if (!slow) {
		if (spike_average_ms == 0)
			spike_average_ms = frame_ms;
		else
			spike_average_ms += (frame_ms - spike_average_ms) * 0.05;
		return;
	}

	GetLocalTime(&time);
	fwprintf(spike_log, L"[%02u:%02u:%02u.%03u] frame %u: %.2fms (recent average %.2fms)%ls\n",
			time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
			G->frame_no, frame_ms, spike_average_ms, mods_disabled ? L", mods OFF" : L"");

	INT64 accounted = 0;
	for (int i = 0; i < FIRST_NESTED; i++)
		accounted += ticks[i][0];
	fwprintf(spike_log, L"  Render thread:\n");
	spike_log_totals(NULL, ticks, count, 0, FIRST_NESTED, 0);
	fwprintf(spike_log, L"  %10.2fms  outside 3DMigoto (game, driver)\n",
			spike_ms(frame_ticks > accounted ? frame_ticks - accounted : 0));
	spike_log_totals(L"Included in the above", ticks, count, FIRST_NESTED, NUM_CATEGORIES, 0);
	spike_log_totals(L"Other threads", ticks, count, 0, NUM_CATEGORIES, 1);

	if (!events.empty()) {
		std::sort(events.begin(), events.end(), [](const SpikeEvent &lhs, const SpikeEvent &rhs) {
			return lhs.ticks > rhs.ticks;
		});
		fwprintf(spike_log, L"  Slowest operations:\n");
		size_t logged = events.size() < spike_max_events_logged ? events.size() : spike_max_events_logged;
		for (size_t i = 0; i < logged; i++) {
			fwprintf(spike_log, L"  %10.2fms  %ls: %ls%ls\n",
					spike_ms(events[i].ticks), spike_category_names[events[i].category],
					events[i].name.c_str(), events[i].other_thread ? L" (other thread)" : L"");
		}
		if (events.size() + events_dropped > logged)
			fwprintf(spike_log, L"              ... and %Iu more\n", events.size() + events_dropped - logged);
	}

	bool reasons_logged = false;
	for (unsigned i = 0; i < NUM_REASONS; i++) {
		if (!reasons[i])
			continue;
		fwprintf(spike_log, L"%ls%ls %d", reasons_logged ? L", " : L"  Stopped for: ", reason_names[i], reasons[i]);
		reasons_logged = true;
	}
	if (reasons_logged)
		fwprintf(spike_log, L"\n");

	bool counters_logged = false;
	for (size_t i = 0; i < ARRAYSIZE(spike_counters); i++) {
		if (!counters[i])
			continue;
		fwprintf(spike_log, L"%ls%ls %u", counters_logged ? L", " : L"  Counters: ", spike_counters[i].name, counters[i]);
		counters_logged = true;
	}
	fwprintf(spike_log, counters_logged ? L"\n\n" : L"\n");
	fflush(spike_log);
}
