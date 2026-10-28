// SPDX-License-Identifier: GPL-3.0-or-later
// A smoke test of libamd_fidelityfx_vk.so that needs no game and no window:
// load it, make a Vulkan device (any - CI has the software one, lavapipe), make
// an FSR 3.1 upscaler context on it (which builds every pass's pipeline from the
// generated shader headers, so a wrong SPIR-V or binding table fails here),
// ask the library what render size "quality" gives for 1920x1080, and destroy it.
//
//   test_context <folder with libamd_fidelityfx_vk.so>
#define __declspec(x)   // ffx_api.h's export marking
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_types.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#undef __declspec

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void message(uint32_t type, const wchar_t* msg) {
    std::printf("  [ffx %u] %ls\n", type, msg);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf("usage: test_context <folder>\n"); return 2; }
    const std::string path = std::string(argv[1]) + "/libamd_fidelityfx_vk.so";
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("FAIL dlopen %s: %s\n", path.c_str(), dlerror()); return 1; }
    auto create = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*)>(dlsym(lib, "ffxCreateContext"));
    auto destroy = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*)>(dlsym(lib, "ffxDestroyContext"));
    auto query = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*)>(dlsym(lib, "ffxQuery"));
    if (!create || !destroy || !query) { std::printf("FAIL: entry points missing\n"); return 1; }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { std::printf("FAIL: no Vulkan instance\n"); return 1; }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (!n) { std::printf("FAIL: no Vulkan device\n"); return 1; }
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(inst, &n, phys.data());
    VkPhysicalDevice pd = phys[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("device: %s\n", props.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    uint32_t family = 0;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(pd, &feats);   // what the device has, all of it
    // the extensions the viewer enables for the upscaler (vkrender.cpp), when the device has them
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> have(en);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, have.data());
    std::vector<const char*> exts;
    for (const char* want : {"VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation", "VK_EXT_subgroup_size_control"})
        for (const auto& e : have)
            if (std::string(e.extensionName) == want) { exts.push_back(want); break; }
    for (const char* e : exts) std::printf("extension: %s\n", e);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) { std::printf("FAIL: vkCreateDevice\n"); return 1; }

    ffxCreateContextDescUpscale cd{};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    cd.maxRenderSize = {1280, 720};
    cd.maxUpscaleSize = {1920, 1080};
    cd.fpMessage = message;
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.vkDevice = dev;
    be.vkPhysicalDevice = pd;
    be.vkDeviceProcAddr = vkGetDeviceProcAddr;
    cd.header.pNext = &be.header;
    ffxContext ctx = nullptr;
    const ffxReturnCode_t rc = create(&ctx, &cd.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx) { std::printf("FAIL: ffxCreateContext returned %u\n", (unsigned)rc); return 1; }

    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    query(&ctx, &pv.header);
    std::printf("provider: %s\n", pv.versionName ? pv.versionName : "?");

    uint32_t w = 0, h = 0;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    q.displayWidth = 1920;
    q.displayHeight = 1080;
    q.qualityMode = FFX_UPSCALE_QUALITY_MODE_QUALITY;
    q.pOutRenderWidth = &w;
    q.pOutRenderHeight = &h;
    const ffxReturnCode_t qr = query(&ctx, &q.header);
    std::printf("quality 1920x1080 -> %ux%u (rc %u)\n", w, h, (unsigned)qr);
    const bool sized = qr == FFX_API_RETURN_OK && w == 1280 && h == 720;

    destroy(&ctx, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    std::printf(sized ? "OK\n" : "FAIL: the render size is not 1280x720\n");
    return sized ? 0 : 1;
}
