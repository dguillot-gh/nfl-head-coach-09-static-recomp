// vkpipeprobe: creates a real Vulkan graphics pipeline for each HC09 SPIR-V shader on the phone's driver
// (drivers compile at pipeline creation, so this is where a driver rejects a shader) and times it.
//   adb push vkpipeprobe <spv files> /data/local/tmp/... && adb shell .../vkpipeprobe <ref.vs.spv> <ref.ps.spv> <file.spv>...
// Each vertex shader is paired with the reference pixel shader and each pixel shader with the reference
// vertex shader (all HC09 vertex shaders export the same 16 interpolators). Descriptor bindings are read from
// the SPIR-V and typed by range, matching tools/shaders_to_spirv.py:
//   0-1 uniform buffer, 2-17 storage buffer, 18-99 sampled image, 100-131 sampler.
// Render pass: 4 x RGBA8 colour (SV_Target0..3) + D24S8 depth. Prints one line per shader.
// Build (NDK): clang --target=aarch64-linux-android26 -O2 vkpipeprobe.c -lvulkan -o vkpipeprobe
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    uint32_t* words;
    size_t count;
    uint32_t bindings[64];
    int bindingCount;
} Spirv;

static int load(const char* path, Spirv* s)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s->words = malloc(n);
    s->count = fread(s->words, 4, n / 4, f);
    fclose(f);
    s->bindingCount = 0;
    // OpDecorate (opcode 71) <id> Binding (33) <n>
    for (size_t i = 5; i < s->count;)
    {
        uint32_t word = s->words[i], op = word & 0xFFFF, length = word >> 16;
        if (length == 0)
            break;
        if (op == 71 && length >= 4 && s->words[i + 2] == 33 && s->bindingCount < 64)
            s->bindings[s->bindingCount++] = s->words[i + 3];
        i += length;
    }
    return s->count > 5 && s->words[0] == 0x07230203;
}

static VkDescriptorType typeOf(uint32_t binding)
{
    if (binding <= 1)
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (binding <= 17)
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (binding < 100)
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return VK_DESCRIPTOR_TYPE_SAMPLER;
}

static double nowMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static VkDevice g_device;
static VkRenderPass g_pass;

static VkShaderModule module(const Spirv* s)
{
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = s->count * 4;
    info.pCode = s->words;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g_device, &info, NULL, &m);
    return m;
}

// Returns the vkCreateGraphicsPipelines result; *ms gets the time it took.
static VkResult pipeline(const Spirv* vs, const Spirv* ps, double* ms)
{
    VkDescriptorSetLayoutBinding bindings[128];
    int count = 0;
    const Spirv* stages[2] = {vs, ps};
    const VkShaderStageFlags flags[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (int s = 0; s < 2; s++)
        for (int b = 0; b < stages[s]->bindingCount; b++)
        {
            uint32_t binding = stages[s]->bindings[b];
            int found = -1;
            for (int i = 0; i < count; i++)
                if (bindings[i].binding == binding)
                    found = i;
            if (found >= 0)
            {
                bindings[found].stageFlags |= flags[s];
                continue;
            }
            VkDescriptorSetLayoutBinding* d = &bindings[count++];
            memset(d, 0, sizeof(*d));
            d->binding = binding;
            d->descriptorType = typeOf(binding);
            d->descriptorCount = 1;
            d->stageFlags = flags[s];
        }
    VkDescriptorSetLayoutCreateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = count;
    setInfo.pBindings = bindings;
    VkDescriptorSetLayout set;
    VkResult r = vkCreateDescriptorSetLayout(g_device, &setInfo, NULL, &set);
    if (r != VK_SUCCESS)
        return r;
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &set;
    VkPipelineLayout layout;
    r = vkCreatePipelineLayout(g_device, &layoutInfo, NULL, &layout);
    if (r != VK_SUCCESS)
        return r;

    VkPipelineShaderStageCreateInfo shaderStages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = module(vs);
    shaderStages[0].pName = "main";
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = module(ps);
    shaderStages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend[4];
    memset(blend, 0, sizeof(blend));
    for (int i = 0; i < 4; i++)
    {
        blend[i].colorWriteMask = 0xF;
        blend[i].blendEnable = i == 0;
        blend[i].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend[i].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 4;
    colorBlend.pAttachments = blend;
    VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = shaderStages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    info.renderPass = g_pass;
    VkPipeline p = VK_NULL_HANDLE;
    double start = nowMs();
    r = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &info, NULL, &p);
    *ms = nowMs() - start;
    if (p)
        vkDestroyPipeline(g_device, p, NULL);
    vkDestroyShaderModule(g_device, shaderStages[0].module, NULL);
    vkDestroyShaderModule(g_device, shaderStages[1].module, NULL);
    vkDestroyPipelineLayout(g_device, layout, NULL);
    vkDestroyDescriptorSetLayout(g_device, set, NULL);
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: vkpipeprobe <ref.vs.spv> <ref.ps.spv> <file.spv>...\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t version = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion env = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (env)
        env(&version);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
        return 1;
    uint32_t n = 1;
    VkPhysicalDevice gpu;
    vkEnumeratePhysicalDevices(instance, &n, &gpu);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU %s, Vulkan %u.%u\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(gpu, &dci, NULL, &g_device) != VK_SUCCESS)
        return 1;

    VkAttachmentDescription attachments[5];
    VkAttachmentReference colors[4];
    memset(attachments, 0, sizeof(attachments));
    for (int i = 0; i < 5; i++)
    {
        attachments[i].format = i < 4 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_D24_UNORM_S8_UINT;
        attachments[i].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[i].initialLayout = i < 4 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[i].finalLayout = attachments[i].initialLayout;
        if (i < 4)
            colors[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    VkAttachmentReference depthRef = {4, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 4;
    subpass.pColorAttachments = colors;
    subpass.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 5;
    rpi.pAttachments = attachments;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subpass;
    if (vkCreateRenderPass(g_device, &rpi, NULL, &g_pass) != VK_SUCCESS)
        return 1;

    Spirv refVs, refPs;
    if (!load(argv[1], &refVs) || !load(argv[2], &refPs))
    {
        fprintf(stderr, "cannot read the reference shaders\n");
        return 1;
    }
    int ok = 0, failed = 0;
    double total = 0, worst = 0;
    for (int i = 3; i < argc; i++)
    {
        const char* name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        Spirv s;
        if (!load(argv[i], &s))
        {
            printf("FAIL %s unreadable\n", name);
            failed++;
            continue;
        }
        const int pixel = strstr(name, ".ps.") != NULL;
        double ms = 0;
        VkResult r = pixel ? pipeline(&refVs, &s, &ms) : pipeline(&s, &refPs, &ms);
        if (r == VK_SUCCESS)
        {
            ok++;
            total += ms;
            if (ms > worst)
                worst = ms;
            printf("ok   %s %.1f ms\n", name, ms);
        }
        else
        {
            failed++;
            printf("FAIL %s VkResult %d\n", name, r);
        }
        free(s.words);
    }
    printf("%d ok, %d failed; pipeline compile %.0f ms total, %.1f ms average, %.1f ms worst\n", ok, failed, total,
        ok ? total / ok : 0.0, worst);
    vkDestroyRenderPass(g_device, g_pass, NULL);
    vkDestroyDevice(g_device, NULL);
    vkDestroyInstance(instance, NULL);
    return failed ? 1 : 0;
}
