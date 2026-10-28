// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR for the Vulkan backend (`upscale_vk.h`, `todo/enhancements.md` row
// 18), through the FidelityFX API.
//
// The API is five C entry points in ONE library, `amd_fidelityfx_vk.dll`
// (FidelityFX SDK v1.1.4's prebuilt, signed Vulkan build; AMD ships no other),
// taken with GetProcAddress / dlsym - so nothing here links an import
// library, and a viewer whose directory lacks the library says so and draws
// natively. Only the SDK's HEADERS are compiled in (`-DOMK_FSR_VK`,
// `Makefile`); without them this file is the stub at the bottom.
//
// WHICH FSR: that library's upscaler is FSR 3.1 - AMD's SDK 2.x (the D3D12
// backend's FSR 4) has no Vulkan library. The provider reports its own version,
// and that is what is logged. It runs on any Vulkan 1.1 GPU; the device was
// created by the backend (`pickDevice` enables what the SDK's shaders may use).
#include "upscale_vk.h"
#include "platform/env.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(OMK_FSR_VK)

#ifndef _WIN32
// ffx_api.h marks its entry points __declspec(dllexport); this file only calls them
#define __declspec(x)
#endif
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_api_types.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#ifndef _WIN32
#undef __declspec
#endif

namespace omk::vkup {
namespace {

#ifdef _WIN32
constexpr const char* kLibrary = "amd_fidelityfx_vk.dll";
#else
constexpr const char* kLibrary = "libamd_fidelityfx_vk.so";
#endif

// The provider's messages (its debug checks, a rejected parameter) - to
// stderr, where the Vulkan validation layer's go too.
void fsrMessage(uint32_t type, const wchar_t* msg) {
    std::fprintf(stderr, "fsr %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

uint32_t surfaceFormat(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
        default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// An image as the API describes it. The Vulkan helper of the SDK reads a
// VkImageCreateInfo; this backend knows its images' parameters. The depth is
// handed over as a plain R32_SFLOAT colour image (`vkrender.cpp` `tupDepth_`): the
// SDK's depth-target path views it as D32_SFLOAT and names the depth aspect alone.
FfxApiResource resourceOf(const Image& i, uint32_t usage, uint32_t state) {
    FfxApiResourceDescription d{};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = surfaceFormat(i.format);
    d.width = i.width;
    d.height = i.height;
    d.depth = 1;
    d.mipCount = 1;
    d.flags = FFX_API_RESOURCE_FLAGS_NONE;
    d.usage = usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(i.image), d, state);
}

class Fsr final : public Upscaler {
public:
    ~Fsr() override {
        if (ctx_ && destroy_) destroy_(&ctx_, nullptr);
        closeLibrary(mod_);
    }
    const char* name() const override { return "FSR"; }
    std::string version() const override { return version_; }

    bool make(const Device& dev, int dw, int dh, int quality) {
        dw_ = dw; dh_ = dh; quality_ = quality;
        mod_ = openLibrary(kLibrary);
        if (!mod_) {
            std::printf("upscaler: FSR unavailable on Vulkan - %s is not next to the viewer\n", kLibrary);
            return false;
        }
        create_ = reinterpret_cast<PfnFfxCreateContext>(symbol(mod_, "ffxCreateContext"));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(symbol(mod_, "ffxDestroyContext"));
        query_ = reinterpret_cast<PfnFfxQuery>(symbol(mod_, "ffxQuery"));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(symbol(mod_, "ffxDispatch"));
        if (!create_ || !destroy_ || !query_ || !dispatch_) {
            std::printf("upscaler: FSR unavailable on Vulkan - %s lacks the FidelityFX entry points\n", kLibrary);
            return false;
        }
        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // the colour is this game's: LDR and PERCEPTUAL (gamma-encoded bytes),
        // the depth standard (not inverted) with a finite far plane
        cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        if (omk::envFlag("OMK_FSR_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        cd.maxRenderSize = {static_cast<uint32_t>(dw), static_cast<uint32_t>(dh)};
        cd.maxUpscaleSize = {static_cast<uint32_t>(dw), static_cast<uint32_t>(dh)};
        cd.fpMessage = fsrMessage;
        ffxCreateBackendVKDesc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.vkDevice = dev.device;
        be.vkPhysicalDevice = dev.phys;
        be.vkDeviceProcAddr = dev.getDeviceProcAddr;
        cd.header.pNext = &be.header;
        const ffxReturnCode_t rc = create_(&ctx_, &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !ctx_) {
            std::printf("upscaler: FSR unavailable on Vulkan - ffxCreateContext returned %u\n",
                        static_cast<unsigned>(rc));
            ctx_ = nullptr;
            return false;
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        query_(&ctx_, &pv.header);
        version_ = pv.versionName ? pv.versionName : "?";
        std::printf("upscaler: FSR %s loaded on Vulkan (provider 0x%llx) - an ENHANCEMENT: the frame drawn at "
                    "a lower size and rebuilt over a jitter (%s)\n", version_.c_str(),
                    static_cast<unsigned long long>(pv.versionId), qualityName(quality));
        return true;
    }

    void renderSize(int& rw, int& rh) override {
        uint32_t w = 0, h = 0;
        ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
        q.displayWidth = static_cast<uint32_t>(dw_);
        q.displayHeight = static_cast<uint32_t>(dh_);
        q.qualityMode = static_cast<uint32_t>(quality_);
        q.pOutRenderWidth = &w;
        q.pOutRenderHeight = &h;
        if (query_(&ctx_, &q.header) == FFX_API_RETURN_OK && w && h) {
            rw = static_cast<int>(w); rh = static_cast<int>(h);
            return;
        }
        rw = static_cast<int>(static_cast<float>(dw_) / qualityRatio(quality_));
        rh = static_cast<int>(static_cast<float>(dh_) / qualityRatio(quality_));
    }

    int jitterPhases(int rw, int dw) override {
        int32_t n = 0;
        ffxQueryDescUpscaleGetJitterPhaseCount q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
        q.renderWidth = static_cast<uint32_t>(rw);
        q.displayWidth = static_cast<uint32_t>(dw);
        q.pOutPhaseCount = &n;
        query_(&ctx_, &q.header);
        return n > 0 ? n : 8;
    }

    void jitter(int index, int phases, float& x, float& y) override {
        x = y = 0.0f;
        ffxQueryDescUpscaleGetJitterOffset q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
        q.index = index;
        q.phaseCount = phases;
        q.pOutX = &x;
        q.pOutY = &y;
        query_(&ctx_, &q.header);
    }

    bool dispatch(const Frame& f) override {
        // the states the caller left them in, in the API's words
        const uint32_t in = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = reinterpret_cast<void*>(f.cb);
        d.color = resourceOf(f.colour, FFX_API_RESOURCE_USAGE_READ_ONLY, in);
        d.depth = resourceOf(f.depth, FFX_API_RESOURCE_USAGE_READ_ONLY, in);
        d.motionVectors = resourceOf(f.motion, FFX_API_RESOURCE_USAGE_READ_ONLY, in);
        d.output = resourceOf(f.output, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitterX, f.jitterY};
        // UV units -> render pixels (`upscale_vk.h`)
        d.motionVectorScale = {static_cast<float>(f.rw), static_cast<float>(f.rh)};
        d.renderSize = {static_cast<uint32_t>(f.rw), static_cast<uint32_t>(f.rh)};
        d.upscaleSize = {static_cast<uint32_t>(f.dw), static_cast<uint32_t>(f.dh)};
        d.enableSharpening = false;   // the picture is the game's; no sharpening it never had
        d.sharpness = 0.0f;
        d.frameTimeDelta = f.frameMs;
        d.preExposure = 1.0f;
        d.reset = f.reset;
        d.cameraNear = f.cam.nearP;
        d.cameraFar = f.cam.farP;
        d.cameraFovAngleVertical = f.cam.fovV;
        d.viewSpaceToMetersFactor = 0.0254f;   // the world is in INCHES
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const ffxReturnCode_t rc = dispatch_(&ctx_, &d.header);
        if (rc != FFX_API_RETURN_OK) {
            static int said = 0;
            if (said++ < 3) std::fprintf(stderr, "upscaler: FSR dispatch returned %u\n", static_cast<unsigned>(rc));
            return false;
        }
        return true;
    }

private:
    void* mod_ = nullptr;
    PfnFfxCreateContext create_ = nullptr;
    PfnFfxDestroyContext destroy_ = nullptr;
    PfnFfxQuery query_ = nullptr;
    PfnFfxDispatch dispatch_ = nullptr;
    ffxContext ctx_ = nullptr;
    std::string version_;
    int dw_ = 0, dh_ = 0, quality_ = 1;
};

}  // namespace

std::unique_ptr<Upscaler> makeFsr(const Device& dev, int dw, int dh, int quality) {
    auto f = std::make_unique<Fsr>();
    if (!f->make(dev, dw, dh, quality)) return nullptr;
    return f;
}

}  // namespace omk::vkup

#else  // !OMK_FSR_VK

namespace omk::vkup {
std::unique_ptr<Upscaler> makeFsr(const Device&, int, int, int) {
    std::printf("upscaler: FSR unavailable on Vulkan - this viewer was built without the FidelityFX Vulkan SDK\n");
    return nullptr;
}
}  // namespace omk::vkup

#endif
