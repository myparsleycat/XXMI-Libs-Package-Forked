#pragma once

#include "VkCommon.h"

// For the spike log: how long the GPU spends on each frame on each queue.
// Each batch the game submits gets a command buffer of ours before and after
// it that writes a timestamp, and the results are read without waiting a few
// frames later. A queue of a family without timestamps is not measured.

struct VkGpuTimingApi {
	PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
	PFN_vkCreateQueryPool CreateQueryPool;
	PFN_vkDestroyQueryPool DestroyQueryPool;
	PFN_vkGetQueryPoolResults GetQueryPoolResults;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCmdResetQueryPool CmdResetQueryPool;
	PFN_vkCmdWriteTimestamp CmdWriteTimestamp;
};

void VkGpuTimingDevice(VkDevice device, VkPhysicalDevice physical_device, const VkGpuTimingApi *api);
// Before the device is destroyed, when the game has to have waited for it
void VkGpuTimingForgetDevice(VkDevice device);
void VkGpuTimingQueue(VkDevice device, VkQueue queue, uint32_t family);

// What to pass on in place of the batches the game submits: a copy with our
// command buffers around them, or the batches given. The copy lives until the
// next call on the same thread.
const VkSubmitInfo* VkGpuTimingSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits);
const VkSubmitInfo2* VkGpuTimingSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits);

// The submissions since the previous Present make up the frame the spike log
// knows by frame_no
void VkGpuTimingPresent(VkQueue queue, unsigned frame_no);
