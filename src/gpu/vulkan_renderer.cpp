// Executes Xenos draws, resolves and swaps with Vulkan: the Android app's renderer, following the
// D3D12 one (renderer.cpp) section by section.
//
// Same model: one host render target per EDRAM colour base + format (1280x720 x HC09_RESOLUTION_SCALE),
// one depth/stencil target per EDRAM depth base, resolves copied into textures keyed by destination
// address, and the resolved front buffer drawn to the window at each swap. Differences:
//   - Shaders are not compiled here: the SPIR-V built offline by tools/shaders_to_spirv.py ships in
//     the APK as assets/shaders/<microcode hash>.<vs|ps>.spv. The translator still runs (it is cheap)
//     for what the renderer needs to know about each shader (vertex streams, textures, filters).
//   - Vertex data and textures take the CPU paths (byte-swapped, untiled and uploaded per frame).
//   - Vulkan's clip space has y pointing down, so the viewport transform in DrawState is not flipped.
//   - Draws record into render passes, one per (colour, depth) target pair, kept open across draws.
//   - Rectangle lists use a geometry shader, so devices without one (the LG V30) skip them for now.
//   - No MSAA yet, no scene effects (SSAO, reflections), no BMP dumps.
//
// Debug: HC09_DUMP_FRAME=<n> prints every draw of frame n (and traces its packets).

#include <android/asset_manager.h>
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "android/app_glue.h"
#include "gpu/command_processor.h"
#include "gpu/draw.h"
#include "gpu/hlsl_translator.h"
#include "gpu/shaders.h"
#include "gpu/vulkan_context.h"
#include "gpu/vulkan_texture_cache.h"
#include "kernel/guest.h"
#include "memory.h"

namespace hc09::gpu
{
    namespace
    {
#include "gpu/vulkan_builtin_shaders.inc"

        namespace reg
        {
            constexpr uint32_t kSurfaceInfo = 0x2000;
            constexpr uint32_t kColorInfo = 0x2001;
            constexpr uint32_t kDepthInfo = 0x2002;
            constexpr uint32_t kDepthControl = 0x2200;
            constexpr uint32_t kStencilRefMask = 0x210D;
            constexpr uint32_t kDepthClear = 0x231C;
            constexpr uint32_t kColor1Info = 0x2003;
            constexpr uint32_t kColorMask = 0x2104;
            constexpr uint32_t kBlendControl0 = 0x2201;
            constexpr uint32_t kScModeControl = 0x2205;
            constexpr uint32_t kVteControl = 0x2206;
            constexpr uint32_t kPolyOffsetFront = 0x2380;  // scale, offset; back at 0x2382
            constexpr uint32_t kModeControl = 0x2208;
            constexpr uint32_t kWindowOffset = 0x2080;
            constexpr uint32_t kWindowScissorTl = 0x2081;
            constexpr uint32_t kWindowScissorBr = 0x2082;
            constexpr uint32_t kIndexOffset = 0x2102;
            constexpr uint32_t kViewportXScale = 0x210F;
            constexpr uint32_t kVsConst = 0x2307;
            constexpr uint32_t kPsConst = 0x2308;
            constexpr uint32_t kCopyControl = 0x2318;
            constexpr uint32_t kCopyDestBase = 0x2319;
            constexpr uint32_t kCopyDestPitch = 0x231A;
            constexpr uint32_t kColorClear = 0x231D;
            constexpr uint32_t kFloatConstants = 0x4000;
            constexpr uint32_t kFetchConstants = 0x4800;
            constexpr uint32_t kBoolConstants = 0x4900;
            constexpr uint32_t kLoopConstants = 0x4908;
        }

        constexpr uint32_t kModeColorDepth = 4;
        constexpr uint32_t kModeDepth = 5;
        constexpr uint32_t kModeCopy = 6;
        constexpr uint32_t kTargetWidth = 1280;
        constexpr uint32_t kTargetHeight = 720;
        constexpr uint32_t kPhysicalSize = 0x20000000;
        // HC09_RESOLUTION_SCALE=<1..4>: render targets (and resolve destinations) at that multiple of
        // the guest's 1280x720. Coordinates from the guest stay in guest pixels and are scaled at
        // the API boundary (viewports, scissors, copy/clear rectangles).
        // The options here are read when the renderer starts (ReadOptions), after the app has applied
        // env.txt: the library is loaded before that.
        uint32_t g_scale = 1;

        // Descriptor set 0, as tools/shaders_to_spirv.py shifts the translator's D3D registers.
        constexpr uint32_t kFloatConstantsBinding = 0;  // b0
        constexpr uint32_t kDrawStateBinding = 1;       // b1
        constexpr uint32_t kStreamBinding = 2;          // t0..t15
        constexpr uint32_t kTextureBinding = 2 + kTextureSlotBase;           // t16..t47
        constexpr uint32_t kFallbackBinding = 2 + kTextureFallbackSlotBase;  // t48..t79
        constexpr uint32_t kSamplerBinding = 100;       // s0..s31

        // HC09_DUMP_FRAME=<first>/<every>: dump that frame and every <every>th one after it.
        int g_dumpFrame = -1;
        int g_dumpEvery = 0;

        void ReadOptions()
        {
            const char* scale = std::getenv("HC09_RESOLUTION_SCALE");
            g_scale = uint32_t(std::clamp(scale ? std::atoi(scale) : 1, 1, 4));
            const char* spec = std::getenv("HC09_DUMP_FRAME");
            g_dumpFrame = spec ? std::atoi(spec) : -1;
            const char* slash = spec ? std::strchr(spec, '/') : nullptr;
            g_dumpEvery = slash ? std::max(std::atoi(slash + 1), 0) : 0;
        }
        int g_frame = 0;
        int g_drawInFrame = 0;

        float RegisterFloat(uint32_t index)
        {
            const uint32_t bits = ReadRegister(index);
            float value;
            std::memcpy(&value, &bits, 4);
            return value;
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

        // Bytes per texel of a texture format a resolve writes to memory (0 when not known).
        uint32_t MemoryTexelBytes(uint32_t format)
        {
            switch (format)
            {
            case 2: return 1;                                      // k_8
            case 3: case 4: case 10: case 15: case 24: case 30: return 2;  // 1_5_5_5, 5_6_5, 8_8, 4_4_4_4, 16, 16_FLOAT
            case 6: case 7: case 25: case 31: case 36: return 4;   // 8_8_8_8, 2_10_10_10, 16_16, 16_16_FLOAT, 32_FLOAT
            case 26: case 32: case 37: return 8;                   // 16_16_16_16, 16_16_16_16_FLOAT, 32_32_FLOAT
            case 38: return 16;                                    // 32_32_32_32_FLOAT
            default: return 0;
            }
        }

        uint64_t Mix(uint64_t hash, uint64_t value)
        {
            return (hash ^ value) * 0x100000001B3ull + (hash >> 29);
        }

        // Matches cbuffer DrawState in hlsl_translator.cpp.
        struct DrawState
        {
            uint32_t vsConstBase;
            uint32_t psConstBase;
            uint32_t wIsReciprocal;
            uint32_t xyPreDivided;
            float ndcScale[4];
            float ndcOffset[4];
            uint32_t boolConstants[8];
            uint32_t loopConstants[32];
            float textureSizes[32][4];
            uint32_t alphaFunc;  // 7 = always (alpha test disabled)
            float alphaRef;
            uint32_t vertexIndexOffset;  // VGT_INDX_OFFSET, added to every vertex index
            uint32_t pad0;
            uint32_t vertexStreams[16];  // per vertex stream: endian swap | byte offset in its view
        };

        VkFormat ColorFormat(uint32_t format)
        {
            switch (format)
            {
            case 2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case 3: case 7: case 12: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case 4: return VK_FORMAT_R16G16_SNORM;
            case 5: return VK_FORMAT_R16G16B16A16_SNORM;
            case 6: return VK_FORMAT_R16G16_SFLOAT;
            case 14: return VK_FORMAT_R32_SFLOAT;
            case 15: return VK_FORMAT_R32G32_SFLOAT;
            default: return VK_FORMAT_R8G8B8A8_UNORM;  // 8_8_8_8 and its gamma variant
            }
        }

        VkBlendFactor BlendFactor(uint32_t factor, bool alpha)
        {
            switch (factor)
            {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 1: return VK_BLEND_FACTOR_ONE;
            case 4: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
            case 9: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: case 14: return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: case 15: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return VK_BLEND_OP_SUBTRACT;
            case 2: return VK_BLEND_OP_MIN;
            case 3: return VK_BLEND_OP_MAX;
            case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
            default: return VK_BLEND_OP_ADD;
            }
        }

        // An image with its default view and the layout it was last left in.
        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;  // all aspects (colour, or depth + stencil)
            VkFormat format = VK_FORMAT_UNDEFINED;
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        };

        // Moves an image to another layout, waiting for all earlier work on it (simple and safe; the
        // stages can be narrowed later).
        void Transition(VkCommandBuffer commands, Image& image, VkImageLayout to)
        {
            if (image.layout == to)
                return;
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.oldLayout = image.layout;
            barrier.newLayout = to;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image.image;
            barrier.subresourceRange = { image.aspect, 0, 1, 0, 1 };
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                nullptr, 1, &barrier);
            image.layout = to;
        }

        struct RenderTarget
        {
            Image image;
            uint32_t samples = 1;
        };

        // Depth/stencil buffer for one EDRAM depth base (D24S8 and D24FS8 both map to D24S8).
        struct DepthTarget
        {
            Image image;
            VkImageView depthView = VK_NULL_HANDLE;  // depth aspect only, for depth resolves
            uint32_t samples = 1;
        };

        struct ResolveTarget
        {
            Image image;
            uint32_t width = 0;   // in guest pixels
            uint32_t height = 0;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;  // depth resolve targets only
            int lastResolved = -1;                       // frame of the latest resolve into it
        };

        struct CompiledShader
        {
            bool loaded = false;  // translation and SPIR-V lookup done
            bool ok = false;
            TranslatedShader info;
            VkShaderModule module = VK_NULL_HANDLE;
            std::vector<uint32_t> bindings;  // descriptor bindings the SPIR-V declares
        };

        // The binding a shader's resource has in the set: the samplers move down from 100..131 to
        // follow the textures (82..113), since sparse binding numbers that high are risky on old drivers.
        uint32_t HostBinding(uint32_t binding)
        {
            return binding >= kSamplerBinding ? kFallbackBinding + 32 + (binding - kSamplerBinding) : binding;
        }

        // Descriptor bindings (from SPIR-V OpDecorate ... Binding) of a shader module, which are
        // renumbered in place to HostBinding's.
        std::vector<uint32_t> SpirvBindings(uint32_t* words, size_t count)
        {
            std::vector<uint32_t> bindings;
            for (size_t i = 5; i < count;)
            {
                const uint32_t opcode = words[i] & 0xFFFF, length = words[i] >> 16;
                if (length == 0)
                    break;
                if (opcode == 71 && length >= 4 && words[i + 2] == 33)  // OpDecorate <id> Binding <n>
                {
                    bindings.push_back(words[i + 3]);
                    words[i + 3] = HostBinding(words[i + 3]);
                }
                i += length;
            }
            std::sort(bindings.begin(), bindings.end());
            bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
            return bindings;
        }

        VkDescriptorType BindingType(uint32_t binding)
        {
            if (binding <= kDrawStateBinding)
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            if (binding < kTextureBinding)
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (binding < kSamplerBinding)
                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        }

        // A pipeline as the renderer builds it: the shader pair and the render state it depends on. The
        // pre-compiler replays these from a list (pipelines.txt), so they are plain numbers.
        struct PipelineDesc
        {
            uint64_t vs = 0, ps = 0;  // microcode hashes
            uint32_t depthControl = 0, stencilMasks = 0, cull = 0;
            uint32_t polyOffsetEnable = 0, polyScale = 0, polyOffset = 0;
            uint32_t blend = 0, writeMask = 0, clipDisabled = 0;
            uint32_t format = 0, topology = 0, rectangles = 0, samples = 1;
            uint32_t pad = 0;

            uint64_t Key() const { return XXH3_64bits(this, sizeof(*this)); }

            std::string ToLine() const
            {
                char line[256];
                std::snprintf(line, sizeof(line), "%016llX %016llX %X %X %X %X %X %X %X %X %X %X %X %X %X\n", (unsigned long long)vs,
                    (unsigned long long)ps, depthControl, stencilMasks, cull, polyOffsetEnable, polyScale, polyOffset, blend, writeMask,
                    clipDisabled, format, topology, rectangles, samples);
                return line;
            }

            bool FromLine(const char* line)
            {
                unsigned long long v = 0, p = 0;
                const int fields = std::sscanf(line, "%llX %llX %X %X %X %X %X %X %X %X %X %X %X %X %X", &v, &p, &depthControl,
                    &stencilMasks, &cull, &polyOffsetEnable, &polyScale, &polyOffset, &blend, &writeMask, &clipDisabled, &format,
                    &topology, &rectangles, &samples);
                vs = v;
                ps = p;
                pad = 0;
                return fields == 15;
            }
        };
        static_assert(sizeof(PipelineDesc) == 72);  // no padding: hashed as bytes

        // A pipeline's create info with everything it points to, so it can be built on another thread.
        struct PipelineBuild
        {
            VkPipelineShaderStageCreateInfo stages[3];
            VkPipelineVertexInputStateCreateInfo vertexInput;
            VkPipelineInputAssemblyStateCreateInfo assembly;
            VkPipelineViewportStateCreateInfo viewport;
            VkPipelineRasterizationStateCreateInfo raster;
            VkPipelineMultisampleStateCreateInfo multisample;
            VkPipelineDepthStencilStateCreateInfo ds;
            VkPipelineColorBlendAttachmentState target;
            VkPipelineColorBlendStateCreateInfo blendState;
            VkDynamicState dynamicStates[4];
            VkPipelineDynamicStateCreateInfo dynamic;
            VkGraphicsPipelineCreateInfo info;
            PipelineDesc desc;
            std::chrono::steady_clock::time_point start;
            std::mutex mutex;
            std::condition_variable finished;
            bool done = false;
            VkResult result = VK_INCOMPLETE;
            VkPipeline pipeline = VK_NULL_HANDLE;
        };

        // Fills `build` to make the pipeline `desc` describes (pointers into `build` itself).
        void FillPipeline(const PipelineDesc& desc, VkShaderModule vs, VkShaderModule ps, VkShaderModule gs, VkPipelineLayout layout,
            VkRenderPass renderPass, bool depthClamp, bool blendable, PipelineBuild& build)
        {
            uint32_t stageCount = 0;
            build.stages[stageCount++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                vs, "main", nullptr };
            if (desc.rectangles)
                build.stages[stageCount++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT,
                    gs, "rectangle_gs", nullptr };
            build.stages[stageCount++] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                ps, "main", nullptr };

            build.vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            build.assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            const auto topology = VkPrimitiveTopology(desc.topology);
            build.assembly.topology = topology;
            build.assembly.primitiveRestartEnable =
                topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            build.viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            build.viewport.viewportCount = build.viewport.scissorCount = 1;

            auto& raster = build.raster;
            raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.lineWidth = 1.0f;
            // PA_SU_SC_MODE_CNTL: cull_front (bit 0), cull_back (bit 1), face (bit 2: 1 = CW is front).
            // Positions reach the framebuffer as on D3D12 (same screen-space winding), so the mapping is too.
            const uint32_t cull = desc.cull;
            raster.cullMode = (cull & 3) == 1 ? VK_CULL_MODE_FRONT_BIT : (cull & 3) == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
            raster.frontFace = (cull & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.depthClampEnable = desc.clipDisabled && depthClamp;
            if (desc.polyOffsetEnable)
            {
                float scale, offset;
                std::memcpy(&scale, &desc.polyScale, 4);
                std::memcpy(&offset, &desc.polyOffset, 4);
                raster.depthBiasEnable = VK_TRUE;
                raster.depthBiasSlopeFactor = scale / 16.0f;
                // Rounded away from zero so a small offset still separates the layers.
                const float units = offset * 16777216.0f;
                raster.depthBiasConstantFactor = units < 0 ? std::floor(units) : std::ceil(units);
            }
            build.multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            build.multisample.rasterizationSamples = VkSampleCountFlagBits(desc.samples);

            // RB_DEPTHCONTROL: stencil enable (0), z enable (1), z write (2), z func (4-6),
            // backface stencil enable (7), front stencil func/fail/zpass/zfail (8-19), back (20-31).
            // Xenos compare functions and stencil ops are Vulkan's enums.
            const uint32_t depthControl = desc.depthControl, stencilMasks = desc.stencilMasks;
            auto& ds = build.ds;
            ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            ds.depthTestEnable = (depthControl >> 1) & 1;
            static const bool noDepthTest = std::getenv("HC09_NO_DEPTH_TEST") != nullptr;  // debugging
            if (noDepthTest)
                ds.depthTestEnable = VK_FALSE;
            ds.depthWriteEnable = ds.depthTestEnable && ((depthControl >> 2) & 1);
            ds.depthCompareOp = VkCompareOp((depthControl >> 4) & 7);
            ds.stencilTestEnable = depthControl & 1;
            auto face = [&](uint32_t shift) {
                VkStencilOpState op{};
                op.compareOp = VkCompareOp((depthControl >> shift) & 7);
                op.failOp = VkStencilOp((depthControl >> (shift + 3)) & 7);
                op.passOp = VkStencilOp((depthControl >> (shift + 6)) & 7);
                op.depthFailOp = VkStencilOp((depthControl >> (shift + 9)) & 7);
                op.compareMask = (stencilMasks >> 8) & 0xFF;
                op.writeMask = (stencilMasks >> 16) & 0xFF;
                return op;
            };
            ds.front = face(8);
            ds.back = ((depthControl >> 7) & 1) ? face(20) : ds.front;

            auto& target = build.target;
            target = {};
            const uint32_t blend = desc.blend;
            const uint32_t srcColor = blend & 0x1F, colorOp = (blend >> 5) & 7, dstColor = (blend >> 8) & 0x1F;
            const uint32_t srcAlpha = (blend >> 16) & 0x1F, alphaOp = (blend >> 21) & 7, dstAlpha = (blend >> 24) & 0x1F;
            target.blendEnable = !(srcColor == 1 && dstColor == 0 && colorOp == 0 && srcAlpha == 1 && dstAlpha == 0 && alphaOp == 0);
            // Formats that cannot blend (some float targets) draw unblended.
            if (!blendable)
                target.blendEnable = VK_FALSE;
            target.srcColorBlendFactor = BlendFactor(srcColor, false);
            target.dstColorBlendFactor = BlendFactor(dstColor, false);
            target.colorBlendOp = BlendOp(colorOp);
            target.srcAlphaBlendFactor = BlendFactor(srcAlpha, true);
            target.dstAlphaBlendFactor = BlendFactor(dstAlpha, true);
            target.alphaBlendOp = BlendOp(alphaOp);
            target.colorWriteMask = desc.writeMask;
            build.blendState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            build.blendState.attachmentCount = 1;
            build.blendState.pAttachments = &build.target;

            const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS };
            std::copy(std::begin(dynamicStates), std::end(dynamicStates), build.dynamicStates);
            build.dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            build.dynamic.dynamicStateCount = uint32_t(std::size(dynamicStates));
            build.dynamic.pDynamicStates = build.dynamicStates;

            auto& info = build.info;
            info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            info.stageCount = stageCount;
            info.pStages = build.stages;
            info.pVertexInputState = &build.vertexInput;
            info.pInputAssemblyState = &build.assembly;
            info.pViewportState = &build.viewport;
            info.pRasterizationState = &build.raster;
            info.pMultisampleState = &build.multisample;
            info.pDepthStencilState = &build.ds;
            info.pColorBlendState = &build.blendState;
            info.pDynamicState = &build.dynamic;
            info.layout = layout;
            info.renderPass = renderPass;
            build.desc = desc;
        }

        // At most this many pipelines are compiled at once by the game's draws (the compiler shares the
        // CPU with the game).
        class BuildSlots
        {
        public:
            void Acquire()
            {
                std::unique_lock lock(mutex_);
                free_.wait(lock, [this] { return used_ < 3; });
                used_++;
            }
            void Release()
            {
                std::lock_guard lock(mutex_);
                used_--;
                free_.notify_one();
            }

        private:
            std::mutex mutex_;
            std::condition_variable free_;
            int used_ = 0;
        };
        BuildSlots g_buildSlots;

        // Pre-compile progress for the app's "Preparing shaders" screen (total 0: not running).
        std::atomic<int> g_precompileTotal{ 0 }, g_precompileDone{ 0 };

        // A file of the APK's assets (null when missing).
        bool ReadAsset(const char* path, std::vector<uint8_t>& data)
        {
            AAsset* asset = android::Assets() ? AAssetManager_open(android::Assets(), path, AASSET_MODE_BUFFER) : nullptr;
            if (!asset)
                return false;
            data.resize(size_t(AAsset_getLength(asset)));
            const bool read = AAsset_read(asset, data.data(), data.size()) == int(data.size());
            AAsset_close(asset);
            return read;
        }

        // A colour + depth/stencil render pass that loads and stores both (null on failure, logged).
        VkRenderPass MakeRenderPass(VkDevice device, VkFormat format, uint32_t samples, VkFormat depthFormat)
        {
            VkAttachmentDescription attachments[2]{};
            attachments[0].format = format;
            attachments[0].samples = VkSampleCountFlagBits(samples);
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[0].initialLayout = attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments[1].format = depthFormat;
            attachments[1].samples = VkSampleCountFlagBits(samples);
            attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[1].storeOp = attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].initialLayout = attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            VkAttachmentReference color{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkAttachmentReference depth{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color;
            subpass.pDepthStencilAttachment = &depth;
            VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            info.attachmentCount = 2;
            info.pAttachments = attachments;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            VkRenderPass pass = VK_NULL_HANDLE;
            if (vkCreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[gpu] render pass creation failed (format %d)\n", int(format));
                return VK_NULL_HANDLE;
            }
            return pass;
        }

        // The descriptor set and pipeline layouts of a vertex + pixel shader pair.
        struct ShaderPair
        {
            std::vector<uint32_t> bindings;
            VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
            VkPipelineLayout layout = VK_NULL_HANDLE;
        };

        class Renderer
        {
        public:
            bool Initialize();
            void Draw(const DrawInfo& draw);
            void Swap(uint32_t frontBuffer);

        private:
            VkDevice device() const { return context_.device(); }
            VkCommandBuffer commands() const { return context_.commands(); }

            bool CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                VkImageAspectFlags aspect, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D, uint32_t layers = 1, uint32_t depth = 1);
            void DestroyImageLater(Image& image);
            VkShaderModule CreateModule(const uint32_t* words, size_t bytes);
            bool CreateBuiltins();
            bool CreatePresentPipeline();
            VkPipeline CreateFullscreenPipeline(VkShaderModule ps, VkPipelineLayout layout, VkRenderPass pass);
            void CheckWindow();

            const CompiledShader& GetShader(const std::shared_ptr<const ShaderMicrocode>& microcode);
            ShaderPair* GetShaderPair(const CompiledShader& vs, const CompiledShader& ps);
            RenderTarget& GetRenderTarget(uint32_t colorInfo, uint32_t samples = 1);
            DepthTarget& GetDepthTarget(uint32_t depthInfo, uint32_t samples = 1);
            uint32_t SampleCount(uint32_t surfaceInfo) const { return 1; }
            std::set<uint32_t> tiledSurfaces_;  // RB_SURFACE_INFO values seen while tiling
            VkRenderPass GetRenderPass(VkFormat format, uint32_t samples);
            VkFramebuffer GetFramebuffer(RenderTarget& target, DepthTarget& depth);
            VkPipeline GetPipeline(const CompiledShader& vs, const CompiledShader& ps, const ShaderPair& pair, VkFormat format,
                VkPrimitiveTopology topology, bool rectangles, bool colorWrites, uint32_t samples);
            VkSampler GetSampler(const VkSamplerCreateInfo& info);
            // Render pass on a target pair (kept open while draws keep using the same pair).
            void BeginPass(RenderTarget& target, DepthTarget& depth);
            void EndPass();
            RenderTarget* passTarget_ = nullptr;
            DepthTarget* passDepth_ = nullptr;

            void Resolve();
            void ResolveCopy();
            void ResolveDepth(uint32_t control);
            void ClearRect(RenderTarget& target, DepthTarget& depth, VkImageAspectFlags aspects, const VkClearValue& value,
                const VkRect2D& rect);
            VkRect2D ResolveRectangle(uint32_t destWidth, uint32_t destHeight);
            bool PrepareIndices(const DrawInfo& draw, std::vector<uint32_t>& indices, bool& indexed, VkPrimitiveTopology& topology,
                bool& rectangles);
            // Rectangle lists without a geometry shader: new vertex streams holding two triangles per
            // rectangle (non-indexed, index offset applied); returns the vertex count.
            uint32_t ExpandRectangles(const CompiledShader& vs, const std::vector<uint32_t>& indices, uint32_t count,
                VkDescriptorBufferInfo* streams);
            // Releases resolve targets neither resolved to nor sampled for a while, and stale textures.
            void EvictIdleResources(uint32_t frontBuffer);

            VulkanContext context_;
            uint64_t windowGeneration_ = 0;
            std::unique_ptr<VulkanTextureCache> textures_;
            VkFormat depthFormat_ = VK_FORMAT_D24_UNORM_S8_UINT;
            bool depthSampleable_ = false;

            VkShaderModule presentVs_ = VK_NULL_HANDLE, presentPs_ = VK_NULL_HANDLE, fullscreenVs_ = VK_NULL_HANDLE,
                depthCopyPs_ = VK_NULL_HANDLE, rectangleGs_ = VK_NULL_HANDLE;
            VkSampler linearClamp_ = VK_NULL_HANDLE;
            VkDescriptorSetLayout presentSetLayout_ = VK_NULL_HANDLE, depthCopySetLayout_ = VK_NULL_HANDLE;
            VkPipelineLayout presentLayout_ = VK_NULL_HANDLE, depthCopyLayout_ = VK_NULL_HANDLE;
            VkPipeline presentPipeline_ = VK_NULL_HANDLE;
            VkRenderPass presentPipelinePass_ = VK_NULL_HANDLE;  // the pass presentPipeline_ was made for
            VkRenderPass depthCopyPass_ = VK_NULL_HANDLE;
            VkPipeline depthCopyPipeline_ = VK_NULL_HANDLE;

            // Bound where a draw's shader declares a resource the draw has none for.
            Image dummy2D_, dummyCube_, dummy3D_;
            VkBuffer dummyBuffer_ = VK_NULL_HANDLE;
            VkDeviceMemory dummyBufferMemory_ = VK_NULL_HANDLE;

            std::unordered_map<uint64_t, CompiledShader> shaders_;
            std::unordered_map<uint64_t, ShaderPair> shaderPairs_;
            std::unordered_map<uint64_t, VkPipeline> pipelines_;
            std::set<uint64_t> failedPairs_;  // shader pairs whose pipelines the driver refused
            std::unordered_map<uint64_t, std::shared_ptr<PipelineBuild>> pendingPipelines_;
            VkPipeline FinishPipeline(uint64_t key, uint64_t pairKey, PipelineBuild& build);
            // Pipelines seen so far (this phone's list, HC09_PIPELINE_LIST, plus the APK's pipelines.txt):
            // built in the background at start, so the driver's cache has them before the game asks.
            void StartPrecompile();
            void RecordPipeline(const PipelineDesc& desc);
            std::set<uint64_t> recordedPipelines_;
            bool pipelinesAdded_ = false;  // since the pipeline cache was last saved
            std::map<std::pair<VkFormat, uint32_t>, VkRenderPass> renderPasses_;
            std::map<std::pair<VkImageView, VkImageView>, VkFramebuffer> framebuffers_;
            std::unordered_map<uint64_t, VkSampler> samplers_;
            std::unordered_map<uint32_t, RenderTarget> renderTargets_;
            std::unordered_map<uint32_t, DepthTarget> depthTargets_;
            // Vertex data already converted this frame, by guest address and endian mode: draws sharing
            // one vertex buffer reuse a single conversion.
            struct VertexUpload
            {
                VulkanContext::Allocation allocation;
                uint32_t dwords = 0;
            };
            std::unordered_map<uint64_t, VertexUpload> vertexUploads_;
            uint64_t vertexUploadFrame_ = ~0ull;
            // The float constants as last uploaded (raw guest bytes) and where they went.
            uint32_t lastConstants_[2048]{};
            VulkanContext::Allocation lastConstantsUpload_{};
            uint64_t lastConstantsFrame_ = ~0ull;
            std::unordered_map<uint32_t, ResolveTarget> resolveTargets_;
            uint32_t skippedDraws_ = 0;
            uint32_t draws_ = 0;
        };

        bool Renderer::Initialize()
        {
            android::TakeOverWindow();
            ANativeWindow* window = nullptr;
            android::PollWindow(windowGeneration_, window);
            const bool ok = context_.Initialize(window);
            if (window)
                ANativeWindow_release(window);
            if (!context_.device())
                return false;
            if (!ok)
                std::fprintf(stderr, "[gpu] no window yet\n");
            textures_ = std::make_unique<VulkanTextureCache>(context_);
            if (!context_.SupportsFormat(depthFormat_, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
                depthFormat_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
            depthSampleable_ = context_.SupportsFormat(depthFormat_, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
            std::fprintf(stderr, "[gpu] Vulkan renderer: depth format %d (sampled %d), resolution scale %u\n", int(depthFormat_),
                int(depthSampleable_), g_scale);
            if (!CreateBuiltins())
                return false;
            StartPrecompile();
            return true;
        }

        bool Renderer::CreateImage(Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
            VkImageAspectFlags aspect, VkImageViewType viewType, uint32_t layers, uint32_t depth)
        {
            image = {};
            VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            info.flags = viewType == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            info.imageType = viewType == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = { width, height, depth };
            info.mipLevels = 1;
            info.arrayLayers = layers;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!context_.CreateImage(info, image.image, image.memory))
                return false;
            VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            view.image = image.image;
            view.viewType = viewType;
            view.format = format;
            view.subresourceRange = { aspect, 0, 1, 0, layers };
            if (vkCreateImageView(device(), &view, nullptr, &image.view) != VK_SUCCESS)
                return false;
            image.format = format;
            image.aspect = aspect;
            return true;
        }

        void Renderer::DestroyImageLater(Image& image)
        {
            VkDevice device = this->device();
            const Image old = image;
            context_.DestroyLater([device, old] {
                vkDestroyImageView(device, old.view, nullptr);
                vkDestroyImage(device, old.image, nullptr);
                vkFreeMemory(device, old.memory, nullptr);
            });
            image = {};
        }

        VkShaderModule Renderer::CreateModule(const uint32_t* words, size_t bytes)
        {
            VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            info.codeSize = bytes;
            info.pCode = words;
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device(), &info, nullptr, &module) != VK_SUCCESS)
                std::fprintf(stderr, "[gpu] vkCreateShaderModule failed\n");
            return module;
        }

        bool Renderer::CreateBuiltins()
        {
            presentVs_ = CreateModule(kPresentVs, sizeof(kPresentVs));
            presentPs_ = CreateModule(kPresentPs, sizeof(kPresentPs));
            fullscreenVs_ = CreateModule(kFullscreenVs, sizeof(kFullscreenVs));
            depthCopyPs_ = CreateModule(kDepthCopyPs, sizeof(kDepthCopyPs));
            // HC09_NO_GEOMETRY_SHADER=1 (debugging): expand rectangles on the CPU, as devices without one do.
            if (context_.features().geometryShader && !std::getenv("HC09_NO_GEOMETRY_SHADER"))
                rectangleGs_ = CreateModule(kRectangleGs, sizeof(kRectangleGs));
            if (!presentVs_ || !presentPs_ || !fullscreenVs_ || !depthCopyPs_)
                return false;

            VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sampler.maxLod = VK_LOD_CLAMP_NONE;
            linearClamp_ = GetSampler(sampler);

            // Present: the front buffer and a sampler. Depth copy: the depth plane.
            const VkDescriptorSetLayoutBinding presentBindings[] = {
                { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            };
            auto makeLayouts = [&](const VkDescriptorSetLayoutBinding* bindings, uint32_t count, VkDescriptorSetLayout& setLayout,
                                   VkPipelineLayout& layout) {
                VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                setInfo.bindingCount = count;
                setInfo.pBindings = bindings;
                VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                layoutInfo.setLayoutCount = 1;
                layoutInfo.pSetLayouts = &setLayout;
                return vkCreateDescriptorSetLayout(device(), &setInfo, nullptr, &setLayout) == VK_SUCCESS &&
                    vkCreatePipelineLayout(device(), &layoutInfo, nullptr, &layout) == VK_SUCCESS;
            };
            if (!makeLayouts(presentBindings, 2, presentSetLayout_, presentLayout_) ||
                !makeLayouts(presentBindings, 1, depthCopySetLayout_, depthCopyLayout_))
                return false;

            // Depth resolves render the depth plane into an R32_SFLOAT target.
            VkAttachmentDescription attachment{};
            attachment.format = VK_FORMAT_R32_SFLOAT;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            VkAttachmentReference reference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &reference;
            VkRenderPassCreateInfo passInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
            passInfo.attachmentCount = 1;
            passInfo.pAttachments = &attachment;
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            if (vkCreateRenderPass(device(), &passInfo, nullptr, &depthCopyPass_) != VK_SUCCESS)
                return false;
            if (depthSampleable_)
                depthCopyPipeline_ = CreateFullscreenPipeline(depthCopyPs_, depthCopyLayout_, depthCopyPass_);

            // Stand-ins for resources a draw does not have: black, transparent textures and a zeroed buffer.
            const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (!CreateImage(dummy2D_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT) ||
                !CreateImage(dummyCube_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_CUBE, 6) ||
                !CreateImage(dummy3D_, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D))
                return false;
            for (Image* image : { &dummy2D_, &dummyCube_, &dummy3D_ })
            {
                const uint32_t layers = image == &dummyCube_ ? 6 : 1;
                VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image->image;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
                vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                    nullptr, 1, &barrier);
                // HC09_DUMMY_TEXTURES=1 (debugging): every draw samples these, grey.
                static const bool grey = std::getenv("HC09_DUMMY_TEXTURES") != nullptr;
                const VkClearColorValue zero{ { grey ? 0.5f : 0.0f, grey ? 0.5f : 0.0f, grey ? 0.5f : 0.0f, grey ? 1.0f : 0.0f } };
                vkCmdClearColorImage(commands(), image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &barrier.subresourceRange);
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(commands(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                    nullptr, 1, &barrier);
                image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bufferInfo.size = 256;
            bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            if (vkCreateBuffer(device(), &bufferInfo, nullptr, &dummyBuffer_) != VK_SUCCESS)
                return false;
            VkMemoryRequirements requirements;
            vkGetBufferMemoryRequirements(device(), dummyBuffer_, &requirements);
            VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = context_.FindMemoryType(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (vkAllocateMemory(device(), &allocate, nullptr, &dummyBufferMemory_) != VK_SUCCESS)
                return false;
            vkBindBufferMemory(device(), dummyBuffer_, dummyBufferMemory_, 0);
            void* mapped = nullptr;
            if (vkMapMemory(device(), dummyBufferMemory_, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS)
            {
                std::memset(mapped, 0, size_t(requirements.size));
                vkUnmapMemory(device(), dummyBufferMemory_);
            }
            return true;
        }

        VkPipeline Renderer::CreateFullscreenPipeline(VkShaderModule ps, VkPipelineLayout layout, VkRenderPass pass)
        {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                ps == presentPs_ ? presentVs_ : fullscreenVs_, "main", nullptr };
            stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps,
                "main", nullptr };
            VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blendTarget{};
            blendTarget.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            blend.attachmentCount = 1;
            blend.pAttachments = &blendTarget;
            const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dynamicStates;
            VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewport;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = layout;
            info.renderPass = pass;
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (vkCreateGraphicsPipelines(device(), context_.pipelineCache(), 1, &info, nullptr, &pipeline) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[gpu] full-screen pipeline creation failed\n");
                return VK_NULL_HANDLE;
            }
            return pipeline;
        }

        bool Renderer::CreatePresentPipeline()
        {
            if (presentPipeline_ && presentPipelinePass_ == context_.presentPass())
                return true;
            if (!context_.presentPass())
                return false;
            if (presentPipeline_)
            {
                VkDevice device = this->device();
                VkPipeline old = presentPipeline_;
                context_.DestroyLater([device, old] { vkDestroyPipeline(device, old, nullptr); });
            }
            presentPipeline_ = CreateFullscreenPipeline(presentPs_, presentLayout_, context_.presentPass());
            presentPipelinePass_ = context_.presentPass();
            return presentPipeline_ != VK_NULL_HANDLE;
        }

        // The window may have changed (rotated, resized, gone to the background and back).
        void Renderer::CheckWindow()
        {
            ANativeWindow* window = nullptr;
            if (!android::PollWindow(windowGeneration_, window))
                return;
            if (!context_.Initialize(window))
                std::fprintf(stderr, "[gpu] cannot present to the new window\n");
            if (window)
                ANativeWindow_release(window);
        }

        const CompiledShader& Renderer::GetShader(const std::shared_ptr<const ShaderMicrocode>& microcode)
        {
            auto& compiled = shaders_[microcode->hash];
            if (compiled.loaded)
                return compiled;
            compiled.loaded = true;
            const bool pixel = microcode->type == ShaderType::Pixel;
            char name[64];
            std::snprintf(name, sizeof(name), "%016llX.%s", (unsigned long long)microcode->hash, pixel ? "ps" : "vs");
            // HC09_DUMP_ON_SHADER=<hash> (debugging): dump the rest of the frame where that shader first draws.
            if (const char* dump = std::getenv("HC09_DUMP_ON_SHADER"); dump && std::strstr(dump, std::string(name, 16).c_str()))
            {
                g_dumpFrame = g_frame;
                std::fprintf(stderr, "[frame] %s first used: dumping frame %d\n", name, g_frame);
            }
            // HC09_SKIP_SHADERS=<hash>[,<hash>...] (debugging): draws with these shaders are skipped.
            if (const char* skip = std::getenv("HC09_SKIP_SHADERS"); skip && std::strstr(skip, std::string(name, 16).c_str()))
            {
                std::fprintf(stderr, "[shader] %s skipped (HC09_SKIP_SHADERS)\n", name);
                return compiled;
            }
            compiled.info = TranslateShader(*microcode);
            if (!compiled.info.valid)
            {
                std::fprintf(stderr, "[gpu] shader %s not translated: %s\n", name, compiled.info.error.c_str());
                return compiled;
            }
            compiled.info.hlsl.clear();  // only the translator's description is needed here
            // The SPIR-V of the same translation, built offline (tools/shaders_to_spirv.py).
            char path[96];
            std::snprintf(path, sizeof(path), "shaders/%s.spv", name);
            AAsset* asset = android::Assets() ? AAssetManager_open(android::Assets(), path, AASSET_MODE_BUFFER) : nullptr;
            if (!asset)
            {
                std::fprintf(stderr, "[shader] missing %s\n", name);
                return compiled;
            }
            const size_t bytes = size_t(AAsset_getLength(asset));
            std::vector<uint32_t> words((bytes + 3) / 4);
            const bool read = AAsset_read(asset, words.data(), bytes) == int(bytes);
            AAsset_close(asset);
            if (!read || bytes < 20 || words[0] != 0x07230203)
            {
                std::fprintf(stderr, "[shader] %s is not SPIR-V\n", name);
                return compiled;
            }
            compiled.bindings = SpirvBindings(words.data(), words.size());
            compiled.module = CreateModule(words.data(), bytes);
            compiled.ok = compiled.module != VK_NULL_HANDLE;
            return compiled;
        }

        ShaderPair* Renderer::GetShaderPair(const CompiledShader& vs, const CompiledShader& ps)
        {
            ShaderPair& pair = shaderPairs_[Mix(reinterpret_cast<uintptr_t>(&vs), reinterpret_cast<uintptr_t>(&ps))];
            if (pair.layout)
                return &pair;
            std::set<uint32_t> bindings(vs.bindings.begin(), vs.bindings.end());
            bindings.insert(ps.bindings.begin(), ps.bindings.end());
            pair.bindings.assign(bindings.begin(), bindings.end());
            std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
            for (uint32_t binding : pair.bindings)
                layoutBindings.push_back({ HostBinding(binding), BindingType(binding), 1,
                    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
            if (!context_.features().geometryShader)
                for (auto& binding : layoutBindings)
                    binding.stageFlags &= ~VK_SHADER_STAGE_GEOMETRY_BIT;
            VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            setInfo.bindingCount = uint32_t(layoutBindings.size());
            setInfo.pBindings = layoutBindings.data();
            VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &pair.setLayout;
            if (vkCreateDescriptorSetLayout(device(), &setInfo, nullptr, &pair.setLayout) != VK_SUCCESS ||
                vkCreatePipelineLayout(device(), &layoutInfo, nullptr, &pair.layout) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[gpu] pipeline layout creation failed (%zu bindings)\n", layoutBindings.size());
                return nullptr;
            }
            return &pair;
        }

        VkRenderPass Renderer::GetRenderPass(VkFormat format, uint32_t samples)
        {
            VkRenderPass& pass = renderPasses_[{ format, samples }];
            if (!pass)
                pass = MakeRenderPass(device(), format, samples, depthFormat_);
            return pass;
        }

        VkFramebuffer Renderer::GetFramebuffer(RenderTarget& target, DepthTarget& depth)
        {
            VkFramebuffer& framebuffer = framebuffers_[{ target.image.view, depth.image.view }];
            if (framebuffer)
                return framebuffer;
            const VkImageView views[2] = { target.image.view, depth.image.view };
            VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            info.renderPass = GetRenderPass(target.image.format, target.samples);
            info.attachmentCount = 2;
            info.pAttachments = views;
            info.width = kTargetWidth * g_scale;
            info.height = kTargetHeight * g_scale;
            info.layers = 1;
            if (vkCreateFramebuffer(device(), &info, nullptr, &framebuffer) != VK_SUCCESS)
                std::fprintf(stderr, "[gpu] framebuffer creation failed\n");
            return framebuffer;
        }

        void Renderer::BeginPass(RenderTarget& target, DepthTarget& depth)
        {
            if (passTarget_ == &target && passDepth_ == &depth)
                return;
            EndPass();
            Transition(commands(), target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            Transition(commands(), depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            info.renderPass = GetRenderPass(target.image.format, target.samples);
            info.framebuffer = GetFramebuffer(target, depth);
            info.renderArea.extent = { kTargetWidth * g_scale, kTargetHeight * g_scale };
            vkCmdBeginRenderPass(commands(), &info, VK_SUBPASS_CONTENTS_INLINE);
            passTarget_ = &target;
            passDepth_ = &depth;
        }

        void Renderer::EndPass()
        {
            if (!passTarget_)
                return;
            vkCmdEndRenderPass(commands());
            passTarget_ = nullptr;
            passDepth_ = nullptr;
        }

        RenderTarget& Renderer::GetRenderTarget(uint32_t colorInfo, uint32_t samples)
        {
            const uint32_t key = (colorInfo & 0xF0FFF) | (samples << 24);  // EDRAM base + format + samples
            RenderTarget& target = renderTargets_[key];
            if (!target.image.image)
            {
                EndPass();
                target.samples = samples;
                VkFormat format = ColorFormat((colorInfo >> 16) & 0xF);
                if (!context_.SupportsFormat(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                    format = VK_FORMAT_R16G16B16A16_SFLOAT;
                CreateImage(target.image, format, kTargetWidth * g_scale, kTargetHeight * g_scale,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT);
                Transition(commands(), target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkClearColorValue black{};
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdClearColorImage(commands(), target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
                std::fprintf(stderr, "[gpu] render target: EDRAM base 0x%X format %u (Vulkan %d), %u samples\n", colorInfo & 0xFFF,
                    (colorInfo >> 16) & 0xF, int(format), samples);
            }
            return target;
        }

        DepthTarget& Renderer::GetDepthTarget(uint32_t depthInfo, uint32_t samples)
        {
            DepthTarget& target = depthTargets_[(depthInfo & 0xFFF) | (samples << 24)];
            if (!target.image.image)
            {
                EndPass();
                target.samples = samples;
                CreateImage(target.image, depthFormat_, kTargetWidth * g_scale, kTargetHeight * g_scale,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        (depthSampleable_ ? VK_IMAGE_USAGE_SAMPLED_BIT : 0),
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
                if (depthSampleable_)
                {
                    VkImageViewCreateInfo view{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    view.image = target.image.image;
                    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    view.format = depthFormat_;
                    view.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    vkCreateImageView(device(), &view, nullptr, &target.depthView);
                }
                Transition(commands(), target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkClearDepthStencilValue clear{ 1.0f, 0 };
                const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
                vkCmdClearDepthStencilImage(commands(), target.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            }
            return target;
        }

        VkSampler Renderer::GetSampler(const VkSamplerCreateInfo& info)
        {
            const uint64_t key = XXH3_64bits(&info.magFilter, offsetof(VkSamplerCreateInfo, unnormalizedCoordinates) -
                offsetof(VkSamplerCreateInfo, magFilter));
            VkSampler& sampler = samplers_[key];
            if (!sampler && vkCreateSampler(device(), &info, nullptr, &sampler) != VK_SUCCESS)
            {
                std::fprintf(stderr, "[gpu] vkCreateSampler failed\n");
                sampler = VK_NULL_HANDLE;
            }
            return sampler;
        }

        VkPipeline Renderer::GetPipeline(const CompiledShader& vs, const CompiledShader& ps, const ShaderPair& pair, VkFormat format,
            VkPrimitiveTopology topology, bool rectangles, bool colorWrites, uint32_t samples)
        {
            PipelineDesc desc;
            desc.vs = ActiveShader(ShaderType::Vertex)->hash;
            desc.ps = ActiveShader(ShaderType::Pixel)->hash;
            desc.depthControl = ReadRegister(reg::kDepthControl);
            desc.stencilMasks = ReadRegister(reg::kStencilRefMask) & 0xFFFF00;
            desc.cull = ReadRegister(reg::kScModeControl) & 7;
            // Polygon offset: PA_SU_SC_MODE_CNTL poly_offset_front_enable (bit 11) / back (bit 12), with
            // PA_SU_POLY_OFFSET_FRONT_SCALE/OFFSET (0x2380/1) and BACK (0x2382/3). The title draws layers
            // over the field with it; without it they depth-fight the turf. One bias serves both faces,
            // so the front's is taken when enabled. The scale is in 1/16 pixel units; the offset in
            // depth units, which Vulkan takes as a count of 2^-24 for the D24 buffer.
            desc.polyOffsetEnable = (ReadRegister(reg::kScModeControl) >> 11) & 3;
            if (desc.polyOffsetEnable)
            {
                const uint32_t base = (desc.polyOffsetEnable & 1) ? reg::kPolyOffsetFront : reg::kPolyOffsetFront + 2;
                desc.polyScale = ReadRegister(base);
                desc.polyOffset = ReadRegister(base + 1);
            }
            desc.blend = ReadRegister(reg::kBlendControl0);
            desc.writeMask = colorWrites ? ReadRegister(reg::kColorMask) & 0xF : 0;
            // PA_CL_CLIP_CNTL bit 16: clip_disable (no clipping, so no depth clipping either).
            desc.clipDisabled = (ReadRegister(0x2204) >> 16) & 1;
            desc.format = uint32_t(format);
            desc.topology = uint32_t(topology);
            desc.rectangles = rectangles;
            desc.samples = samples;
            const uint64_t key = desc.Key();
            // A shader pair the driver could not build a pipeline for (the V30's gives up on some large
            // shaders with VK_INCOMPLETE, after a long try) is not tried again: its draws are skipped.
            const uint64_t pairKey = Mix(desc.vs, desc.ps);
            if (failedPairs_.count(pairKey))
                return VK_NULL_HANDLE;
            if (auto found = pipelines_.find(key); found != pipelines_.end())
                return found->second;
            // Still building in the background: these draws are skipped until it is done.
            if (auto pending = pendingPipelines_.find(key); pending != pendingPipelines_.end())
            {
                const auto build = pending->second;
                {
                    std::lock_guard lock(build->mutex);
                    if (!build->done)
                        return VK_NULL_HANDLE;
                }
                pendingPipelines_.erase(pending);
                return FinishPipeline(key, pairKey, *build);
            }

            // Built on its own thread: some of the game's largest shaders take the Pixel 8's compiler
            // 10-60 s (51ADA21A38FAF13D.ps, C0DEA8AD11DC6C29.ps). The draw waits a little (most builds
            // take well under that); past it, the pipeline's draws are skipped until the build is done.
            // Listed for the next start's pre-compile as soon as it is asked for (the slowest may never finish now).
            RecordPipeline(desc);
            auto build = std::make_shared<PipelineBuild>();
            FillPipeline(desc, vs.module, ps.module, rectangleGs_, pair.layout, GetRenderPass(format, samples), context_.features().depthClamp,
                context_.SupportsFormat(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT), *build);
            build->start = std::chrono::steady_clock::now();
            std::thread([build, device = device(), cache = context_.pipelineCache()] {
                VkPipeline built = VK_NULL_HANDLE;
                g_buildSlots.Acquire();
                const VkResult result = vkCreateGraphicsPipelines(device, cache, 1, &build->info, nullptr, &built);
                g_buildSlots.Release();
                std::lock_guard lock(build->mutex);
                build->result = result;
                build->pipeline = built;
                build->done = true;
                build->finished.notify_all();
            }).detach();
            {
                std::unique_lock lock(build->mutex);
                if (!build->finished.wait_for(lock, std::chrono::milliseconds(100), [&] { return build->done; }))
                {
                    std::fprintf(stderr, "[gpu] frame %d: pipeline still building after 100 ms, continuing without it (vs %016llX ps %016llX)\n",
                        g_frame, (unsigned long long)desc.vs, (unsigned long long)desc.ps);
                    pendingPipelines_[key] = build;
                    return VK_NULL_HANDLE;
                }
            }
            return FinishPipeline(key, pairKey, *build);
        }

        VkPipeline Renderer::FinishPipeline(uint64_t key, uint64_t pairKey, PipelineBuild& build)
        {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build.start).count();
            if (build.result != VK_SUCCESS)
            {
                std::fprintf(stderr, "[gpu] pipeline creation failed (%d): vs %016llX ps %016llX\n", int(build.result),
                    (unsigned long long)build.desc.vs, (unsigned long long)build.desc.ps);
                failedPairs_.insert(pairKey);
                return VK_NULL_HANDLE;
            }
            pipelines_[key] = build.pipeline;
            pipelinesAdded_ = true;
            std::fprintf(stderr, "[gpu] frame %d: pipeline %zu built in %.0f ms (vs %016llX ps %016llX%s)\n", g_frame, pipelines_.size(), ms,
                (unsigned long long)build.desc.vs, (unsigned long long)build.desc.ps, build.desc.rectangles ? ", rectangles" : "");
            return build.pipeline;
        }

        void Renderer::RecordPipeline(const PipelineDesc& desc)
        {
            if (!recordedPipelines_.insert(desc.Key()).second)
                return;
            if (const char* path = std::getenv("HC09_PIPELINE_LIST"))
                if (FILE* file = std::fopen(path, "a"))
                {
                    std::fputs(desc.ToLine().c_str(), file);
                    std::fclose(file);
                }
        }

        void Renderer::StartPrecompile()
        {
            // The APK's list first (pipelines seen while testing), then this phone's own.
            std::vector<PipelineDesc> list;
            auto add = [&](const char* text) {
                for (const char* line = text; line && *line;)
                {
                    PipelineDesc desc;
                    if (desc.FromLine(line) && recordedPipelines_.insert(desc.Key()).second)
                        list.push_back(desc);
                    line = std::strchr(line, '\n');
                    if (line)
                        line++;
                }
            };
            std::vector<uint8_t> bundled;
            if (ReadAsset("pipelines.txt", bundled))
            {
                bundled.push_back(0);
                add(reinterpret_cast<const char*>(bundled.data()));
            }
            const size_t fromApk = list.size();
            if (const char* path = std::getenv("HC09_PIPELINE_LIST"))
                if (FILE* file = std::fopen(path, "rb"))
                {
                    std::string text;
                    char buffer[4096];
                    size_t count;
                    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
                        text.append(buffer, count);
                    std::fclose(file);
                    add(text.c_str());
                }
            if (list.empty() || std::getenv("HC09_NO_PRECOMPILE"))
                return;
            std::fprintf(stderr, "[gpu] pre-compiling %zu pipelines (%zu from the APK)\n", list.size(), fromApk);

            // The game waits here (the app shows "Preparing shaders" with the progress): worker threads
            // build each listed pipeline once and throw it away. What matters is the driver's pipeline
            // cache, which the game's own builds then hit; it is saved, so later starts only check it.
            constexpr int kWorkers = 4;
            struct Shared
            {
                std::vector<PipelineDesc> list;
                std::atomic<size_t> next{ 0 }, built{ 0 }, failed{ 0 };
                std::atomic<int> running{ kWorkers };
                std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            };
            auto shared = std::make_shared<Shared>();
            shared->list = std::move(list);
            g_precompileTotal = int(shared->list.size());
            g_precompileDone = 0;
            std::vector<std::thread> workers;
            const VkDevice device = this->device();
            const VkPipelineCache cache = context_.pipelineCache();
            const VkShaderModule gs = rectangleGs_;
            const VkFormat depthFormat = depthFormat_;
            const bool depthClamp = context_.features().depthClamp;
            VulkanContext* context = &context_;
            for (int worker = 0; worker < kWorkers; worker++)
            {
                workers.emplace_back([=] {
                    // Each worker loads its own shader modules (by hash) and makes its own layouts and passes.
                    std::unordered_map<uint64_t, std::pair<VkShaderModule, std::vector<uint32_t>>> modules;
                    auto module = [&](uint64_t hash, bool pixel) -> std::pair<VkShaderModule, std::vector<uint32_t>>* {
                        auto& entry = modules[(hash << 1) | (pixel ? 1 : 0)];
                        if (entry.first)
                            return &entry;
                        char path[96];
                        std::snprintf(path, sizeof(path), "shaders/%016llX.%s.spv", (unsigned long long)hash, pixel ? "ps" : "vs");
                        std::vector<uint8_t> bytes;
                        if (!ReadAsset(path, bytes) || bytes.size() < 20)
                            return nullptr;
                        std::vector<uint32_t> words(bytes.size() / 4);
                        std::memcpy(words.data(), bytes.data(), words.size() * 4);
                        entry.second = SpirvBindings(words.data(), words.size());
                        VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
                        info.codeSize = words.size() * 4;
                        info.pCode = words.data();
                        if (vkCreateShaderModule(device, &info, nullptr, &entry.first) != VK_SUCCESS)
                            return nullptr;
                        return &entry;
                    };
                    for (size_t i; (i = shared->next++) < shared->list.size();)
                    {
                        const PipelineDesc& desc = shared->list[i];
                        auto* vs = module(desc.vs, false);
                        auto* ps = module(desc.ps, true);
                        if (!vs || !ps || (desc.rectangles && !gs))
                        {
                            g_precompileDone++;
                            continue;
                        }
                        // The same set layout as GetShaderPair makes (pipeline layouts only need to match).
                        std::set<uint32_t> bindings(vs->second.begin(), vs->second.end());
                        bindings.insert(ps->second.begin(), ps->second.end());
                        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
                        for (uint32_t binding : bindings)
                            layoutBindings.push_back({ HostBinding(binding), BindingType(binding), 1,
                                VkShaderStageFlags(VK_SHADER_STAGE_VERTEX_BIT | (gs ? VK_SHADER_STAGE_GEOMETRY_BIT : 0) | VK_SHADER_STAGE_FRAGMENT_BIT), nullptr });
                        VkDescriptorSetLayoutCreateInfo setInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
                        setInfo.bindingCount = uint32_t(layoutBindings.size());
                        setInfo.pBindings = layoutBindings.data();
                        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
                        VkPipelineLayout layout = VK_NULL_HANDLE;
                        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
                        layoutInfo.setLayoutCount = 1;
                        layoutInfo.pSetLayouts = &setLayout;
                        VkRenderPass pass = MakeRenderPass(device, VkFormat(desc.format), desc.samples, depthFormat);
                        if (vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) == VK_SUCCESS &&
                            vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout) == VK_SUCCESS && pass)
                        {
                            PipelineBuild build;
                            FillPipeline(desc, vs->first, ps->first, gs, layout, pass, depthClamp,
                                context->SupportsFormat(VkFormat(desc.format), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT), build);
                            VkPipeline pipeline = VK_NULL_HANDLE;
                            if (vkCreateGraphicsPipelines(device, cache, 1, &build.info, nullptr, &pipeline) == VK_SUCCESS)
                            {
                                // Saved as it goes: quitting during "Preparing shaders" keeps what was done.
                                if (++shared->built % 8 == 0)
                                    context->SavePipelineCache();
                                vkDestroyPipeline(device, pipeline, nullptr);
                            }
                            else
                            {
                                shared->failed++;
                            }
                        }
                        if (pass)
                            vkDestroyRenderPass(device, pass, nullptr);
                        vkDestroyPipelineLayout(device, layout, nullptr);
                        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
                        g_precompileDone++;
                    }
                    for (auto& [key, entry] : modules)
                        vkDestroyShaderModule(device, entry.first, nullptr);
                    if (--shared->running == 0)
                    {
                        std::fprintf(stderr, "[gpu] pre-compile done: %zu built, %zu failed, %.1f s\n", size_t(shared->built),
                            size_t(shared->failed),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - shared->start).count());
                        context->SavePipelineCache();
                    }
                });
            }
            for (std::thread& worker : workers)
                worker.join();
            g_precompileTotal = 0;
        }

        bool Renderer::PrepareIndices(const DrawInfo& draw, std::vector<uint32_t>& indices, bool& indexed, VkPrimitiveTopology& topology,
            bool& rectangles)
        {
            rectangles = false;
            bool convert = false;
            switch (draw.primitive)
            {
            case PrimitiveType::LineList: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case PrimitiveType::LineStrip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
            case PrimitiveType::LineLoop: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; convert = true; break;
            case PrimitiveType::TriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
            case PrimitiveType::TriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case PrimitiveType::TriangleFan:
            case PrimitiveType::Polygon:
            case PrimitiveType::QuadList:
                topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                convert = true;
                break;
            case PrimitiveType::RectangleList:
                topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                rectangles = true;
                break;
            default:
                // Point lists need the vertex shader to write a point size: not yet.
                return false;
            }

            indexed = draw.source != IndexSource::Auto || convert;
            if (!indexed)
                return true;

            // Source indices (as 32-bit; 16-bit restart values widen to 0xFFFFFFFF).
            std::vector<uint32_t> source(draw.indexCount);
            for (uint32_t i = 0; i < draw.indexCount; i++)
            {
                uint32_t value = i;
                if (draw.source == IndexSource::Dma)
                {
                    if (draw.index32)
                    {
                        uint32_t raw;
                        std::memcpy(&raw, GuestToHost(draw.indexAddress + i * 4), 4);
                        value = GpuSwap(raw, draw.indexEndian);
                    }
                    else
                    {
                        uint32_t raw;
                        std::memcpy(&raw, GuestToHost(draw.indexAddress + (i & ~1u) * 2), 4);
                        raw = GpuSwap(raw, draw.indexEndian);
                        value = (i & 1) ? raw >> 16 : raw & 0xFFFF;
                        if (value == 0xFFFF)
                            value = 0xFFFFFFFF;
                    }
                }
                else if (draw.source == IndexSource::Immediate)
                {
                    const uint32_t word = i / 2 < draw.immediateDwords ? draw.immediateIndices[i / 2] : 0;
                    value = (i & 1) ? word >> 16 : word & 0xFFFF;
                }
                source[i] = value;
            }

            switch (draw.primitive)
            {
            case PrimitiveType::TriangleFan:
            case PrimitiveType::Polygon:
                for (uint32_t i = 1; i + 1 < source.size(); i++)
                    indices.insert(indices.end(), { source[0], source[i], source[i + 1] });
                break;
            case PrimitiveType::QuadList:
                for (uint32_t i = 0; i + 3 < source.size(); i += 4)
                    indices.insert(indices.end(), { source[i], source[i + 1], source[i + 2], source[i], source[i + 2], source[i + 3] });
                break;
            case PrimitiveType::LineLoop:
                indices = source;
                if (!source.empty())
                    indices.push_back(source[0]);
                break;
            default:
                indices = std::move(source);
                break;
            }
            return true;
        }

        // Xenos rectangle lists give three corners; the fourth is opposite the right-angle corner (the
        // vertex not on the longest edge). Without a geometry shader the fourth vertex is made from the
        // guest's vertex data instead: a + b - c per float, which matches the shader's output when the
        // shader is affine in its inputs (2D quads and clears are). Words equal in all three corners
        // (packed colours, say) are copied as they are. The corners are told apart by the first two
        // floats of stream 0 (the position, for these draws).
        uint32_t Renderer::ExpandRectangles(const CompiledShader& vs, const std::vector<uint32_t>& indices, uint32_t count,
            VkDescriptorBufferInfo* streams)
        {
            const uint32_t indexOffset = ReadRegister(reg::kIndexOffset);
            const uint32_t sourceCount = indices.empty() ? count : uint32_t(indices.size());
            auto sourceIndex = [&](uint32_t i) { return (indices.empty() ? i : indices[i]) + indexOffset; };
            struct Stream
            {
                const uint32_t* data;
                uint32_t dwords, stride, swap;
            };
            std::vector<Stream> sources;
            for (uint32_t stream = 0; stream < vs.info.vertexFetchConstants.size() && stream < kMaxVertexStreams; stream++)
            {
                const uint32_t index = vs.info.vertexFetchConstants[stream];
                const uint32_t d0 = ReadRegister(reg::kFetchConstants + index * 2);
                const uint32_t d1 = ReadRegister(reg::kFetchConstants + index * 2 + 1);
                const uint32_t dwords = std::min<uint32_t>((d1 >> 2) & 0xFFFFFF, (kPhysicalSize - (d0 & 0x1FFFFFFC)) / 4);
                sources.push_back({ reinterpret_cast<const uint32_t*>(GuestToHost(PhysicalToGuest(d0 & ~3u))), dwords,
                    std::max(vs.info.vertexStreamStrides[stream], 1u), d1 & 3 });
            }
            auto word = [](const Stream& s, uint32_t vertex, uint32_t i) {
                const uint64_t at = uint64_t(vertex) * s.stride + i;
                return at < s.dwords ? GpuSwap(s.data[at], s.swap) : 0u;
            };
            auto asFloat = [](uint32_t bits) {
                float value;
                std::memcpy(&value, &bits, 4);
                return value;
            };
            // Output vertices: corners c, a, b and the made-up d, as triangles (c, a, b) and (b, a, d).
            struct Corner
            {
                uint32_t a, b, c;
                bool made;
            };
            std::vector<Corner> corners;
            for (uint32_t i = 0; i + 2 < sourceCount; i += 3)
            {
                uint32_t v[3] = { sourceIndex(i), sourceIndex(i + 1), sourceIndex(i + 2) };
                float p[3][2]{};
                if (!sources.empty())
                    for (int k = 0; k < 3; k++)
                        for (int axis = 0; axis < 2; axis++)
                            p[k][axis] = asFloat(word(sources[0], v[k], axis));
                auto distance = [&](int x, int y) {
                    const float dx = p[x][0] - p[y][0], dy = p[x][1] - p[y][1];
                    return dx * dx + dy * dy;
                };
                const float d01 = distance(0, 1), d12 = distance(1, 2), d20 = distance(2, 0);
                uint32_t c = 2, a = 0, b = 1;
                if (d12 >= d01 && d12 >= d20)
                    c = 0, a = 1, b = 2;
                else if (d20 >= d01)
                    c = 1, a = 2, b = 0;
                const Corner made{ v[a], v[b], v[c], true };
                for (const Corner& corner : { Corner{ v[c], 0, 0, false }, Corner{ v[a], 0, 0, false }, Corner{ v[b], 0, 0, false },
                         Corner{ v[b], 0, 0, false }, Corner{ v[a], 0, 0, false }, made })
                    corners.push_back(corner);
            }
            const uint32_t vertexCount = uint32_t(corners.size());
            if (g_frame == g_dumpFrame)
            {
                std::fprintf(stderr, "[frame]   rectangles: %u source vertices, %zu streams, index offset %u\n", sourceCount, sources.size(),
                    indexOffset);
                for (size_t stream = 0; stream < sources.size(); stream++)
                    for (uint32_t i = 0; i < sourceCount && i < 3; i++)
                    {
                        std::fprintf(stderr, "[frame]     stream %zu (stride %u) v%u:", stream, sources[stream].stride, sourceIndex(i));
                        for (uint32_t w = 0; w < sources[stream].stride && w < 8; w++)
                            std::fprintf(stderr, " %08X(%g)", word(sources[stream], sourceIndex(i), w), asFloat(word(sources[stream], sourceIndex(i), w)));
                        std::fprintf(stderr, "\n");
                    }
            }
            if (vertexCount == 0)
                return 0;
            const VkDeviceSize alignment = context_.properties().limits.minStorageBufferOffsetAlignment;
            for (size_t stream = 0; stream < sources.size(); stream++)
            {
                const Stream& s = sources[stream];
                const VulkanContext::Allocation upload = context_.Allocate(uint64_t(vertexCount) * s.stride * 4, alignment);
                if (!upload.cpu)
                    return 0;
                auto* dest = reinterpret_cast<uint32_t*>(upload.cpu);
                for (uint32_t k = 0; k < vertexCount; k++)
                {
                    const Corner& corner = corners[k];
                    for (uint32_t i = 0; i < s.stride; i++)
                    {
                        const uint32_t wa = word(s, corner.a, i);
                        uint32_t value = wa;
                        if (corner.made)
                        {
                            const uint32_t wb = word(s, corner.b, i), wc = word(s, corner.c, i);
                            if (!(wa == wb && wb == wc))
                            {
                                const float sum = asFloat(wa) + asFloat(wb) - asFloat(wc);
                                std::memcpy(&value, &sum, 4);
                            }
                        }
                        dest[uint64_t(k) * s.stride + i] = value;
                    }
                }
                streams[stream] = { upload.buffer, upload.offset, uint64_t(vertexCount) * s.stride * 4 };
            }
            return vertexCount;
        }

        void Renderer::Draw(const DrawInfo& draw)
        {
            const uint32_t mode = ReadRegister(reg::kModeControl) & 7;
            if (mode == kModeCopy)
            {
                Resolve();
                return;
            }
            // HC09_SKIP_DRAWS=<n>[,<n>...] (debugging): skip those draws (numbered from 0 in each frame).
            if (static const char* skip = std::getenv("HC09_SKIP_DRAWS"); skip)
            {
                const std::string list = std::string(",") + skip + ",";
                if (list.find("," + std::to_string(g_drawInFrame - 1) + ",") != std::string::npos)
                    return;
            }
            // Mode 5 renders depth/stencil only. Colour-masked draws still matter when they
            // write depth or stencil.
            const uint32_t depthControl = ReadRegister(reg::kDepthControl);
            const bool writesDepthStencil = ((depthControl >> 1) & 1 && (depthControl >> 2) & 1) || (depthControl & 1);
            const bool colorWrites = mode == kModeColorDepth && (ReadRegister(reg::kColorMask) & 0xF) != 0;
            if ((mode != kModeColorDepth && mode != kModeDepth) || draw.indexCount == 0 || (!colorWrites && !writesDepthStencil))
                return;

            const auto vsCode = ActiveShader(ShaderType::Vertex);
            const auto psCode = ActiveShader(ShaderType::Pixel);
            if (!vsCode || !psCode)
                return;
            const CompiledShader& vs = GetShader(vsCode);
            const CompiledShader& ps = GetShader(psCode);
            if (!vs.ok || !ps.ok)
            {
                skippedDraws_++;
                return;
            }

            std::vector<uint32_t> indices;
            bool indexed = false, rectangles = false;
            VkPrimitiveTopology topology;
            if (!PrepareIndices(draw, indices, indexed, topology, rectangles))
            {
                skippedDraws_++;
                return;
            }
            // Without geometry shaders (the V30), rectangles are expanded to two triangles on the CPU.
            const bool expandRectangles = rectangles && !rectangleGs_;

            if (TilingActive())
                tiledSurfaces_.insert(ReadRegister(reg::kSurfaceInfo));
            const uint32_t samples = SampleCount(ReadRegister(reg::kSurfaceInfo));
            RenderTarget& target = GetRenderTarget(ReadRegister(reg::kColorInfo), samples);
            DepthTarget& depth = GetDepthTarget(ReadRegister(reg::kDepthInfo), samples);
            ShaderPair* pair = GetShaderPair(vs, ps);
            if (!pair)
                return;
            VkPipeline pipeline = GetPipeline(vs, ps, *pair, target.image.format, topology, rectangles && !expandRectangles, colorWrites, samples);
            if (!pipeline)
            {
                skippedDraws_++;
                return;
            }
            const VkPhysicalDeviceLimits& limits = context_.properties().limits;

            // Float constants: 2048 big-endian registers, re-uploaded only when they changed.
            const uint32_t* constants = reinterpret_cast<const uint32_t*>(GuestToHost(kRegisterBase + reg::kFloatConstants * 4));
            VulkanContext::Allocation floats;
            if (lastConstantsFrame_ == context_.frameCount() && std::memcmp(constants, lastConstants_, sizeof(lastConstants_)) == 0)
            {
                floats = lastConstantsUpload_;
            }
            else
            {
                floats = context_.Allocate(512 * 16, limits.minUniformBufferOffsetAlignment);
                if (!floats.cpu)
                    return;
                std::memcpy(lastConstants_, constants, sizeof(lastConstants_));
                auto* dest = reinterpret_cast<uint32_t*>(floats.cpu);
                for (uint32_t i = 0; i < 2048; i++)
                    dest[i] = __builtin_bswap32(lastConstants_[i]);
                lastConstantsUpload_ = floats;
                lastConstantsFrame_ = context_.frameCount();
            }

            DrawState state{};
            state.vsConstBase = ReadRegister(reg::kVsConst) & 0x1FF;
            state.psConstBase = ReadRegister(reg::kPsConst) & 0x1FF;
            const uint32_t vte = ReadRegister(reg::kVteControl);
            state.wIsReciprocal = ((vte >> 10) & 1) == 0;  // vtx_w0_fmt set: W is plain, the hardware takes 1/W
            state.xyPreDivided = (vte >> 8) & 1;
            float scale[3], offset[3];
            for (int axis = 0; axis < 3; axis++)
            {
                scale[axis] = (vte & (1u << (axis * 2))) ? RegisterFloat(reg::kViewportXScale + axis * 2) : 1.0f;
                offset[axis] = (vte & (2u << (axis * 2))) ? RegisterFloat(reg::kViewportXScale + axis * 2 + 1) : 0.0f;
            }
            // The window offset (used for predicated tiling) is deliberately ignored: targets are
            // kept in screen space and resolves copy screen-space rectangles.
            //
            // Surfaces used by predicated tiling hold one tile in EDRAM, so the title clears them
            // with tile-sized rectangles outside the tiled passes. Here they cover the whole
            // screen: stretch such rectangles to the full target width.
            {
                const uint32_t surfaceInfo = ReadRegister(reg::kSurfaceInfo);
                const uint32_t pitch = surfaceInfo & 0x3FFF;
                const uint32_t msaa = (surfaceInfo >> 16) & 3;  // 0 1x, 1 2x (1x2 samples), 2 4x (2x2 samples)
                // Widest single-sampled surface drawn at each EDRAM colour base: a multisampled
                // surface at the same base only aliases it if that one is (at least) twice as wide.
                static std::unordered_map<uint32_t, uint32_t> singleSampledPitch;
                const uint32_t colorBase = ReadRegister(reg::kColorInfo) & 0xFFF;
                if (msaa == 0)
                    singleSampledPitch[colorBase] = std::max(singleSampledPitch[colorBase], pitch);
                const auto aliased = singleSampledPitch.find(colorBase);
                const bool aliasesSingleSampled = aliased != singleSampledPitch.end() &&
                    aliased->second >= (msaa == 2 ? pitch * 2 : pitch);
                if (TilingActive())
                {
                }
                else if (rectangles && pitch > 0 && pitch < kTargetWidth && tiledSurfaces_.contains(surfaceInfo))
                {
                    const float stretch = float(kTargetWidth) / float(pitch);
                    scale[0] *= stretch;
                    offset[0] *= stretch;
                }
                else if (rectangles && msaa != 0 && pitch > 0 && pitch < kTargetWidth && aliasesSingleSampled)
                {
                    // Fast clears through EDRAM aliasing: the title clears a single-sampled surface
                    // by drawing a rectangle into a multisampled one at the same address, where each
                    // pixel covers 2x2 (4x) or 1x2 (2x) single-sampled pixels. Targets here are
                    // single-sampled, so cover the same area.
                    if (msaa == 2)
                    {
                        scale[0] *= 2.0f;
                        offset[0] *= 2.0f;
                    }
                    scale[1] *= 2.0f;
                    offset[1] *= 2.0f;
                }
            }
            // Screen to clip space; Vulkan's y points down like the guest's screen.
            state.ndcScale[0] = scale[0] * 2.0f / kTargetWidth;
            state.ndcOffset[0] = offset[0] * 2.0f / kTargetWidth - 1.0f;
            state.ndcScale[1] = scale[1] * 2.0f / kTargetHeight;
            state.ndcOffset[1] = offset[1] * 2.0f / kTargetHeight - 1.0f;
            state.ndcScale[2] = scale[2];
            state.ndcOffset[2] = offset[2];
            state.ndcScale[3] = float((vte >> 9) & 1);
            // RB_COLORCONTROL: alpha_func (bits 0-2), alpha_test_enable (bit 3); RB_ALPHA_REF.
            const uint32_t colorControl = ReadRegister(0x2202);
            state.alphaFunc = (colorControl >> 3) & 1 ? colorControl & 7 : 7;
            state.alphaRef = RegisterFloat(0x210E);
            state.vertexIndexOffset = ReadRegister(reg::kIndexOffset);
            for (uint32_t i = 0; i < 8; i++)
                state.boolConstants[i] = ReadRegister(reg::kBoolConstants + i);
            for (uint32_t i = 0; i < 32; i++)
                state.loopConstants[i] = ReadRegister(reg::kLoopConstants + i);

            // Vertex streams: the vertices this draw can reach, byte-swapped into this frame's memory.
            VkDescriptorBufferInfo streams[kMaxVertexStreams];
            for (auto& stream : streams)
                stream = { dummyBuffer_, 0, VK_WHOLE_SIZE };
            uint32_t maxVertexIndex = draw.indexCount ? draw.indexCount - 1 : 0;
            if (indexed)
            {
                maxVertexIndex = 0;
                for (uint32_t index : indices)
                    if (index != 0xFFFFFFFF)
                        maxVertexIndex = std::max(maxVertexIndex, index);
            }
            const uint32_t baseVertexForUpload = ReadRegister(reg::kIndexOffset);
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame]   indices: %s, max vertex index %u, index offset %u\n", indexed ? "indexed" : "auto",
                    maxVertexIndex, baseVertexForUpload);
            if (vertexUploadFrame_ != context_.frameCount())
            {
                vertexUploads_.clear();
                vertexUploadFrame_ = context_.frameCount();
            }
            uint32_t expandedVertices = 0;
            if (expandRectangles)
            {
                expandedVertices = ExpandRectangles(vs, indexed ? indices : std::vector<uint32_t>(), draw.indexCount, streams);
                state.vertexIndexOffset = 0;
                indexed = false;
            }
            for (uint32_t stream = 0; stream < vs.info.vertexFetchConstants.size() && stream < kMaxVertexStreams && !expandRectangles; stream++)
            {
                const uint32_t index = vs.info.vertexFetchConstants[stream];
                const uint32_t d0 = ReadRegister(reg::kFetchConstants + index * 2);
                const uint32_t d1 = ReadRegister(reg::kFetchConstants + index * 2 + 1);
                const uint32_t address = PhysicalToGuest(d0 & ~3u);
                // Only the vertices this draw can reach: up to the largest index (or the last
                // vertex of a non-indexed draw), not the whole buffer the fetch constant spans.
                const uint64_t lastVertex = uint64_t(baseVertexForUpload) + maxVertexIndex;
                const uint64_t reach = lastVertex * vs.info.vertexStreamStrides[stream] + vs.info.vertexStreamTails[stream];
                const uint32_t bufferDwords = std::min<uint32_t>((d1 >> 2) & 0xFFFFFF, (kPhysicalSize - (d0 & 0x1FFFFFFC)) / 4);
                const uint32_t dwords = uint32_t(std::min<uint64_t>({ bufferDwords, uint64_t(4u << 20), reach }));
                if (dwords == 0)
                    continue;
                const uint64_t uploadKey = (uint64_t(address) << 2) | (d1 & 3);
                auto cached = vertexUploads_.find(uploadKey);
                VulkanContext::Allocation upload;
                if (cached != vertexUploads_.end() && cached->second.dwords >= dwords)
                {
                    upload = cached->second.allocation;
                }
                else
                {
                    upload = context_.Allocate(uint64_t(dwords) * 4, limits.minStorageBufferOffsetAlignment);
                    if (!upload.cpu)
                        continue;
                    auto* dest = reinterpret_cast<uint32_t*>(upload.cpu);
                    const uint32_t* src = reinterpret_cast<const uint32_t*>(GuestToHost(address));
                    const uint32_t swap = d1 & 3;
                    if (swap == 2)
                    {
                        for (uint32_t i = 0; i < dwords; i++)
                            dest[i] = __builtin_bswap32(src[i]);
                    }
                    else
                    {
                        for (uint32_t i = 0; i < dwords; i++)
                            dest[i] = GpuSwap(src[i], swap);
                    }
                    vertexUploads_[uploadKey] = { upload, dwords };
                }
                streams[stream] = { upload.buffer, upload.offset, uint64_t(dwords) * 4 };
            }

            // Textures and samplers for every fetch constant either stage samples.
            const uint32_t textureMask = vs.info.textureFetchMask | ps.info.textureFetchMask;
            const uint32_t cubeMask = vs.info.textureCubeMask | ps.info.textureCubeMask;
            const uint32_t volumeMask = vs.info.textureVolumeMask | ps.info.textureVolumeMask;
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame]   blend 0x%08X color control 0x%08X color mask 0x%08X depth control 0x%08X stencil 0x%08X depth info 0x%08X\n",
                    ReadRegister(reg::kBlendControl0), ReadRegister(0x2202), ReadRegister(reg::kColorMask), ReadRegister(reg::kDepthControl),
                    ReadRegister(reg::kStencilRefMask), ReadRegister(reg::kDepthInfo));
            VkImageView textureViews[32]{}, fallbackViews[32]{};
            VkSampler samplers[32]{};
            for (uint32_t n = 0; n < 32; n++)
            {
                if (!(textureMask & (1u << n)))
                    continue;
                const TextureFetch fetch = ReadTextureFetch(n);
                BoundTexture texture;
                const VkImageViewType wanted = (cubeMask >> n) & 1 ? VK_IMAGE_VIEW_TYPE_CUBE
                    : (volumeMask >> n) & 1 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
                const bool fetched = textures_->Get(fetch, texture);
                const bool bound = fetched && texture.type == wanted;
                // The shader samples a cube or 3D texture but the constant holds a 2D one: the
                // hardware samples by the constant, so bind the 2D view in the fallback slot.
                if (fetched && !bound && wanted != VK_IMAGE_VIEW_TYPE_2D && texture.type == VK_IMAGE_VIEW_TYPE_2D)
                {
                    fallbackViews[n] = texture.view;
                    state.textureSizes[n][0] = float(texture.width);
                    state.textureSizes[n][1] = float(texture.height);
                    state.textureSizes[n][2] = 1.0f;
                    state.textureSizes[n][3] = 2.0f;  // sample the 2D view
                }
                if (g_frame == g_dumpFrame)
                    std::fprintf(stderr, "[frame]   tex %u: base 0x%08X fmt %u %ux%u tiled %d swizzle 0x%03X endian %u dim %u -> %s"
                        " | mips 0x%08X levels %u-%u packed %d filter mag %u min %u mip %u aniso %u bias %d clamp %u/%u\n", n,
                        fetch.baseAddress(), fetch.format(), fetch.width(), fetch.height(), int(fetch.tiled()), fetch.swizzle(),
                        fetch.endian(), fetch.dimension(), bound ? "bound" : "UNBOUND", fetch.mipAddress(),
                        fetch.mipMinLevel(), fetch.mipMaxLevel(), int(fetch.packedMips()), fetch.magFilter(), fetch.minFilter(),
                        fetch.mipFilter(), fetch.anisoFilter(), fetch.lodBias(), fetch.clampX(), fetch.clampY());
                if (bound)
                {
                    textureViews[n] = texture.view;
                    state.textureSizes[n][0] = float(texture.width);
                    state.textureSizes[n][1] = float(texture.height);
                    state.textureSizes[n][2] = float(texture.depth);
                    state.textureSizes[n][3] = texture.stacked ? 1.0f : 0.0f;
                }
                const bool mirrorOnce = context_.mirrorClampToEdge();
                auto address = [mirrorOnce](uint32_t clamp) {
                    switch (clamp)
                    {
                    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                    case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                    case 3: case 5: return mirrorOnce ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    }
                };
                VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
                // Xenos filters: 0 point, 1 linear; mip filter 2 = base map only. Anisotropy n
                // allows up to 2^(n-1):1. A fetch instruction can choose its own (3, or 7 for
                // anisotropy, keeps the constant's).
                const uint16_t chosen = ps.info.textureFilters[n] != TranslatedShader::kFiltersFromConstant ? ps.info.textureFilters[n]
                                                                                                           : vs.info.textureFilters[n];
                auto pick = [](uint32_t instruction, uint32_t useConstant, uint32_t constant) {
                    return instruction == useConstant ? constant : instruction;
                };
                const uint32_t magFilter = pick(chosen & 3, 3, fetch.magFilter());
                const uint32_t minFilter = pick((chosen >> 2) & 3, 3, fetch.minFilter());
                const bool linear = minFilter == 1 || magFilter == 1;
                const uint32_t mipFilter = pick((chosen >> 4) & 3, 3, fetch.mipFilter());
                const uint32_t aniso = pick((chosen >> 6) & 7, 7, fetch.anisoFilter());
                // Formats the device cannot filter (32-bit floats on the V30: depth resolves) are sampled
                // with point filtering; linear filtering them is undefined, and hung the V30's GPU.
                static std::unordered_map<VkFormat, bool> filterable;
                auto canFilter = filterable.find(texture.format);
                if (canFilter == filterable.end())
                    canFilter = filterable.emplace(texture.format, texture.format == VK_FORMAT_UNDEFINED ||
                        context_.SupportsFormat(texture.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)).first;
                if (!canFilter->second)
                {
                    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                }
                else if (aniso >= 2 && aniso <= 5 && context_.features().samplerAnisotropy)
                {
                    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    sampler.anisotropyEnable = VK_TRUE;
                    sampler.maxAnisotropy = std::min(float(1u << (aniso - 1)), limits.maxSamplerAnisotropy);
                }
                else if (linear || aniso >= 2)
                {
                    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
                    sampler.mipmapMode = mipFilter == 0 ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
                }
                else
                {
                    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
                    sampler.mipmapMode = mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                }
                sampler.addressModeU = address(fetch.clampX());
                sampler.addressModeV = address(fetch.clampY());
                sampler.addressModeW = address(fetch.clampZ());
                // Border colour for the clamp-to-border modes (e.g. shadow maps: white = unshadowed).
                const uint32_t border = fetch.borderColor();
                sampler.borderColor = border == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                    : border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
                sampler.mipLodBias = std::clamp(float(fetch.lodBias()) / 32.0f, -limits.maxSamplerLodBias, limits.maxSamplerLodBias);
                sampler.minLod = float(fetch.mipMinLevel());
                sampler.maxLod = mipFilter == 2 ? 0.0f : float(fetch.mipMaxLevel());
                if (sampler.maxLod < sampler.minLod)
                    sampler.maxLod = sampler.minLod;
                samplers[n] = GetSampler(sampler);
            }

            // The draw's descriptors, for every binding its shaders declare.
            VkDescriptorSet set = context_.AllocateSet(pair->setLayout);
            auto drawState = context_.Allocate(sizeof(DrawState), limits.minUniformBufferOffsetAlignment);
            if (!set || !drawState.cpu)
                return;
            std::memcpy(drawState.cpu, &state, sizeof(state));
            const size_t bindingCount = pair->bindings.size();
            std::vector<VkWriteDescriptorSet> writes(bindingCount);
            std::vector<VkDescriptorBufferInfo> buffers(bindingCount);
            std::vector<VkDescriptorImageInfo> images(bindingCount);
            for (size_t i = 0; i < bindingCount; i++)
            {
                const uint32_t binding = pair->bindings[i];
                VkWriteDescriptorSet& write = writes[i];
                write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                write.dstSet = set;
                write.dstBinding = HostBinding(binding);
                write.descriptorCount = 1;
                write.descriptorType = BindingType(binding);
                if (binding == kFloatConstantsBinding)
                    buffers[i] = { floats.buffer, floats.offset, 512 * 16 };
                else if (binding == kDrawStateBinding)
                    buffers[i] = { drawState.buffer, drawState.offset, sizeof(DrawState) };
                else if (binding < kTextureBinding)
                    buffers[i] = streams[binding - kStreamBinding];
                else if (binding < kFallbackBinding)
                {
                    const uint32_t n = binding - kTextureBinding;
                    const VkImageView dummy = (cubeMask >> n) & 1 ? dummyCube_.view : (volumeMask >> n) & 1 ? dummy3D_.view : dummy2D_.view;
                    static const bool dummyOnly = std::getenv("HC09_DUMMY_TEXTURES") != nullptr;
                    images[i] = { VK_NULL_HANDLE, textureViews[n] && !dummyOnly ? textureViews[n] : dummy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                }
                else if (binding < kSamplerBinding)
                {
                    const uint32_t n = binding - kFallbackBinding;
                    images[i] = { VK_NULL_HANDLE, fallbackViews[n] ? fallbackViews[n] : dummy2D_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                }
                else
                {
                    const uint32_t n = binding - kSamplerBinding;
                    images[i] = { n < 32 && samplers[n] ? samplers[n] : linearClamp_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
                }
                if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || write.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                    write.pBufferInfo = &buffers[i];
                else
                    write.pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(device(), uint32_t(writes.size()), writes.data(), 0, nullptr);

            VulkanContext::Allocation indexUpload{};
            if (indexed)
            {
                indexUpload = context_.Allocate(indices.size() * 4, 4);
                if (!indexUpload.cpu)
                    return;
                std::memcpy(indexUpload.cpu, indices.data(), indices.size() * 4);
            }

            BeginPass(target, depth);
            VkCommandBuffer list = commands();
            vkCmdBindPipeline(list, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdBindDescriptorSets(list, VK_PIPELINE_BIND_POINT_GRAPHICS, pair->layout, 0, 1, &set, 0, nullptr);
            vkCmdSetStencilReference(list, VK_STENCIL_FACE_FRONT_AND_BACK, ReadRegister(reg::kStencilRefMask) & 0xFF);
            const float blendFactor[4] = { 1, 1, 1, 1 };
            vkCmdSetBlendConstants(list, blendFactor);
            const VkViewport viewport{ 0, 0, float(kTargetWidth * g_scale), float(kTargetHeight * g_scale), 0, 1 };
            vkCmdSetViewport(list, 0, 1, &viewport);
            const uint32_t tl = ReadRegister(reg::kWindowScissorTl), br = ReadRegister(reg::kWindowScissorBr);
            int32_t left = int32_t(tl & 0x7FFF), top = int32_t((tl >> 16) & 0x7FFF);
            int32_t right = int32_t(std::min<uint32_t>(br & 0x7FFF, kTargetWidth)), bottom = int32_t(std::min<uint32_t>((br >> 16) & 0x7FFF, kTargetHeight));
            // Predicated tiling: the first pass renders the whole frame into the screen-sized
            // target (later passes skip their draws), so the per-tile scissor doesn't apply.
            if (InFirstTilePass())
                left = 0, top = 0, right = int32_t(kTargetWidth), bottom = int32_t(kTargetHeight);
            if (right <= left || bottom <= top)
                left = top = right = bottom = 0;
            const VkRect2D scissor{ { left * int32_t(g_scale), top * int32_t(g_scale) },
                { uint32_t(right - left) * g_scale, uint32_t(bottom - top) * g_scale } };
            vkCmdSetScissor(list, 0, 1, &scissor);
            if (indexed)
            {
                vkCmdBindIndexBuffer(list, indexUpload.buffer, indexUpload.offset, VK_INDEX_TYPE_UINT32);
                // VGT_INDX_OFFSET is applied in the vertex shader (vertexIndexOffset).
                vkCmdDrawIndexed(list, uint32_t(indices.size()), 1, 0, 0, 0);
            }
            else
            {
                vkCmdDraw(list, expandRectangles ? expandedVertices : draw.indexCount, 1, 0, 0);
            }
            draws_++;
        }

        // Clears a rectangle of a target pair's colour or depth/stencil (inside a render pass on it).
        void Renderer::ClearRect(RenderTarget& target, DepthTarget& depth, VkImageAspectFlags aspects, const VkClearValue& value,
            const VkRect2D& rect)
        {
            if (rect.extent.width == 0 || rect.extent.height == 0)
                return;
            BeginPass(target, depth);
            VkClearAttachment attachment{ aspects, aspects == VK_IMAGE_ASPECT_COLOR_BIT ? 0u : VK_ATTACHMENT_UNUSED, value };
            const VkClearRect clearRect{ rect, 0, 1 };
            vkCmdClearAttachments(commands(), 1, &attachment, 1, &clearRect);
        }

        void Renderer::Resolve()
        {
            ResolveCopy();
            // On Adreno (the V30) the frame's work so far goes to the GPU: a frame of many full-screen
            // passes (the title's blur chain, resolved after each pass) in one submission makes its
            // driver give up. Elsewhere the extra submissions only cost time (Pixel 8 title: ~23 fps with
            // them, ~28 without, on a warm phone).
            // HC09_SPLIT_SUBMITS=0/1 overrides.
            static const bool split = [this] {
                const char* value = std::getenv("HC09_SPLIT_SUBMITS");
                return value ? std::atoi(value) != 0 : context_.properties().vendorID == 0x5143;  // Qualcomm
            }();
            if (split)
            {
                EndPass();
                context_.Submit();
            }
        }

        void Renderer::ResolveCopy()
        {
            const uint32_t control = ReadRegister(reg::kCopyControl);
            const uint32_t source = control & 7;
            if (source == 4)
            {
                ResolveDepth(control);
                return;
            }
            if (source > 4)
                return;
            const uint32_t colorInfo = source == 0 ? ReadRegister(reg::kColorInfo) : ReadRegister(reg::kColor1Info + source - 1);
            RenderTarget& target = GetRenderTarget(colorInfo, SampleCount(ReadRegister(reg::kSurfaceInfo)));
            EndPass();

            uint32_t destBase = ReadRegister(reg::kCopyDestBase) & 0x1FFFFFFF;
            const uint32_t destPitch = ReadRegister(reg::kCopyDestPitch);
            const uint32_t width = std::clamp<uint32_t>(destPitch & 0x3FFF, 1, kTargetWidth);
            const uint32_t height = std::clamp<uint32_t>((destPitch >> 16) & 0x3FFF, 1, kTargetHeight);
            const VkFormat format = target.image.format;

            // Predicated tiling resolves each screen strip with the destination base advanced to
            // that strip's tiles inside one surface. Such a base lies inside an existing target of
            // the same size: resolve into that target (the rectangle is already in screen space).
            {
                const uint32_t bytesPerPixel = format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
                const uint32_t surfaceBytes = ((width + 31) & ~31u) * ((height + 31) & ~31u) * bytesPerPixel;
                // Only a target the first strip resolved into this frame qualifies: targets left from
                // earlier scenes can overlap the strip's address too.
                uint32_t owner = destBase;
                for (const auto& [base, existing] : resolveTargets_)
                    if (base < destBase && destBase < base + surfaceBytes && existing.width == width && existing.height == height &&
                        existing.image.format == format && existing.lastResolved == g_frame && (owner == destBase || base > owner))
                        owner = base;
                destBase = owner;
            }

            ResolveTarget& dest = resolveTargets_[destBase];
            if (!dest.image.image || dest.image.format != format || dest.width != width || dest.height != height)
            {
                if (dest.image.image)
                {
                    textures_->ForgetResolved(destBase);
                    DestroyImageLater(dest.image);
                }
                dest = {};
                if (!CreateImage(dest.image, format, width * g_scale, height * g_scale,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT))
                {
                    resolveTargets_.erase(destBase);
                    return;
                }
                dest.width = width;
                dest.height = height;
            }

            // The resolve draw's three vertices (fetch constant 0, float2 each) give the
            // rectangle in screen coordinates. Draws ignore the window offset, so with
            // predicated tiling every strip pass renders into the same screen-space target and
            // the rectangle is both the source and the destination position.
            const VkRect2D box = ResolveRectangle(width, height);
            static const bool traceResolves = std::getenv("HC09_TRACE_RESOLVES") != nullptr;
            if (traceResolves || g_frame == g_dumpFrame)
                std::fprintf(stderr, "[resolve] frame %d dest 0x%08X %ux%u rect %d,%d %ux%u surface 0x%08X control 0x%08X\n", g_frame, destBase,
                    width, height, box.offset.x, box.offset.y, box.extent.width, box.extent.height, ReadRegister(reg::kSurfaceInfo), control);
            if (box.extent.width == 0 || box.extent.height == 0)
            {
                // Keep a new target sampleable even when nothing was copied into it.
                Transition(commands(), dest.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                return;
            }

            const VkRect2D scaled{ { box.offset.x * int32_t(g_scale), box.offset.y * int32_t(g_scale) },
                { box.extent.width * g_scale, box.extent.height * g_scale } };
            Transition(commands(), target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(commands(), dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffset = region.dstOffset = { scaled.offset.x, scaled.offset.y, 0 };
            region.extent = { scaled.extent.width, scaled.extent.height, 1 };
            vkCmdCopyImage(commands(), target.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            Transition(commands(), dest.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            // RB_COPY_DEST_INFO bit 24: copy_dest_swap (red and blue exchanged in memory); bits 7-12:
            // copy_dest_format, the texture format the resolve writes (the title samples it as such).
            dest.lastResolved = g_frame;
            const uint32_t destInfo = ReadRegister(0x231B);
            textures_->SetResolved(destBase, dest.image.image, format, width, height, (destInfo >> 24) & 1,
                MemoryTexelBytes((destInfo >> 7) & 0x3F));

            // Tile resolves clear their EDRAM tile for the next tile. Targets here hold the whole
            // screen (the first pass rendered it all), so clearing a strip would wipe content
            // that later full-screen resolves still copy.
            const bool tileClear = TilingActive();
            // During tiling every strip resolves its depth before its colour resolve clears the
            // tile. All strips share one screen-sized depth buffer here, so clear the whole buffer
            // only once the last (bottom-right) strip has been resolved.
            const bool lastTile = uint32_t(box.offset.x) + box.extent.width >= width && uint32_t(box.offset.y) + box.extent.height >= height;
            DepthTarget& depth = GetDepthTarget(ReadRegister(reg::kDepthInfo), target.samples);
            if (((control >> 9) & 1) && (!tileClear || lastTile))  // depth_clear_enable: RB_DEPTH_CLEAR is depth (24 bits) << 8 | stencil
            {
                const uint32_t clear = ReadRegister(reg::kDepthClear);
                VkClearValue value{};
                value.depthStencil = { float(clear >> 8) / float(0xFFFFFF), clear & 0xFF };
                const VkRect2D whole{ { 0, 0 }, { kTargetWidth * g_scale, kTargetHeight * g_scale } };
                ClearRect(target, depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, value, tileClear ? whole : scaled);
            }
            if (((control >> 8) & 1) && !tileClear)  // color_clear_enable
            {
                // For 32bpp targets the clear value is RB_COLOR_CLEAR_LO, packed in the target's
                // format (8_8_8_8: red in the low byte). RB_COLOR_CLEAR holds the high half for 64bpp.
                const uint32_t clear = ReadRegister(reg::kColorClear + 1);
                VkClearValue value{};
                value.color.float32[0] = (clear & 0xFF) / 255.0f;
                value.color.float32[1] = ((clear >> 8) & 0xFF) / 255.0f;
                value.color.float32[2] = ((clear >> 16) & 0xFF) / 255.0f;
                value.color.float32[3] = (clear >> 24) / 255.0f;
                ClearRect(target, depth, VK_IMAGE_ASPECT_COLOR_BIT, value, scaled);
            }
        }

        // Resolve of the depth buffer: its depth (0..1) into an R32_SFLOAT texture at the
        // destination, over the resolve rectangle; then the requested depth clear.
        void Renderer::ResolveDepth(uint32_t control)
        {
            const uint32_t rawDestBase = ReadRegister(reg::kCopyDestBase) & 0x1FFFFFFF;
            const uint32_t destPitch = ReadRegister(reg::kCopyDestPitch);
            const uint32_t width = std::clamp<uint32_t>(destPitch & 0x3FFF, 1, kTargetWidth);
            const uint32_t height = std::clamp<uint32_t>((destPitch >> 16) & 0x3FFF, 1, kTargetHeight);
            const uint32_t samples = SampleCount(ReadRegister(reg::kSurfaceInfo));
            DepthTarget& depth = GetDepthTarget(ReadRegister(reg::kDepthInfo), samples);
            EndPass();
            const VkRect2D box = ResolveRectangle(width, height);

            // Predicated tiling resolves depth strip by strip with the destination advanced into one
            // surface, as for colour: resolve every strip into the surface's single target.
            uint32_t destBase = rawDestBase;
            {
                const uint32_t surfaceBytes = ((width + 31) & ~31u) * ((height + 31) & ~31u) * 4;
                uint32_t owner = destBase;
                for (const auto& [base, existing] : resolveTargets_)
                    if (base < destBase && destBase < base + surfaceBytes && existing.width == width && existing.height == height &&
                        existing.image.format == VK_FORMAT_R32_SFLOAT && existing.lastResolved == g_frame && (owner == destBase || base > owner))
                        owner = base;
                destBase = owner;
            }

            ResolveTarget& dest = resolveTargets_[destBase];
            if (!dest.image.image || dest.image.format != VK_FORMAT_R32_SFLOAT || dest.width != width || dest.height != height)
            {
                if (dest.image.image)
                {
                    textures_->ForgetResolved(destBase);
                    VkDevice device = this->device();
                    VkFramebuffer framebuffer = dest.framebuffer;
                    if (framebuffer)
                        context_.DestroyLater([device, framebuffer] { vkDestroyFramebuffer(device, framebuffer, nullptr); });
                    DestroyImageLater(dest.image);
                }
                dest = {};
                if (!CreateImage(dest.image, VK_FORMAT_R32_SFLOAT, width * g_scale, height * g_scale,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
                {
                    resolveTargets_.erase(destBase);
                    return;
                }
                VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
                info.renderPass = depthCopyPass_;
                info.attachmentCount = 1;
                info.pAttachments = &dest.image.view;
                info.width = width * g_scale;
                info.height = height * g_scale;
                info.layers = 1;
                vkCreateFramebuffer(device(), &info, nullptr, &dest.framebuffer);
                dest.width = width;
                dest.height = height;
            }
            if (g_frame == g_dumpFrame)
                std::fprintf(stderr, "[frame] depth resolve: control 0x%08X dest 0x%08X %ux%u rect %d,%d %ux%u\n", control, destBase,
                    width, height, box.offset.x, box.offset.y, box.extent.width, box.extent.height);
            const VkRect2D scaled{ { box.offset.x * int32_t(g_scale), box.offset.y * int32_t(g_scale) },
                { box.extent.width * g_scale, box.extent.height * g_scale } };
            if (box.extent.width > 0 && box.extent.height > 0 && depthCopyPipeline_ && dest.framebuffer)
            {
                Transition(commands(), depth.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                Transition(commands(), dest.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                VkDescriptorSet set = context_.AllocateSet(depthCopySetLayout_);
                if (set)
                {
                    const VkDescriptorImageInfo image{ VK_NULL_HANDLE, depth.depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    write.dstSet = set;
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                    write.pImageInfo = &image;
                    vkUpdateDescriptorSets(device(), 1, &write, 0, nullptr);
                    VkRenderPassBeginInfo pass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                    pass.renderPass = depthCopyPass_;
                    pass.framebuffer = dest.framebuffer;
                    pass.renderArea.extent = { width * g_scale, height * g_scale };
                    vkCmdBeginRenderPass(commands(), &pass, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(commands(), VK_PIPELINE_BIND_POINT_GRAPHICS, depthCopyPipeline_);
                    vkCmdBindDescriptorSets(commands(), VK_PIPELINE_BIND_POINT_GRAPHICS, depthCopyLayout_, 0, 1, &set, 0, nullptr);
                    const VkViewport viewport{ 0, 0, float(width * g_scale), float(height * g_scale), 0, 1 };
                    vkCmdSetViewport(commands(), 0, 1, &viewport);
                    vkCmdSetScissor(commands(), 0, 1, &scaled);
                    vkCmdDraw(commands(), 3, 1, 0, 0);
                    vkCmdEndRenderPass(commands());
                }
            }
            Transition(commands(), dest.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            dest.lastResolved = g_frame;
            textures_->SetResolved(destBase, dest.image.image, VK_FORMAT_R32_SFLOAT, width, height);

            // As for colour resolves: during tiling, clear the shared depth buffer only after the
            // last strip.
            const bool lastTile = uint32_t(box.offset.x) + box.extent.width >= width && uint32_t(box.offset.y) + box.extent.height >= height;
            if (((control >> 9) & 1) && (!TilingActive() || lastTile))  // depth_clear_enable
            {
                const uint32_t clear = ReadRegister(reg::kDepthClear);
                VkClearValue value{};
                value.depthStencil = { float(clear >> 8) / float(0xFFFFFF), clear & 0xFF };
                const VkRect2D whole{ { 0, 0 }, { kTargetWidth * g_scale, kTargetHeight * g_scale } };
                ClearRect(GetRenderTarget(ReadRegister(reg::kColorInfo), samples), depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    value, TilingActive() ? whole : scaled);
            }
        }

        // Bounding box of the resolve draw's rectangle, clamped to the destination size.
        VkRect2D Renderer::ResolveRectangle(uint32_t destWidth, uint32_t destHeight)
        {
            const uint32_t d0 = ReadRegister(reg::kFetchConstants), d1 = ReadRegister(reg::kFetchConstants + 1);
            const uint32_t address = PhysicalToGuest(d0 & ~3u);
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (uint32_t i = 0; i < 6; i++)
            {
                uint32_t raw;
                std::memcpy(&raw, GuestToHost(address + i * 4), 4);
                const uint32_t bits = GpuSwap(raw, d1 & 3);
                float value;
                std::memcpy(&value, &bits, 4);
                if (i & 1)
                    minY = std::min(minY, value), maxY = std::max(maxY, value);
                else
                    minX = std::min(minX, value), maxX = std::max(maxX, value);
            }
            auto clamp = [](float value, uint32_t limit) {
                return uint32_t(std::clamp(value, 0.0f, float(limit)));
            };
            const uint32_t limitX = std::min(destWidth, kTargetWidth), limitY = std::min(destHeight, kTargetHeight);
            const uint32_t left = clamp(minX, limitX), top = clamp(minY, limitY), right = clamp(maxX, limitX), bottom = clamp(maxY, limitY);
            return { { int32_t(left), int32_t(top) }, { right > left ? right - left : 0, bottom > top ? bottom - top : 0 } };
        }

        void Renderer::EvictIdleResources(uint32_t frontBuffer)
        {
            constexpr int kIdleFrames = 1800;  // ~1 minute
            textures_->Evict(g_frame);
            if (g_frame % 300 != 0)
                return;
            for (auto it = resolveTargets_.begin(); it != resolveTargets_.end();)
            {
                const int lastUse = std::max(it->second.lastResolved, textures_->ResolvedLastUse(it->first));
                // The front buffer is shown every frame. A result is the only copy of what the title
                // rendered (resolves are not written to memory): one built once and sampled later, such
                // as the turf's grass texture made while a game loads, is released only after the
                // title has put other data in its memory.
                if (!it->second.framebuffer && it->first != (frontBuffer & 0x1FFFFFFF) && g_frame - lastUse > kIdleFrames &&
                    textures_->ResolvedMemoryChanged(it->first))
                {
                    textures_->ForgetResolved(it->first);
                    DestroyImageLater(it->second.image);
                    it = resolveTargets_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void Renderer::Swap(uint32_t frontBuffer)
        {
            EvictIdleResources(frontBuffer);
            EndPass();
            // New pipelines go to the on-disk cache now and then (every 10 s or so).
            if (pipelinesAdded_ && g_frame % 300 == 0)
            {
                pipelinesAdded_ = false;
                context_.SavePipelineCache();
            }
            CheckWindow();
            // Frame rate and draw counts every 5 s.
            {
                static auto last = std::chrono::steady_clock::now();
                static int lastFrame = 0;
                static uint32_t lastDraws = 0, lastSkipped = 0;
                const auto now = std::chrono::steady_clock::now();
                const double seconds = std::chrono::duration<double>(now - last).count();
                if (seconds >= 5.0)
                {
                    const int frames = std::max(1, g_frame - lastFrame);
                    std::fprintf(stderr, "[gpu] frame %d: %.1f fps, %.0f draws/frame, %.0f skipped/frame, %zu pipelines, %zu textures\n", g_frame,
                        (g_frame - lastFrame) / seconds, double(draws_ - lastDraws) / frames, double(skippedDraws_ - lastSkipped) / frames,
                        pipelines_.size(), textures_->GetStats().textures);
                    last = now;
                    lastFrame = g_frame;
                    lastDraws = draws_;
                    lastSkipped = skippedDraws_;
                }
            }

            auto resolved = resolveTargets_.find(frontBuffer & 0x1FFFFFFF);
            ResolveTarget* front = resolved != resolveTargets_.end() && resolved->second.image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                ? &resolved->second : nullptr;
            // HC09_TRACE_SWAPS=1: the front buffer each swap shows and when it was last resolved.
            static const bool traceSwaps = std::getenv("HC09_TRACE_SWAPS") != nullptr;
            if (traceSwaps)
                std::fprintf(stderr, "[swap] frame %d front 0x%08X %s (last resolved frame %d)\n", g_frame, frontBuffer,
                    front ? "found" : "MISSING", front ? front->lastResolved : -1);
            static bool loggedFront = false;
            if (front && !loggedFront)
            {
                loggedFront = true;
                std::fprintf(stderr, "[gpu] frame %d: first resolved front buffer 0x%08X (%ux%u)\n", g_frame, frontBuffer, front->width,
                    front->height);
            }
            const bool havePipeline = CreatePresentPipeline();
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (front && havePipeline)
            {
                set = context_.AllocateSet(presentSetLayout_);
                if (set)
                {
                    const VkDescriptorImageInfo image{ VK_NULL_HANDLE, front->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                    const VkDescriptorImageInfo sampler{ linearClamp_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
                    VkWriteDescriptorSet writes[2]{};
                    for (int i = 0; i < 2; i++)
                    {
                        writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                        writes[i].dstSet = set;
                        writes[i].dstBinding = uint32_t(i);
                        writes[i].descriptorCount = 1;
                        writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
                        writes[i].pImageInfo = i == 0 ? &image : &sampler;
                    }
                    vkUpdateDescriptorSets(device(), 2, writes, 0, nullptr);
                }
            }
            const float gray[3] = { 0.08f, 0.08f, 0.1f }, black[3] = { 0, 0, 0 };
            context_.EndFrame(
                [&](VkCommandBuffer list, VkExtent2D extent) {
                    if (!set)
                        return;
                    // Keep the frame's aspect ratio: the largest centred rectangle of that shape, with
                    // black bars around it.
                    const float windowWidth = float(extent.width), windowHeight = float(extent.height);
                    const float aspect = float(front->width) / float(front->height);
                    float viewWidth = windowWidth, viewHeight = windowWidth / aspect;
                    if (viewHeight > windowHeight)
                    {
                        viewHeight = windowHeight;
                        viewWidth = windowHeight * aspect;
                    }
                    const float viewX = std::floor((windowWidth - viewWidth) / 2), viewY = std::floor((windowHeight - viewHeight) / 2);
                    const VkViewport viewport{ viewX, viewY, viewWidth, viewHeight, 0, 1 };
                    const VkRect2D scissor{ { int32_t(viewX), int32_t(viewY) }, { uint32_t(viewWidth), uint32_t(viewHeight) } };
                    vkCmdBindPipeline(list, VK_PIPELINE_BIND_POINT_GRAPHICS, presentPipeline_);
                    vkCmdBindDescriptorSets(list, VK_PIPELINE_BIND_POINT_GRAPHICS, presentLayout_, 0, 1, &set, 0, nullptr);
                    vkCmdSetViewport(list, 0, 1, &viewport);
                    vkCmdSetScissor(list, 0, 1, &scissor);
                    vkCmdDraw(list, 3, 1, 0, 0);
                },
                front ? black : gray);
        }

        Renderer* g_renderer = nullptr;
        bool g_rendererFailed = false;

        Renderer* GetRenderer()
        {
            if (!g_renderer && !g_rendererFailed && !std::getenv("HC09_NO_RENDER"))
            {
                ReadOptions();
                g_renderer = new Renderer();
                if (!g_renderer->Initialize())
                {
                    std::fprintf(stderr, "[gpu] renderer initialization failed; continuing without graphics\n");
                    g_rendererFailed = true;
                    g_renderer = nullptr;  // leaked: the device may still be in use
                }
            }
            return g_renderer;
        }

        void DumpDraw(const DrawInfo& draw)
        {
            const auto vs = ActiveShader(ShaderType::Vertex);
            const auto ps = ActiveShader(ShaderType::Pixel);
            std::fprintf(stderr,
                "[frame] draw %d: prim %u source %u count %u%s | mode %u color 0x%08X surface 0x%08X depth 0x%08X "
                "mask 0x%X | vs %016llX ps %016llX | vte 0x%X vport scale (%g %g %g) offset (%g %g %g) | "
                "window offset 0x%X scissor 0x%X-0x%X | consts vs 0x%X ps 0x%X\n",
                g_drawInFrame, uint32_t(draw.primitive), uint32_t(draw.source), draw.indexCount, draw.index32 ? " (32-bit)" : "",
                ReadRegister(0x2208) & 7, ReadRegister(0x2001), ReadRegister(0x2000), ReadRegister(0x2002), ReadRegister(0x2104),
                vs ? (unsigned long long)vs->hash : 0ull, ps ? (unsigned long long)ps->hash : 0ull, ReadRegister(0x2206),
                RegisterFloat(0x210F), RegisterFloat(0x2111), RegisterFloat(0x2113), RegisterFloat(0x2110), RegisterFloat(0x2112),
                RegisterFloat(0x2114), ReadRegister(0x2080), ReadRegister(0x2081), ReadRegister(0x2082), ReadRegister(0x2307),
                ReadRegister(0x2308));
        }
    }

    bool PrecompileProgress(int& done, int& total)
    {
        total = g_precompileTotal;
        done = g_precompileDone;
        return total > 0;
    }

    bool IsDumpFrame()
    {
        return g_frame == g_dumpFrame;
    }

    void ExecuteDraw(const DrawInfo& draw)
    {
        if (g_frame == g_dumpFrame)
            DumpDraw(draw);
        g_drawInFrame++;
        if (Renderer* renderer = GetRenderer())
            renderer->Draw(draw);
    }

    int FrameNumber()
    {
        return g_frame;
    }

    void ExecuteSwap(uint32_t frontBuffer, uint32_t width, uint32_t height)
    {
        if (g_frame == g_dumpFrame)
            std::fprintf(stderr, "[frame] swap: front buffer 0x%08X %ux%u\n", frontBuffer, width, height);
        g_frame++;
        g_drawInFrame = 0;
        if (g_dumpEvery > 0 && g_dumpFrame >= 0 && g_frame > g_dumpFrame)
            g_dumpFrame += g_dumpEvery;
        if (Renderer* renderer = GetRenderer())
            renderer->Swap(frontBuffer);
    }
}
