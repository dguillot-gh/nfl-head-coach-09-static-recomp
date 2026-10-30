#pragma once

// Vulkan device and presentation on an Android window (the D3D12Context counterpart; grows with the
// renderer). Works on Vulkan 1.0 devices (the LG V30): the instance asks for the loader's own version,
// and only core 1.0 plus VK_KHR_swapchain is required.
//
// Not thread-safe: one thread (the render thread) owns it.

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

struct ANativeWindow;

namespace hc09::gpu
{
    class VulkanContext
    {
    public:
        ~VulkanContext();

        // Creates the instance and device (once) and a swapchain for `window`.
        bool Initialize(ANativeWindow* window);
        // Drops the swapchain and surface (the window is going away); the device stays.
        void ReleaseWindow();
        void Shutdown();

        // Clears the next swapchain image to the colour and presents it. False when the frame was
        // skipped (window resized or gone: the swapchain is rebuilt on the next call).
        bool ClearAndPresent(float red, float green, float blue);

        VkPhysicalDeviceProperties DeviceProperties() const { return properties_; }
        VkExtent2D Extent() const { return extent_; }

    private:
        static constexpr uint32_t kFramesInFlight = 2;

        bool CreateDevice();
        bool CreateSwapchain();
        void DestroySwapchain();

        VkInstance instance_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_ = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties_{};
        VkDevice device_ = VK_NULL_HANDLE;
        uint32_t queueFamily_ = 0;
        VkQueue queue_ = VK_NULL_HANDLE;

        ANativeWindow* window_ = nullptr;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkExtent2D extent_{};
        VkRenderPass clearPass_ = VK_NULL_HANDLE;
        std::vector<VkImage> images_;
        std::vector<VkImageView> views_;
        std::vector<VkFramebuffer> framebuffers_;
        bool swapchainStale_ = false;

        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        VkCommandBuffer commands_[kFramesInFlight]{};
        VkFence frameDone_[kFramesInFlight]{};
        VkSemaphore imageReady_[kFramesInFlight]{};
        VkSemaphore renderDone_[kFramesInFlight]{};
        uint32_t frame_ = 0;
    };
}
