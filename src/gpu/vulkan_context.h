#pragma once

// Vulkan device, per-frame resources and presentation on an Android window (the D3D12Context
// counterpart). Works on Vulkan 1.0 devices (the LG V30): the instance asks for the loader's own
// version, and only core 1.0 plus VK_KHR_swapchain is required.
//
// A frame records into two command buffers submitted together: uploads() (texture uploads, run first)
// and commands() (everything else). Transient memory (Allocate) and descriptor sets live until the
// frame's work has finished on the GPU, kFramesInFlight frames later.
//
// Not thread-safe: one thread owns it (the render thread's test loop before the game draws, then the
// command-processor thread through the renderer).

#include <cstdint>
#include <functional>
#include <vector>

#include <vulkan/vulkan.h>

struct ANativeWindow;

namespace hc09::gpu
{
    class VulkanContext
    {
    public:
        ~VulkanContext();

        // Creates the instance and device (once) and, when `window` is not null, a swapchain for it.
        bool Initialize(ANativeWindow* window);
        // Drops the swapchain and surface (the window is going away); the device stays.
        void ReleaseWindow();
        void Shutdown();

        // Clears the next swapchain image to the colour and presents it. False when the frame was
        // skipped (window resized or gone: the swapchain is rebuilt on the next call).
        bool ClearAndPresent(float red, float green, float blue);

        // Ends the frame: when there is a window, acquires its next image and runs `draw` inside a
        // render pass (presentPass()) that clears it to black; submits the frame, presents, and starts
        // the next frame. False when nothing was presented.
        using PresentDraw = std::function<void(VkCommandBuffer commands, VkExtent2D extent)>;
        bool EndFrame(const PresentDraw& draw, const float clear[3] = nullptr);

        VkDevice device() const { return device_; }
        VkPhysicalDevice physical() const { return physical_; }
        const VkPhysicalDeviceProperties& properties() const { return properties_; }
        const VkPhysicalDeviceFeatures& features() const { return enabledFeatures_; }
        bool mirrorClampToEdge() const { return mirrorClampToEdge_; }
        VkPipelineCache pipelineCache() const { return pipelineCache_; }
        VkPhysicalDeviceProperties DeviceProperties() const { return properties_; }
        VkExtent2D Extent() const { return extent_; }
        bool hasWindow() const { return surface_ != VK_NULL_HANDLE; }
        VkFormat windowFormat() const { return format_; }
        // Render pass for the window's images (null until the first swapchain).
        VkRenderPass presentPass() const { return presentPass_; }

        // The frame's command buffer, and the one that runs before it (texture uploads).
        VkCommandBuffer commands() const { return commands_[frame_]; }
        VkCommandBuffer uploads();
        // Frames begun so far (transient allocations of earlier frames are gone).
        uint64_t frameCount() const { return frameCount_; }

        // Host-visible memory for this frame (uniforms, vertex data, indices, texture uploads).
        struct Allocation
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            uint8_t* cpu = nullptr;
        };
        Allocation Allocate(VkDeviceSize size, VkDeviceSize alignment = 16);
        // A descriptor set for this frame.
        VkDescriptorSet AllocateSet(VkDescriptorSetLayout layout);
        // Runs `release` once the GPU has finished the frame being recorded (destroys resources it uses).
        void DestroyLater(std::function<void()> release);

        // Device-local image with its own memory; false on failure (logged).
        bool CreateImage(const VkImageCreateInfo& info, VkImage& image, VkDeviceMemory& memory);
        uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;
        bool SupportsFormat(VkFormat format, VkFormatFeatureFlags features) const;

    private:
        static constexpr uint32_t kFramesInFlight = 2;

        bool CreateDevice();
        bool CreateSwapchain();
        void DestroySwapchain();
        void BeginFrame();

        VkInstance instance_ = VK_NULL_HANDLE;
        VkPhysicalDevice physical_ = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties_{};
        VkPhysicalDeviceMemoryProperties memoryProperties_{};
        VkPhysicalDeviceFeatures enabledFeatures_{};
        bool mirrorClampToEdge_ = false;
        VkDevice device_ = VK_NULL_HANDLE;
        uint32_t queueFamily_ = 0;
        VkQueue queue_ = VK_NULL_HANDLE;
        VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;

        ANativeWindow* window_ = nullptr;
        VkSurfaceKHR surface_ = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
        VkFormat format_ = VK_FORMAT_UNDEFINED;
        VkExtent2D extent_{};
        VkRenderPass presentPass_ = VK_NULL_HANDLE;
        std::vector<VkImage> images_;
        std::vector<VkImageView> views_;
        std::vector<VkFramebuffer> framebuffers_;
        bool swapchainStale_ = false;

        struct Chunk
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint8_t* cpu = nullptr;
            VkDeviceSize size = 0;
        };
        struct Frame
        {
            std::vector<Chunk> chunks;  // [0] is kept; the others are released when the frame is reused
            VkDeviceSize used = 0;      // in chunks.back()
            std::vector<VkDescriptorPool> pools;
            size_t pool = 0;            // the pool sets come from
            std::vector<std::function<void()>> releases;
            bool uploadsBegun = false;
        };
        Frame frames_[kFramesInFlight];
        bool CreateChunk(VkDeviceSize size, Chunk& chunk);
        void DestroyChunk(Chunk& chunk);
        VkDescriptorPool CreatePool();

        VkCommandPool commandPool_ = VK_NULL_HANDLE;
        VkCommandBuffer commands_[kFramesInFlight]{};
        VkCommandBuffer uploads_[kFramesInFlight]{};
        VkFence frameDone_[kFramesInFlight]{};
        VkSemaphore imageReady_[kFramesInFlight]{};
        VkSemaphore renderDone_[kFramesInFlight]{};
        uint32_t frame_ = 0;
        uint64_t frameCount_ = 0;
    };
}
