// SPDX-License-Identifier: GPL-3.0-or-later
// THE UPSCALERS ON VULKAN - `todo/enhancements.md` row 18, the Vulkan backend's.
//
// The Direct3D 12 backend's temporal upscaling (`backends/d3d12/upscale.h`)
// on this one: the frame drawn at a RENDER size below the display size over a
// sub-pixel jitter that changes every frame, and a temporal upscaler rebuilds
// the display-size picture from the colour, the depth and the motion vectors
// the scene pass writes (the same `scene.vert`/`scene.frag` -DOMK_MOTION
// builds frame generation uses). Two SDKs stand behind this one interface:
//
//   * AMD FSR 3.1 (`upscale_fsr_vk.cpp`), through the FidelityFX API's Vulkan
//     library (`amd_fidelityfx_vk.dll` of FidelityFX SDK v1.1.4, which AMD
//     ships signed; `libamd_fidelityfx_vk.so` where one has been built from the
//     SDK's source). FSR 4 is NOT here: AMD's SDK 2.x ships DX12 libraries only;
//   * NVIDIA DLSS (`upscale_dlss_vk.cpp`), through Streamline's interposer on
//     Windows. **Written to the SDK documentation, NOT run on NVIDIA hardware.**
//
// Both libraries are loaded at RUN time (LoadLibrary / dlopen) from the
// viewer's own directory, so a build or a machine without them runs, says
// why, and draws the native frame. Only the SDKs' HEADERS are compiled in
// (`-DOMK_FSR_VK`, `-DOMK_DLSS_VK`, the `Makefile`).
//
// CONVENTIONS are the Direct3D 12 glue's (`backends/d3d12/upscale.h`), and the
// shaders are the same GLSL: the MOTION VECTOR is PREVIOUS minus CURRENT
// position in texture-UV units (y down), FSR's scale the render size; the
// JITTER is in render PIXELS in [-0.5, 0.5] and the backend adds `2 x / vw`,
// `2 y / vh` to clip space (Vulkan's clip space is Y-down, as the pixels are);
// matrices are this engine's, COLUMN-major, the depth NOT inverted and the far
// plane finite.
#pragma once

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <vector>

#include "o3de/upscaleshared.h"

namespace omk::vkup {

using omk::upscale::Kind;
using omk::upscale::kOff;
using omk::upscale::kFsr;
using omk::upscale::kDlss;
using omk::upscale::qualityRatio;
using omk::upscale::qualityName;
using omk::upscale::Camera;

// The device the SDKs are given - the backend's own native one.
struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    uint32_t vendorId = 0;   // VkPhysicalDeviceProperties::vendorID (0x10DE NVIDIA, 0x1002 AMD)
    uint32_t extraQueueStart = 0;   // the first queue of the family past the backend's own (Streamline's)
};

// One image the SDK reads or writes: its handle, view and creation parameters
// (the Vulkan API cannot be asked for them), in the layout named.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;   // where the caller left it (and wants it back)
};

// One dispatch. The inputs are in SHADER_READ_ONLY_OPTIMAL, the output in GENERAL.
struct Frame {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    Image colour;    // the scene's: display-or-larger image, the TOP-LEFT render size live (R8G8B8A8, perceptual, LDR)
    Image depth;     // render size, D32S8, not inverted (the depth aspect)
    Image motion;    // render size, R16G16_SFLOAT (the convention above)
    Image output;    // display size, R8G8B8A8, storage
    int rw = 0, rh = 0, dw = 0, dh = 0;
    float jitterX = 0.0f, jitterY = 0.0f;   // render pixels
    bool reset = false;                      // the camera cut: drop the history
    float frameMs = 33.333f;
    Camera cam;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual const char* name() const = 0;       // "FSR", "DLSS"
    virtual std::string version() const = 0;    // what the provider reported
    // the render size for the display size it was made for
    virtual void renderSize(int& rw, int& rh) = 0;
    // the jitter sequence: its length for this ratio, and its `index`th offset
    virtual int jitterPhases(int rw, int dw) = 0;
    virtual void jitter(int index, int phases, float& x, float& y) = 0;
    // record the upscale into `f.cb`; true when it did
    virtual bool dispatch(const Frame& f) = 0;
};

// Each -> null, after one log line saying why, when its SDK was not built in,
// its library is not next to the viewer, or this device cannot run it - and the
// caller then draws natively (or tries the other).
std::unique_ptr<Upscaler> makeFsr(const Device& dev, int dw, int dh, int quality);
std::unique_ptr<Upscaler> makeDlss(const Device& dev, int dw, int dh, int quality);

// DLSS asks for instance and device extensions, features and queues BEFORE the
// instance and the device are made (Streamline's `slGetFeatureRequirements`):
// `dlssPrepare` loads Streamline, initialises it for Vulkan and answers what
// to add - or false (a line says why: no SDK built in, not Windows, no
// interposer next to the viewer, not signed). It is process-wide, undone by
// `dlssShutdown` (after the last Upscaler of it is gone, before the device).
struct DlssNeeds {
    std::vector<std::string> instanceExtensions, deviceExtensions;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    bool uses12 = false, uses13 = false;
    uint32_t computeQueues = 0, graphicsQueues = 0;   // extra queues Streamline's plugins want
};
bool dlssPrepare(DlssNeeds& needs);
void dlssShutdown();

// The viewer's own directory, where the `Makefile` puts the SDKs' libraries - and
// the ONLY place they are loaded from (never the search path).
std::string moduleDir();

// the libraries, by name, from there. `open` -> a handle (null when absent),
// `symbol` -> a function, `close` frees it.
void* openLibrary(const std::string& file);
void* symbol(void* lib, const char* name);
void closeLibrary(void* lib);

}  // namespace omk::vkup
