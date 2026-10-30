#pragma once

// Guest textures (from texture fetch constants) as Vulkan images, plus textures produced by EDRAM
// resolves: the Vulkan counterpart of texture_cache.h. Textures are untiled and byte-swapped on the
// CPU; compressed formats (DXT, DXN, DXT5A) and 4_4_4_4 are decoded to RGBA8 there too, since
// neither target phone samples BC formats. Uploads go into the frame's upload command buffer, which
// runs before the frame's draws.
//
// Not yet ported from the D3D12 cache: compute untiling of guest memory on the GPU, replacement
// images (PNG), resolved mip chains (ComposeResolvedMips) and the dump options.

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "gpu/texture_fetch.h"
#include "gpu/vulkan_context.h"

namespace hc09::gpu
{
    struct BoundTexture
    {
        VkImageView view = VK_NULL_HANDLE;
        VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 1;    // slices of a 3D or stacked texture
        bool stacked = false;  // sampled with a layer index rather than a normalized depth
    };

    class VulkanTextureCache
    {
    public:
        explicit VulkanTextureCache(VulkanContext& context) : context_(context) {}
        ~VulkanTextureCache();

        // Texture for a fetch constant, uploaded/refreshed as needed. Returns false when the
        // format or dimension is not supported yet.
        bool Get(const TextureFetch& fetch, BoundTexture& out);
        // Changes whenever Get might answer a fetch constant differently than before (new frame, a resolve,
        // an eviction): callers may keep Get's answers while it stays the same.
        uint64_t generation() const { return recentGeneration_; }

        // Registers a resolve destination so later fetches from `address` sample it (kept in
        // SHADER_READ_ONLY_OPTIMAL between resolves).
        // swapRedBlue: the resolve swapped red and blue on the way to memory (RB_COPY_DEST_INFO copy_dest_swap).
        // memoryTexelBytes: the size of a texel in the format the resolve writes to memory
        // (copy_dest_format), which fetches of the result use; 0 for the target's own.
        void SetResolved(uint32_t address, VkImage image, VkFormat format, uint32_t width, uint32_t height,
            bool swapRedBlue = false, uint32_t memoryTexelBytes = 0);

        // Last frame a fetch sampled the resolve at `address` (-1 if none or unknown).
        int ResolvedLastUse(uint32_t address) const;
        // True when the title has written other data over the resolve at `address` (or there is none):
        // only then can its result be released, since resolves are not written to memory.
        bool ResolvedMemoryChanged(uint32_t address) const;
        // Drops the record of a resolve (its target is being released).
        void ForgetResolved(uint32_t address);
        // Releases textures no draw has sampled for a while (called once per frame).
        void Evict(int frame);

        struct Stats
        {
            size_t textures = 0;
            size_t resolved = 0;
            uint64_t mismatchedResolveHits = 0;
            uint64_t staleResolves = 0;
            uint64_t evictedTextures = 0;
        };
        // HC09_GPU_STATS: work done by Get since the last call (lookups past the per-frame memo, bytes
        // hashed to detect changes, uploads and their bytes).
        struct Work
        {
            uint64_t lookups = 0, hashedBytes = 0, uploads = 0, uploadBytes = 0;
        };
        Work TakeWork()
        {
            const Work work = work_;
            work_ = {};
            return work;
        }
        Stats GetStats() const { return { entries_.size(), resolved_.size(), mismatchedResolveHits_, staleResolves_, evictedTextures_ }; }

    private:
        // Image views by type, component mapping and level count.
        using Views = std::unordered_map<uint64_t, VkImageView>;
        VkImageView GetView(Views& views, VkImage image, VkFormat format, VkImageViewType type, const VkComponentMapping& mapping,
            uint32_t levels, uint32_t layers);
        void ReleaseViews(Views& views);

        struct Entry
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            Views views;
            bool uploaded = false;     // in SHADER_READ_ONLY_OPTIMAL
            uint64_t contentHash = 0;
            int hashedFrame = -1;      // contents are re-checked at most once per frame
            uint64_t sampledHash = 0;  // large textures: hash of sampled chunks
            bool rewritten = false;    // changed since its first upload: hashed in full every frame
            bool watched = false;      // its memory is write-watched since watchStamp (then not hashed while unwritten)
            uint32_t watchStamp = 0;
        };
        void ReleaseEntry(Entry& entry);

        struct Resolved
        {
            VkImage image;
            VkFormat format;
            uint32_t width;
            uint32_t height;
            bool swapRedBlue;
            uint32_t texelBytes = 4;  // in memory
            // Resolves are not written back to guest memory, so the memory under a resolve keeps
            // whatever it held. A fingerprint of it, taken at each resolve, shows when the title
            // has since loaded something else there (the resolve is then stale).
            uint64_t memoryFingerprint = 0;
            int checkedFrame = -1;
            int lastUsedFrame = -1;
            Views views;
        };
        // True when the resolve still describes the memory at `address` (checked once per frame).
        bool ResolveStillValid(uint32_t address, Resolved& resolved, int frame);

        // Textures from memory already looked up this frame, by fetch constant: their contents are
        // checked once per frame, so a repeat lookup returns the same. Cleared by any resolve.
        struct Recent
        {
            TextureFetch fetch;
            BoundTexture texture;
        };
        std::unordered_map<uint64_t, Recent> recent_;
        int recentFrame_ = -1;
        uint64_t recentGeneration_ = 0;  // counts the clears of recent_
        Work work_;

        VulkanContext& context_;
        std::unordered_map<uint64_t, Entry> entries_;
        std::map<uint32_t, Resolved> resolved_;
        bool loggedUnsupported_[64]{};
        uint64_t mismatchedResolveHits_ = 0;
        uint64_t staleResolves_ = 0;
        uint64_t evictedTextures_ = 0;
    };
}
