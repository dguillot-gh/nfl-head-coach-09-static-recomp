#include "gpu/vulkan_texture_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/command_processor.h"
#include "gpu/draw.h"
#include "kernel/guest.h"
#include "memory.h"

namespace hc09::gpu
{
    namespace
    {
        struct FormatInfo
        {
            VkFormat format;       // the image's
            uint32_t blockBytes;   // in guest memory: bytes per texel, or per 4x4 block when compressed
            bool compressed;
            bool decode;           // decoded to RGBA8 (Xenos components X, Y, Z, W) on the CPU
            uint8_t remap[4];      // host component holding Xenos component X, Y, Z, W
        };

        bool LookupFormat(uint32_t format, FormatInfo& info)
        {
            switch (format)
            {
            case 2: info = { VK_FORMAT_R8_UNORM, 1, false, false, { 0, 1, 2, 3 } }; return true;                 // k_8
            case 3: info = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, false, false, { 2, 1, 0, 3 } }; return true;    // k_1_5_5_5
            case 4: info = { VK_FORMAT_R5G6B5_UNORM_PACK16, 2, false, false, { 2, 1, 0, 3 } }; return true;      // k_5_6_5
            case 6: info = { VK_FORMAT_R8G8B8A8_UNORM, 4, false, false, { 0, 1, 2, 3 } }; return true;           // k_8_8_8_8
            case 7: info = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, false, false, { 0, 1, 2, 3 } }; return true; // k_2_10_10_10
            case 10: info = { VK_FORMAT_R8G8_UNORM, 2, false, false, { 0, 1, 2, 3 } }; return true;              // k_8_8
            case 15: info = { VK_FORMAT_R8G8B8A8_UNORM, 2, false, true, { 2, 1, 0, 3 } }; return true;           // k_4_4_4_4
            case 18: info = { VK_FORMAT_R8G8B8A8_UNORM, 8, true, true, { 0, 1, 2, 3 } }; return true;            // k_DXT1
            case 19: info = { VK_FORMAT_R8G8B8A8_UNORM, 16, true, true, { 0, 1, 2, 3 } }; return true;           // k_DXT2_3
            case 20: info = { VK_FORMAT_R8G8B8A8_UNORM, 16, true, true, { 0, 1, 2, 3 } }; return true;           // k_DXT4_5
            case 24: info = { VK_FORMAT_R16_UNORM, 2, false, false, { 0, 1, 2, 3 } }; return true;               // k_16
            case 25: info = { VK_FORMAT_R16G16_UNORM, 4, false, false, { 0, 1, 2, 3 } }; return true;            // k_16_16
            case 26: info = { VK_FORMAT_R16G16B16A16_UNORM, 8, false, false, { 0, 1, 2, 3 } }; return true;      // k_16_16_16_16
            case 30: info = { VK_FORMAT_R16_SFLOAT, 2, false, false, { 0, 1, 2, 3 } }; return true;              // k_16_FLOAT
            case 31: info = { VK_FORMAT_R16G16_SFLOAT, 4, false, false, { 0, 1, 2, 3 } }; return true;           // k_16_16_FLOAT
            case 32: info = { VK_FORMAT_R16G16B16A16_SFLOAT, 8, false, false, { 0, 1, 2, 3 } }; return true;     // k_16_16_16_16_FLOAT
            case 36: info = { VK_FORMAT_R32_SFLOAT, 4, false, false, { 0, 1, 2, 3 } }; return true;              // k_32_FLOAT
            case 37: info = { VK_FORMAT_R32G32_SFLOAT, 8, false, false, { 0, 1, 2, 3 } }; return true;           // k_32_32_FLOAT
            case 38: info = { VK_FORMAT_R32G32B32A32_SFLOAT, 16, false, false, { 0, 1, 2, 3 } }; return true;    // k_32_32_32_32_FLOAT
            case 49: info = { VK_FORMAT_R8G8B8A8_UNORM, 16, true, true, { 0, 1, 2, 3 } }; return true;           // k_DXN
            case 59: info = { VK_FORMAT_R8G8B8A8_UNORM, 8, true, true, { 0, 1, 2, 3 } }; return true;            // k_DXT5A
            default: return false;
            }
        }

        uint32_t GpuSwap(uint32_t value, uint32_t mode)
        {
            switch (mode & 3)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(value);
            case 3: return (value << 16) | (value >> 16);
            default: return value;
            }
        }

        // Untiled, byte-swapped texels (rows `pitch` bytes apart) as RGBA8 holding Xenos components
        // X, Y, Z, W (before the fetch swizzle). The formats LookupFormat decodes.
        bool DecodeTexels(uint32_t format, const FormatInfo& info, const uint8_t* rows, uint32_t pitch, uint32_t width, uint32_t height,
            uint8_t* out)
        {
            std::memset(out, 0, size_t(width) * height * 4);
            auto put = [&](uint32_t x, uint32_t y, const uint8_t host[4]) {
                if (x < width && y < height)
                    for (int c = 0; c < 4; c++)
                        out[(size_t(y) * width + x) * 4 + c] = host[info.remap[c]];
            };
            auto color565 = [](uint16_t v, uint8_t rgba[4]) {
                rgba[0] = uint8_t((v >> 11) * 255 / 31);
                rgba[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
                rgba[2] = uint8_t((v & 31) * 255 / 31);
                rgba[3] = 255;
            };
            switch (format)
            {
            case 18: case 19: case 20:  // DXT1, DXT2/3, DXT4/5
            {
                const uint32_t blockBytes = info.blockBytes;
                for (uint32_t by = 0; by < (height + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
                    {
                        const uint8_t* block = rows + size_t(by) * pitch + size_t(bx) * blockBytes;
                        const uint8_t* colorBlock = format == 18 ? block : block + 8;
                        const uint16_t c0 = uint16_t(colorBlock[0] | (colorBlock[1] << 8)), c1 = uint16_t(colorBlock[2] | (colorBlock[3] << 8));
                        uint8_t palette[4][4];
                        color565(c0, palette[0]);
                        color565(c1, palette[1]);
                        const bool fourColors = format != 18 || c0 > c1;
                        for (int c = 0; c < 3; c++)
                        {
                            palette[2][c] = uint8_t(fourColors ? (2 * palette[0][c] + palette[1][c]) / 3 : (palette[0][c] + palette[1][c]) / 2);
                            palette[3][c] = uint8_t(fourColors ? (palette[0][c] + 2 * palette[1][c]) / 3 : 0);
                        }
                        palette[2][3] = 255;
                        palette[3][3] = fourColors ? 255 : 0;
                        uint8_t alphas[8];
                        if (format == 20)
                        {
                            alphas[0] = block[0];
                            alphas[1] = block[1];
                            for (int i = 2; i < 8; i++)
                                alphas[i] = block[0] > block[1] ? uint8_t(((8 - i) * block[0] + (i - 1) * block[1]) / 7)
                                    : i < 6 ? uint8_t(((6 - i) * block[0] + (i - 1) * block[1]) / 5) : i == 6 ? 0 : 255;
                        }
                        uint64_t alphaBits = 0;
                        for (int i = 0; i < 6; i++)
                            alphaBits |= uint64_t(block[2 + i]) << (8 * i);
                        const uint32_t indices = colorBlock[4] | (colorBlock[5] << 8) | (colorBlock[6] << 16) | (uint32_t(colorBlock[7]) << 24);
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            uint8_t texel[4];
                            std::memcpy(texel, palette[(indices >> (2 * i)) & 3], 4);
                            if (format == 19)
                                texel[3] = uint8_t(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
                            else if (format == 20)
                                texel[3] = alphas[(alphaBits >> (3 * i)) & 7];
                            put(bx * 4 + i % 4, by * 4 + i / 4, texel);
                        }
                    }
                return true;
            }
            case 49: case 59:  // DXN (two channels), DXT5A (one): blocks like DXT5's alpha
            {
                const int channels = format == 49 ? 2 : 1;
                for (uint32_t by = 0; by < (height + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (width + 3) / 4; bx++)
                    {
                        uint8_t values[2][16]{};
                        for (int channel = 0; channel < channels; channel++)
                        {
                            const uint8_t* block = rows + size_t(by) * pitch + size_t(bx) * info.blockBytes + channel * 8;
                            uint8_t levels[8] = { block[0], block[1] };
                            for (int i = 2; i < 8; i++)
                                levels[i] = block[0] > block[1] ? uint8_t(((8 - i) * block[0] + (i - 1) * block[1]) / 7)
                                    : i < 6 ? uint8_t(((6 - i) * block[0] + (i - 1) * block[1]) / 5) : i == 6 ? 0 : 255;
                            uint64_t bits = 0;
                            for (int i = 0; i < 6; i++)
                                bits |= uint64_t(block[2 + i]) << (8 * i);
                            for (int i = 0; i < 16; i++)
                                values[channel][i] = levels[(bits >> (3 * i)) & 7];
                        }
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            const uint8_t texel[4] = { values[0][i], values[1][i], 0, 255 };
                            put(bx * 4 + i % 4, by * 4 + i / 4, texel);
                        }
                    }
                return true;
            }
            case 15:  // B4G4R4A4
                for (uint32_t y = 0; y < height; y++)
                    for (uint32_t x = 0; x < width; x++)
                    {
                        const uint8_t* at = rows + size_t(y) * pitch + size_t(x) * 2;
                        const uint16_t v = uint16_t(at[0] | (at[1] << 8));
                        const uint8_t texel[4] = { uint8_t(((v >> 8) & 15) * 17), uint8_t(((v >> 4) & 15) * 17), uint8_t((v & 15) * 17),
                            uint8_t((v >> 12) * 17) };
                        put(x, y, texel);
                    }
                return true;
            default:
                return false;
            }
        }

        // Offset in units of texels (or blocks) of (x, y) in a 2D tiled surface whose
        // width is `pitch` units of `bytesPerUnit` bytes (the standard Xbox 360 2D tiling).
        uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bytesPerUnit)
        {
            const uint32_t alignedPitch = (pitch + 31) & ~31u;
            const uint32_t logBpp = (bytesPerUnit >> 2) + ((bytesPerUnit >> 1) >> (bytesPerUnit >> 2));
            const uint32_t macro = ((x >> 5) + (y >> 5) * (alignedPitch >> 5)) << (logBpp + 7);
            const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
            const uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
            return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
                (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >> logBpp;
        }

        // Byte offset of block (x, y, z) in a tiled 3D surface whose pitch and height (in
        // blocks) are aligned to 32. Volumes use 32x16x4-block macro tiles of 128 blocks; the
        // bank/pipe/row bits are scattered into the address as for 2D surfaces.
        uint64_t TiledOffset3D(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch, uint32_t height, uint32_t blockBytes)
        {
            const uint32_t log2Bytes = blockBytes == 16 ? 4 : blockBytes == 8 ? 3 : blockBytes == 4 ? 2 : blockBytes == 2 ? 1 : 0;
            const uint64_t macro = ((uint64_t(z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5) + (x >> 5)) << 7;
            const uint64_t micro = ((z & 3) << 5) | (((y >> 1) & 3) << 3) | (x & 7);
            const uint64_t linear = (macro | micro) << log2Bytes;
            const uint64_t bank = ((y >> 3) ^ (z >> 2)) & 1;
            const uint64_t pipe = ((x >> 3) & 3) ^ (bank << 1);
            return (linear & 0xF) | (((linear >> 4) & 1) << 5) | (((linear >> 5) & 7) << 8) | ((linear >> 8) << 12) |
                (uint64_t(y & 1) << 4) | (pipe << 6) | (bank << 11);
        }

        VkComponentMapping EncodeMapping(uint32_t swizzle, const uint8_t remap[4])
        {
            static const bool identity = std::getenv("HC09_IDENTITY_SWIZZLE") != nullptr;  // debugging
            if (identity)
                return {};
            VkComponentSwizzle components[4];
            for (int i = 0; i < 4; i++)
            {
                const uint32_t select = (swizzle >> (i * 3)) & 7;
                if (select < 4)
                    components[i] = VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R + remap[select]);
                else if (select == 5)
                    components[i] = VK_COMPONENT_SWIZZLE_ONE;
                else
                    components[i] = VK_COMPONENT_SWIZZLE_ZERO;
            }
            return { components[0], components[1], components[2], components[3] };
        }

        void Barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to, uint32_t levels, uint32_t layers)
        {
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            barrier.oldLayout = from;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers };
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                1, &barrier);
        }

        constexpr int kEvictAfterFrames = 1800;  // ~1 minute without being sampled

        uint32_t ResolveTexelBytes(VkFormat format)
        {
            return format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        }

        // Fingerprint of the guest memory a resolve of this size covers (a tiled surface: 32x32-texel
        // aligned). Large regions are sampled: whole-texture loads change many of the samples.
        uint64_t MemoryFingerprint(uint32_t address, uint32_t width, uint32_t height, uint32_t texelBytes)
        {
            const uint32_t physical = address & 0x1FFFFFFF;
            uint64_t bytes = uint64_t((width + 31) & ~31u) * ((height + 31) & ~31u) * texelBytes;
            bytes = std::min<uint64_t>(bytes, 0x20000000ull - physical);
            const uint8_t* memory = GuestToHost(PhysicalToGuest(physical));
            constexpr uint64_t kChunk = 256, kChunks = 64;
            if (bytes <= kChunk * kChunks)
                return XXH3_64bits(memory, size_t(bytes));
            uint64_t hash = 0;
            const uint64_t step = (bytes - kChunk) / (kChunks - 1);
            for (uint64_t i = 0; i < kChunks; i++)
                hash = XXH3_64bits_withSeed(memory + i * step, kChunk, hash);
            return hash;
        }
    }

    TextureFetch ReadTextureFetch(uint32_t index)
    {
        TextureFetch fetch;
        for (uint32_t i = 0; i < 6; i++)
            fetch.words[i] = ReadRegister(0x4800 + index * 6 + i);
        return fetch;
    }

    VulkanTextureCache::~VulkanTextureCache()
    {
        for (auto& [key, entry] : entries_)
            ReleaseEntry(entry);
        for (auto& [address, resolved] : resolved_)
            ReleaseViews(resolved.views);
    }

    VkImageView VulkanTextureCache::GetView(Views& views, VkImage image, VkFormat format, VkImageViewType type,
        const VkComponentMapping& mapping, uint32_t levels, uint32_t layers)
    {
        const uint64_t key = uint64_t(type) | (uint64_t(mapping.r) << 4) | (uint64_t(mapping.g) << 8) | (uint64_t(mapping.b) << 12) |
            (uint64_t(mapping.a) << 16) | (uint64_t(levels) << 20);
        VkImageView& view = views[key];
        if (!view)
        {
            VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            info.image = image;
            info.viewType = type;
            info.format = format;
            info.components = mapping;
            info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers };
            if (vkCreateImageView(context_.device(), &info, nullptr, &view) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[texture] vkCreateImageView failed (format %d type %d)\n", int(format), int(type));
                view = VK_NULL_HANDLE;
                views.erase(key);
                return VK_NULL_HANDLE;
            }
        }
        return view;
    }

    void VulkanTextureCache::ReleaseViews(Views& views)
    {
        if (views.empty())
            return;
        std::vector<VkImageView> list;
        for (const auto& [key, view] : views)
            list.push_back(view);
        views.clear();
        VkDevice device = context_.device();
        context_.DestroyLater([device, list] {
            for (VkImageView view : list)
                vkDestroyImageView(device, view, nullptr);
        });
    }

    void VulkanTextureCache::ReleaseEntry(Entry& entry)
    {
        ReleaseViews(entry.views);
        VkDevice device = context_.device();
        VkImage image = entry.image;
        VkDeviceMemory memory = entry.memory;
        entry.image = VK_NULL_HANDLE;
        entry.memory = VK_NULL_HANDLE;
        context_.DestroyLater([device, image, memory] {
            vkDestroyImage(device, image, nullptr);
            vkFreeMemory(device, memory, nullptr);
        });
    }

    void VulkanTextureCache::SetResolved(uint32_t address, VkImage image, VkFormat format, uint32_t width, uint32_t height,
        bool swapRedBlue, uint32_t memoryTexelBytes)
    {
        Resolved& resolved = resolved_[address & 0x1FFFFFFF];
        if (resolved.image != image || resolved.format != format)
            ReleaseViews(resolved.views);
        resolved.image = image;
        resolved.format = format;
        resolved.width = width;
        resolved.height = height;
        resolved.swapRedBlue = swapRedBlue;
        resolved.texelBytes = memoryTexelBytes ? memoryTexelBytes : ResolveTexelBytes(format);
        resolved.memoryFingerprint = MemoryFingerprint(address, width, height, resolved.texelBytes);
        resolved.checkedFrame = FrameNumber();
        resolved.lastUsedFrame = FrameNumber();
        recent_.clear();
        recentGeneration_++;
    }

    bool VulkanTextureCache::ResolveStillValid(uint32_t address, Resolved& resolved, int frame)
    {
        if (resolved.checkedFrame == frame)
            return true;
        resolved.checkedFrame = frame;
        if (MemoryFingerprint(address, resolved.width, resolved.height, resolved.texelBytes) == resolved.memoryFingerprint)
            return true;
        // The title has written other data over this address since the resolve.
        static const bool trace = std::getenv("HC09_GPU_STATS") != nullptr;
        if (trace)
            std::fprintf(stderr, "[texture] resolve at 0x%08X (%ux%u) is stale: the memory now holds other data (frame %d)\n", address,
                resolved.width, resolved.height, frame);
        staleResolves_++;
        ForgetResolved(address);
        return false;
    }

    int VulkanTextureCache::ResolvedLastUse(uint32_t address) const
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        return it == resolved_.end() ? -1 : it->second.lastUsedFrame;
    }

    bool VulkanTextureCache::ResolvedMemoryChanged(uint32_t address) const
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        return it == resolved_.end() ||
            MemoryFingerprint(it->first, it->second.width, it->second.height, it->second.texelBytes) != it->second.memoryFingerprint;
    }

    void VulkanTextureCache::ForgetResolved(uint32_t address)
    {
        auto it = resolved_.find(address & 0x1FFFFFFF);
        if (it == resolved_.end())
            return;
        ReleaseViews(it->second.views);
        resolved_.erase(it);
        recent_.clear();
        recentGeneration_++;
    }

    void VulkanTextureCache::Evict(int frame)
    {
        if (frame % 300 != 0)
            return;
        for (auto it = entries_.begin(); it != entries_.end();)
        {
            if (frame - it->second.hashedFrame > kEvictAfterFrames)
            {
                ReleaseEntry(it->second);
                it = entries_.erase(it);
                evictedTextures_++;
            }
            else
            {
                ++it;
            }
        }
        recent_.clear();
        recentGeneration_++;
    }

    bool VulkanTextureCache::Get(const TextureFetch& fetch, BoundTexture& out)
    {
        const int frameNow = FrameNumber();
        if (recentFrame_ != frameNow)
        {
            recent_.clear();
            recentGeneration_++;
            recentFrame_ = frameNow;
        }
        const uint64_t recentKey = XXH3_64bits(fetch.words, sizeof(fetch.words));
        if (auto found = recent_.find(recentKey);
            found != recent_.end() && std::memcmp(found->second.fetch.words, fetch.words, sizeof(fetch.words)) == 0)
        {
            out = found->second.texture;
            return true;
        }
        const uint32_t format = fetch.format();
        FormatInfo info;
        const bool cube = fetch.dimension() == 3;
        const bool volume = fetch.dimension() == 2;
        // Stacked textures (2D arrays) are kept as 3D textures, one slice per layer.
        const bool stacked = fetch.dimension() == 1 && fetch.stacked() && fetch.stackDepth() > 1;
        const uint8_t identity[4] = { 0, 1, 2, 3 };
        // Depth formats (k_24_8, k_24_8_FLOAT) come from depth resolves, stored as R32_SFLOAT.
        if ((format == 22 || format == 23) && fetch.dimension() == 1)
        {
            auto it = resolved_.find(fetch.baseAddress() & 0x1FFFFFFF);
            if (it == resolved_.end() || !ResolveStillValid(it->first, it->second, FrameNumber()))
                return false;
            it->second.lastUsedFrame = FrameNumber();
            out = {};
            out.width = it->second.width;
            out.height = it->second.height;
            out.format = it->second.format;
            const VkComponentMapping red{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
            out.view = GetView(it->second.views, it->second.image, it->second.format, VK_IMAGE_VIEW_TYPE_2D, red, 1, 1);
            return out.view != VK_NULL_HANDLE;
        }
        bool supported = fetch.type() == 2 && fetch.dimension() != 0 && LookupFormat(format, info);
        if (supported && !info.decode)
        {
            static int checked[64]{};  // 0 unknown, 1 sampled, -1 not
            if (!checked[format])
                checked[format] = context_.SupportsFormat(info.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 1 : -1;
            supported = checked[format] > 0;
        }
        if (!supported)
        {
            if (!loggedUnsupported_[format])
            {
                loggedUnsupported_[format] = true;
                std::fprintf(stderr, "[texture] unsupported: format %u dimension %u\n", format, fetch.dimension());
            }
            return false;
        }

        const uint32_t width = volume ? fetch.width3D() : fetch.width();
        const uint32_t height = volume ? fetch.height3D() : fetch.height();
        const uint32_t depth = volume ? fetch.depth3D() : stacked ? fetch.stackDepth() : 1;
        out = {};
        out.depth = depth;
        out.stacked = stacked;
        out.width = width;
        out.height = height;
        out.type = cube ? VK_IMAGE_VIEW_TYPE_CUBE : (volume || stacked) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;

        // Render-to-texture: a resolve wrote this address. A fetch with another texel size than the
        // resolve wrote to memory (or a compressed format) cannot be reading that resolve; neither
        // can one after the title has loaded other data over it. A resolve to a one-channel format
        // (k_8, written from the target's red) is read through the target's red channel.
        auto it = resolved_.find(fetch.baseAddress() & 0x1FFFFFFF);
        if (it != resolved_.end() && (cube || volume || stacked || info.compressed || info.blockBytes != it->second.texelBytes))
        {
            mismatchedResolveHits_++;
            it = resolved_.end();
        }
        if (it != resolved_.end() && ResolveStillValid(it->first, it->second, FrameNumber()))
        {
            it->second.lastUsedFrame = FrameNumber();
            // The target holds red, green, blue, alpha; memory has red and blue exchanged when
            // the resolve swapped them, and the fetch swizzle applies to the memory order.
            const uint8_t swapped[4] = { 2, 1, 0, 3 };
            out.format = it->second.format;
            out.view = GetView(it->second.views, it->second.image, it->second.format, VK_IMAGE_VIEW_TYPE_2D,
                EncodeMapping(fetch.swizzle(), it->second.swapRedBlue ? swapped : identity), 1, 1);
            out.type = VK_IMAGE_VIEW_TYPE_2D;
            return out.view != VK_NULL_HANDLE;
        }

        // Source layout in guest memory: rows of blocks (or texels).
        const uint32_t blockSize = info.compressed ? 4 : 1;
        const uint32_t unit = info.blockBytes;
        const uint32_t blocksWide = (width + blockSize - 1) / blockSize;
        const uint32_t blocksHigh = (height + blockSize - 1) / blockSize;
        const uint32_t pitchBlocks = std::max(fetch.pitchTexels() / blockSize, blocksWide);
        const uint32_t sourceRows = fetch.tiled() ? ((blocksHigh + 31) & ~31u) : blocksHigh;
        const uint32_t sourcePitchBlocks = fetch.tiled() ? ((pitchBlocks + 31) & ~31u) : pitchBlocks;
        // Cube maps store their six faces one after another, each aligned to 4 KiB.
        // Volumes store depth slices (as a 3D-tiled surface when tiled); their slice stride uses
        // the height rounded up to 32 blocks.
        const uint32_t faces = cube ? 6 : (volume || stacked) ? depth : 1;
        const uint32_t volumeRows = (blocksHigh + 31) & ~31u;
        const uint64_t faceBytes = volume ? uint64_t(sourcePitchBlocks) * volumeRows * unit
                                          : uint64_t(sourcePitchBlocks) * sourceRows * unit;
        const uint64_t faceStride = volume ? faceBytes : (faceBytes + 4095) & ~4095ull;
        const uint64_t sourceBytes = volume && fetch.tiled()
            ? uint64_t(sourcePitchBlocks) * volumeRows * ((depth + 3) & ~3u) * unit
            : faceStride * (faces - 1) + faceBytes;
        const uint8_t* source = GuestToHost(PhysicalToGuest(fetch.baseAddress()));

        // Mip levels 1..n of 2D textures and cube maps live at the fetch constant's mip address.
        // A stored level's pitch and height are the base size rounded up to a power of two and
        // shifted by the level, aligned to 32x32-block tiles (linear: 256-byte rows); each level
        // (per face) is aligned to 4 KiB and the levels follow each other. With packed mips,
        // every level whose shorter side is 16 texels or less lives inside one stored level at a
        // fixed offset (the packed mip tail).
        uint32_t levels = 1;
        if (!volume && !stacked && fetch.mipAddress() != 0 && fetch.mipMaxLevel() > 0)
        {
            uint32_t sizeMax = 0;
            while ((std::max(width, height) >> (sizeMax + 1)) != 0)
                sizeMax++;
            levels = std::min(fetch.mipMaxLevel(), sizeMax) + 1;
        }
        auto log2Ceil = [](uint32_t value) {
            uint32_t log2 = 0;
            while ((1u << log2) < value)
                log2++;
            return log2;
        };
        const uint32_t log2W = log2Ceil(width), log2H = log2Ceil(height);
        const uint32_t shorterLog2 = std::min(log2W, log2H);
        const uint32_t packedBase = !fetch.packedMips() ? 32u : shorterLog2 > 4 ? shorterLog2 - 4 : 0u;
        struct LevelLayout
        {
            const uint8_t* data;        // the stored level holding this one (face 0)
            uint32_t pitch, rows;       // of the stored level, in blocks
            uint64_t faceStride;
            uint32_t x, y;              // packed offset inside the stored level, in blocks
            uint32_t blocksWide, blocksHigh;
        };
        std::vector<LevelLayout> layouts(levels);
        const uint8_t* mipSource = levels > 1 ? GuestToHost(PhysicalToGuest(fetch.mipAddress())) : nullptr;
        uint64_t mipBytes = 0;
        {
            uint64_t offset = 0;
            LevelLayout stored{ source, sourcePitchBlocks, sourceRows, faceStride, 0, 0, 0, 0 };
            for (uint32_t level = 0; level < levels; level++)
            {
                if (level > 0 && level <= packedBase)
                {
                    // A new stored level (levels past the packed base share its storage).
                    const uint32_t texelPitch = std::max((1u << log2W) >> level, 1u);
                    const uint32_t texelRows = std::max((1u << log2H) >> level, 1u);
                    uint32_t pitch = (texelPitch + blockSize - 1) / blockSize;
                    uint32_t rows = (texelRows + blockSize - 1) / blockSize;
                    if (fetch.tiled())
                    {
                        pitch = (pitch + 31) & ~31u;
                        rows = (rows + 31) & ~31u;
                    }
                    else
                    {
                        pitch = ((pitch * unit + 255) & ~255u) / unit;
                    }
                    const uint64_t stride = (uint64_t(pitch) * rows * unit + 4095) & ~4095ull;
                    stored = { mipSource + offset, pitch, rows, stride, 0, 0, 0, 0 };
                    offset += stride * faces;
                    mipBytes = offset;
                }
                LevelLayout& layout = layouts[level];
                layout = stored;
                layout.blocksWide = (std::max(width >> level, 1u) + blockSize - 1) / blockSize;
                layout.blocksHigh = (std::max(height >> level, 1u) + blockSize - 1) / blockSize;
                if (level >= packedBase)
                {
                    const uint32_t packed = level - packedBase;
                    const bool wide = log2W > log2H;
                    uint32_t x = 0, y = 0;
                    if (packed < 3)
                        (wide ? y : x) = 16u >> packed;
                    else
                        (wide ? x : y) = (1u << ((wide ? log2W : log2H) - packedBase)) >> (packed - 2);
                    layout.x = x / blockSize;
                    layout.y = y / blockSize;
                }
            }
        }

        const uint64_t key = (uint64_t(fetch.baseAddress()) << 32) ^ (uint64_t(format) << 26) ^ (uint64_t(width) << 13) ^ height ^
            (uint64_t(fetch.tiled()) << 63) ^ (uint64_t(fetch.endian()) << 61) ^ (uint64_t(pitchBlocks) << 40) ^ (uint64_t(cube) << 60) ^
            (uint64_t(depth) << 50) ^ (uint64_t(stacked) << 59) ^ (uint64_t(levels) << 54) ^ (uint64_t(fetch.mipAddress()) >> 3);
        Entry& entry = entries_[key];
        if (!entry.image)
        {
            VkImageCreateInfo image{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            image.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            image.imageType = (volume || stacked) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            image.format = info.format;
            image.extent = { width, height, (volume || stacked) ? faces : 1 };
            image.mipLevels = levels;
            image.arrayLayers = cube ? 6 : 1;
            image.samples = VK_SAMPLE_COUNT_1_BIT;
            image.tiling = VK_IMAGE_TILING_OPTIMAL;
            image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!context_.CreateImage(image, entry.image, entry.memory))
            {
                entries_.erase(key);
                return false;
            }
            entry.format = info.format;
            entry.contentHash = ~0ull;
        }

        // Hashing large textures on every draw dominates frame time, so a texture's contents
        // are checked once per frame (guest writes to it mid-frame are picked up next frame).
        auto fullHash = [&] {
            work_.hashedBytes += sourceBytes + mipBytes;
            uint64_t value = XXH3_64bits(source, sourceBytes);
            if (mipBytes)
                value ^= XXH3_64bits(mipSource, mipBytes) * 0x9E3779B97F4A7C15ull;
            return value;
        };
        const int frame = FrameNumber();
        work_.lookups++;
        // A texture that has never changed since its upload (most of them: 16 MB of hashing a frame in a
        // game) is checked every 4th frame, spread over textures by address; one that has changed before
        // (text glyph cache, video) every frame. A first change shows up to 3 frames late.
        const bool due = entry.rewritten || entry.contentHash == ~0ull || ((uint32_t(frame) + (fetch.baseAddress() >> 12)) & 3) == 0;
        const bool check = entry.hashedFrame != frame && due;
        entry.hashedFrame = frame;
        uint64_t hash = entry.contentHash;
        if (check)
        {
            // Large textures: a sampled hash (64 chunks spread over the base level) at each check,
            // which catches whole-image updates such as video, and a full hash every 15th check (~60 frames).
            // One that has changed since its first upload is hashed in full every frame: the
            // samples miss small updates such as the text glyph cache (a 256x512 texture the game
            // adds each new character to), whose new text would otherwise show other glyphs.
            constexpr uint64_t kFullHashLimit = 256 * 1024, kChunk = 256, kChunks = 64;
            if (sourceBytes + mipBytes <= kFullHashLimit || (frame / 4) % 15 == int(entry.contentHash % 15) || entry.contentHash == ~0ull ||
                entry.rewritten)
            {
                hash = fullHash();
                entry.sampledHash = 0;
            }
            else
            {
                uint64_t sampled = 0;
                const uint64_t step = (sourceBytes - kChunk) / (kChunks - 1);
                for (uint64_t i = 0; i < kChunks; i++)
                    sampled = XXH3_64bits_withSeed(source + i * step, kChunk, sampled);
                if (entry.sampledHash != 0 && sampled != entry.sampledHash)
                    hash = fullHash();  // changed: confirm with a full hash
                entry.sampledHash = sampled;
            }
        }
        if (hash != entry.contentHash)
        {
            entry.rewritten = entry.contentHash != ~0ull;
            entry.contentHash = hash;
            // Some textures (the field's grass detail noise) store levels 1..n that are not downscales of
            // level 0 but fresh full-contrast noise, which makes distant surfaces sparkle and repeat.
            // For 8-bit-per-channel 2D textures, level 1 is compared with a box-filtered level 0, and
            // when they are unrelated every level is rebuilt from level 0 instead.
            // HC09_ORIGINAL_MIPS=1 keeps the stored levels.
            static const bool originalMips = std::getenv("HC09_ORIGINAL_MIPS") != nullptr;
            const bool rebuildCandidate = !originalMips && !info.decode && levels > 1 && faces == 1 && !volume && !stacked &&
                (info.format == VK_FORMAT_R8_UNORM || info.format == VK_FORMAT_R8G8_UNORM || info.format == VK_FORMAT_R8G8B8A8_UNORM);
            bool rebuild = false;
            std::vector<uint8_t> previous;  // the level just uploaded (rows packed), for candidates
            std::vector<uint8_t> filtered;  // box-filtered previous level
            uint32_t previousWide = 0, previousHigh = 0;
            std::vector<uint8_t> rowsData;

            VkCommandBuffer commands = context_.uploads();
            const uint32_t layers = cube ? 6 : 1;
            Barrier(commands, entry.image, entry.uploaded ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, layers);
            std::vector<VkBufferImageCopy> regions;
            VkBuffer uploadBuffer = VK_NULL_HANDLE;
            for (uint32_t faceIndex = 0; faceIndex < faces; faceIndex++)
            {
                for (uint32_t level = 0; level < levels; level++)
                {
                    // Untile / unswap into packed rows.
                    const LevelLayout& layout = layouts[level];
                    const uint32_t rowBytes = layout.blocksWide * unit;
                    rowsData.resize(uint64_t(rowBytes) * layout.blocksHigh);
                    uint8_t* dest = rowsData.data();
                    const uint8_t* faceSource = layout.data + layout.faceStride * faceIndex;
                    for (uint32_t y = 0; y < layout.blocksHigh; y++)
                    {
                        uint8_t* row = dest + uint64_t(y) * rowBytes;
                        if (!fetch.tiled())
                        {
                            std::memcpy(row, faceSource + (uint64_t(y + layout.y) * layout.pitch + layout.x) * unit, rowBytes);
                        }
                        else if (volume)
                        {
                            for (uint32_t x = 0; x < layout.blocksWide; x++)
                                std::memcpy(row + x * unit, source + TiledOffset3D(x, y, faceIndex, sourcePitchBlocks, volumeRows, unit), unit);
                        }
                        else
                        {
                            for (uint32_t x = 0; x < layout.blocksWide; x++)
                                std::memcpy(row + x * unit,
                                    faceSource + uint64_t(TiledOffset2D(x + layout.x, y + layout.y, layout.pitch, unit)) * unit, unit);
                        }
                        if (fetch.endian() != 0)
                        {
                            for (uint32_t i = 0; i + 4 <= rowBytes; i += 4)
                            {
                                uint32_t word;
                                std::memcpy(&word, row + i, 4);
                                word = GpuSwap(word, fetch.endian());
                                std::memcpy(row + i, &word, 4);
                            }
                        }
                    }
                    if (rebuildCandidate && (level == 0 || rebuild || level == 1))
                    {
                        if (level > 0)
                        {
                            // Average each 2x2 footprint of the previous level (edges clamp on odd sizes).
                            filtered.resize(uint64_t(rowBytes) * layout.blocksHigh);
                            for (uint32_t y = 0; y < layout.blocksHigh; y++)
                            {
                                const uint32_t y0 = std::min(y * 2, previousHigh - 1), y1 = std::min(y * 2 + 1, previousHigh - 1);
                                for (uint32_t x = 0; x < layout.blocksWide; x++)
                                {
                                    const uint32_t x0 = std::min(x * 2, previousWide - 1), x1 = std::min(x * 2 + 1, previousWide - 1);
                                    for (uint32_t c = 0; c < unit; c++)
                                    {
                                        const auto at = [&](uint32_t px, uint32_t py) {
                                            return uint32_t(previous[(uint64_t(py) * previousWide + px) * unit + c]);
                                        };
                                        filtered[(uint64_t(y) * layout.blocksWide + x) * unit + c] =
                                            uint8_t((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                                    }
                                }
                            }
                            if (level == 1)
                            {
                                // Correlate the stored level 1 with the filtered level 0, per channel; any
                                // channel with real contrast that does not match marks the chain unrelated.
                                for (uint32_t c = 0; c < unit && !rebuild; c++)
                                {
                                    double sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0;
                                    const uint64_t count = uint64_t(layout.blocksWide) * layout.blocksHigh;
                                    for (uint64_t i = 0; i < count; i++)
                                    {
                                        const double a = filtered[i * unit + c];
                                        const double b = dest[i * unit + c];
                                        sumA += a, sumB += b, sumAA += a * a, sumBB += b * b, sumAB += a * b;
                                    }
                                    const double varA = sumAA - sumA * sumA / count, varB = sumBB - sumB * sumB / count;
                                    const double cov = sumAB - sumA * sumB / count;
                                    // Standard deviation above 8 (of 255) in both, correlation under 0.25.
                                    if (count >= 64 && varA > 64.0 * count && varB > 64.0 * count && cov < 0.25 * std::sqrt(varA * varB))
                                        rebuild = true;
                                }
                                if (rebuild)
                                    std::fprintf(stderr, "[gpu] texture 0x%08X (%ux%u format %u): stored mips do not match level 0; rebuilding them\n",
                                        fetch.baseAddress(), width, height, fetch.format());
                            }
                            if (rebuild)
                                std::memcpy(dest, filtered.data(), filtered.size());
                        }
                        previous.assign(dest, dest + uint64_t(rowBytes) * layout.blocksHigh);
                        previousWide = layout.blocksWide;
                        previousHigh = layout.blocksHigh;
                    }

                    const uint32_t levelWidth = std::max(width >> level, 1u), levelHeight = std::max(height >> level, 1u);
                    const uint64_t bytes = info.decode ? uint64_t(levelWidth) * levelHeight * 4 : rowsData.size();
                    const VulkanContext::Allocation upload = context_.Allocate(bytes, 16);
                    if (!upload.cpu)
                        continue;
                    work_.uploadBytes += bytes;
                    if (info.decode)
                        DecodeTexels(format, info, dest, rowBytes, levelWidth, levelHeight, upload.cpu);
                    else
                        std::memcpy(upload.cpu, dest, bytes);
                    static const bool fill = std::getenv("HC09_TEXTURE_FILL") != nullptr;  // debugging: every texel 0x80
                    if (fill)
                        std::memset(upload.cpu, 0x80, bytes);
                    if (uploadBuffer != upload.buffer && !regions.empty())
                    {
                        // The upload memory moved to a new chunk: copy what went into the last one.
                        vkCmdCopyBufferToImage(commands, uploadBuffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            uint32_t(regions.size()), regions.data());
                        regions.clear();
                    }
                    uploadBuffer = upload.buffer;
                    VkBufferImageCopy region{};
                    region.bufferOffset = upload.offset;
                    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, cube ? faceIndex : 0, 1 };
                    region.imageOffset = { 0, 0, int32_t((volume || stacked) ? faceIndex : 0) };
                    region.imageExtent = { levelWidth, levelHeight, 1 };
                    regions.push_back(region);
                }
            }
            if (!regions.empty())
                vkCmdCopyBufferToImage(commands, uploadBuffer, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                    regions.data());
            Barrier(commands, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, levels, layers);
            entry.uploaded = true;
            work_.uploads++;
        }
        out.format = entry.format;
        out.view = GetView(entry.views, entry.image, entry.format, out.type, EncodeMapping(fetch.swizzle(), info.decode ? identity : info.remap),
            levels, cube ? 6 : 1);
        if (!out.view)
            return false;
        recent_[recentKey] = { fetch, out };
        return true;
    }
}
