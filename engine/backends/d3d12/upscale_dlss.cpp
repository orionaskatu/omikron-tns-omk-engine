// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS for the Direct3D 12 backend (`upscale.h`, `todo/enhancements.md`
// row 18), through Streamline.
//
// **Written to the SDK's documentation and NOT RUN on NVIDIA hardware** - the
// machine it was written on has an AMD GPU, where Streamline answers that the
// adapter is not supported and this falls back (`d3d12render.cpp`: to FSR,
// else the native frame). What HAS run there: the interposer found, its
// signature verified, `slInit`, `slSetD3DDevice`, the two support queries and
// the clean shutdown. What has not: `slDLSSGetOptimalSettings` and the whole
// per-frame half (constants, tags, `slEvaluateFeature`), the matrix order and
// the jitter's sign - the parts a first NVIDIA run must look at.
//
// The integration is Streamline's MANUAL HOOKING (`ProgrammingGuideManualHooking.md`):
// the interposer is loaded at run time, never linked - mingw cannot link its
// MSVC import library, and the SDK documents the dynamic path - and the
// device is the backend's own native one, handed over with `slSetD3DDevice`,
// so no D3D12 or DXGI call goes through a proxy. Its one cost: the guide asks
// a manual integration to route PRESENT through Streamline (`slUpgradeInterface`
// on the swapchain) so its common plugin runs once a frame, which this does
// not do - DLSS super resolution evaluates on the frame's command list and is
// documented to work so; frame generation, which would need it, is not used.
//
// The interposer is loaded only from the viewer's own directory and only after
// `sl::security::verifyEmbeddedSignature` accepts it (the SDK's own check,
// which loads WinTrust itself), as the programming guide's "secure load" asks.
//
// DLSS 5's "3D-Guided Neural Rendering", `kFeatureDLSS_NR` (1004), is in the
// SDK's feature list with no public header or plugin: it is QUERIED and the
// answer logged, nothing more.
#include "upscale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(OMK_DLSS)

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_helpers.h>
#include <sl_security.h>

namespace omk::d3dup {
namespace {

void slLog(sl::LogType type, const char* msg) {
    // Streamline's warnings and errors; its info chatter is not ours to print
    if (type == sl::LogType::eError || type == sl::LogType::eWarn)
        std::fprintf(stderr, "streamline %s: %s", type == sl::LogType::eError ? "ERROR" : "warning", msg);
}

const char* resultName(sl::Result r) { return sl::getResultAsStr(r); }

using omk::upscale::halton;
using omk::upscale::invert;
using omk::upscale::mul;

// column-major (this engine's) -> Streamline's row-major row-vector matrix:
// the same sixteen floats, read the other way (`upscale.h`)
sl::float4x4 slMatrix(const float m[16]) {
    sl::float4x4 o;
    for (int r = 0; r < 4; ++r) o.row[r] = sl::float4(m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    return o;
}

template <class F> F proc(HMODULE m, const char* name) {
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

class Dlss final : public Upscaler {
public:
    ~Dlss() override {
        if (inited_) {
            if (allocated_ && freeResources_) freeResources_(sl::kFeatureDLSS, viewport_);
            if (shutdown_) shutdown_();
        }
        if (mod_) FreeLibrary(mod_);
    }
    const char* name() const override { return "DLSS"; }
    std::string version() const override { return version_; }

    bool make(ID3D12Device* dev, IDXGIAdapter1* adapter, int dw, int dh, int quality) {
        dw_ = dw; dh_ = dh; quality_ = quality;
        const std::wstring dir = moduleDir();
        const std::wstring dll = dir + L"sl.interposer.dll";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::printf("upscaler: DLSS unavailable - sl.interposer.dll (Streamline) is not next to the viewer\n");
            return false;
        }
        if (!sl::security::verifyEmbeddedSignature(dll.c_str())) {
            std::printf("upscaler: DLSS unavailable - sl.interposer.dll is not signed by NVIDIA, not loaded\n");
            return false;
        }
        mod_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod_) {
            std::printf("upscaler: DLSS unavailable - sl.interposer.dll did not load\n");
            return false;
        }
        init_ = proc<PFun_slInit*>(mod_, "slInit");
        shutdown_ = proc<PFun_slShutdown*>(mod_, "slShutdown");
        supported_ = proc<PFun_slIsFeatureSupported*>(mod_, "slIsFeatureSupported");
        setDevice_ = proc<PFun_slSetD3DDevice*>(mod_, "slSetD3DDevice");
        featureFn_ = proc<PFun_slGetFeatureFunction*>(mod_, "slGetFeatureFunction");
        newToken_ = proc<PFun_slGetNewFrameToken*>(mod_, "slGetNewFrameToken");
        setConstants_ = proc<PFun_slSetConstants*>(mod_, "slSetConstants");
        setTags_ = proc<PFun_slSetTagForFrame*>(mod_, "slSetTagForFrame");
        evaluate_ = proc<PFun_slEvaluateFeature*>(mod_, "slEvaluateFeature");
        freeResources_ = proc<PFun_slFreeResources*>(mod_, "slFreeResources");
        getVersion_ = proc<PFun_slGetFeatureVersion*>(mod_, "slGetFeatureVersion");
        if (!init_ || !shutdown_ || !supported_ || !setDevice_ || !featureFn_ || !newToken_ ||
            !setConstants_ || !setTags_ || !evaluate_) {
            std::printf("upscaler: DLSS unavailable - sl.interposer.dll lacks the Streamline entry points\n");
            return false;
        }
        const wchar_t* paths[] = {dir.c_str()};
        const sl::Feature features[] = {sl::kFeatureDLSS};
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eDefault;
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        pref.logMessageCallback = slLog;
        // MANUAL hooking (the header), and no over-the-air plugin downloads:
        // what runs is what the build copied next to the viewer
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 1;
        pref.engine = sl::EngineType::eCustom;
        pref.renderAPI = sl::RenderAPI::eD3D12;
        sl::Result r = init_(pref, sl::kSDKVersion);
        if (r != sl::Result::eOk) {
            std::printf("upscaler: DLSS unavailable - slInit: %s\n", resultName(r));
            return false;
        }
        inited_ = true;
        r = setDevice_(dev);
        if (r != sl::Result::eOk) {
            std::printf("upscaler: DLSS unavailable - slSetD3DDevice: %s\n", resultName(r));
            return false;
        }
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<uint8_t*>(&ad.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(LUID);
        // DLSS 5's neural rendering: asked, and the answer said - nothing more
        const sl::Result nr = supported_(sl::kFeatureDLSS_NR, info);
        std::printf("upscaler: DLSS NR (kFeatureDLSS_NR, feature 1004) on this adapter: %s\n",
                    nr == sl::Result::eOk ? "supported (not used: no public interface)" : resultName(nr));
        r = supported_(sl::kFeatureDLSS, info);
        if (r != sl::Result::eOk) {
            std::printf("upscaler: DLSS unsupported on this GPU (Streamline: %s)\n", resultName(r));
            return false;
        }
        void* fn = nullptr;
        if (featureFn_(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", fn) == sl::Result::eOk)
            optimal_ = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(fn);
        fn = nullptr;
        if (featureFn_(sl::kFeatureDLSS, "slDLSSSetOptions", fn) == sl::Result::eOk)
            setOptions_ = reinterpret_cast<PFun_slDLSSSetOptions*>(fn);
        if (!optimal_ || !setOptions_) {
            std::printf("upscaler: DLSS unavailable - the DLSS plugin's functions are missing\n");
            return false;
        }
        options_.mode = quality <= 0 ? sl::DLSSMode::eDLAA : quality == 1 ? sl::DLSSMode::eMaxQuality
                      : quality == 2 ? sl::DLSSMode::eBalanced : quality == 3 ? sl::DLSSMode::eMaxPerformance
                                     : sl::DLSSMode::eUltraPerformance;
        options_.outputWidth = static_cast<uint32_t>(dw);
        options_.outputHeight = static_cast<uint32_t>(dh);
        options_.colorBuffersHDR = sl::Boolean::eFalse;   // LDR, perceptual (`upscale.h`)
        options_.useAutoExposure = sl::Boolean::eFalse;
        sl::FeatureVersion fv{};
        if (getVersion_ && getVersion_(sl::kFeatureDLSS, fv) == sl::Result::eOk) {
            char b[96];
            std::snprintf(b, sizeof b, "SL %u.%u.%u, NGX %u.%u.%u", fv.versionSL.major, fv.versionSL.minor,
                          fv.versionSL.build, fv.versionNGX.major, fv.versionNGX.minor, fv.versionNGX.build);
            version_ = b;
        } else {
            version_ = "?";
        }
        std::printf("upscaler: DLSS loaded (%s) - an ENHANCEMENT (%s)\n", version_.c_str(), qualityName(quality));
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
        sl::FrameToken* token = nullptr;
        const uint32_t index = frame_++;
        if (newToken_(token, &index) != sl::Result::eOk || !token) return false;
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
        c.mvecScale = sl::float2(1.0f, 1.0f);   // UV units already (`upscale.h`)
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
        if (setConstants_(c, *token, viewport_) != sl::Result::eOk) return false;
        if (setOptions_(viewport_, options_) != sl::Result::eOk) return false;
        const uint32_t in = static_cast<uint32_t>(f.inputState);
        sl::Resource colour(sl::ResourceType::eTex2d, f.colour, in);
        sl::Resource depth(sl::ResourceType::eTex2d, f.depth, in);
        sl::Resource motion(sl::ResourceType::eTex2d, f.motion, in);
        sl::Resource output(sl::ResourceType::eTex2d, f.output, static_cast<uint32_t>(f.outputState));
        const sl::Extent render{0, 0, static_cast<uint32_t>(f.rw), static_cast<uint32_t>(f.rh)};
        const sl::Extent display{0, 0, static_cast<uint32_t>(f.dw), static_cast<uint32_t>(f.dh)};
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&colour, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &render),
            sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &display),
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &render),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &render)};
        auto* cmd = reinterpret_cast<sl::CommandBuffer*>(f.list);
        if (setTags_(*token, viewport_, tags, 4, cmd) != sl::Result::eOk) return false;
        const sl::BaseStructure* inputs[] = {&viewport_};
        const sl::Result r = evaluate_(sl::kFeatureDLSS, *token, inputs, 1, cmd);
        allocated_ = true;
        if (r != sl::Result::eOk) {
            static int said = 0;
            if (said++ < 3) std::fprintf(stderr, "upscaler: slEvaluateFeature(DLSS): %s\n", resultName(r));
            return false;
        }
        return true;
    }

private:
    HMODULE mod_ = nullptr;
    PFun_slInit* init_ = nullptr;
    PFun_slShutdown* shutdown_ = nullptr;
    PFun_slIsFeatureSupported* supported_ = nullptr;
    PFun_slSetD3DDevice* setDevice_ = nullptr;
    PFun_slGetFeatureFunction* featureFn_ = nullptr;
    PFun_slGetNewFrameToken* newToken_ = nullptr;
    PFun_slSetConstants* setConstants_ = nullptr;
    PFun_slSetTagForFrame* setTags_ = nullptr;
    PFun_slEvaluateFeature* evaluate_ = nullptr;
    PFun_slFreeResources* freeResources_ = nullptr;
    PFun_slGetFeatureVersion* getVersion_ = nullptr;
    PFun_slDLSSGetOptimalSettings* optimal_ = nullptr;
    PFun_slDLSSSetOptions* setOptions_ = nullptr;
    sl::DLSSOptions options_{};
    sl::ViewportHandle viewport_{0u};
    bool inited_ = false, allocated_ = false;
    uint32_t frame_ = 0;
    std::string version_;
    int dw_ = 0, dh_ = 0, quality_ = 1;
};

}  // namespace

std::unique_ptr<Upscaler> makeDlss(ID3D12Device* dev, IDXGIAdapter1* adapter, int dw, int dh, int quality) {
    auto d = std::make_unique<Dlss>();
    if (!adapter || !d->make(dev, adapter, dw, dh, quality)) return nullptr;
    return d;
}

}  // namespace omk::d3dup

#else  // !OMK_DLSS

namespace omk::d3dup {
std::unique_ptr<Upscaler> makeDlss(ID3D12Device*, IDXGIAdapter1*, int, int, int) {
    std::printf("upscaler: DLSS unavailable - this viewer was built without the Streamline SDK\n");
    return nullptr;
}
}  // namespace omk::d3dup

#endif
