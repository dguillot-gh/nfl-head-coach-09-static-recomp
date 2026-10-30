// vkprobe: reports what a phone offers an Xbox 360 recomp port (HC09): Vulkan version, features, limits,
// extensions, texture/render formats, memory, CPU features, and whether a 4 GB guest space can be reserved.
//   adb push vkprobe /data/local/tmp/ && adb shell /data/local/tmp/vkprobe [--all-ext] [--memtest MB]
// --all-ext lists every device extension; --memtest commits and touches MB of guest memory (can get killed on
// a low-RAM phone, which is itself the answer).
// Build (NDK): clang --target=aarch64-linux-android26 -O2 vkprobe.c -lvulkan -o vkprobe
#include <vulkan/vulkan.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef HWCAP_ATOMICS
#define HWCAP_ATOMICS (1 << 8)
#endif
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif

static void prop(const char* name) {
    char v[PROP_VALUE_MAX] = "";
    __system_property_get(name, v);
    printf("  %-28s %s\n", name, v);
}

static void meminfo(void) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, "MemTotal", 8) || !strncmp(line, "MemAvailable", 12) || !strncmp(line, "SwapTotal", 9))
            printf("  %s", line);
    fclose(f);
}

static void system_info(void) {
    printf("== System\n");
    prop("ro.product.model");
    prop("ro.build.version.release");
    prop("ro.build.version.sdk");
    prop("ro.board.platform");
    meminfo();
    printf("  cpus online                  %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    printf("  page size                    %ld\n", sysconf(_SC_PAGESIZE));
    unsigned long hw = getauxval(AT_HWCAP);
    printf("  LSE atomics                  %s\n", (hw & HWCAP_ATOMICS) ? "yes" : "no");
    printf("  dotprod                      %s\n", (hw & HWCAP_ASIMDDP) ? "yes" : "no");
}

// The guest is a 32-bit address space; reserve it all up front like the port will.
static void guest_memory(long memtest_mb) {
    printf("== Guest memory\n");
    const size_t size = 0x100000000ull;
    unsigned char* p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { printf("  reserve 4 GB                 FAILED\n"); return; }
    printf("  reserve 4 GB                 ok\n");
    if (memtest_mb > 0) {
        size_t n = (size_t)memtest_mb << 20;
        if (mprotect(p, n, PROT_READ | PROT_WRITE)) {
            printf("  commit %ld MB                FAILED\n", memtest_mb);
        } else {
            long page = sysconf(_SC_PAGESIZE);
            for (size_t i = 0; i < n; i += page) p[i] = 1;
            printf("  commit+touch %ld MB          ok\n", memtest_mb);
            meminfo();
        }
    }
    munmap(p, size);
}

static const char* vendor_name(uint32_t id) {
    switch (id) {
    case 0x5143: return "Qualcomm";
    case 0x13B5: return "ARM";
    case 0x1010: return "ImgTec";
    default: return "?";
    }
}

static void print_version(const char* label, uint32_t v) {
    printf("  %-28s %u.%u.%u\n", label, VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
}

#define FEAT(f) printf("  %-36s %s\n", #f, feats.f ? "yes" : "NO")

static void features(const VkPhysicalDeviceFeatures* fp) {
    VkPhysicalDeviceFeatures feats = *fp;
    printf("== Features\n");
    FEAT(vertexPipelineStoresAndAtomics); // Xenos memexport; GoldenEye-Recomp says Mali lacks it
    FEAT(fragmentStoresAndAtomics);
    FEAT(shaderInt64);
    FEAT(shaderInt16);
    FEAT(shaderFloat64);
    FEAT(geometryShader);
    FEAT(tessellationShader);
    FEAT(independentBlend);
    FEAT(dualSrcBlend);
    FEAT(logicOp);
    FEAT(depthClamp);
    FEAT(depthBiasClamp);
    FEAT(depthBounds);
    FEAT(fillModeNonSolid);
    FEAT(sampleRateShading);
    FEAT(imageCubeArray);
    FEAT(multiDrawIndirect);
    FEAT(drawIndirectFirstInstance);
    FEAT(samplerAnisotropy);
    FEAT(textureCompressionBC);
    FEAT(textureCompressionETC2);
    FEAT(textureCompressionASTC_LDR);
    FEAT(shaderStorageImageWriteWithoutFormat);
    FEAT(shaderStorageImageExtendedFormats);
    FEAT(shaderClipDistance);
    FEAT(robustBufferAccess);
    FEAT(fullDrawIndexUint32);
}

static void limits(const VkPhysicalDeviceLimits* l) {
    printf("== Limits\n");
    printf("  maxImageDimension2D                  %u\n", l->maxImageDimension2D);
    printf("  maxStorageBufferRange                %u\n", l->maxStorageBufferRange);
    printf("  maxUniformBufferRange                %u\n", l->maxUniformBufferRange);
    printf("  maxPushConstantsSize                 %u\n", l->maxPushConstantsSize);
    printf("  maxBoundDescriptorSets               %u\n", l->maxBoundDescriptorSets);
    printf("  maxPerStageDescriptorSamplers        %u\n", l->maxPerStageDescriptorSamplers);
    printf("  maxPerStageDescriptorSampledImages   %u\n", l->maxPerStageDescriptorSampledImages);
    printf("  maxPerStageDescriptorStorageBuffers  %u\n", l->maxPerStageDescriptorStorageBuffers);
    printf("  maxVertexInputAttributes             %u\n", l->maxVertexInputAttributes);
    printf("  maxColorAttachments                  %u\n", l->maxColorAttachments);
    printf("  maxComputeWorkGroupInvocations       %u\n", l->maxComputeWorkGroupInvocations);
    printf("  maxSamplerAnisotropy                 %.0f\n", l->maxSamplerAnisotropy);
    printf("  framebufferColorSampleCounts         0x%x\n", l->framebufferColorSampleCounts);
    printf("  minStorageBufferOffsetAlignment      %llu\n", (unsigned long long)l->minStorageBufferOffsetAlignment);
}

// Extensions the Xbox 360 Android ports lean on.
static const char* kWanted[] = {
    "VK_KHR_driver_properties",       "VK_KHR_timeline_semaphore",       "VK_KHR_dynamic_rendering",
    "VK_KHR_push_descriptor",         "VK_EXT_descriptor_indexing",      "VK_KHR_16bit_storage",
    "VK_KHR_8bit_storage",            "VK_KHR_shader_float16_int8",      "VK_KHR_shader_draw_parameters",
    "VK_EXT_extended_dynamic_state",  "VK_EXT_custom_border_color",      "VK_EXT_vertex_attribute_divisor",
    "VK_EXT_fragment_shader_interlock", "VK_EXT_shader_stencil_export",  "VK_EXT_depth_clip_enable",
    "VK_EXT_provoking_vertex",        "VK_KHR_maintenance1",             "VK_KHR_maintenance2",
    "VK_KHR_maintenance3",            "VK_KHR_maintenance4",             "VK_KHR_create_renderpass2",
    "VK_KHR_imageless_framebuffer",   "VK_EXT_memory_budget",            "VK_KHR_pipeline_library",
    "VK_EXT_graphics_pipeline_library", "VK_KHR_swapchain",
};

static void extensions(VkPhysicalDevice dev, int all, int* has_driver_props) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(dev, NULL, &n, NULL);
    VkExtensionProperties* e = calloc(n, sizeof *e);
    vkEnumerateDeviceExtensionProperties(dev, NULL, &n, e);
    printf("== Extensions (%u total)\n", n);
    *has_driver_props = 0;
    for (size_t w = 0; w < sizeof kWanted / sizeof *kWanted; w++) {
        int found = 0;
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, kWanted[w])) found = 1;
        if (found && !strcmp(kWanted[w], "VK_KHR_driver_properties")) *has_driver_props = 1;
        printf("  %-36s %s\n", kWanted[w], found ? "yes" : "NO");
    }
    if (all)
        for (uint32_t i = 0; i < n; i++) printf("  + %s (rev %u)\n", e[i].extensionName, e[i].specVersion);
    free(e);
}

struct Fmt { VkFormat f; const char* name; };

// Formats a Xenos renderer maps guest surfaces/textures to; D24S8 and BC are the usual mobile gaps.
static const struct Fmt kFormats[] = {
    {VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"},
    {VK_FORMAT_D32_SFLOAT, "D32_SFLOAT"},
    {VK_FORMAT_D16_UNORM, "D16_UNORM"},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM"},
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM"},
    {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT"},
    {VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM"},
    {VK_FORMAT_R16G16B16A16_SNORM, "R16G16B16A16_SNORM"},
    {VK_FORMAT_R16G16_UNORM, "R16G16_UNORM"},
    {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT"},
    {VK_FORMAT_R32_SFLOAT, "R32_SFLOAT"},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT"},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, "E5B9G9R9_UFLOAT"},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, "R5G6B5_UNORM"},
    {VK_FORMAT_R4G4B4A4_UNORM_PACK16, "R4G4B4A4_UNORM"},
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 (DXT1)"},
    {VK_FORMAT_BC2_UNORM_BLOCK, "BC2 (DXT3)"},
    {VK_FORMAT_BC3_UNORM_BLOCK, "BC3 (DXT5)"},
    {VK_FORMAT_BC5_UNORM_BLOCK, "BC5 (DXN)"},
};

static void formats(VkPhysicalDevice dev) {
    printf("== Formats (S=sampled L=linear filter C=color target B=blend D=depth target W=storage)\n");
    for (size_t i = 0; i < sizeof kFormats / sizeof *kFormats; i++) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(dev, kFormats[i].f, &p);
        VkFormatFeatureFlags o = p.optimalTilingFeatures;
        printf("  %-22s %c%c%c%c%c%c\n", kFormats[i].name,
               (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? 'S' : '-',
               (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 'L' : '-',
               (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ? 'C' : '-',
               (o & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) ? 'B' : '-',
               (o & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? 'D' : '-',
               (o & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? 'W' : '-');
    }
}

static void memory(VkPhysicalDevice dev) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(dev, &m);
    printf("== Memory\n");
    for (uint32_t i = 0; i < m.memoryHeapCount; i++)
        printf("  heap %u: %llu MB%s\n", i, (unsigned long long)(m.memoryHeaps[i].size >> 20),
               (m.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m.memoryTypes[i].propertyFlags;
        printf("  type %u heap %u:%s%s%s%s%s\n", i, m.memoryTypes[i].heapIndex,
               (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " device" : "",
               (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " visible" : "",
               (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " coherent" : "",
               (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " cached" : "",
               (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) ? " lazy" : "");
    }
}

static void queues(VkPhysicalDevice dev) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, NULL);
    VkQueueFamilyProperties* q = calloc(n, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &n, q);
    printf("== Queue families\n");
    for (uint32_t i = 0; i < n; i++)
        printf("  family %u: %u queue(s)%s%s%s\n", i, q[i].queueCount,
               (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? " graphics" : "",
               (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ? " compute" : "",
               (q[i].queueFlags & VK_QUEUE_TRANSFER_BIT) ? " transfer" : "");
    free(q);
}

// The N64 racer hit a Vulkan error on the V30 at device creation, so check that a device can be made at all,
// with and without the memexport feature.
static void try_device(VkPhysicalDevice dev, const VkPhysicalDeviceFeatures* avail) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice d;
    VkResult r = vkCreateDevice(dev, &dci, NULL, &d);
    printf("== Device creation\n  plain                        %s (%d)\n", r == VK_SUCCESS ? "ok" : "FAILED", r);
    if (r == VK_SUCCESS) vkDestroyDevice(d, NULL);
    if (avail->vertexPipelineStoresAndAtomics) {
        VkPhysicalDeviceFeatures want = {0};
        want.vertexPipelineStoresAndAtomics = VK_TRUE;
        want.fragmentStoresAndAtomics = avail->fragmentStoresAndAtomics;
        dci.pEnabledFeatures = &want;
        r = vkCreateDevice(dev, &dci, NULL, &d);
        printf("  with store/atomics           %s (%d)\n", r == VK_SUCCESS ? "ok" : "FAILED", r);
        if (r == VK_SUCCESS) vkDestroyDevice(d, NULL);
    }
}

static void driver_props(VkInstance inst, VkPhysicalDevice dev) {
    PFN_vkGetPhysicalDeviceProperties2 get2 =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    if (!get2) get2 = (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2KHR");
    if (!get2) return;
    VkPhysicalDeviceDriverPropertiesKHR dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &dp;
    get2(dev, &p2);
    printf("  driver name                  %s\n", dp.driverName);
    printf("  driver info                  %s\n", dp.driverInfo);
}

int main(int argc, char** argv) {
    int all_ext = 0;
    long memtest_mb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--all-ext")) all_ext = 1;
        else if (!strcmp(argv[i], "--memtest") && i + 1 < argc) memtest_mb = atol(argv[++i]);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    system_info();
    guest_memory(memtest_mb);

    printf("== Vulkan\n");
    uint32_t inst_ver = VK_API_VERSION_1_0;
    PFN_vkEnumerateInstanceVersion env =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    if (env) env(&inst_ver);
    print_version("instance (loader)", inst_ver);

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkprobe";
    app.apiVersion = inst_ver >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { printf("  vkCreateInstance FAILED (%d)\n", r); return 1; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    printf("  physical devices             %u\n", n);
    if (!n) return 1;
    VkPhysicalDevice* devs = calloc(n, sizeof *devs);
    vkEnumeratePhysicalDevices(inst, &n, devs);
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkPhysicalDeviceFeatures f;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        vkGetPhysicalDeviceFeatures(devs[i], &f);
        printf("== GPU %u\n", i);
        printf("  name                         %s\n", p.deviceName);
        printf("  vendor                       0x%04x %s\n", p.vendorID, vendor_name(p.vendorID));
        printf("  device id                    0x%08x\n", p.deviceID);
        print_version("api version", p.apiVersion);
        printf("  driver version (raw)         0x%08x\n", p.driverVersion);
        int has_dp = 0;
        features(&f);
        limits(&p.limits);
        extensions(devs[i], all_ext, &has_dp);
        if (has_dp || p.apiVersion >= VK_API_VERSION_1_2) driver_props(inst, devs[i]);
        formats(devs[i]);
        memory(devs[i]);
        queues(devs[i]);
        try_device(devs[i], &f);
    }
    free(devs);
    vkDestroyInstance(inst, NULL);
    return 0;
}
