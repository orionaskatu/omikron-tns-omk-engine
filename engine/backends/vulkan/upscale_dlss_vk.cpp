// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS for the Vulkan backend (`upscale_vk.h`, `todo/enhancements.md`
// row 18), through Streamline.
//
// **Written to the SDK's documentation and NOT RUN on NVIDIA hardware** - the
// machine it was written on has an AMD GPU, where the device lacks the
// extensions DLSS asks for and this falls back (`vkrender.cpp`: to FSR 3.1,
// else the native frame). What HAS run there: the interposer found, its
// signature verified, `slInit` for Vulkan, `slGetFeatureRequirements`, and the
// clean refusal. What has not: `slSetVulkanInfo`, the support query,
// `slDLSSGetOptimalSettings` and the whole per-frame half (constants, tags,
// `slEvaluateFeature`) - and the matrix order and the jitter's sign, which are
// the Direct3D 12 glue's (`backends/d3d12/upscale_dlss.cpp`), the same parts a
// first NVIDIA run must look at.
//
// The integration is Streamline's MANUAL HOOKING (`ProgrammingGuideManualHooking.md`
// 4.2, 5.2, 6.2): the interposer is loaded at run time, never linked, and the
// instance and device are the backend's own, made with the extensions,
// features and queues `slGetFeatureRequirements` named (so `dlssPrepare` runs
// BEFORE `vkCreateInstance`), then handed over with `slSetVulkanInfo`; no
// Vulkan call goes through a proxy. Its one cost: the guide asks a manual
// integration to route PRESENT through Streamline so its common plugin runs once
// a frame, which this does not do - DLSS super resolution evaluates on the
// frame's command buffer and is documented to work so; frame generation, which
// would need it, is not used.
//
// The interposer is loaded only from the viewer's own directory and only after
// `sl::security::verifyEmbeddedSignature` accepts it. Windows only: no
// Streamline library for Linux is part of the SDK this was written against
// (v2.14.16); NVIDIA's standalone NGX DLSS SDK is that platform's route.
#include "upscale_vk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(OMK_DLSS_VK) && defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#include <sl_helpers_vk.h>
// `sl_security.h` DEFINES its functions (a header-only check): exactly one translation unit of the viewer
// may include it. The Direct3D 12 glue's does when it is built (`OMK_DLSS`); a build without it gives this
// one the header (`-DOMK_SL_SECURITY`, the `Makefile`)
#if defined(OMK_SL_SECURITY)
#  include <sl_security.h>
#else
namespace sl::security {
bool verifyEmbeddedSignature(const wchar_t* pathToFile);
}
#endif

namespace omk::vkup {
namespace {

using omk::upscale::halton;
using omk::upscale::invert;
using omk::upscale::mul;

void slLog(sl::LogType type, const char* msg) {
    // Streamline's warnings and errors; its info chatter is not ours to print
    if (type == sl::LogType::eError || type == sl::LogType::eWarn)
        std::fprintf(stderr, "streamline %s: %s", type == sl::LogType::eError ? "ERROR" : "warning", msg);
}

const char* resultName(sl::Result r) { return sl::getResultAsStr(r); }

// column-major (this engine's) -> Streamline's row-major row-vector matrix:
// the same sixteen floats, read the other way (`upscale_vk.h`)
sl::float4x4 slMatrix(const float m[16]) {
    sl::float4x4 o;
    for (int r = 0; r < 4; ++r) o.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    return o;
}

template <class F> F proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

// ONE Streamline per process: initialised by `dlssPrepare` (before the
// instance), borrowed by every `Dlss`, shut down by `dlssShutdown`
struct Session {
    HMODULE mod = nullptr;
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* supported = nullptr;
    PFun_slSetVulkanInfo* setInfo = nullptr;
    PFun_slGetFeatureFunction* featureFn = nullptr;
    PFun_slGetNewFrameToken* newToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTags = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slGetFeatureVersion* getVersion = nullptr;
    bool inited = false;
    ~Session() {
        if (inited && shutdown) shutdown();
        if (mod) FreeLibrary(mod);
    }
};
Session* g_session = nullptr;

class Dlss final : public Upscaler {
public:
    ~Dlss() override {
        if (g_session && allocated_ && g_session->freeResources) g_session->freeResources(sl::kFeatureDLSS, viewport_);
    }
    const char* name() const override { return "DLSS"; }
    std::string version() const override { return version_; }

    bool make(const Device& dev, int dw, int dh, int quality) {
        dw_ = dw; dh_ = dh; quality_ = quality;
        Session& s = *g_session;
        if (!s.setInfo) {
            std::printf("upscaler: DLSS unavailable on Vulkan - Streamline has no slSetVulkanInfo\n");
            return false;
        }
        if (dev.vendorId != 0x10DE) {
            std::printf("upscaler: DLSS unsupported on this GPU (not an NVIDIA device)\n");
            return false;
        }
        sl::VulkanInfo vi{};
        vi.device = dev.device;
        vi.instance = dev.instance;
        vi.physicalDevice = dev.phys;
        vi.computeQueueIndex = dev.extraQueueStart;
        vi.computeQueueFamily = dev.family;
        vi.graphicsQueueIndex = dev.extraQueueStart;
        vi.graphicsQueueFamily = dev.family;
        sl::Result r = s.setInfo(vi);
        if (r != sl::Result::eOk) {
            std::printf("upscaler: DLSS unavailable on Vulkan - slSetVulkanInfo: %s\n", resultName(r));
            return false;
        }
        sl::AdapterInfo info{};
        info.vkPhysicalDevice = dev.phys;
        r = s.supported(sl::kFeatureDLSS, info);
        if (r != sl::Result::eOk) {
            std::printf("upscaler: DLSS unsupported on this GPU (Streamline: %s)\n", resultName(r));
            return false;
        }
        void* fn = nullptr;
        if (s.featureFn(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) == sl::Result::eOk)
            optimal_ = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
        fn = nullptr;
        if (s.featureFn(sl::kFeatureDLSS, "slDLSSSetOptions", fn) == sl::Result::eOk)
            setOptions_ = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
        if (!optimal_ || !setOptions_) {
            std::printf("upscaler: DLSS unavailable on Vulkan - the DLSS plugin's functions are missing\n");
            return false;
        }
        options_.mode = quality <= 0 ? sl::DLSSMode::eDLAA : quality == 1 ? sl::DLSSMode::eMaxQuality
                      : quality == 2 ? sl::DLSSMode::eBalanced : quality == 3 ? sl::DLSSMode::eMaxPerformance
                                     : sl::DLSSMode::eUltraPerformance;
        options_.outputWidth = static_cast<uint32_t>(dw);
        options_.outputHeight = static_cast<uint32_t>(dh);
        options_.colorBuffersHDR = sl::Boolean::eFalse;   // LDR, perceptual (`upscale_vk.h`)
        options_.useAutoExposure = sl::Boolean::eFalse;
        sl::FeatureVersion fv{};
        if (s.getVersion && s.getVersion(sl::kFeatureDLSS, fv) == sl::Result::eOk) {
            char b[96];
            std::snprintf(b, sizeof b, "SL %u.%u.%u, NGX %u.%u.%u", fv.versionSL.major, fv.versionSL.minor,
                          fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);
            version_ = b;
        } else {
            version_ = "?";
        }
        std::printf("upscaler: DLSS loaded on Vulkan (%s) - an ENHANCEMENT (%s)\n", version_.c_str(),
                    qualityName(quality));
        return true;
    }

    void renderSize(int& rw, int& rh) override {
        sl::DLSSOptimalSettings s{};
        if (optimal_ && optimal_(options_, s) == sl::Result::eOk && s.optimalRenderWidth && s.optimalRenderHeight) {
            rw = static_cast<int>(s.optimalRenderWidth);
            rh = static_cast<int>(s.optimalRenderHeight);
            return;
        }
        rw = static_cast<int>(static_cast<float>(dw_) / qualityRatio(quality_));
        rh = static_cast<int>(static_cast<float>(dh_) / qualityRatio(quality_));
    }

    // FSR's rule for the length (8 x the area ratio), Halton(2,3) for the offsets
    int jitterPhases(int rw, int dw) override {
        const float r = static_cast<float>(dw) / static_cast<float>(rw > 0 ? rw : 1);
        return std::max(8, static_cast<int>(std::ceil(8.0f * r * r)));
    }
    void jitter(int index, int phases, float& x, float& y) override {
        const int i = (phases > 0 ? index % phases : index) + 1;
        x = halton(i, 2) - 0.5f;
        y = halton(i, 3) - 0.5f;
    }

    bool dispatch(const Frame& f) override {
        Session& s = *g_session;
        sl::FrameToken* token = nullptr;
        const uint32_t index = frame_++;
        if (s.newToken(token, &index) != sl::Result::eOk || !token) return false;
        sl::Constants c{};
        float clipToView[16], clipToPrev[16], prevToClip[16];
        invert(f.cam.viewToClip, clipToView);
        float inv[16];
        if (!invert(f.cam.worldToClip, inv)) return false;
        mul(f.cam.prevWorldToClip, inv, clipToPrev);   // current clip -> world -> previous clip
        invert(clipToPrev, prevToClip);
        c.cameraViewToClip = slMatrix(f.cam.viewToClip);
        c.clipToCameraView = slMatrix(clipToView);
        c.clipToPrevClip = slMatrix(clipToPrev);
        c.prevClipToClip = slMatrix(prevToClip);
        c.jitterOffset = sl::float2(f.jitterX, f.jitterY);
        c.mvecScale = sl::float2(1.0f, 1.0f);   // UV units already (`upscale_vk.h`)
        c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        c.cameraPos = sl::float3(f.cam.pos[0], f.cam.pos[1], f.cam.pos[2]);
        c.cameraUp = sl::float3(f.cam.up[0], f.cam.up[1], f.cam.up[2]);
        c.cameraRight = sl::float3(f.cam.right[0], f.cam.right[1], f.cam.right[2]);
        c.cameraFwd = sl::float3(f.cam.fwd[0], f.cam.fwd[1], f.cam.fwd[2]);
        c.cameraNear = f.cam.nearP;
        c.cameraFar = f.cam.farP;
        c.cameraFOV = f.cam.fovV;
        c.cameraAspectRatio = f.cam.aspect;
        c.depthInverted = sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        if (s.setConstants(c, *token, viewport_) != sl::Result::eOk) return false;
        if (setOptions_(viewport_, options_) != sl::Result::eOk) return false;
        // Vulkan: Streamline cannot ask a VkImage for its description, so the host gives it
        // (`ProgrammingGuideManualHooking.md` 6.2): the view, the layout it will be in, the extent,
        // format and usage
        const auto tag = [](const Image& i) {
            sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(i.image), nullptr,
                           reinterpret_cast<void*>(i.view), static_cast<uint32_t>(i.layout));
            r.width = i.width;
            r.height = i.height;
            r.nativeFormat = static_cast<uint32_t>(i.format);
            r.mipLevels = 1;
            r.arrayLayers = 1;
            r.flags = 0;
            r.usage = static_cast<uint32_t>(i.usage);
            return r;
        };
        sl::Resource colour = tag(f.colour), depth = tag(f.depth), motion = tag(f.motion), output = tag(f.output);
        const sl::Extent render{0, 0, static_cast<uint32_t>(f.rw), static_cast<uint32_t>(f.rh)};
        const sl::Extent display{0, 0, static_cast<uint32_t>(f.dw), static_cast<uint32_t>(f.dh)};
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&colour, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &render),
            sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &display),
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &render)};
        auto* cmd = reinterpret_cast<sl::CommandBuffer*>(f.cb);
        if (s.setTags(*token, viewport_, tags, 4, cmd) != sl::Result::eOk) return false;
        const sl::BaseStructure* inputs[] = {&viewport_};
        const sl::Result r = s.evaluate(sl::kFeatureDLSS, *token, inputs, 1, cmd);
        allocated_ = true;
        if (r != sl::Result::eOk) {
            static int said = 0;
            if (said++ < 3) std::fprintf(stderr, "upscaler: slEvaluateFeature(DLSS): %s\n", resultName(r));
            return false;
        }
        return true;
    }

private:
    PFun_slDLSSGetOptimalSettings* optimal_ = nullptr;
    PFun_slDLSSSetOptions* setOptions_ = nullptr;
    sl::DLSSOptions options_{};
    sl::ViewportHandle viewport_{0u};
    bool allocated_ = false;
    uint32_t frame_ = 0;
    std::string version_;
    int dw_ = 0, dh_ = 0, quality_ = 1;
};

}  // namespace

bool dlssPrepare(DlssNeeds& needs) {
    if (g_session) return true;   // (already: the instance asked, the device asks again)
    const std::string dir = moduleDir();
    const std::string dll = dir + "sl.interposer.dll";
    if (GetFileAttributesA(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("upscaler: DLSS unavailable on Vulkan - sl.interposer.dll (Streamline) is not next to the viewer\n");
        return false;
    }
    int wl = MultiByteToWideChar(CP_ACP, 0, dll.c_str(), -1, nullptr, 0);
    std::wstring wdll(static_cast<std::size_t>(wl > 0 ? wl : 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, dll.c_str(), -1, wdll.data(), wl);
    if (!sl::security::verifyEmbeddedSignature(wdll.c_str())) {
        std::printf("upscaler: DLSS unavailable on Vulkan - sl.interposer.dll is not signed by NVIDIA, not loaded\n");
        return false;
    }
    auto s = std::make_unique<Session>();
    s->mod = LoadLibraryExW(wdll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!s->mod) {
        std::printf("upscaler: DLSS unavailable on Vulkan - sl.interposer.dll did not load\n");
        return false;
    }
    s->init = proc<PFun_slInit*>(s->mod, "slInit");
    s->shutdown = proc<PFun_slShutdown*>(s->mod, "slShutdown");
    s->supported = proc<PFun_slIsFeatureSupported*>(s->mod, "slIsFeatureSupported");
    s->setInfo = proc<PFun_slSetVulkanInfo*>(s->mod, "slSetVulkanInfo");
    s->featureFn = proc<PFun_slGetFeatureFunction*>(s->mod, "slGetFeatureFunction");
    s->newToken = proc<PFun_slGetNewFrameToken*>(s->mod, "slGetNewFrameToken");
    s->setConstants = proc<PFun_slSetConstants*>(s->mod, "slSetConstants");
    s->setTags = proc<PFun_slSetTagForFrame*>(s->mod, "slSetTagForFrame");
    s->evaluate = proc<PFun_slEvaluateFeature*>(s->mod, "slEvaluateFeature");
    s->freeResources = proc<PFun_slFreeResources*>(s->mod, "slFreeResources");
    s->getVersion = proc<PFun_slGetFeatureVersion*>(s->mod, "slGetFeatureVersion");
    auto getReqs = proc<PFun_slGetFeatureRequirements*>(s->mod, "slGetFeatureRequirements");
    if (!s->init || !s->shutdown || !s->supported || !s->setInfo || !s->featureFn || !s->newToken ||
        !s->setConstants || !s->setTags || !s->evaluate || !getReqs) {
        std::printf("upscaler: DLSS unavailable on Vulkan - sl.interposer.dll lacks the Streamline entry points\n");
        return false;
    }
    int pl = MultiByteToWideChar(CP_ACP, 0, dir.c_str(), -1, nullptr, 0);
    std::wstring wdir(static_cast<std::size_t>(pl > 0 ? pl : 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, dir.c_str(), -1, wdir.data(), pl);
    const wchar_t* paths[] = {wdir.c_str()};
    const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = slLog;
    // MANUAL hooking, and no over-the-air plugin downloads: what runs is what the build copied next to the viewer
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.renderAPI = sl::RenderAPI::eVulkan;   // (so the requirements are Vulkan's)
    const sl::Result r = s->init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        std::printf("upscaler: DLSS unavailable on Vulkan - slInit: %s\n", resultName(r));
        return false;
    }
    s->inited = true;
    sl::FeatureRequirements reqs{};
    const sl::Result rr = getReqs(sl::kFeatureDLSS, reqs);
    if (rr != sl::Result::eOk) {
        std::printf("upscaler: DLSS unavailable on Vulkan - slGetFeatureRequirements: %s\n", resultName(rr));
        return false;
    }
    for (uint32_t i = 0; i < reqs.vkNumInstanceExtensions; ++i) needs.instanceExtensions.push_back(reqs.vkInstanceExtensions[i]);
    for (uint32_t i = 0; i < reqs.vkNumDeviceExtensions; ++i) needs.deviceExtensions.push_back(reqs.vkDeviceExtensions[i]);
    if (reqs.vkNumFeatures12) {
        needs.f12 = sl::getVkPhysicalDeviceVulkan12Features(reqs.vkNumFeatures12, reqs.vkFeatures12);
        needs.uses12 = true;
    }
    if (reqs.vkNumFeatures13) {
        needs.f13 = sl::getVkPhysicalDeviceVulkan13Features(reqs.vkNumFeatures13, reqs.vkFeatures13);
        needs.uses13 = true;
    }
    needs.computeQueues = reqs.vkNumComputeQueuesRequired;
    needs.graphicsQueues = reqs.vkNumGraphicsQueuesRequired;
    std::printf("upscaler: DLSS on Vulkan asks for %zu instance and %zu device extensions, %u + %u queues\n",
                needs.instanceExtensions.size(), needs.deviceExtensions.size(), needs.computeQueues,
                needs.graphicsQueues);
    g_session = s.release();
    return true;
}

void dlssShutdown() {
    delete g_session;
    g_session = nullptr;
}

std::unique_ptr<Upscaler> makeDlss(const Device& dev, int dw, int dh, int quality) {
    if (!g_session) {
        std::printf("upscaler: DLSS unavailable on Vulkan - Streamline was not prepared before the device\n");
        return nullptr;
    }
    auto d = std::make_unique<Dlss>();
    if (!d->make(dev, dw, dh, quality)) return nullptr;
    return d;
}

}  // namespace omk::vkup

#else  // !OMK_DLSS_VK, or not Windows

namespace omk::vkup {
bool dlssPrepare(DlssNeeds&) {
#if defined(_WIN32)
    std::printf("upscaler: DLSS unavailable on Vulkan - this viewer was built without the Streamline SDK\n");
#else
    std::printf("upscaler: DLSS unavailable on Vulkan - Streamline is Windows-only here (no Linux library in its SDK)\n");
#endif
    return false;
}
void dlssShutdown() {}
std::unique_ptr<Upscaler> makeDlss(const Device&, int, int, int) { return nullptr; }
}  // namespace omk::vkup

#endif
