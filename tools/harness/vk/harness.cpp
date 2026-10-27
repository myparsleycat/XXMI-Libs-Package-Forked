// A small Vulkan program that renders like a game would for a mesh: shaders
// from SPIR-V (shader.vert and shader.frag as vs.spv and ps.spv, build.bat
// compiles them), a vertex and an index buffer uploaded through a staging
// buffer, an indexed draw into a swap chain. For checking the Vulkan side of
// d3d11.dll without a game.
//
//   harness.exe [seconds] [--instance] [--pool] [--nodll] [--noext] [--draws N]
//               [--rewrite] [--bonetick] [--unmapped | --map2 | --gpufilled]
//               [--upload late|unmapped|discard|fill|srcfill|lastfill]
//               [--generated N [--overlay]] [--reload 1|2|3] [--profile]
//               [--efmi file [--characters N] [--passes N] [--appear N]] [--lowmip [--wide]]
//
// --instance gets the device's functions from vkGetInstanceProcAddr instead
// of vkGetDeviceProcAddr, which are the loader's exports. --pool draws the
// mesh from one mapped buffer that was filled without a copy. --noext leaves
// out the external memory extensions. --profile samples the recording thread
// (STEADY in the environment: the steady frames of --appear). The other
// options are described where main declares them. Prints the pixels at the
// middle and a corner and the checksum of the last picture; exits with 1 when
// a Vulkan call fails or an argument is unknown.

#define _CRT_SECURE_NO_WARNINGS
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <thread>
#include <functional>
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

// The sampling profiler of the D3D12 harness: --profile, for what is between
// beginning the command buffer of a frame and having handed it to the queue
#include "../common/profile.inc"

#define INSTANCE_FUNCS(X) \
	X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceQueueFamilyProperties) \
	X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) X(GetDeviceProcAddr) X(CreateWin32SurfaceKHR) \
	X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR) X(GetPhysicalDeviceSurfaceCapabilitiesKHR) \
	X(GetPhysicalDeviceSurfaceFormatsKHR) X(GetPhysicalDeviceProperties)

#define DEVICE_FUNCS(X) \
	X(DestroyDevice) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) \
	X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) \
	X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule) \
	X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
	X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
	X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(CreateCommandPool) X(DestroyCommandPool) \
	X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdCopyBuffer) X(CmdBeginRenderPass) \
	X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed) \
	X(CmdSetViewport) X(CmdSetScissor) X(QueueSubmit) X(QueueWaitIdle) X(DeviceWaitIdle) X(CreateSemaphore) \
	X(DestroySemaphore) X(ResetCommandBuffer) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
	X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
	X(CmdBindDescriptorSets) X(CmdPipelineBarrier) X(CmdCopyImageToBuffer) X(CreateImage) X(DestroyImage) \
	X(GetImageMemoryRequirements) X(BindImageMemory) X(CmdCopyBufferToImage) X(CreateSampler) X(DestroySampler) \
	X(CmdFillBuffer) X(CmdCopyImage)

static struct {
#define X(name) PFN_vk##name name;
	INSTANCE_FUNCS(X)
	DEVICE_FUNCS(X)
#undef X
} vk;

#define CHECK(call) \
	do { \
		VkResult check_result = (call); \
		if (check_result != VK_SUCCESS) { \
			printf("FAILED %s = %d\n", #call, check_result); \
			exit(1); \
		} \
	} while (0)

static std::vector<uint32_t> load(const char *name)
{
	std::vector<uint32_t> code;
	FILE *file = fopen(name, "rb");
	long bytes;

	if (!file) {
		printf("FAILED no %s\n", name);
		exit(1);
	}
	fseek(file, 0, SEEK_END);
	bytes = ftell(file);
	fseek(file, 0, SEEK_SET);
	code.resize(bytes / 4);
	fread(code.data(), 4, code.size(), file);
	fclose(file);
	return code;
}

static VkPhysicalDeviceMemoryProperties memory_properties;
static VkDevice device;

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags wanted, VkDeviceMemory *memory)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryAllocateInfo allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;
	VkBuffer buffer;
	uint32_t i;

	info.size = size;
	info.usage = usage;
	CHECK(vk.CreateBuffer(device, &info, NULL, &buffer));
	vk.GetBufferMemoryRequirements(device, buffer, &requirements);

	for (i = 0; i < memory_properties.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i))
		    && (memory_properties.memoryTypes[i].propertyFlags & wanted) == wanted)
			break;
	}
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = i;
	CHECK(vk.AllocateMemory(device, &allocate, NULL, memory));
	CHECK(vk.BindBufferMemory(device, buffer, *memory, 0));
	return buffer;
}

struct Vertex {
	float pos[3];
	float color[4];
	unsigned char indices[4];
};

int main(int argc, char **argv)
{
	static const Vertex vertices[4] = {
		{ { -0.5f, -0.5f, 0.5f }, { 1, 0, 0, 1 } },
		{ { 0.5f, -0.5f, 0.5f }, { 0, 1, 0, 1 } },
		{ { 0.5f, 0.5f, 0.5f }, { 0, 0, 1, 1 } },
		{ { -0.5f, 0.5f, 0.5f }, { 1, 1, 0, 1 } },
	};
	static const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
	bool instance_path = false, load_dll = true, pool_path = false, share = true, profile = false;
	int draws = 1;
	// Presents between those of the frames drawn, each with a submission
	// that draws nothing, as frame generation makes them
	int generated = 0;
	// --overlay: those presents are made by a thread of their own, as frame
	// generation's pacer does, and their submissions draw 3 indices with the
	// game's pipeline, as an overlay over every picture presented does
	bool overlay = false;
	// --unmapped: the memory the texture is filled through is unmapped before
	// the copy is recorded, as the game does for many of its textures
	bool unmapped = false;
	// --map2: that memory is mapped with vkMapMemory2KHR and stays mapped
	bool map2 = false;
	// --gpufilled: the GPU fills that buffer with vkCmdCopyBuffer just before
	// the copy to the texture; the CPU never maps it
	bool gpufilled = false;
	// --lowmip: the texture is filled by vkCmdCopyImage from the second level
	// of one twice its size with two levels, as texture streaming makes a
	// texture without its top level
	bool lowmip = false;
	// --wide: with --lowmip, that one is 4x1 and the texture 2x1, a side of
	// 1 being that of the level above it too
	bool wide = false;
	// --upload <how>: the order of the upload of vb and ib through the staging
	// buffer, for a draw without --pool. late: the staging buffer is filled
	// after the copies are recorded, before they are submitted. unmapped: it is
	// unmapped before they are recorded. discard: after the upload, a copy of
	// other indices to ib is recorded and the command buffer reset unsubmitted.
	// fill: after the upload, ib is filled with zeroes on the GPU. srcfill:
	// after the upload, the indices in the staging buffer are filled with
	// zeroes on the GPU and copied to ib again, in one submission. lastfill:
	// the command buffer of the last frame fills ib with zeroes before its
	// draw.
	const char *upload = "";
	// --rewrite: halfway through, the indices in the pool are written in
	// another order, which is other indices with another hash for the same draw
	bool rewrite = false;
	// --bonetick: the first bone of the pool is another every frame, the
	// last of which is printed, for the picture to be checked against
	bool bonetick = false;
	// --efmi <file>: the draws of characters as the game of the EFMI mods
	// makes them, for the parts the file lists by the hash and the number
	// of their indices, of --characters characters in --passes passes
	const char *efmi = NULL;
	// --appear N: the parts of the mod of the third column of the list are
	// drawn from frame 120 + mod * N for N frames, and the frames timed
	int appear = 0, groups = 0;
	struct Appearance { int group; double ms[4], record[4]; };
	std::vector<Appearance> appearances;
	int efmi_passes = 5, efmi_characters = 4;
	setvbuf(stdout, NULL, _IONBF, 0);
	double seconds = 8;
	int reload = 0;
	HMODULE loader;
	PFN_vkGetInstanceProcAddr get_instance;
	PFN_vkCreateInstance create_instance;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--instance"))
			instance_path = true;
		else if (!strcmp(argv[i], "--pool"))
			pool_path = true;
		else if (!strcmp(argv[i], "--nodll"))
			load_dll = false;
		else if (!strcmp(argv[i], "--noext"))
			share = false;
		else if (!strcmp(argv[i], "--profile"))
			profile = true;
		else if (!strcmp(argv[i], "--rewrite"))
			rewrite = true;
		else if (!strcmp(argv[i], "--bonetick"))
			bonetick = true;
		else if (!strcmp(argv[i], "--appear") && i + 1 < argc)
			appear = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--efmi") && i + 1 < argc)
			efmi = argv[++i];
		else if (!strcmp(argv[i], "--passes") && i + 1 < argc)
			efmi_passes = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--characters") && i + 1 < argc)
			efmi_characters = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--overlay"))
			overlay = true;
		else if (!strcmp(argv[i], "--unmapped"))
			unmapped = true;
		else if (!strcmp(argv[i], "--map2"))
			map2 = true;
		else if (!strcmp(argv[i], "--gpufilled"))
			gpufilled = true;
		else if (!strcmp(argv[i], "--lowmip"))
			lowmip = true;
		else if (!strcmp(argv[i], "--wide"))
			wide = true;
		else if (!strcmp(argv[i], "--upload") && i + 1 < argc && (!strcmp(argv[i + 1], "late")
		    || !strcmp(argv[i + 1], "unmapped") || !strcmp(argv[i + 1], "discard") || !strcmp(argv[i + 1], "fill")
		    || !strcmp(argv[i + 1], "srcfill") || !strcmp(argv[i + 1], "lastfill")))
			upload = argv[++i];
		else if (!strcmp(argv[i], "--generated") && i + 1 < argc)
			generated = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--reload") && i + 1 < argc)
			reload = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--draws") && i + 1 < argc)
			draws = atoi(argv[++i]);
		else {
			char *end;
			seconds = strtod(argv[i], &end);
			if (end == argv[i] || *end) {
				printf("unknown argument %s\n", argv[i]);
				return 1;
			}
		}
	}

	if (load_dll && !LoadLibraryA("d3d11.dll")) {
		printf("FAILED loading d3d11.dll: %lu\n", GetLastError());
		return 1;
	}

	// vulkan-1.dll loaded and unloaded before the real use, as a probe of an
	// engine would: 1 to load it again at the same address, 2 elsewhere
	if (reload) {
		// A D3D11 support check first, which opens the log of the DLL as a game would
		HRESULT (WINAPI *create_device)(void*, int, HMODULE, UINT, const void*, UINT, UINT, void**, void*, void**) =
			(HRESULT (WINAPI*)(void*, int, HMODULE, UINT, const void*, UINT, UINT, void**, void*, void**))GetProcAddress(GetModuleHandleA("d3d11.dll"), "D3D11CreateDevice");
		printf("d3d11.dll %p, D3D11CreateDevice %p\n", GetModuleHandleA("d3d11.dll"), create_device);
		if (create_device)
			printf("D3D11CreateDevice %p = 0x%lx\n", create_device, create_device(NULL, 1, NULL, 0, NULL, 0, 7, NULL, NULL, NULL));
		if (reload != 3) {
			HMODULE probe = LoadLibraryW(L"vulkan-1.dll");
			while (GetModuleHandleW(L"vulkan-1.dll"))
				FreeLibrary(probe);
			if (reload == 2 && !VirtualAlloc(probe, 0x10000, MEM_RESERVE, PAGE_NOACCESS))
				printf("FAILED reserving the old address of vulkan-1.dll\n");
			printf("vulkan-1.dll probed at %p and unloaded\n", probe);
		}
	}

	// --reload 3: the code the DLL patched in vulkan-1.dll put back as it was,
	// as whatever restores changed code would, with the DLL still loaded.
	// What is put back is found in a copy of the file mapped as an image,
	// along the jumps the exports start with there.
	if (reload == 3) {
		HMODULE live = LoadLibraryW(L"vulkan-1.dll");
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(live, path, MAX_PATH);
		// A copy, as mapping the loaded file again hands back the module
		wchar_t copy[MAX_PATH];
		GetTempPathW(MAX_PATH, copy);
		wcscat_s(copy, L"vulkan-1-pristine.dll");
		CopyFileW(path, copy, FALSE);
		HMODULE image = LoadLibraryExW(copy, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		printf("vulkan-1.dll %p, its copy mapped at %p\n", live, image);
		BYTE *pristine = (BYTE*)((ULONG_PTR)image & ~(ULONG_PTR)3);
		IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS*)((BYTE*)live + ((IMAGE_DOS_HEADER*)live)->e_lfanew);
		ptrdiff_t size = nt->OptionalHeader.SizeOfImage;
		const char *names[] = { "vkCreateInstance", "vkCreateDevice", "vkGetDeviceProcAddr" };
		for (const char *name : names) {
			ptrdiff_t offset = (BYTE*)GetProcAddress(live, name) - (BYTE*)live;
			int put_back = 0;
			for (int jump = 0; jump < 8 && offset >= 0 && offset + 16 <= size; jump++) {
				BYTE *at = (BYTE*)live + offset, *from = pristine + offset;
				if (memcmp(at, from, 16)) {
					DWORD old;
					VirtualProtect(at, 16, PAGE_EXECUTE_READWRITE, &old);
					memcpy(at, from, 16);
					VirtualProtect(at, 16, old, &old);
					put_back++;
				}
				if (from[0] == 0xE9)
					offset += 5 + *(INT32*)(from + 1);
				else if (from[0] == 0xEB)
					offset += 2 + *(INT8*)(from + 1);
				else
					break;
			}
			printf("%s: %d patched places put back\n", name, put_back);
		}
		FreeLibrary(image);
		LoadLibraryW(L"version.dll");
	}

	loader = LoadLibraryW(L"vulkan-1.dll");
	printf("vulkan-1.dll at %p\n", loader);
	get_instance = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
	create_instance = (PFN_vkCreateInstance)get_instance(NULL, "vkCreateInstance");

	const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	VkInstanceCreateInfo instance_info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	VkInstance instance;
	app.apiVersion = VK_API_VERSION_1_1;
	instance_info.pApplicationInfo = &app;
	instance_info.enabledExtensionCount = ARRAYSIZE(instance_extensions);
	instance_info.ppEnabledExtensionNames = instance_extensions;
	CHECK(create_instance(&instance_info, NULL, &instance));

#define X(name) vk.name = (PFN_vk##name)get_instance(instance, "vk" #name);
	INSTANCE_FUNCS(X)
#undef X

	WNDCLASSA window_class = {};
	window_class.lpfnWndProc = DefWindowProcA;
	window_class.hInstance = GetModuleHandle(NULL);
	window_class.lpszClassName = "VkHarness";
	RegisterClassA(&window_class);
	HWND window = CreateWindowA("VkHarness", "VkHarness", WS_OVERLAPPEDWINDOW, 100, 100, 640, 480,
		NULL, NULL, window_class.hInstance, NULL);
	ShowWindow(window, SW_SHOWNOACTIVATE);

	VkWin32SurfaceCreateInfoKHR surface_info = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	VkSurfaceKHR surface;
	surface_info.hinstance = window_class.hInstance;
	surface_info.hwnd = window;
	CHECK(vk.CreateWin32SurfaceKHR(instance, &surface_info, NULL, &surface));

	VkPhysicalDevice physical_devices[8];
	uint32_t physical_count = ARRAYSIZE(physical_devices);
	if (vk.EnumeratePhysicalDevices(instance, &physical_count, physical_devices) < 0 || !physical_count) {
		printf("FAILED no Vulkan device\n");
		return 1;
	}
	VkPhysicalDevice physical = physical_devices[0];
	VkPhysicalDeviceProperties properties;
	vk.GetPhysicalDeviceProperties(physical, &properties);
	printf("device: %s\n", properties.deviceName);
	vk.GetPhysicalDeviceMemoryProperties(physical, &memory_properties);

	VkQueueFamilyProperties families[16];
	uint32_t family_count = ARRAYSIZE(families), family = 0;
	vk.GetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
	for (family = 0; family < family_count; family++) {
		VkBool32 presents = VK_FALSE;
		vk.GetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &presents);
		if (presents && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			break;
	}
	if (family == family_count) {
		printf("FAILED no queue family that draws and presents\n");
		return 1;
	}

	// The last two as a game has them that shares memory with another API
	std::vector<const char*> device_extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME };
	if (!share)
		device_extensions.resize(1);
	if (map2)
		device_extensions.push_back(VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
	float priority = 1;
	VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	VkDeviceCreateInfo device_info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	queue_info.queueFamilyIndex = family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = (uint32_t)device_extensions.size();
	device_info.ppEnabledExtensionNames = device_extensions.data();
	CHECK(vk.CreateDevice(physical, &device_info, NULL, &device));

	if (instance_path) {
#define X(name) vk.name = (PFN_vk##name)get_instance(instance, "vk" #name);
		DEVICE_FUNCS(X)
#undef X
	} else {
#define X(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(device, "vk" #name);
		DEVICE_FUNCS(X)
#undef X
	}
#define X(name) if (!vk.name) { printf("FAILED no vk" #name "\n"); return 1; }
	DEVICE_FUNCS(X)
#undef X

	VkQueue queue;
	vk.GetDeviceQueue(device, family, 0, &queue);

	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[64];
	uint32_t format_count = ARRAYSIZE(formats);
	vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities);
	if (vk.GetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count, formats) < 0 || !format_count) {
		printf("FAILED no surface format\n");
		return 1;
	}

	VkSwapchainCreateInfoKHR swapchain_info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSwapchainKHR swapchain;
	swapchain_info.surface = surface;
	swapchain_info.minImageCount = capabilities.minImageCount < 2 ? 2 : capabilities.minImageCount;
	swapchain_info.imageFormat = formats[0].format;
	swapchain_info.imageColorSpace = formats[0].colorSpace;
	swapchain_info.imageExtent = capabilities.currentExtent;
	swapchain_info.imageArrayLayers = 1;
	// To read back what the last frame looks like
	swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	printf("format: %d\n", formats[0].format);
	swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_info.preTransform = capabilities.currentTransform;
	swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchain_info.clipped = VK_TRUE;
	CHECK(vk.CreateSwapchainKHR(device, &swapchain_info, NULL, &swapchain));

	VkImage images[16];
	uint32_t image_count = ARRAYSIZE(images);
	CHECK(vk.GetSwapchainImagesKHR(device, swapchain, &image_count, images));

	VkAttachmentDescription attachment = {};
	VkAttachmentReference reference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
	VkSubpassDescription subpass = {};
	VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
	VkRenderPass pass;
	attachment.format = formats[0].format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	pass_info.attachmentCount = 1;
	pass_info.pAttachments = &attachment;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	CHECK(vk.CreateRenderPass(device, &pass_info, NULL, &pass));

	VkImageView views[16];
	VkFramebuffer framebuffers[16];
	for (uint32_t n = 0; n < image_count; n++) {
		VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
		view_info.image = images[n];
		view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view_info.format = formats[0].format;
		view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		CHECK(vk.CreateImageView(device, &view_info, NULL, &views[n]));
		framebuffer_info.renderPass = pass;
		framebuffer_info.attachmentCount = 1;
		framebuffer_info.pAttachments = &views[n];
		framebuffer_info.width = capabilities.currentExtent.width;
		framebuffer_info.height = capabilities.currentExtent.height;
		framebuffer_info.layers = 1;
		CHECK(vk.CreateFramebuffer(device, &framebuffer_info, NULL, &framebuffers[n]));
	}

	std::vector<uint32_t> vs_code = load("vs.spv");
	std::vector<uint32_t> ps_code = load("ps.spv");
	VkShaderModuleCreateInfo module_info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule vs, ps;
	module_info.codeSize = vs_code.size() * 4;
	module_info.pCode = vs_code.data();
	CHECK(vk.CreateShaderModule(device, &module_info, NULL, &vs));
	module_info.codeSize = ps_code.size() * 4;
	module_info.pCode = ps_code.data();
	CHECK(vk.CreateShaderModule(device, &module_info, NULL, &ps));

	// The uniform block of the vertex shader, bound with an offset the way
	// an engine with one buffer for the constants of all its draws does
	// and the bones, as a storage buffer
	VkDescriptorSetLayoutBinding set_bindings[2] = {};
	VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorSetLayout set_layout;
	set_bindings[0].binding = 1;
	set_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	set_bindings[0].descriptorCount = 1;
	set_bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	set_bindings[1].binding = 2;
	set_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	set_bindings[1].descriptorCount = 1;
	set_bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	set_layout_info.bindingCount = 2;
	set_layout_info.pBindings = set_bindings;
	CHECK(vk.CreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));

	// What the fragment shader samples, in a set of its own
	VkDescriptorSetLayoutBinding texture_bindings[2] = {};
	VkDescriptorSetLayout set_layouts[2];
	texture_bindings[0].binding = 2;
	texture_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	texture_bindings[0].descriptorCount = 1;
	texture_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	texture_bindings[1].binding = 4;
	texture_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	texture_bindings[1].descriptorCount = 1;
	texture_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	set_layout_info.pBindings = texture_bindings;
	CHECK(vk.CreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layouts[1]));
	set_layouts[0] = set_layout;

	VkDescriptorPoolSize pool_size[4] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
	VkDescriptorPoolCreateInfo descriptor_pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorPool descriptor_pool;
	descriptor_pool_info.maxSets = 2;
	descriptor_pool_info.poolSizeCount = 4;
	descriptor_pool_info.pPoolSizes = pool_size;
	CHECK(vk.CreateDescriptorPool(device, &descriptor_pool_info, NULL, &descriptor_pool));

	VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkDescriptorSet descriptor_sets[2];
	set_info.descriptorPool = descriptor_pool;
	set_info.descriptorSetCount = 2;
	set_info.pSetLayouts = set_layouts;
	CHECK(vk.AllocateDescriptorSets(device, &set_info, descriptor_sets));
	VkDescriptorSet descriptor_set = descriptor_sets[0];

	VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkPipelineLayout layout;
	layout_info.setLayoutCount = 2;
	layout_info.pSetLayouts = set_layouts;
	CHECK(vk.CreatePipelineLayout(device, &layout_info, NULL, &layout));

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vs;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = ps;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding = { 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	VkVertexInputAttributeDescription attributes[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 },
		{ 2, 0, VK_FORMAT_R8G8B8A8_UINT, 28 },
	};
	VkPipelineVertexInputStateCreateInfo input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo viewport_state = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend_attachment = {};
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkDynamicState dynamic_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkGraphicsPipelineCreateInfo pipeline_info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	VkPipeline pipeline;
	input.vertexBindingDescriptionCount = 1;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 3;
	input.pVertexAttributeDescriptions = attributes;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	viewport_state.viewportCount = 1;
	viewport_state.scissorCount = 1;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	blend_attachment.colorWriteMask = 0xf;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = stages;
	pipeline_info.pVertexInputState = &input;
	pipeline_info.pInputAssemblyState = &assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState = &multisample;
	pipeline_info.pColorBlendState = &blend;
	pipeline_info.pDynamicState = &dynamic;
	pipeline_info.layout = layout;
	pipeline_info.renderPass = pass;
	CHECK(vk.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
	// As engines do once the pipeline has them
	vk.DestroyShaderModule(device, vs, NULL);
	vk.DestroyShaderModule(device, ps, NULL);

	// One staging buffer for both, as an allocator would hand out
	VkDeviceMemory staging_memory, vb_memory, ib_memory;
	VkBuffer staging = make_buffer(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT
		| (!strcmp(upload, "srcfill") ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging_memory);
	VkBuffer vb = make_buffer(sizeof(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vb_memory);
	VkBuffer ib = make_buffer(sizeof(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ib_memory);
	void *mapped;
	bool late = !strcmp(upload, "late"), staging_mapped = true;
	CHECK(vk.MapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
	if (!late) {
		memcpy((char*)mapped + 256, vertices, sizeof(vertices));
		memcpy((char*)mapped + 1024, indices, sizeof(indices));
	}
	if (!strcmp(upload, "unmapped")) {
		vk.UnmapMemory(device, staging_memory);
		staging_mapped = false;
	}

	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkCommandPool pool;
	VkCommandBuffer cmd;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = family;
	CHECK(vk.CreateCommandPool(device, &pool_info, NULL, &pool));
	cmd_info.commandPool = pool;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cmd_info.commandBufferCount = 1;
	CHECK(vk.AllocateCommandBuffers(device, &cmd_info, &cmd));
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;

	VkBufferCopy vb_copy = { 256, 0, sizeof(vertices) }, ib_copy = { 1024, 0, sizeof(indices) };
	CHECK(vk.BeginCommandBuffer(cmd, &begin));
	vk.CmdCopyBuffer(cmd, staging, vb, 1, &vb_copy);
	vk.CmdCopyBuffer(cmd, staging, ib, 1, &ib_copy);
	CHECK(vk.EndCommandBuffer(cmd));
	if (late) {
		memcpy((char*)mapped + 256, vertices, sizeof(vertices));
		memcpy((char*)mapped + 1024, indices, sizeof(indices));
	}
	CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
	CHECK(vk.QueueWaitIdle(queue));
	if (!strcmp(upload, "discard")) {
		static const uint16_t other[6] = { 0, 2, 3, 0, 1, 2 };
		VkBufferCopy other_copy = { 2048, 0, sizeof(other) };

		memcpy((char*)mapped + 2048, other, sizeof(other));
		CHECK(vk.BeginCommandBuffer(cmd, &begin));
		vk.CmdCopyBuffer(cmd, staging, ib, 1, &other_copy);
		CHECK(vk.EndCommandBuffer(cmd));
		CHECK(vk.ResetCommandBuffer(cmd, 0));
	} else if (!strcmp(upload, "fill")) {
		CHECK(vk.BeginCommandBuffer(cmd, &begin));
		vk.CmdFillBuffer(cmd, ib, 0, VK_WHOLE_SIZE, 0);
		CHECK(vk.EndCommandBuffer(cmd));
		CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		CHECK(vk.QueueWaitIdle(queue));
	} else if (!strcmp(upload, "srcfill")) {
		VkBufferMemoryBarrier filled = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		filled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		filled.srcQueueFamilyIndex = filled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		filled.buffer = staging;
		filled.offset = 1024;
		filled.size = sizeof(indices);
		CHECK(vk.BeginCommandBuffer(cmd, &begin));
		vk.CmdFillBuffer(cmd, staging, 1024, sizeof(indices), 0);
		vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &filled, 0, NULL);
		vk.CmdCopyBuffer(cmd, staging, ib, 1, &ib_copy);
		CHECK(vk.EndCommandBuffer(cmd));
		CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		CHECK(vk.QueueWaitIdle(queue));
	}

	// Or the way of an engine that keeps its meshes in one buffer it has
	// mapped: no copy for Vulkan to see, and the index buffer followed by
	// the vertices after some padding
	VkDeviceMemory pool_memory;
	VkBuffer pool_buffer = make_buffer(16 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
		| VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &pool_memory);
	VkDeviceSize pool_ib = 256, pool_vb = 272;
	void *pool_mapped;
	// A white texture of two by two, so that a draw is the colour it was
	// before there was one
	VkImageCreateInfo texture_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageViewCreateInfo texture_view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkMemoryAllocateInfo texture_allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageMemoryBarrier texture_barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkBufferImageCopy texture_copy = {};
	VkMemoryRequirements texture_needs;
	VkDeviceMemory texture_memory, texels_memory;
	VkImage texture;
	VkImageView texture_view;
	VkSampler sampler;
	void *texels_mapped;
	VkDeviceMemory texels_source_memory;
	VkBuffer texels_source = VK_NULL_HANDLE;
	VkDeviceSize texel_bytes = lowmip ? 80 : 16;
	VkBuffer texels = make_buffer(texel_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | (gpufilled ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0),
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &texels_memory);
	if (gpufilled) {
		texels_source = make_buffer(16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &texels_source_memory);
		CHECK(vk.MapMemory(device, texels_source_memory, 0, VK_WHOLE_SIZE, 0, &texels_mapped));
	} else if (map2) {
		PFN_vkMapMemory2KHR map_memory2 = (PFN_vkMapMemory2KHR)vk.GetDeviceProcAddr(device, "vkMapMemory2KHR");
		VkMemoryMapInfoKHR map_info = { VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR };
		map_info.memory = texels_memory;
		map_info.size = VK_WHOLE_SIZE;
		if (!map_memory2) {
			printf("FAILED no vkMapMemory2KHR\n");
			return 1;
		}
		CHECK(map_memory2(device, &map_info, &texels_mapped));
	} else {
		CHECK(vk.MapMemory(device, texels_memory, 0, VK_WHOLE_SIZE, 0, &texels_mapped));
	}
	memset(texels_mapped, 0xff, (size_t)texel_bytes);
	if (unmapped && map2) {
		PFN_vkUnmapMemory2KHR unmap_memory2 = (PFN_vkUnmapMemory2KHR)vk.GetDeviceProcAddr(device, "vkUnmapMemory2KHR");
		VkMemoryUnmapInfoKHR unmap_info = { VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR };
		unmap_info.memory = texels_memory;
		CHECK(unmap_memory2(device, &unmap_info));
	} else if (unmapped) {
		vk.UnmapMemory(device, texels_memory);
	}
	texture_info.imageType = VK_IMAGE_TYPE_2D;
	texture_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	texture_info.extent = { 2, lowmip && wide ? 1u : 2u, 1 };
	texture_info.mipLevels = 1;
	texture_info.arrayLayers = 1;
	texture_info.samples = VK_SAMPLE_COUNT_1_BIT;
	texture_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	CHECK(vk.CreateImage(device, &texture_info, NULL, &texture));
	vk.GetImageMemoryRequirements(device, texture, &texture_needs);
	texture_allocate.allocationSize = texture_needs.size;
	for (texture_allocate.memoryTypeIndex = 0; !(texture_needs.memoryTypeBits & (1u << texture_allocate.memoryTypeIndex));
	     texture_allocate.memoryTypeIndex++);
	CHECK(vk.AllocateMemory(device, &texture_allocate, NULL, &texture_memory));
	CHECK(vk.BindImageMemory(device, texture, texture_memory, 0));
	texture_view_info.image = texture;
	texture_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	texture_view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
	texture_view_info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	CHECK(vk.CreateImageView(device, &texture_view_info, NULL, &texture_view));
	sampler_info.maxLod = 16;
	CHECK(vk.CreateSampler(device, &sampler_info, NULL, &sampler));

	VkImage full = VK_NULL_HANDLE;
	VkDeviceMemory full_memory;
	UINT full_height = wide ? 1 : 4;
	if (lowmip) {
		VkImageCreateInfo full_info = texture_info;
		VkMemoryRequirements full_needs;
		VkMemoryAllocateInfo full_allocate = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };

		full_info.extent = { 4, full_height, 1 };
		full_info.mipLevels = 2;
		full_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		CHECK(vk.CreateImage(device, &full_info, NULL, &full));
		vk.GetImageMemoryRequirements(device, full, &full_needs);
		full_allocate.allocationSize = full_needs.size;
		for (full_allocate.memoryTypeIndex = 0; !(full_needs.memoryTypeBits & (1u << full_allocate.memoryTypeIndex));
		     full_allocate.memoryTypeIndex++);
		CHECK(vk.AllocateMemory(device, &full_allocate, NULL, &full_memory));
		CHECK(vk.BindImageMemory(device, full, full_memory, 0));
	}

	texture_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	texture_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	texture_barrier.srcQueueFamilyIndex = texture_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	texture_barrier.image = texture;
	texture_barrier.subresourceRange = texture_view_info.subresourceRange;
	texture_copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	texture_copy.imageExtent = texture_info.extent;
	CHECK(vk.BeginCommandBuffer(cmd, &begin));
	if (gpufilled) {
		VkBufferCopy fill = { 0, 0, 16 };
		VkMemoryBarrier filled = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		filled.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vk.CmdCopyBuffer(cmd, texels_source, texels, 1, &fill);
		vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &filled, 0, NULL, 0, NULL);
	}
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &texture_barrier);
	if (lowmip) {
		VkImageMemoryBarrier full_barrier = texture_barrier;
		VkBufferImageCopy levels[2] = {};
		VkImageCopy copy = {};

		full_barrier.image = full;
		full_barrier.subresourceRange.levelCount = 2;
		vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &full_barrier);
		levels[0].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		levels[0].imageExtent = { 4, full_height, 1 };
		levels[1].bufferOffset = 4 * full_height * 4;
		levels[1].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 };
		levels[1].imageExtent = texture_info.extent;
		vk.CmdCopyBufferToImage(cmd, texels, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, levels);
		full_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		full_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		full_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		full_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &full_barrier);
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1 };
		copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.extent = texture_info.extent;
		vk.CmdCopyImage(cmd, full, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	} else {
		vk.CmdCopyBufferToImage(cmd, texels, texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &texture_copy);
	}
	texture_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	texture_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	texture_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	texture_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &texture_barrier);
	CHECK(vk.EndCommandBuffer(cmd));
	CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
	CHECK(vk.QueueWaitIdle(queue));

	VkDescriptorImageInfo texture_image = { VK_NULL_HANDLE, texture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo texture_sampler = { sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
	VkWriteDescriptorSet texture_writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
	texture_writes[0].dstSet = descriptor_sets[1];
	texture_writes[0].dstBinding = 2;
	texture_writes[0].descriptorCount = 1;
	texture_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	texture_writes[0].pImageInfo = &texture_image;
	texture_writes[1].dstSet = descriptor_sets[1];
	texture_writes[1].dstBinding = 4;
	texture_writes[1].descriptorCount = 1;
	texture_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
	texture_writes[1].pImageInfo = &texture_sampler;
	vk.UpdateDescriptorSets(device, 2, texture_writes, 0, NULL);

	CHECK(vk.MapMemory(device, pool_memory, 0, VK_WHOLE_SIZE, 0, &pool_mapped));
	memset(pool_mapped, 0xcd, 65536);
	memcpy((char*)pool_mapped + pool_ib, indices, sizeof(indices));
	memcpy((char*)pool_mapped + pool_vb, vertices, sizeof(vertices));

	// The constants are at 2048 + 4096: where the descriptor says and the
	// offset it is bound with. Floats 12 to 14 and 16 to 18 are the same
	// place, 20 to 22 another.
	VkDescriptorBufferInfo constants_info = { pool_buffer, 2048, 128 };
	VkWriteDescriptorSet constants_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	uint32_t constants_offset = 4096;
	float constants[32] = {};
	constants[0] = constants[5] = constants[10] = constants[15] = 1;
	constants[12] = constants[16] = 1;
	constants[13] = constants[17] = 2;
	constants[14] = constants[18] = 3;
	constants[20] = 7;
	constants[21] = 8;
	constants[22] = 9;
	// tint[2], which the colour of a vertex is multiplied by
	constants[24] = constants[25] = constants[26] = constants[27] = 1;
	memcpy((char*)pool_mapped + 2048 + 4096, constants, sizeof(constants));

	// Four bones at 16384. The shader adds the second to the colour, which
	// is nothing here.
	float bones[16] = { 0.25f, 0.5f, 0.75f, 1 };
	VkDescriptorBufferInfo bones_info = { pool_buffer, 16384, 8 << 20 };
	VkWriteDescriptorSet bones_write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	memcpy((char*)pool_mapped + 16384, bones, sizeof(bones));
	bones_write.dstSet = descriptor_set;
	bones_write.dstBinding = 2;
	bones_write.descriptorCount = 1;
	bones_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bones_write.pBufferInfo = &bones_info;
	vk.UpdateDescriptorSets(device, 1, &bones_write, 0, NULL);

	VkDeviceMemory shot_memory;
	VkDeviceSize shot_bytes = (VkDeviceSize)capabilities.currentExtent.width * capabilities.currentExtent.height * 4;
	VkBuffer shot = make_buffer(shot_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &shot_memory);
	constants_write.dstSet = descriptor_set;
	constants_write.dstBinding = 1;
	constants_write.descriptorCount = 1;
	constants_write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
	constants_write.pBufferInfo = &constants_info;
	vk.UpdateDescriptorSets(device, 1, &constants_write, 0, NULL);

	struct EfmiPart { uint32_t first, count; int group; };
	std::vector<EfmiPart> efmi_parts;
	const VkDeviceSize EFMI_CBS = 8 << 20, EFMI_IB = 16 << 20, EFMI_VB = 12 << 20, EFMI_SMALL = 15 << 20, EFMI_SIZE = 64 << 20;
	VkPipeline efmi_pipeline = VK_NULL_HANDLE;
	VkPipelineLayout efmi_layout = VK_NULL_HANDLE;
	VkDescriptorSet efmi_sets[3] = {};
	VkBuffer efmi_buffer = VK_NULL_HANDLE;
	char *efmi_mapped = NULL;
	if (efmi) {
		// CRC32C, to give a part indices with the hash a mod looks for:
		// all but the last two are what they are, and those two are what
		// makes the hash of all of them the one wanted
		static uint32_t table[256];
		static uint8_t reverse[256];
		for (uint32_t n = 0; n < 256; n++) {
			uint32_t c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? (c >> 1) ^ 0x82F63B78 : c >> 1;
			table[n] = c;
			reverse[c >> 24] = (uint8_t)n;
		}
		auto crc = [&](const uint8_t *data, size_t bytes) {
			uint32_t c = ~0u;
			for (size_t n = 0; n < bytes; n++)
				c = table[(c ^ data[n]) & 0xff] ^ (c >> 8);
			return ~c;
		};

		VkDeviceMemory efmi_memory;
		efmi_buffer = make_buffer(EFMI_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
			| VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &efmi_memory);
		CHECK(vk.MapMemory(device, efmi_memory, 0, VK_WHOLE_SIZE, 0, (void**)&efmi_mapped));
		memset(efmi_mapped, 0, EFMI_SIZE);

		FILE *list = fopen(efmi, "r");
		uint32_t hash, count, at = 0;
		if (!list) { printf("FAILED no %s\n", efmi); return 1; }
		char list_line[256];
		int group;
		while (fgets(list_line, sizeof(list_line), list)) {
			group = 0;
			if (sscanf(list_line, "%x %u %d", &hash, &count, &group) < 2)
				continue;
			groups = max(groups, group + 1);
			if (count < 3) { printf("FAILED a part of %u indices in %s\n", count, efmi); return 1; }
			if (EFMI_IB + ((VkDeviceSize)at + count) * 2 > EFMI_SIZE) {
				printf("FAILED the indices of %s do not fit in the %llu MB from %llu MB on\n", efmi,
					(unsigned long long)(EFMI_SIZE - EFMI_IB) >> 20, (unsigned long long)EFMI_IB >> 20);
				return 1;
			}
			uint16_t *indices16 = (uint16_t*)(efmi_mapped + EFMI_IB) + at;
			uint8_t *bytes = (uint8_t*)indices16;
			for (uint32_t n = 0; n + 2 < count; n++)
				indices16[n] = (uint16_t)(n % 3 + (hash & 0xff));
			uint32_t state = ~crc(bytes, (count - 2) * 2), wanted = ~hash;
			uint8_t picked[4];
			for (int n = 3; n >= 0; n--) {
				picked[n] = reverse[wanted >> 24];
				wanted = (wanted ^ table[picked[n]]) << 8;
			}
			for (int n = 0; n < 4; n++) {
				bytes[(count - 2) * 2 + n] = (uint8_t)(state ^ picked[n]);
				state = table[picked[n]] ^ (state >> 8);
			}
			if (crc(bytes, count * 2) != hash) { printf("FAILED to give indices the hash %08x\n", hash); return 1; }
			efmi_parts.push_back({ at, count, group });
			at += count;
		}
		fclose(list);
		printf("efmi: %zu parts, %u indices, %d characters, %d passes: %zu draws a frame\n", efmi_parts.size(), at,
			efmi_characters, efmi_passes, efmi_parts.size() * efmi_characters * efmi_passes);

		// Each character somewhere else, which is what tells them apart
		for (int c = 0; c < efmi_characters; c++) {
			float *instance = (float*)(efmi_mapped + EFMI_CBS + c * 65536);
			instance[0] = instance[5] = instance[10] = instance[15] = 1;
			instance[12] = c * 10.0f + 1;
			instance[13] = 2;
			instance[14] = 3;
		}
		for (size_t n = 0; n < (8 << 20) / 4; n++)
			((float*)efmi_mapped)[n] = (float)(n & 1023);

		std::vector<uint32_t> efmi_vs_code = load("efmi_vs.spv");
		std::vector<uint32_t> efmi_ps_code = load("efmi_ps.spv");
		VkShaderModule efmi_vs, efmi_ps;
		module_info.codeSize = efmi_vs_code.size() * 4;
		module_info.pCode = efmi_vs_code.data();
		CHECK(vk.CreateShaderModule(device, &module_info, NULL, &efmi_vs));
		module_info.codeSize = efmi_ps_code.size() * 4;
		module_info.pCode = efmi_ps_code.data();
		CHECK(vk.CreateShaderModule(device, &module_info, NULL, &efmi_ps));

		VkDescriptorSetLayoutBinding frame_bindings[2] = {}, material_bindings[24] = {}, instance_binding = {};
		VkDescriptorSetLayoutCreateInfo efmi_set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		VkDescriptorSetLayout efmi_set_layouts[3];
		frame_bindings[0] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT };
		frame_bindings[1] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT };
		for (uint32_t n = 0; n < 22; n++)
			material_bindings[n] = { n, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
		material_bindings[22] = { 22, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
		material_bindings[23] = { 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
		instance_binding = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };
		efmi_set_info.bindingCount = 2;
		efmi_set_info.pBindings = frame_bindings;
		CHECK(vk.CreateDescriptorSetLayout(device, &efmi_set_info, NULL, &efmi_set_layouts[0]));
		efmi_set_info.bindingCount = 24;
		efmi_set_info.pBindings = material_bindings;
		CHECK(vk.CreateDescriptorSetLayout(device, &efmi_set_info, NULL, &efmi_set_layouts[1]));
		efmi_set_info.bindingCount = 1;
		efmi_set_info.pBindings = &instance_binding;
		CHECK(vk.CreateDescriptorSetLayout(device, &efmi_set_info, NULL, &efmi_set_layouts[2]));

		VkDescriptorPoolSize efmi_pool_size[5] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 22 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
		VkDescriptorPoolCreateInfo efmi_pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		VkDescriptorPool efmi_pool;
		efmi_pool_info.maxSets = 3;
		efmi_pool_info.poolSizeCount = 5;
		efmi_pool_info.pPoolSizes = efmi_pool_size;
		CHECK(vk.CreateDescriptorPool(device, &efmi_pool_info, NULL, &efmi_pool));
		VkDescriptorSetAllocateInfo efmi_allocate = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		efmi_allocate.descriptorPool = efmi_pool;
		efmi_allocate.descriptorSetCount = 3;
		efmi_allocate.pSetLayouts = efmi_set_layouts;
		CHECK(vk.AllocateDescriptorSets(device, &efmi_allocate, efmi_sets));

		VkDescriptorBufferInfo frame_info = { efmi_buffer, EFMI_SMALL, 128 }, efmi_bones_info = { efmi_buffer, 0, 8 << 20 };
		VkDescriptorBufferInfo material_info = { efmi_buffer, EFMI_SMALL + 4096, 64 }, instance_info = { efmi_buffer, EFMI_CBS, 65536 };
		std::vector<VkWriteDescriptorSet> efmi_writes;
		auto write_buffer = [&](VkDescriptorSet set, uint32_t at, VkDescriptorType type, const VkDescriptorBufferInfo *info) {
			VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = set;
			write.dstBinding = at;
			write.descriptorCount = 1;
			write.descriptorType = type;
			write.pBufferInfo = info;
			efmi_writes.push_back(write);
		};
		write_buffer(efmi_sets[0], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &frame_info);
		write_buffer(efmi_sets[0], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &efmi_bones_info);
		write_buffer(efmi_sets[1], 23, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &material_info);
		write_buffer(efmi_sets[2], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &instance_info);
		for (uint32_t n = 0; n < 23; n++) {
			VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
			write.dstSet = efmi_sets[1];
			write.dstBinding = n;
			write.descriptorCount = 1;
			write.descriptorType = n < 22 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
			write.pImageInfo = n < 22 ? &texture_image : &texture_sampler;
			efmi_writes.push_back(write);
		}
		vk.UpdateDescriptorSets(device, (uint32_t)efmi_writes.size(), efmi_writes.data(), 0, NULL);

		VkPipelineLayoutCreateInfo efmi_layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		efmi_layout_info.setLayoutCount = 3;
		efmi_layout_info.pSetLayouts = efmi_set_layouts;
		CHECK(vk.CreatePipelineLayout(device, &efmi_layout_info, NULL, &efmi_layout));

		// The streams of a character: position, texture coordinates, bone
		// indices and weights, the position of the frame before, and one
		// every vertex reads the same of
		VkVertexInputBindingDescription efmi_bindings[5] = { { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 12, VK_VERTEX_INPUT_RATE_VERTEX },
			{ 2, 12, VK_VERTEX_INPUT_RATE_VERTEX }, { 3, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 4, 0, VK_VERTEX_INPUT_RATE_VERTEX } };
		VkVertexInputAttributeDescription efmi_attributes[6] = {
			{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
			{ 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, 0 },
			{ 2, 2, VK_FORMAT_R8G8B8A8_UINT, 0 },
			{ 3, 2, VK_FORMAT_R16G16B16A16_UNORM, 4 },
			{ 4, 3, VK_FORMAT_R32G32B32_SFLOAT, 0 },
			{ 5, 4, VK_FORMAT_R8G8B8A8_UNORM, 0 },
		};
		VkPipelineVertexInputStateCreateInfo efmi_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineShaderStageCreateInfo efmi_stages[2] = { stages[0], stages[1] };
		VkGraphicsPipelineCreateInfo efmi_pipeline_info = pipeline_info;
		efmi_input.vertexBindingDescriptionCount = 5;
		efmi_input.pVertexBindingDescriptions = efmi_bindings;
		efmi_input.vertexAttributeDescriptionCount = 6;
		efmi_input.pVertexAttributeDescriptions = efmi_attributes;
		efmi_stages[0].module = efmi_vs;
		efmi_stages[1].module = efmi_ps;
		efmi_pipeline_info.pStages = efmi_stages;
		efmi_pipeline_info.pVertexInputState = &efmi_input;
		efmi_pipeline_info.layout = efmi_layout;
		CHECK(vk.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &efmi_pipeline_info, NULL, &efmi_pipeline));
		vk.DestroyShaderModule(device, efmi_vs, NULL);
		vk.DestroyShaderModule(device, efmi_ps, NULL);
	}

	VkSemaphoreCreateInfo semaphore_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkSemaphore acquired, rendered;
	CHECK(vk.CreateSemaphore(device, &semaphore_info, NULL, &acquired));
	CHECK(vk.CreateSemaphore(device, &semaphore_info, NULL, &rendered));

	if (profile)
		start_profile();

	ULONGLONG start = GetTickCount64();
	unsigned frames = 0;
	LARGE_INTEGER frequency, before, after;
	double recording = 0;
	// The CPU time of the thread, which leaves out its waits for the GPU
	ULONG64 cycles_before, cycles_after, cycles = 0;
	QueryPerformanceFrequency(&frequency);
	HANDLE pacer_go = CreateEvent(NULL, FALSE, FALSE, NULL), pacer_done = CreateEvent(NULL, FALSE, FALSE, NULL);
	std::function<void()> pacer_job;
	if (overlay) {
		std::thread([&]() {
			for (;;) {
				WaitForSingleObject(pacer_go, INFINITE);
				pacer_job();
				SetEvent(pacer_done);
			}
		}).detach();
	}
	LARGE_INTEGER frame_start;
	QueryPerformanceCounter(&frame_start);
	for (bool last = false; !last;) {
		last = GetTickCount64() - start >= seconds * 1000;
		int in_window = appear && frames >= 120 ? (int)((frames - 120) % appear) : -1;
		int visible = appear && frames >= 120 ? (int)((frames - 120) / appear) : -1;
		if (appear)
			last = frames + 1 >= 120 + (unsigned)(groups * appear);
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkRenderPassBeginInfo pass_begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
		VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		VkClearValue clear = {};
		VkViewport viewport = { 0, 0, (float)capabilities.currentExtent.width, (float)capabilities.currentExtent.height, 0, 1 };
		VkRect2D scissor = { { 0, 0 }, capabilities.currentExtent };
		VkDeviceSize zero = 0;
		uint32_t image;
		MSG message;

		while (PeekMessage(&message, NULL, 0, 0, PM_REMOVE))
			DispatchMessage(&message);

		CHECK(vk.AcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image));

		clear.color.float32[2] = 0.3f;
		pass_begin.renderPass = pass;
		pass_begin.framebuffer = framebuffers[image];
		pass_begin.renderArea = scissor;
		pass_begin.clearValueCount = 1;
		pass_begin.pClearValues = &clear;

		if (rewrite && GetTickCount64() - start >= seconds * 500) {
			static const uint16_t other[6] = { 0, 2, 1, 0, 3, 2 };
			memcpy((char*)pool_mapped + pool_ib, other, sizeof(other));
			rewrite = false;
			printf("rewritten at frame %u\n", frames);
		}
		if (bonetick && pool_path)
			((float*)((char*)pool_mapped + 16384))[0] = (float)(frames % 200 + 20) / 255.0f;
		// Some of the bones are others every frame
		for (size_t n = 0; efmi && n < 65536; n++)
			((float*)efmi_mapped)[(1 << 18) + n] = (float)(frames + n);
		QueryPerformanceCounter(&before);
		QueryThreadCycleTime(GetCurrentThread(), &cycles_before);
		profiling = profile && frames >= 120;
		if (appear)
			profiling = profile && (getenv("STEADY") ? in_window == 5 || in_window == 6 : in_window == 0 || in_window == 1);
		CHECK(vk.BeginCommandBuffer(cmd, &begin));
		if (last && !strcmp(upload, "lastfill")) {
			VkBufferMemoryBarrier filled = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
			filled.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			filled.dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
			filled.srcQueueFamilyIndex = filled.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			filled.buffer = ib;
			filled.size = VK_WHOLE_SIZE;
			vk.CmdFillBuffer(cmd, ib, 0, VK_WHOLE_SIZE, 0);
			vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &filled, 0, NULL);
		}
		vk.CmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
		if (efmi) {
			VkBuffer streams[5] = { efmi_buffer, efmi_buffer, efmi_buffer, efmi_buffer, efmi_buffer };
			VkDeviceSize stream_offsets[5] = { EFMI_VB, EFMI_VB, EFMI_VB, EFMI_VB, EFMI_VB };
			vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, efmi_pipeline);
			vk.CmdSetViewport(cmd, 0, 1, &viewport);
			vk.CmdSetScissor(cmd, 0, 1, &scissor);
			vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, efmi_layout, 0, 2, efmi_sets, 0, NULL);
			vk.CmdBindVertexBuffers(cmd, 0, 5, streams, stream_offsets);
			vk.CmdBindIndexBuffer(cmd, efmi_buffer, EFMI_IB, VK_INDEX_TYPE_UINT16);
			for (int pass_number = 0; pass_number < efmi_passes; pass_number++) {
				for (int c = 0; c < efmi_characters; c++) {
					uint32_t instance_offset = c * 65536;
					vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, efmi_layout, 2, 1, &efmi_sets[2], 1, &instance_offset);
					// A mesh at a time, each from its own place in the buffer
					for (const EfmiPart &part : efmi_parts) {
						if (appear && part.group != visible)
							continue;
						vk.CmdBindIndexBuffer(cmd, efmi_buffer, EFMI_IB + part.first * 2, VK_INDEX_TYPE_UINT16);
						vk.CmdDrawIndexed(cmd, part.count, 1, 0, 0, 0);
					}
				}
			}
		}
		vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		vk.CmdSetViewport(cmd, 0, 1, &viewport);
		vk.CmdSetScissor(cmd, 0, 1, &scissor);
		vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, descriptor_sets, 1, &constants_offset);
		if (pool_path) {
			vk.CmdBindVertexBuffers(cmd, 0, 1, &pool_buffer, &pool_vb);
			vk.CmdBindIndexBuffer(cmd, pool_buffer, pool_ib, VK_INDEX_TYPE_UINT16);
		} else {
			vk.CmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
			vk.CmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
		}
		for (int n = 0; n < draws; n++)
			vk.CmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
		vk.CmdEndRenderPass(cmd);
		if (last) {
			VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			VkBufferImageCopy region = {};
			barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = images[image];
			barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
				0, NULL, 0, NULL, 1, &barrier);
			region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			region.imageExtent = { capabilities.currentExtent.width, capabilities.currentExtent.height, 1 };
			vk.CmdCopyImageToBuffer(cmd, images[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot, 1, &region);
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			barrier.dstAccessMask = 0;
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
				0, NULL, 0, NULL, 1, &barrier);
		}
		CHECK(vk.EndCommandBuffer(cmd));

		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &acquired;
		submit.pWaitDstStageMask = &wait_stage;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &rendered;
		CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
		profiling = false;
		QueryPerformanceCounter(&after);
		QueryThreadCycleTime(GetCurrentThread(), &cycles_after);
		// Not the first frames, which have everything made
		if (frames >= 30) {
			recording += (double)(after.QuadPart - before.QuadPart) * 1000 / frequency.QuadPart;
			cycles += cycles_after - cycles_before;
		}

		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &rendered;
		present.swapchainCount = 1;
		present.pSwapchains = &swapchain;
		present.pImageIndices = &image;
		CHECK(vk.QueuePresentKHR(queue, &present));
		CHECK(vk.QueueWaitIdle(queue));
		pacer_job = [&]() {
			for (int n = 0; n < generated; n++) {
				CHECK(vk.AcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &image));
				pass_begin.framebuffer = framebuffers[image];
				CHECK(vk.BeginCommandBuffer(cmd, &begin));
				vk.CmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);
				if (overlay) {
					vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
					vk.CmdSetViewport(cmd, 0, 1, &viewport);
					vk.CmdSetScissor(cmd, 0, 1, &scissor);
					vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 2, descriptor_sets, 1, &constants_offset);
					vk.CmdBindVertexBuffers(cmd, 0, 1, &vb, &zero);
					vk.CmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT16);
					vk.CmdDrawIndexed(cmd, 3, 1, 0, 0, 0);
				}
				vk.CmdEndRenderPass(cmd);
				CHECK(vk.EndCommandBuffer(cmd));
				CHECK(vk.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
				CHECK(vk.QueuePresentKHR(queue, &present));
				CHECK(vk.QueueWaitIdle(queue));
			}
		};
		if (overlay) {
			SetEvent(pacer_go);
			WaitForSingleObject(pacer_done, INFINITE);
		} else {
			pacer_job();
		}
		LARGE_INTEGER frame_end;
		QueryPerformanceCounter(&frame_end);
		if (in_window >= 0) {
			if (in_window == 0)
				appearances.push_back({ visible });
			int slot = min(in_window, 3);
			appearances.back().ms[slot] += (double)(frame_end.QuadPart - frame_start.QuadPart) * 1000 / frequency.QuadPart;
			appearances.back().record[slot] += (double)(after.QuadPart - before.QuadPart) * 1000 / frequency.QuadPart;
		}
		frame_start = frame_end;
		frames++;
	}
	if (appear && !appearances.empty()) {
		std::vector<double> steady, first;
		std::vector<std::pair<double, int>> worst;
		double sum[3] = {}, sum_record = 0;
		int rest = appear - 3;
		for (auto &a : appearances)
			steady.push_back(a.ms[3] / rest);
		std::sort(steady.begin(), steady.end());
		double base = steady[steady.size() / 2];
		for (auto &a : appearances) {
			for (int k = 0; k < 3; k++)
				sum[k] += a.ms[k] - base;
			sum_record += a.record[0] - a.record[3] / rest;
			first.push_back(a.ms[0] - base);
			worst.push_back({ a.ms[0] + a.ms[1] + a.ms[2] - 3 * base, a.group });
		}
		std::sort(first.begin(), first.end());
		std::sort(worst.begin(), worst.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
		printf("appear: %zu mods, steady frame %.3f ms (median, whole frame)\n", appearances.size(), base);
		printf("  first frame over steady: total %.1f ms, mean %.2f, p50 %.2f, p90 %.2f, max %.2f\n", sum[0], sum[0] / appearances.size(),
			first[first.size() / 2], first[(first.size() - 1) * 9 / 10], first.back());
		printf("  of that recording+submitting: total %.1f ms\n", sum_record);
		printf("  second frame over steady: total %.1f ms; third %.1f ms\n", sum[1], sum[2]);
		for (size_t n = 0; n < worst.size() && n < 8; n++)
			printf("  %8.2f ms  mod %d\n", worst[n].first, worst[n].second);
		printf("APPEAR total %.1f ms over %zu mods (first three frames over steady)\n", sum[0] + sum[1] + sum[2], appearances.size());
	}

	stop_profile();
	if (profile) {
		report_profile();
	}
	printf("frames: %u\n", frames);
	if (bonetick)
		printf("last bone: %u\n", (frames - 1) % 200 + 20);
	if (frames > 30) {
		printf("recording and submitting: %.3f ms a frame\n", recording / (frames - 30));
		printf("recording and submitting: %.3f million cycles of the thread a frame\n", cycles / 1e6 / (frames - 30));
	}

	// The middle is within the game's square and the triangle of the test
	// mods, the other only within the triangle
	const unsigned char *pixels;
	uint32_t width = capabilities.currentExtent.width, height = capabilities.currentExtent.height;
	CHECK(vk.MapMemory(device, shot_memory, 0, VK_WHOLE_SIZE, 0, (void**)&pixels));
	const unsigned char *middle = pixels + ((size_t)(height / 2) * width + width / 2) * 4;
	const unsigned char *corner = pixels + ((size_t)(height / 10) * width + width * 3 / 20) * 4;
	printf("middle: %u %u %u %u\n", middle[0], middle[1], middle[2], middle[3]);
	printf("corner: %u %u %u %u\n", corner[0], corner[1], corner[2], corner[3]);
	{
		unsigned long long sum = 1469598103934665603ull;
		for (size_t n = 0; n < (size_t)width * height * 4; n++)
			sum = (sum ^ pixels[n]) * 1099511628211ull;
		printf("picture checksum %016llx\n", sum);
	}

	vk.DeviceWaitIdle(device);
	vk.DestroyBuffer(device, staging, NULL);
	vk.DestroyBuffer(device, vb, NULL);
	vk.DestroyBuffer(device, ib, NULL);
	if (staging_mapped)
		vk.UnmapMemory(device, staging_memory);
	vk.FreeMemory(device, staging_memory, NULL);
	vk.FreeMemory(device, vb_memory, NULL);
	vk.FreeMemory(device, ib_memory, NULL);
	vk.DestroyPipeline(device, pipeline, NULL);
	vk.DestroyPipelineLayout(device, layout, NULL);
	vk.DestroyDescriptorPool(device, descriptor_pool, NULL);
	vk.DestroyDescriptorSetLayout(device, set_layout, NULL);
	vk.DestroySemaphore(device, acquired, NULL);
	vk.DestroySemaphore(device, rendered, NULL);
	for (uint32_t n = 0; n < image_count; n++) {
		vk.DestroyFramebuffer(device, framebuffers[n], NULL);
		vk.DestroyImageView(device, views[n], NULL);
	}
	vk.DestroyRenderPass(device, pass, NULL);
	vk.DestroyCommandPool(device, pool, NULL);
	vk.DestroySwapchainKHR(device, swapchain, NULL);
	puts("destroying device");
	vk.DestroyDevice(device, NULL);
	printf("done\n");
	fflush(stdout);
	// The rest goes with the process
	TerminateProcess(GetCurrentProcess(), 0);
	return 0;
}
