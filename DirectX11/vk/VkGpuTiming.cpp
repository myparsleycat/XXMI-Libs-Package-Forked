#include "VkGpuTiming.h"

#include <deque>
#include <unordered_map>
#include <vector>

#include "../profiling.h"

namespace {

// Per queue family. Even indexes are written before a batch, odd ones after it.
const uint32_t QUERIES = 4096;
const size_t MAX_PENDING_FRAMES = 64;

struct FamilyTiming {
	VkQueryPool query_pool = VK_NULL_HANDLE;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkCommandBuffer reset_all = VK_NULL_HANDLE;
	bool reset_all_submitted = false;
	uint64_t valid_mask = 0;
	// One per query, recorded when first used: it resets the query and
	// writes it
	std::vector<VkCommandBuffer> markers;
	// A query that is handed out again is still available with what it
	// had before until the GPU gets to its reset, so a result counts only
	// once it is later than that
	std::vector<uint64_t> last_value;
	// Queries handed out and queries whose results were taken, counting on
	// from the start: a query is handed out again once its result was taken
	uint64_t written = 0;
	uint64_t taken = 0;
	bool usable = false;
};

struct Pair {
	VkQueue queue;
	uint32_t family;
	uint32_t query;
};

struct Frame {
	std::vector<Pair> pairs;
	unsigned batches = 0;
	bool complete = true;
	unsigned frame_no = 0;
};

SRWLOCK timing_lock = SRWLOCK_INIT;
VkGpuTimingApi api;
VkDevice timing_device;
VkPhysicalDevice timing_physical_device;
double ns_per_tick;
std::vector<VkQueueFamilyProperties> family_properties;
std::unordered_map<VkQueue, uint32_t> queue_families;
std::unordered_map<uint32_t, FamilyTiming> families;
Frame current;
std::deque<Frame> pending;
bool unusable;

void forget_family(FamilyTiming *family)
{
	if (family->command_pool)
		api.DestroyCommandPool(timing_device, family->command_pool, NULL);
	if (family->query_pool)
		api.DestroyQueryPool(timing_device, family->query_pool, NULL);
	*family = FamilyTiming();
}

bool record(FamilyTiming *family, VkCommandBuffer cmd, uint32_t first, uint32_t count, VkPipelineStageFlagBits stage)
{
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

	// Still pending from the batch it was last submitted with while that
	// batch runs on past the timestamp
	begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
	if (api.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
		return false;
	api.CmdResetQueryPool(cmd, family->query_pool, first, count);
	if (count == 1)
		api.CmdWriteTimestamp(cmd, stage, family->query_pool, first);
	return api.EndCommandBuffer(cmd) == VK_SUCCESS;
}

// With timing_lock held
FamilyTiming* family_timing(uint32_t index)
{
	auto found = families.find(index);
	if (found != families.end())
		return found->second.usable ? &found->second : NULL;

	FamilyTiming *family = &families[index];
	if (index >= family_properties.size() || !family_properties[index].timestampValidBits)
		return NULL;
	uint32_t bits = family_properties[index].timestampValidBits;
	family->valid_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;

	VkQueryPoolCreateInfo query_info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	query_info.queryCount = QUERIES;
	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pool_info.queueFamilyIndex = index;
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;

	if (api.CreateQueryPool(timing_device, &query_info, NULL, &family->query_pool) != VK_SUCCESS
	    || api.CreateCommandPool(timing_device, &pool_info, NULL, &family->command_pool) != VK_SUCCESS
	    || (allocate.commandPool = family->command_pool,
		api.AllocateCommandBuffers(timing_device, &allocate, &family->reset_all) != VK_SUCCESS)
	    || !record(family, family->reset_all, 0, QUERIES, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)) {
		VkLogInfo("GPU timing for the spike log is not available on queue family %u\n", index);
		forget_family(family);
		return NULL;
	}
	family->markers.assign(QUERIES, VK_NULL_HANDLE);
	family->last_value.assign(QUERIES, 0);
	family->usable = true;
	return family;
}

VkCommandBuffer marker(FamilyTiming *family, uint32_t query)
{
	VkCommandBufferAllocateInfo allocate = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBuffer cmd;

	if (family->markers[query])
		return family->markers[query];
	allocate.commandPool = family->command_pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	if (api.AllocateCommandBuffers(timing_device, &allocate, &cmd) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	if (!record(family, cmd, query, 1, query & 1 ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT))
		return VK_NULL_HANDLE;
	family->markers[query] = cmd;
	return cmd;
}

// With timing_lock held: the two command buffers to put around a batch, or
// false if the GPU is too far behind for a pair of queries to be free
bool take_pair(VkQueue queue, uint32_t index, FamilyTiming *family, VkCommandBuffer *before, VkCommandBuffer *after)
{
	uint32_t query;

	if (family->written + 2 - family->taken > QUERIES)
		return false;
	query = (uint32_t)(family->written % QUERIES);
	*before = marker(family, query);
	*after = marker(family, query + 1);
	if (!*before || !*after)
		return false;
	family->written += 2;
	current.pairs.push_back({queue, index, query});
	return true;
}

// Whether the batches of a submission take command buffers of ours
bool chain_allows(const void *chain)
{
	const VkBaseInStructure *next;

	for (next = (const VkBaseInStructure*)chain; next; next = next->pNext) {
		switch (next->sType) {
			case VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO:
			case VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR:
			case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR:
			case VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_NV:
				break;
			default:
				// A device group mask per command buffer, a
				// protected submission and the like
				return false;
		}
	}
	return true;
}

// With timing_lock held
FamilyTiming* queue_timing(VkQueue queue, uint32_t *index)
{
	if (unusable || !timing_device)
		return NULL;
	auto family = queue_families.find(queue);
	if (family == queue_families.end())
		return NULL;
	*index = family->second;
	return family_timing(family->second);
}

// With timing_lock held: whether the query has a result from this time round
bool result(FamilyTiming *family, uint32_t query, uint64_t *value)
{
	uint64_t data[2];

	if (api.GetQueryPoolResults(timing_device, family->query_pool, query, 1, sizeof(data), data, sizeof(data),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != VK_SUCCESS || !data[1])
		return false;
	data[0] &= family->valid_mask;
	if (family->last_value[query] && data[0] <= family->last_value[query])
		return false;
	*value = data[0];
	return true;
}

struct Interval {
	uint64_t start;
	uint64_t end;
};

// With timing_lock held: frames whose results are all there, in order
void take_results()
{
	std::unordered_map<VkQueue, std::vector<Interval>> queues;

	while (!pending.empty()) {
		Frame &frame = pending.front();
		bool ready = true;

		queues.clear();
		for (const Pair &pair : frame.pairs) {
			FamilyTiming &family = families[pair.family];
			Interval interval;

			if (!result(&family, pair.query, &interval.start) || !result(&family, pair.query + 1, &interval.end)) {
				ready = false;
				break;
			}
			queues[pair.queue].push_back(interval);
		}
		if (!ready)
			return;

		// Batches of one queue run in the order they were submitted,
		// and may overlap: the time the queue was busy is their union
		double busiest_ms = 0, busiest_span_ms = 0;
		for (auto &queue : queues) {
			std::vector<Interval> &intervals = queue.second;
			uint64_t busy = 0, first_start = intervals[0].start, last_end = 0;

			for (const Interval &interval : intervals) {
				uint64_t s = interval.start > last_end ? interval.start : last_end;
				if (interval.end > s)
					busy += interval.end - s;
				if (interval.end > last_end)
					last_end = interval.end;
			}
			double busy_ms = busy * ns_per_tick / 1e6;
			if (busy_ms >= busiest_ms) {
				busiest_ms = busy_ms;
				busiest_span_ms = last_end > first_start ? (last_end - first_start) * ns_per_tick / 1e6 : 0;
			}
		}

		for (const Pair &pair : frame.pairs) {
			FamilyTiming &family = families[pair.family];
			uint64_t value;

			// Read again rather than kept: only the newest counts
			if (result(&family, pair.query, &value))
				family.last_value[pair.query] = value;
			if (result(&family, pair.query + 1, &value))
				family.last_value[pair.query + 1] = value;
			family.taken += 2;
		}
		if (!frame.pairs.empty()) {
			Profiling::Spike::gpu_frame(frame.frame_no, busiest_ms, busiest_span_ms,
					frame.batches, (unsigned)queues.size(), frame.complete);
		}
		pending.pop_front();
	}
}

thread_local std::vector<VkSubmitInfo> marked_submits;
thread_local std::vector<VkSubmitInfo2> marked_submits2;
thread_local std::vector<VkCommandBuffer> marked_cmds;
thread_local std::vector<VkCommandBufferSubmitInfo> marked_cmd_infos;

}

void VkGpuTimingDevice(VkDevice device, VkPhysicalDevice physical_device, const VkGpuTimingApi *timing_api)
{
	VkPhysicalDeviceProperties properties;
	uint32_t count = 0;

	AcquireSRWLockExclusive(&timing_lock);
	if (!timing_device) {
		timing_device = device;
		timing_physical_device = physical_device;
		api = *timing_api;
		unusable = !api.GetPhysicalDeviceProperties || !api.GetPhysicalDeviceQueueFamilyProperties
			|| !api.CreateQueryPool || !api.DestroyQueryPool || !api.GetQueryPoolResults
			|| !api.CreateCommandPool || !api.DestroyCommandPool || !api.AllocateCommandBuffers
			|| !api.BeginCommandBuffer || !api.EndCommandBuffer || !api.CmdResetQueryPool || !api.CmdWriteTimestamp;
		if (!unusable) {
			api.GetPhysicalDeviceProperties(physical_device, &properties);
			ns_per_tick = properties.limits.timestampPeriod;
			api.GetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
			family_properties.resize(count);
			api.GetPhysicalDeviceQueueFamilyProperties(physical_device, &count, family_properties.data());
		}
	}
	ReleaseSRWLockExclusive(&timing_lock);
}

void VkGpuTimingForgetDevice(VkDevice device)
{
	AcquireSRWLockExclusive(&timing_lock);
	if (device == timing_device) {
		for (auto &family : families)
			forget_family(&family.second);
		families.clear();
		queue_families.clear();
		family_properties.clear();
		current = Frame();
		pending.clear();
		timing_device = VK_NULL_HANDLE;
		unusable = false;
	}
	ReleaseSRWLockExclusive(&timing_lock);
}

void VkGpuTimingQueue(VkDevice device, VkQueue queue, uint32_t family)
{
	AcquireSRWLockExclusive(&timing_lock);
	if (device == timing_device)
		queue_families[queue] = family;
	ReleaseSRWLockExclusive(&timing_lock);
}

const VkSubmitInfo* VkGpuTimingSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits)
{
	FamilyTiming *family;
	uint32_t index, i, j;
	size_t cmds = 0;

	if (!Profiling::Spike::threshold || !count)
		return submits;

	AcquireSRWLockExclusive(&timing_lock);
	if (!(family = queue_timing(queue, &index))) {
		ReleaseSRWLockExclusive(&timing_lock);
		return submits;
	}

	for (i = 0; i < count; i++)
		cmds += submits[i].commandBufferCount + 3;
	marked_submits.assign(submits, submits + count);
	marked_cmds.clear();
	// The batches point into it
	marked_cmds.reserve(cmds);

	for (i = 0; i < count; i++) {
		VkSubmitInfo &batch = marked_submits[i];
		VkCommandBuffer before, after;
		size_t first = marked_cmds.size();

		if (!batch.commandBufferCount)
			continue;
		current.batches++;
		if (!chain_allows(batch.pNext) || !take_pair(queue, index, family, &before, &after)) {
			current.complete = false;
			continue;
		}
		if (!family->reset_all_submitted) {
			family->reset_all_submitted = true;
			marked_cmds.push_back(family->reset_all);
		}
		marked_cmds.push_back(before);
		for (j = 0; j < batch.commandBufferCount; j++)
			marked_cmds.push_back(batch.pCommandBuffers[j]);
		marked_cmds.push_back(after);
		batch.commandBufferCount = (uint32_t)(marked_cmds.size() - first);
		batch.pCommandBuffers = marked_cmds.data() + first;
	}
	ReleaseSRWLockExclusive(&timing_lock);
	return marked_submits.data();
}

const VkSubmitInfo2* VkGpuTimingSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits)
{
	VkCommandBufferSubmitInfo info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	FamilyTiming *family;
	uint32_t index, i, j;
	size_t cmds = 0;

	if (!Profiling::Spike::threshold || !count)
		return submits;

	AcquireSRWLockExclusive(&timing_lock);
	if (!(family = queue_timing(queue, &index))) {
		ReleaseSRWLockExclusive(&timing_lock);
		return submits;
	}

	for (i = 0; i < count; i++)
		cmds += submits[i].commandBufferInfoCount + 3;
	marked_submits2.assign(submits, submits + count);
	marked_cmd_infos.clear();
	marked_cmd_infos.reserve(cmds);

	for (i = 0; i < count; i++) {
		VkSubmitInfo2 &batch = marked_submits2[i];
		VkCommandBuffer before, after;
		size_t first = marked_cmd_infos.size();

		if (!batch.commandBufferInfoCount)
			continue;
		current.batches++;
		if ((batch.flags & VK_SUBMIT_PROTECTED_BIT) || !chain_allows(batch.pNext)
		    || !take_pair(queue, index, family, &before, &after)) {
			current.complete = false;
			continue;
		}
		if (!family->reset_all_submitted) {
			family->reset_all_submitted = true;
			info.commandBuffer = family->reset_all;
			marked_cmd_infos.push_back(info);
		}
		info.commandBuffer = before;
		marked_cmd_infos.push_back(info);
		for (j = 0; j < batch.commandBufferInfoCount; j++)
			marked_cmd_infos.push_back(batch.pCommandBufferInfos[j]);
		info.commandBuffer = after;
		marked_cmd_infos.push_back(info);
		batch.commandBufferInfoCount = (uint32_t)(marked_cmd_infos.size() - first);
		batch.pCommandBufferInfos = marked_cmd_infos.data() + first;
	}
	ReleaseSRWLockExclusive(&timing_lock);
	return marked_submits2.data();
}

void VkGpuTimingPresent(VkQueue queue, unsigned frame_no)
{
	AcquireSRWLockExclusive(&timing_lock);
	if (timing_device && !unusable) {
		if (current.batches) {
			current.frame_no = frame_no;
			pending.push_back(std::move(current));
		}
		current = Frame();
		// Results that never come, from a submission that failed,
		// would hold back all the frames after them
		while (pending.size() > MAX_PENDING_FRAMES) {
			for (const Pair &pair : pending.front().pairs)
				families[pair.family].taken += 2;
			pending.pop_front();
		}
		take_results();
	}
	ReleaseSRWLockExclusive(&timing_lock);
}
