#include "gpu/vulkan_context.h"

#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace hc09::gpu
{
    namespace
    {
        bool Check(VkResult result, const char* what)
        {
            if (result == VK_SUCCESS)
                return true;
            std::fprintf(stderr, "[vulkan] %s failed (%d)\n", what, int(result));
            return false;
        }

        constexpr VkDeviceSize kChunkSize = 32ull << 20;  // (16 MB overflowed on game scenes: a new chunk mid-frame)

        double SecondsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    VulkanContext::~VulkanContext()
    {
        Shutdown();
    }

    bool VulkanContext::Initialize(ANativeWindow* window)
    {
        if (!device_ && !CreateDevice())
            return false;
        if (window_ != window)
        {
            ReleaseWindow();
            if (!window)
                return true;
            window_ = window;
            ANativeWindow_acquire(window_);
            VkAndroidSurfaceCreateInfoKHR info{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
            info.window = window_;
            if (!Check(vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_), "vkCreateAndroidSurfaceKHR"))
                return false;
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface_, &supported);
            if (!supported)
            {
                std::fprintf(stderr, "[vulkan] the graphics queue cannot present to this window\n");
                return false;
            }
        }
        else if (window)
        {
            swapchainStale_ = true;  // same window, possibly resized
        }
        return !window || swapchain_ || CreateSwapchain();
    }

    bool VulkanContext::CreateDevice()
    {
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        if (auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
            enumerate(&loaderVersion);
        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "HC09";
        app.pEngineName = "hc09-recomp";
        // Asking for more than the loader has fails on 1.0 loaders (the V30); 1.1 is all we use anywhere.
        app.apiVersion = loaderVersion >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
        const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
        VkInstanceCreateInfo instanceInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        if (!Check(vkCreateInstance(&instanceInfo, nullptr, &instance_), "vkCreateInstance"))
            return false;

        uint32_t count = 1;
        VkResult result = vkEnumeratePhysicalDevices(instance_, &count, &physical_);
        if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0)
        {
            std::fprintf(stderr, "[vulkan] no GPU\n");
            return false;
        }
        vkGetPhysicalDeviceProperties(physical_, &properties_);
        vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
        std::fprintf(stderr, "[vulkan] %s, Vulkan %u.%u.%u, driver 0x%08X\n", properties_.deviceName,
            VK_VERSION_MAJOR(properties_.apiVersion), VK_VERSION_MINOR(properties_.apiVersion),
            VK_VERSION_PATCH(properties_.apiVersion), properties_.driverVersion);

        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_, &families, familyProperties.data());
        queueFamily_ = families;
        for (uint32_t i = 0; i < families; i++)
            if (familyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                queueFamily_ = i;
                break;
            }
        if (queueFamily_ == families)
        {
            std::fprintf(stderr, "[vulkan] no graphics queue\n");
            return false;
        }

        // Optional features the renderer uses when present (the V30 has no geometry shaders).
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physical_, &supported);
        enabledFeatures_.robustBufferAccess = supported.robustBufferAccess;  // vertex fetches past a stream read 0
        enabledFeatures_.geometryShader = supported.geometryShader;
        enabledFeatures_.samplerAnisotropy = supported.samplerAnisotropy;
        enabledFeatures_.depthClamp = supported.depthClamp;
        std::vector<const char*> deviceExtensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &extensionCount, extensions.data());
        for (const auto& extension : extensions)
            if (std::strcmp(extension.extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME) == 0)
            {
                mirrorClampToEdge_ = true;
                deviceExtensions.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
            }
        std::fprintf(stderr, "[vulkan] geometry shaders %d, anisotropy %d, depth clamp %d, mirror-once %d\n",
            int(enabledFeatures_.geometryShader), int(enabledFeatures_.samplerAnisotropy), int(enabledFeatures_.depthClamp),
            int(mirrorClampToEdge_));

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = uint32_t(deviceExtensions.size());
        deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
        deviceInfo.pEnabledFeatures = &enabledFeatures_;
        if (!Check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        // The driver's compiled pipelines from earlier runs (HC09_PIPELINE_CACHE, set by the app): some of
        // the game's shaders take the Pixel 8's compiler tens of seconds. A cache from another driver or
        // GPU is ignored by the driver.
        std::vector<char> cacheData;
        if (const char* path = std::getenv("HC09_PIPELINE_CACHE"))
        {
            if (FILE* file = std::fopen(path, "rb"))
            {
                std::fseek(file, 0, SEEK_END);
                const long size = std::ftell(file);
                std::fseek(file, 0, SEEK_SET);
                if (size > 0)
                {
                    cacheData.resize(size_t(size));
                    if (std::fread(cacheData.data(), 1, cacheData.size(), file) != cacheData.size())
                        cacheData.clear();
                }
                std::fclose(file);
            }
            std::fprintf(stderr, "[vulkan] pipeline cache: %zu bytes from %s\n", cacheData.size(), path);
        }
        VkPipelineCacheCreateInfo cacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        cacheInfo.initialDataSize = cacheData.size();
        cacheInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
        if (vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_) != VK_SUCCESS)
        {
            cacheInfo.initialDataSize = 0;
            cacheInfo.pInitialData = nullptr;
            vkCreatePipelineCache(device_, &cacheInfo, nullptr, &pipelineCache_);
        }

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        if (!Check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool"))
            return false;
        if (familyProperties[queueFamily_].timestampValidBits > 0 && properties_.limits.timestampPeriod > 0)
        {
            VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = kFramesInFlight * 2;
            if (vkCreateQueryPool(device_, &queryInfo, nullptr, &timestamps_) != VK_SUCCESS)
                timestamps_ = VK_NULL_HANDLE;
        }
        for (uint32_t i = 0; i < kFramesInFlight; i++)
        {
            VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (!Check(vkCreateFence(device_, &fenceInfo, nullptr, &frameDone_[i]), "vkCreateFence") ||
                !Check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageReady_[i]), "vkCreateSemaphore") ||
                !Check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderDone_[i]), "vkCreateSemaphore"))
                return false;
            Chunk chunk;
            if (!CreateChunk(kChunkSize, chunk))
                return false;
            frames_[i].chunks.push_back(chunk);
            frames_[i].pools.push_back(CreatePool());
        }
        frame_ = 0;
        BeginFrame();
        return true;
    }

    bool VulkanContext::CreateChunk(VkDeviceSize size, Chunk& chunk)
    {
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (!Check(vkCreateBuffer(device_, &info, nullptr, &chunk.buffer), "vkCreateBuffer"))
            return false;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device_, chunk.buffer, &requirements);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!Check(vkAllocateMemory(device_, &allocate, nullptr, &chunk.memory), "vkAllocateMemory (upload)"))
            return false;
        vkBindBufferMemory(device_, chunk.buffer, chunk.memory, 0);
        void* mapped = nullptr;
        if (!Check(vkMapMemory(device_, chunk.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory"))
            return false;
        chunk.cpu = static_cast<uint8_t*>(mapped);
        chunk.size = size;
        return true;
    }

    void VulkanContext::DestroyChunk(Chunk& chunk)
    {
        if (chunk.buffer)
            vkDestroyBuffer(device_, chunk.buffer, nullptr);
        if (chunk.memory)
            vkFreeMemory(device_, chunk.memory, nullptr);
        chunk = {};
    }

    VkDescriptorPool VulkanContext::CreatePool()
    {
        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 8192 },
        };
        VkDescriptorPoolCreateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        info.maxSets = 2048;
        info.poolSizeCount = uint32_t(std::size(sizes));
        info.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        Check(vkCreateDescriptorPool(device_, &info, nullptr, &pool), "vkCreateDescriptorPool");
        return pool;
    }

    uint32_t VulkanContext::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const
    {
        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; i++)
            if ((typeBits & (1u << i)) && (memoryProperties_.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        for (uint32_t i = 0; i < memoryProperties_.memoryTypeCount; i++)
            if (typeBits & (1u << i))
                return i;
        return 0;
    }

    bool VulkanContext::SupportsFormat(VkFormat format, VkFormatFeatureFlags features) const
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical_, format, &properties);
        return (properties.optimalTilingFeatures & features) == features;
    }

    bool VulkanContext::CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory)
    {
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        if (!Check(vkCreateImage(device_, &info, nullptr, &image), "vkCreateImage"))
            return false;
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device_, image, &requirements);
        VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(device_, &allocate, nullptr, &memory), "vkAllocateMemory (image)"))
        {
            vkDestroyImage(device_, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }
        vkBindImageMemory(device_, image, memory, 0);
        return true;
    }

    VulkanContext::Allocation VulkanContext::Allocate(VkDeviceSize size, VkDeviceSize alignment)
    {
        Frame& frame = frames_[frame_];
        VkDeviceSize offset = (frame.used + alignment - 1) / alignment * alignment;
        if (offset + size > frame.chunks.back().size)
        {
            // A new chunk for the rest of the frame (released when the frame is reused).
            Chunk chunk;
            if (!CreateChunk(std::max(size, kChunkSize), chunk))
                return {};
            stats_.chunks++;
            frame.chunks.push_back(chunk);
            offset = 0;
        }
        frame.used = offset + size;
        const Chunk& chunk = frame.chunks.back();
        return { chunk.buffer, offset, chunk.cpu + offset };
    }

    VkDescriptorSet VulkanContext::AllocateSet(VkDescriptorSetLayout layout)
    {
        Frame& frame = frames_[frame_];
        for (;;)
        {
            VkDescriptorSetAllocateInfo info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            info.descriptorPool = frame.pools[frame.pool];
            info.descriptorSetCount = 1;
            info.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (vkAllocateDescriptorSets(device_, &info, &set) == VK_SUCCESS)
                return set;
            // This pool is full: the next one (made when needed).
            if (++frame.pool == frame.pools.size())
            {
                VkDescriptorPool pool = CreatePool();
                if (!pool)
                    return VK_NULL_HANDLE;
                frame.pools.push_back(pool);
            }
        }
    }

    void VulkanContext::DestroyLater(std::function<void()> release)
    {
        frames_[frame_].releases.push_back(std::move(release));
    }

    void VulkanContext::SavePipelineCache()
    {
        const char* path = std::getenv("HC09_PIPELINE_CACHE");
        if (!path || !pipelineCache_)
            return;
        static std::mutex saving;  // (the pre-compile workers save too)
        std::lock_guard lock(saving);
        size_t size = 0;
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0)
            return;
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS)
            return;
        // Written next to the old one and renamed over it, so a crash never leaves half a file.
        const std::string temporary = std::string(path) + ".new";
        FILE* file = std::fopen(temporary.c_str(), "wb");
        if (!file)
            return;
        const bool written = std::fwrite(data.data(), 1, size, file) == size;
        std::fclose(file);
        if (written && std::rename(temporary.c_str(), path) == 0)
            std::fprintf(stderr, "[vulkan] pipeline cache saved: %zu bytes\n", size);
    }

    VkCommandBuffer VulkanContext::BeginCommands()
    {
        Frame& frame = frames_[frame_];
        if (frame.nextBuffer == frame.buffers.size())
        {
            VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            allocateInfo.commandPool = commandPool_;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1;
            VkCommandBuffer buffer = VK_NULL_HANDLE;
            Check(vkAllocateCommandBuffers(device_, &allocateInfo, &buffer), "vkAllocateCommandBuffers");
            frame.buffers.push_back(buffer);
        }
        VkCommandBuffer buffer = frame.buffers[frame.nextBuffer++];
        vkResetCommandBuffer(buffer, 0);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(buffer, &begin);
        return buffer;
    }

    VkCommandBuffer VulkanContext::uploads()
    {
        Frame& frame = frames_[frame_];
        if (!frame.uploads)
            frame.uploads = BeginCommands();
        return frame.uploads;
    }

    bool VulkanContext::SubmitRecorded(const VkSubmitInfo& waits, VkFence fence)
    {
        Frame& frame = frames_[frame_];
        VkCommandBuffer buffers[2];
        uint32_t count = 0;
        if (frame.uploads)
        {
            vkEndCommandBuffer(frame.uploads);
            buffers[count++] = frame.uploads;
            frame.uploads = VK_NULL_HANDLE;
        }
        vkEndCommandBuffer(frame.commands);
        buffers[count++] = frame.commands;
        VkSubmitInfo submit = waits;
        submit.commandBufferCount = count;
        submit.pCommandBuffers = buffers;
        const auto start = std::chrono::steady_clock::now();
        const bool submitted = Check(vkQueueSubmit(queue_, 1, &submit, fence), "vkQueueSubmit");
        stats_.submit += SecondsSince(start);
        stats_.submits++;
        return submitted;
    }

    void VulkanContext::Submit()
    {
        SubmitRecorded({ VK_STRUCTURE_TYPE_SUBMIT_INFO }, VK_NULL_HANDLE);
        frames_[frame_].commands = BeginCommands();
    }

    VulkanContext::Stats VulkanContext::TakeStats()
    {
        const Stats stats = stats_;
        stats_ = {};
        return stats;
    }

    void VulkanContext::BeginFrame()
    {
        const auto start = std::chrono::steady_clock::now();
        vkWaitForFences(device_, 1, &frameDone_[frame_], VK_TRUE, UINT64_MAX);
        stats_.fence += SecondsSince(start);
        if (timestamps_ && timestampsWritten_[frame_])
        {
            uint64_t ticks[2];
            if (vkGetQueryPoolResults(device_, timestamps_, frame_ * 2, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ticks[1] > ticks[0])
            {
                stats_.gpu += double(ticks[1] - ticks[0]) * properties_.limits.timestampPeriod * 1e-9;
                stats_.gpuFrames++;
            }
        }
        Frame& frame = frames_[frame_];
        for (auto& release : frame.releases)
            release();
        frame.releases.clear();
        for (size_t i = 1; i < frame.chunks.size(); i++)
            DestroyChunk(frame.chunks[i]);
        frame.chunks.resize(1);
        frame.used = 0;
        for (VkDescriptorPool pool : frame.pools)
            vkResetDescriptorPool(device_, pool, 0);
        frame.pool = 0;
        frame.nextBuffer = 0;
        frame.uploads = VK_NULL_HANDLE;
        frame.commands = BeginCommands();
        if (timestamps_)
        {
            vkCmdResetQueryPool(frame.commands, timestamps_, frame_ * 2, 2);
            vkCmdWriteTimestamp(frame.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps_, frame_ * 2);
            timestampsWritten_[frame_] = false;
        }
        frameCount_++;
    }

    bool VulkanContext::CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps{};
        if (!Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities"))
            return false;
        uint32_t formatCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &formatCount, formats.data());
        if (formats.empty())
            return false;
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& format : formats)
            if (format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM)
            {
                chosen = format;
                break;
            }
        if (presentPass_ && chosen.format != format_)
        {
            vkDestroyRenderPass(device_, presentPass_, nullptr);
            presentPass_ = VK_NULL_HANDLE;
        }
        format_ = chosen.format;

        // The window's size in its current orientation (the surface reports the display's natural one
        // when the window is rotated): the swapchain is unrotated (identity transform below) and the
        // compositor rotates it (pre-rotation is a later optimization).
        extent_ = { uint32_t(ANativeWindow_getWidth(window_)), uint32_t(ANativeWindow_getHeight(window_)) };
        std::fprintf(stderr, "[vulkan] surface transform 0x%X, extent %ux%u, window %ux%u\n", unsigned(caps.currentTransform),
            caps.currentExtent.width, caps.currentExtent.height, extent_.width, extent_.height);
        if (extent_.width == 0 || extent_.height == 0)
            return false;
        // The surface may report a rotated transform; render in the window's orientation and let the
        // compositor rotate (pre-rotation is a later optimization).
        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha))
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

        VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        info.surface = surface_;
        info.minImageCount = std::max(caps.minImageCount, 3u);
        if (caps.maxImageCount)
            info.minImageCount = std::min(info.minImageCount, caps.maxImageCount);
        info.imageFormat = format_;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                               : caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; paces to the display
        info.clipped = VK_TRUE;
        if (!Check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR"))
            return false;

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        images_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images_.data());

        if (!presentPass_)
        {
            VkAttachmentDescription attachment{};
            attachment.format = format_;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &reference;
            // Wait for the acquire semaphore before writing the image.
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            passInfo.attachmentCount = 1;
            passInfo.pAttachments = &attachment;
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            passInfo.dependencyCount = 1;
            passInfo.pDependencies = &dependency;
            if (!Check(vkCreateRenderPass(device_, &passInfo, nullptr, &presentPass_), "vkCreateRenderPass"))
                return false;
        }

        for (VkImage image : images_)
        {
            VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format_;
            viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkImageView view;
            if (!Check(vkCreateImageView(device_, &viewInfo, nullptr, &view), "vkCreateImageView"))
                return false;
            views_.push_back(view);
            VkFramebufferCreateInfo framebufferInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            framebufferInfo.renderPass = presentPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &view;
            framebufferInfo.width = extent_.width;
            framebufferInfo.height = extent_.height;
            framebufferInfo.layers = 1;
            VkFramebuffer framebuffer;
            if (!Check(vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer"))
                return false;
            framebuffers_.push_back(framebuffer);
        }
        swapchainStale_ = false;
        std::fprintf(stderr, "[vulkan] swapchain %ux%u, %zu images, format %d\n", extent_.width, extent_.height, images_.size(),
            int(format_));
        return true;
    }

    void VulkanContext::DestroySwapchain()
    {
        if (!device_)
            return;
        vkDeviceWaitIdle(device_);
        for (VkFramebuffer framebuffer : framebuffers_)
            vkDestroyFramebuffer(device_, framebuffer, nullptr);
        for (VkImageView view : views_)
            vkDestroyImageView(device_, view, nullptr);
        framebuffers_.clear();
        views_.clear();
        images_.clear();
        if (swapchain_)
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void VulkanContext::ReleaseWindow()
    {
        DestroySwapchain();
        if (surface_)
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        if (window_)
            ANativeWindow_release(window_);
        window_ = nullptr;
    }

    void VulkanContext::Shutdown()
    {
        ReleaseWindow();
        if (device_)
        {
            vkDeviceWaitIdle(device_);
            for (uint32_t i = 0; i < kFramesInFlight; i++)
            {
                for (auto& release : frames_[i].releases)
                    release();
                frames_[i].releases.clear();
                for (Chunk& chunk : frames_[i].chunks)
                    DestroyChunk(chunk);
                frames_[i].chunks.clear();
                for (VkDescriptorPool pool : frames_[i].pools)
                    vkDestroyDescriptorPool(device_, pool, nullptr);
                frames_[i].pools.clear();
                vkDestroyFence(device_, frameDone_[i], nullptr);
                vkDestroySemaphore(device_, imageReady_[i], nullptr);
                vkDestroySemaphore(device_, renderDone_[i], nullptr);
            }
            if (presentPass_)
                vkDestroyRenderPass(device_, presentPass_, nullptr);
            presentPass_ = VK_NULL_HANDLE;
            if (timestamps_)
                vkDestroyQueryPool(device_, timestamps_, nullptr);
            timestamps_ = VK_NULL_HANDLE;
            vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_)
            vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }

    bool VulkanContext::ClearAndPresent(float red, float green, float blue)
    {
        const float clear[3] = { red, green, blue };
        return EndFrame({}, clear);
    }

    bool VulkanContext::EndFrame(const PresentDraw& draw, const float clear[3])
    {
        const uint32_t frame = frame_;
        VkCommandBuffer commands = frames_[frame].commands;
        if (surface_ && (swapchainStale_ || !swapchain_))
        {
            DestroySwapchain();
            CreateSwapchain();
        }

        // The window's next image, when there is one to draw to.
        bool acquired = false;
        uint32_t index = 0;
        if (swapchain_)
        {
            // At most a second: a window that stops handing out images (going away, say) skips this
            // frame's picture rather than hanging the game.
            const auto start = std::chrono::steady_clock::now();
            const VkResult result = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull, imageReady_[frame], VK_NULL_HANDLE, &index);
            stats_.acquire += SecondsSince(start);
            if (result == VK_ERROR_OUT_OF_DATE_KHR)
                swapchainStale_ = true;
            else if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
                acquired = true;
            else if (result == VK_TIMEOUT || result == VK_NOT_READY)
                std::fprintf(stderr, "[vulkan] no window image within 1 s; frame not shown\n");
            else
                Check(result, "vkAcquireNextImageKHR");
        }
        if (acquired)
        {
            VkClearValue clearValue{};
            if (clear)
                for (int c = 0; c < 3; c++)
                    clearValue.color.float32[c] = clear[c];
            clearValue.color.float32[3] = 1.0f;
            VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            pass.renderPass = presentPass_;
            pass.framebuffer = framebuffers_[index];
            pass.renderArea.extent = extent_;
            pass.clearValueCount = 1;
            pass.pClearValues = &clearValue;
            vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
            if (draw)
                draw(commands, extent_);
            vkCmdEndRenderPass(commands);
        }
        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        if (acquired)
        {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &imageReady_[frame];
            submit.pWaitDstStageMask = &wait;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &renderDone_[frame];
        }
        if (timestamps_)
        {
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps_, frame * 2 + 1);
            timestampsWritten_[frame] = true;
        }
        vkResetFences(device_, 1, &frameDone_[frame]);
        bool presented = SubmitRecorded(submit, frameDone_[frame]);

        if (acquired && presented)
        {
            VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &renderDone_[frame];
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain_;
            present.pImageIndices = &index;
            const auto start = std::chrono::steady_clock::now();
            const VkResult result = vkQueuePresentKHR(queue_, &present);
            stats_.present += SecondsSince(start);
            // (VK_SUBOPTIMAL_KHR is expected: the swapchain is unrotated in a rotated window and the
            // compositor rotates it. Rebuilding for it would rebuild every frame.)
            if (result == VK_ERROR_OUT_OF_DATE_KHR)
                swapchainStale_ = true;
            else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
                presented = Check(result, "vkQueuePresentKHR");
        }
        frame_ = (frame_ + 1) % kFramesInFlight;
        BeginFrame();
        return acquired && presented;
    }
}
