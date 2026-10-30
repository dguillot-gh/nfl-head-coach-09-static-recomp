#include "gpu/vulkan_context.h"

#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

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
        return swapchain_ || CreateSwapchain();
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

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = queueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = deviceExtensions;
        if (!Check(vkCreateDevice(physical_, &deviceInfo, nullptr, &device_), "vkCreateDevice"))
            return false;
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily_;
        if (!Check(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool"))
            return false;
        VkCommandBufferAllocateInfo allocateInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = kFramesInFlight;
        if (!Check(vkAllocateCommandBuffers(device_, &allocateInfo, commands_), "vkAllocateCommandBuffers"))
            return false;
        for (uint32_t i = 0; i < kFramesInFlight; i++)
        {
            VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (!Check(vkCreateFence(device_, &fenceInfo, nullptr, &frameDone_[i]), "vkCreateFence") ||
                !Check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageReady_[i]), "vkCreateSemaphore") ||
                !Check(vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderDone_[i]), "vkCreateSemaphore"))
                return false;
        }
        return true;
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
        format_ = chosen.format;

        extent_ = caps.currentExtent;
        if (extent_.width == 0xFFFFFFFF)
            extent_ = { uint32_t(ANativeWindow_getWidth(window_)), uint32_t(ANativeWindow_getHeight(window_)) };
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
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available; paces to the display
        info.clipped = VK_TRUE;
        if (!Check(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_), "vkCreateSwapchainKHR"))
            return false;

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        images_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, images_.data());

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
        if (!Check(vkCreateRenderPass(device_, &passInfo, nullptr, &clearPass_), "vkCreateRenderPass"))
            return false;

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
            framebufferInfo.renderPass = clearPass_;
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
        if (clearPass_)
            vkDestroyRenderPass(device_, clearPass_, nullptr);
        clearPass_ = VK_NULL_HANDLE;
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
                vkDestroyFence(device_, frameDone_[i], nullptr);
                vkDestroySemaphore(device_, imageReady_[i], nullptr);
                vkDestroySemaphore(device_, renderDone_[i], nullptr);
            }
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
        if (!surface_)
            return false;
        if (swapchainStale_ || !swapchain_)
        {
            DestroySwapchain();
            if (!CreateSwapchain())
                return false;
        }

        const uint32_t frame = frame_;
        vkWaitForFences(device_, 1, &frameDone_[frame], VK_TRUE, UINT64_MAX);
        uint32_t index = 0;
        VkResult result = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageReady_[frame], VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            swapchainStale_ = true;
            return false;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return Check(result, "vkAcquireNextImageKHR");
        vkResetFences(device_, 1, &frameDone_[frame]);

        VkCommandBuffer commands = commands_[frame];
        vkResetCommandBuffer(commands, 0);
        VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        VkClearValue clear{};
        clear.color.float32[0] = red;
        clear.color.float32[1] = green;
        clear.color.float32[2] = blue;
        clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        pass.renderPass = clearPass_;
        pass.framebuffer = framebuffers_[index];
        pass.renderArea.extent = extent_;
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commands);
        vkEndCommandBuffer(commands);

        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &imageReady_[frame];
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &renderDone_[frame];
        if (!Check(vkQueueSubmit(queue_, 1, &submit, frameDone_[frame]), "vkQueueSubmit"))
            return false;

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &renderDone_[frame];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &index;
        result = vkQueuePresentKHR(queue_, &present);
        frame_ = (frame_ + 1) % kFramesInFlight;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
            swapchainStale_ = true;
        else if (result != VK_SUCCESS)
            return Check(result, "vkQueuePresentKHR");
        return true;
    }
}
