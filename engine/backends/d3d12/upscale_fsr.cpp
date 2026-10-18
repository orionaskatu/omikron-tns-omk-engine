// SPDX-License-Identifier: GPL-3.0-or-later
// AMD FSR for the Direct3D 12 backend (`upscale.h`, `todo/enhancements.md`
// row 18), through the FidelityFX API.
//
// The API is five C entry points in ONE loader DLL,
// `amd_fidelityfx_loader_dx12.dll`, which finds the effect providers beside
// it (`amd_fidelityfx_upscaler_dx12.dll`) and, per device, offers every
// upscaler version it can run. They are taken with GetProcAddress - the SDK's
// own `ffx_api_loader.h` arrangement - so nothing here links an MSVC import
// library, and a viewer whose directory lacks the DLLs says so and draws
// natively. Only the SDK's HEADERS are compiled in (`-DOMK_FSR`, `Makefile`);
// without them this file is the stub at the bottom.
//
// WHICH FSR: the loader's own choice is its default provider; the versions it
// offers for this device are listed (`ffxQueryDescGetVersions`) and the
// NEWEST by version number is asked for explicitly (`ffxOverrideVersion`) -
// FSR 4, the machine-learned one, wherever the GPU runs it (RDNA 4) and 3.1
// elsewhere - and the version the created context REPORTS is what is logged.
#include "upscale.h"
#include "platform/env.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace omk::d3dup {

std::wstring moduleDir() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path, n);
    const auto cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut + 1);
}

}  // namespace omk::d3dup

#if defined(OMK_FSR)

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

namespace omk::d3dup {
namespace {

// The provider's messages (its debug checks, a rejected parameter) - to
// stderr, where the D3D12 debug layer's go too.
void fsrMessage(uint32_t type, const wchar_t* msg) {
    std::fprintf(stderr, "fsr %s: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "warning", msg);
}

// "4.0.2" -> comparable; a name that does not parse sorts first
std::uint64_t versionKey(const char* name) {
    unsigned a = 0, b = 0, c = 0;
    if (!name || std::sscanf(name, "%u.%u.%u", &a, &b, &c) < 1) return 0;
    return (static_cast<std::uint64_t>(a) << 40) | (static_cast<std::uint64_t>(b) << 20) | c;
}

class Fsr final : public Upscaler {
public:
    ~Fsr() override {
        if (ctx_ && destroy_) destroy_(&ctx_, nullptr);
        if (mod_) FreeLibrary(mod_);
    }
    const char* name() const override { return "FSR"; }
    std::string version() const override { return version_; }

    bool make(ID3D12Device* dev, int dw, int dh, int quality) {
        dw_ = dw; dh_ = dh; quality_ = quality;
        const std::wstring dll = moduleDir() + L"amd_fidelityfx_loader_dx12.dll";
        // its OWN directory first for the providers it loads beside it
        mod_ = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod_) {
            std::printf("upscaler: FSR unavailable - amd_fidelityfx_loader_dx12.dll is not next to the viewer\n");
            return false;
        }
        create_ = reinterpret_cast<PfnFfxCreateContext>(reinterpret_cast<void*>(GetProcAddress(mod_, "ffxCreateContext")));
        destroy_ = reinterpret_cast<PfnFfxDestroyContext>(reinterpret_cast<void*>(GetProcAddress(mod_, "ffxDestroyContext")));
        query_ = reinterpret_cast<PfnFfxQuery>(reinterpret_cast<void*>(GetProcAddress(mod_, "ffxQuery")));
        dispatch_ = reinterpret_cast<PfnFfxDispatch>(reinterpret_cast<void*>(GetProcAddress(mod_, "ffxDispatch")));
        if (!create_ || !destroy_ || !query_ || !dispatch_) {
            std::printf("upscaler: FSR unavailable - the loader DLL lacks the FidelityFX entry points\n");
            return false;
        }
        // THE VERSIONS this device can run, newest asked for
        ffxQueryDescGetVersions vq{};
        vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        vq.device = dev;
        std::uint64_t count = 0;
        vq.outputCount = &count;
        query_(nullptr, &vq.header);
        std::vector<std::uint64_t> ids(count);
        std::vector<const char*> names(count);
        std::uint64_t pick = 0;
        std::string offered;
        if (count) {
            vq.versionIds = ids.data();
            vq.versionNames = names.data();
            query_(nullptr, &vq.header);
            std::uint64_t best = 0;
            for (std::uint64_t i = 0; i < count; ++i) {
                offered += (i ? ", " : "") + std::string(names[i] ? names[i] : "?");
                if (versionKey(names[i]) >= best) { best = versionKey(names[i]); pick = ids[i]; }
            }
        }
        std::printf("upscaler: FSR providers for this device: %s\n", offered.empty() ? "(none listed)" : offered.c_str());

        ffxCreateContextDescUpscale cd{};
        cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        // the colour is this game's: LDR and PERCEPTUAL (gamma-encoded bytes),
        // the depth standard (not inverted) with a finite far plane
        cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
        if (omk::envFlag("OMK_FSR_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        cd.maxRenderSize = {static_cast<uint32_t>(dw), static_cast<uint32_t>(dh)};
        cd.maxUpscaleSize = {static_cast<uint32_t>(dw), static_cast<uint32_t>(dh)};
        cd.fpMessage = fsrMessage;
        ffxCreateBackendDX12Desc be{};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        be.device = dev;
        ffxCreateContextDescUpscaleVersion uv{};
        uv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        uv.version = FFX_UPSCALER_VERSION;
        ffxOverrideVersion ov{};
        ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        ov.versionId = pick;
        cd.header.pNext = &be.header;
        be.header.pNext = &uv.header;
        uv.header.pNext = pick ? &ov.header : nullptr;
        const ffxReturnCode_t rc = create_(&ctx_, &cd.header, nullptr);
        if (rc != FFX_API_RETURN_OK || !ctx_) {
            std::printf("upscaler: FSR unavailable - ffxCreateContext returned %u\n", static_cast<unsigned>(rc));
            ctx_ = nullptr;
            return false;
        }
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        query_(&ctx_, &pv.header);
        version_ = pv.versionName ? pv.versionName : "?";
        std::printf("upscaler: FSR %s loaded (provider 0x%llx) - an ENHANCEMENT: the frame drawn at "
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
        d.commandList = f.list;
        d.color = ffxApiGetResourceDX12(f.colour, in);
        d.depth = ffxApiGetResourceDX12(f.depth, in);
        d.motionVectors = ffxApiGetResourceDX12(f.motion, in);
        d.output = ffxApiGetResourceDX12(f.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {f.jitterX, f.jitterY};
        // UV units -> render pixels (`upscale.h`)
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
    HMODULE mod_ = nullptr;
    PfnFfxCreateContext create_ = nullptr;
    PfnFfxDestroyContext destroy_ = nullptr;
    PfnFfxQuery query_ = nullptr;
    PfnFfxDispatch dispatch_ = nullptr;
    ffxContext ctx_ = nullptr;
    std::string version_;
    int dw_ = 0, dh_ = 0, quality_ = 1;
};

}  // namespace

std::unique_ptr<Upscaler> makeFsr(ID3D12Device* dev, int dw, int dh, int quality) {
    auto f = std::make_unique<Fsr>();
    if (!f->make(dev, dw, dh, quality)) return nullptr;
    return f;
}

}  // namespace omk::d3dup

#else  // !OMK_FSR

namespace omk::d3dup {
std::unique_ptr<Upscaler> makeFsr(ID3D12Device*, int, int, int) {
    std::printf("upscaler: FSR unavailable - this viewer was built without the FidelityFX SDK\n");
    return nullptr;
}
}  // namespace omk::d3dup

#endif
