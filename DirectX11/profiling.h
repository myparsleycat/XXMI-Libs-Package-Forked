#pragma once

#include <wrl.h>
#include <string>

namespace Profiling {
	enum class Mode {
		NONE = 0,
		SUMMARY,
		TOP_COMMAND_LISTS,
		TOP_COMMANDS,
		CTO_WARNING,

		INVALID, // Must be last
	};

	class Overhead {
	public:
		LARGE_INTEGER cpu;
		unsigned count, hits;

		void clear();
	};

	struct State {
		LARGE_INTEGER start_time;
	};

	static inline void start(State *state)
	{
		QueryPerformanceCounter(&state->start_time);
	}

	static inline void end(State *state, Profiling::Overhead *overhead)
	{
		LARGE_INTEGER end_time;

		QueryPerformanceCounter(&end_time);
		overhead->cpu.QuadPart += end_time.QuadPart - state->start_time.QuadPart;
	}

	template<class T>
	static inline typename T::iterator lookup_map(T &map, typename T::key_type key, Profiling::Overhead *overhead)
	{
		Profiling::State state;

		if (Profiling::mode == Profiling::Mode::SUMMARY) {
			overhead->count++;
			Profiling::start(&state);
		}
		auto ret = map.find(key);
		if (Profiling::mode == Profiling::Mode::SUMMARY) {
			Profiling::end(&state, overhead);
			if (ret != end(map))
				overhead->hits++;
		}
		return ret;
	}

	void update_txt();
	void update_cto_warning(bool warn);
	void clear();

	extern Mode mode;
	extern Overhead present_overhead;
	extern Overhead overlay_overhead;
	extern Overhead draw_overhead;
	extern Overhead region_tracking_overhead;
	extern Overhead map_overhead;
	extern Overhead hash_tracking_overhead;
	extern Overhead stat_overhead;
	extern Overhead shaderregex_overhead;
	extern Overhead cursor_overhead;
	extern std::wstring text;
	extern std::wstring cto_warning;
	extern INT64 interval;
	extern bool freeze;

	extern Overhead shader_hash_lookup_overhead;
	extern Overhead shader_reload_lookup_overhead;
	extern Overhead shader_original_lookup_overhead;
	extern Overhead shaderoverride_lookup_overhead;
	extern Overhead texture_handle_info_lookup_overhead;
	extern Overhead textureoverride_lookup_overhead;
	extern Overhead resource_pool_lookup_overhead;
	// Building a resource's cached candidate lists (rare: first sight of the
	// resource, a hash change, or after config reload invalidates them):
	extern Overhead texture_override_fuzzy_match_overhead;
	// Looking up (and per-draw filtering) those already-cached candidates,
	// which is what actually runs on most draw calls:
	extern Overhead texture_override_candidates_lookup_overhead;

	extern unsigned resource_full_copies;
	extern unsigned resource_reference_copies;
	extern unsigned inter_device_copies;
	extern unsigned msaa_resolutions;
	extern unsigned buffer_region_copies;
	extern unsigned views_cleared;
	extern unsigned resources_created;
	extern unsigned resources_shared;
	extern unsigned resource_pool_swaps;
	extern unsigned max_copies_per_frame_exceeded;
	extern unsigned injected_draw_calls;
	extern unsigned skipped_draw_calls;
	extern unsigned max_executions_per_frame_exceeded;
	extern unsigned iniparams_updates;

	// Spike log: the performance monitor above averages over its interval,
	// which hides a single slow frame. This instead keeps per-frame totals
	// that are thrown away at every Present unless the frame took longer
	// than the threshold, in which case they are written to
	// d3d11_spike_log.txt along with the individual slow operations.
	namespace Spike {
		enum Category {
			// Mutually exclusive on the render thread, so the frame
			// time left after subtracting these was spent outside
			// 3DMigoto (game or driver):
			PRESENT_PRE,
			PRESENT_CALL,
			PRESENT_POST,
			DRAW_PRE,
			DRAW_POST,
			MAP,
			SHADER_CREATE,
			RESOURCE_CREATE,
			DX12_APPLY,
			DX12_EXECUTE,
			DX12_EXECUTE_DRIVER,
			DX12_PIPELINE,
			DX12_PIPELINE_DRIVER,
			DX12_TEXTURE_HASH,
			DX12_BUFFER_HASH,
			// Nested inside the above (or inside each other):
			RESOURCE_LOAD,
			SHADER_CREATE_DRIVER,
			SHADER_REGEX,
			SHADER_REGEX_CACHE,
			SHADER_REGEX_ANALYSIS,
			SHADER_REGEX_DRIVER,
			CONFIG_RELOAD,
			LOCK_WAIT,
			DX12_DRAW,
			DX12_DISPATCH,
			DX12_SUBMIT,
			DX12_GPU_WAIT,
			DX12_MOD_PIPELINE,
			DX12_MOD_RESOURCE,
			DX12_RENAME,
			DX12_UPKEEP,
			DX12_REACH,
			DX12_FILTER,
			DX12_DESCRIPTORS,
			DX12_MIRROR,
			RESOURCE_FILE_READ,
			RESOURCE_FILE_SHARE,
			DX12_UPLOAD_COPY,
			VK_DRAW,
			VK_TEXTURE_IMAGE,
			VK_TEXTURE_COPY,
			VK_MEMORY,
			VK_TRANSFERS,
			COMMAND_LIST,
			VK_CB_LOOKUP,
			VK_SRV_LOOKUP,
			VK_BUFFER_PUBLISH,
			VK_REGION_HASH,
			VK_CONSTANT_READ,
			VK_SHADOW_FILL,
			VK_SHADOW_UPLOAD,
			VK_REFRESH,
			VK_REFRESH_WAIT,
			VK_REFRESH_UPLOAD,
			VK_MOD_BINDINGS,
			VK_RECORD_DRAWS,

			NUM_CATEGORIES, // Must be last
		};
		const Category FIRST_NESTED = RESOURCE_LOAD;

		// In QueryPerformanceCounter ticks, 0 while disabled:
		extern INT64 threshold;

		// A frame is logged for taking threshold_ms, or relative times
		// the recent average, whichever is given
		void init(float threshold_ms, float relative, const wchar_t *log_dir);
		void add(Category category, INT64 start, const wchar_t *name, UINT64 id, UINT64 bytes = 0);
		void frame(bool mods_disabled = false);
		void configure_mod_state(const wchar_t *variable);
		void sample_mod_state();

		// Things counted per frame for whoever has names for them,
		// listed with the summaries and the slow frames
		const unsigned NUM_REASONS = 32;
		extern const wchar_t *reason_names[NUM_REASONS];
		extern volatile LONG reason_counts[NUM_REASONS];
		static inline void reason(unsigned index, const wchar_t *name)
		{
			if (!threshold || index >= NUM_REASONS)
				return;
			reason_names[index] = name;
			InterlockedIncrement(&reason_counts[index]);
		}

		class Scope {
			LARGE_INTEGER start;
			Category category;
			// Only read if the operation turns out slow enough to
			// be listed by itself, so has to outlive the scope:
			const wchar_t *name;
			UINT64 id;
			UINT64 bytes;
		public:
			Scope(Category category, const wchar_t *name = NULL, UINT64 id = 0) :
				category(category), name(name), id(id), bytes(0)
			{
				start.QuadPart = 0;
				if (threshold)
					QueryPerformanceCounter(&start);
			}

			~Scope()
			{
				if (start.QuadPart)
					add(category, start.QuadPart, name, id, bytes);
			}

			void add_bytes(UINT64 count) { bytes += count; }
		};
	}

}
