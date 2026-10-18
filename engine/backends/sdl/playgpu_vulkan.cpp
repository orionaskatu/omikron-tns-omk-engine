// SPDX-License-Identifier: GPL-3.0-or-later
// THE GPU WINDOW, VULKAN - `PlayState::gpu...` for the Vulkan variant
// (`todo/play-split.md` S5). Each body is the `#if defined(OMK_VULKAN)` arm it
// replaces, moved unchanged; the game code calls these, and each build links
// exactly one of `playgpu_vulkan.cpp`, `playgpu_gles.cpp`, `playgpu_none.cpp`.
//
// ...and, on Windows, DIRECT3D 12 beside it (`backends/d3d12/`, `OMK_D3D12`):
// the same two slots - `vkRen`, the renderer that owns the window, and
// `worldVk`, the offscreen harness - filled by the other backend when
// `--d3d12` / `[Preferences] renderer = d3d12` or `--world-d3d12` ask. The
// slots keep Vulkan's names because every caller already reads them as "the
// GPU window" and "the harness"; which backend is in one is its `name()`,
// and the presents below go to that backend's entry points. Vulkan stays
// the default.
#include "playframe.h"
#include "playgpu_vulkan.h"
#include "platform/env.h"
#if defined(OMK_D3D12)
#  include "../d3d12/d3d12render.h"
#endif

#include "platform/datafs.h"

#include <cstring>
#include <fstream>

// The Vulkan window - this glue's, not the game's (the gateway: game code
// holds no host handle). One viewer a process, so one window - D3D12's too.
static SDL_Window* vkWin = nullptr;

// The window's Win32 handle, for DXGI (defined at the end of this file, where
// SDL's window-manager header - which brings in <windows.h> and its macros -
// cannot reach the code above).
[[maybe_unused]] static void* nativeWindowHandle(SDL_Window* w);

// Which backend a GPU slot holds. Without the D3D12 backend built in, never it.
static bool onD3d12(omk::Renderer* r) {
#if defined(OMK_D3D12)
    return r && std::strcmp(r->name(), "d3d12") == 0;
#else
    (void)r;
    return false;
#endif
}

// The four presents and the resize, to whichever backend `r` is.
static bool gpuPresentSurfaceOn(omk::Renderer* r, const omk::Surface& s) {
#if defined(OMK_D3D12)
    if (onD3d12(r)) return omk::d3d12PresentSurface(r, s);
#endif
    return omk::vulkanPresentSurface(r, s);
}
static bool gpuPresentWorldOn(omk::Renderer* r, int vy, int vh) {
#if defined(OMK_D3D12)
    if (onD3d12(r)) return omk::d3d12PresentWorld(r, vy, vh);
#endif
    return omk::vulkanPresentWorld(r, vy, vh);
}
static bool gpuWorldPictureOn(omk::Renderer* r, int vy, int vh, std::vector<unsigned char>& rgba) {
#if defined(OMK_D3D12)
    if (onD3d12(r)) return omk::d3d12WorldPicture(r, vy, vh, rgba);
#endif
    return omk::vulkanWorldPicture(r, vy, vh, rgba);
}
static bool gpuResizeOn(omk::Renderer* r, int w, int h) {
#if defined(OMK_D3D12)
    if (onD3d12(r)) return omk::d3d12Resize(r, w, h);
#endif
    return omk::vulkanResize(r, w, h);
}
static const char* gpuDeviceName(omk::Renderer* r) {
#if defined(OMK_D3D12)
    if (onD3d12(r)) return omk::d3d12DeviceName(r);
#endif
    return omk::vulkanDeviceName(r);
}

bool PlayState::gpuWindowBuild() const { return true; }

// Vulkan when the machine has it (see THE RENDERER in `playsetup_devices.cpp`)
void PlayState::gpuOpenWindow() {
    if (!forceSoftware && !worldVulkan) {
        // DIRECT3D 12 FIRST WHEN ASKED - a plain window (no Vulkan surface on
        // it) and a DXGI swapchain on its HWND. A device that does not come
        // up falls back to Vulkan below, then to the software reference, as
        // every other missing backend does.
        const bool wantD3d12 = d3d12Flag || settings.renderer == 1;
#if defined(OMK_D3D12)
        if (wantD3d12 && SDL_Init(SDL_INIT_VIDEO) == 0) {
            vkWin = SDL_CreateWindow("OMK Engine (d3d12)",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     dispW, dispH,
                                     omk::envFlag("OMK_HIDDEN_WINDOW") ? static_cast<Uint32>(SDL_WINDOW_HIDDEN)
                                                                      : omk::sdlFrontend(front).windowFlags());
            void* hwnd = vkWin ? nativeWindowHandle(vkWin) : nullptr;
            omk::Renderer* dx = hwnd ? omk::makeD3d12Renderer() : nullptr;
            if (dx) {
                if (aaSamples > 1) dx->setMultisample(aaSamples);   // the enhancements
                if (ssao) dx->setAmbientOcclusion(true);
                if (raytracing) dx->setRaytracing(true);         // row 19
                if (texFilter > 0) dx->setTextureFilter(texFilter);
                if (texAniso > 1) dx->setAnisotropy(texAniso);
                if (ssaa > 1) dx->setSupersample(ssaa);
                if (upscaler > 0) dx->setUpscaler(upscaler, upscaleQuality);   // row 18
            }
            if (dx && omk::d3d12AttachWindow(dx, hwnd) && dx->init(dispW, dispH)) {
                vkRen = dx;
                omk::sdlFrontend(front).attachWindow(vkWin);   // F11 and row 2 act on it
                std::printf("renderer: D3D12 - %s\n", omk::d3d12DeviceName(dx));
                return;
            }
            delete dx;
            if (vkWin) { SDL_DestroyWindow(vkWin); vkWin = nullptr; }
            std::printf("renderer: no Direct3D 12 device - trying Vulkan\n");
        }
#else
        if (wantD3d12)
            std::printf("renderer: Direct3D 12 asked for, but this viewer was built without it - Vulkan\n");
#endif
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            vkWin = SDL_CreateWindow("OMK Engine (vulkan)",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     dispW, dispH, SDL_WINDOW_VULKAN |
                                     // hidden for a profile of the direct present
                                     // (`OMK_HIDDEN_WINDOW`, playgpu_gles.cpp)
                                     (omk::envFlag("OMK_HIDDEN_WINDOW") ? SDL_WINDOW_HIDDEN
                                                                       : omk::sdlFrontend(front).windowFlags()));
        }
        if (vkWin) {
            unsigned nx = 0;
            SDL_Vulkan_GetInstanceExtensions(vkWin, &nx, nullptr);
            std::vector<const char*> ext(nx);
            SDL_Vulkan_GetInstanceExtensions(vkWin, &nx, ext.data());
            omk::Renderer* vkr = omk::makeVulkanRenderer();
            if (aaSamples > 1) vkr->setMultisample(aaSamples);   // the enhancements
            if (ssao) vkr->setAmbientOcclusion(true);
            if (raytracing && !vkr->setRaytracing(true))
                std::printf("raytracing: the Direct3D 12 backend's (--d3d12, DXR 1.1) - off on Vulkan\n");
            if (texFilter > 0) vkr->setTextureFilter(texFilter);
            if (texAniso > 1) vkr->setAnisotropy(texAniso);
            if (ssaa > 1) vkr->setSupersample(ssaa);
            if (upscaler > 0 && !vkr->setUpscaler(upscaler, upscaleQuality))
                std::printf("upscaler: %s is the Direct3D 12 backend's (--d3d12) - off on Vulkan\n",
                            omk::upscalerName(upscaler));
            omk::vulkanNeedExtensions(vkr, ext.data(), nx);
            void* inst = omk::vulkanCreateInstance(vkr);
            VkSurfaceKHR surf{};
            if (inst &&
                SDL_Vulkan_CreateSurface(vkWin, static_cast<VkInstance>(inst), &surf) &&
                omk::vulkanAttachSurface(vkr, reinterpret_cast<unsigned long long>(surf)) &&
                vkr->init(dispW, dispH)) {
                vkRen = vkr;
                omk::sdlFrontend(front).attachWindow(vkWin);   // F11 and row 2 act on it
                std::printf("renderer: VULKAN - %s\n", omk::vulkanDeviceName(vkr));
            } else {
                delete vkr;
                SDL_DestroyWindow(vkWin); vkWin = nullptr;
                std::printf("renderer: no Vulkan device - the software "
                            "reference\n");
            }
        }
    }
}

void PlayState::gpuOpenWorldHarness() {
    // ...and the harness: a Vulkan renderer with no surface, for the WORLD
    // alone. `run_vulkan` and `shadow_probe` already prove the backend comes
    // up offscreen; this is the same thing inside the viewer. `--world-d3d12`
    // fills the same slot with the Direct3D 12 renderer, offscreen likewise -
    // which is what `verify.py: engine: d3d12` differences against Vulkan.
    if (!vkRen && worldVulkan) {
        omk::Renderer* wv = nullptr;
        const char* api = "VULKAN";
#if defined(OMK_D3D12)
        if (worldD3d12) { wv = omk::makeD3d12Renderer(); api = "D3D12"; }
#else
        if (worldD3d12) {
            std::printf("--world-d3d12: this viewer was built without Direct3D 12\n");
            return;
        }
#endif
        if (!wv) wv = omk::makeVulkanRenderer();
        if (wv && aaSamples > 1) wv->setMultisample(aaSamples);
        if (wv && ssao) wv->setAmbientOcclusion(true);
        // HYBRID RAY TRACING (row 19): the D3D12 renderer takes it, Vulkan declines
        if (wv && raytracing && !wv->setRaytracing(true))
            std::printf("raytracing: the Direct3D 12 backend's (--world-d3d12, DXR 1.1) - off on Vulkan\n");
        if (wv && texFilter > 0) wv->setTextureFilter(texFilter);
        if (wv && texAniso > 1) wv->setAnisotropy(texAniso);
        if (wv && ssaa > 1) wv->setSupersample(ssaa);
        // the UPSCALER (row 18): the D3D12 renderer takes it, Vulkan declines
        if (wv && upscaler > 0 && !wv->setUpscaler(upscaler, upscaleQuality))
            std::printf("upscaler: %s is the Direct3D 12 backend's (--world-d3d12) - off on Vulkan\n",
                        omk::upscalerName(upscaler));
        if (wv && wv->init(dispW, dispH)) {
            worldVk = wv;
            std::printf("renderer: the world through %s offscreen - %s "
                        "(a harness; the frame is still presented on the CPU)\n",
                        api, gpuDeviceName(wv));
        } else {
            delete wv;
            std::printf("%s: no offscreen device\n", worldD3d12 ? "--world-d3d12" : "--world-vulkan");
        }
    }
}

// THE WINDOW'S OWN PICTURE (`OMK_WINDUMP=<path>`): on the last frame of a
// `--frames` run, what the window was handed, RGBA8 top row first, written
// raw. The `--dump` is the software reference's RGB565 and cannot show colour
// depth 32 (`verify.py: engine: colour depth`); this can. On Direct3D 12 it is
// the swapchain's BACK BUFFER, copied out by the present itself (the window's
// size); on Vulkan the image the present blits 1:1 into the window - the
// present pass's picture, read again, or the uploaded `fb` on a CPU frame.
// Armed in `gpuPresentVerify` (the present phase's first call), written after
// whichever present ran.
static const char* winDumpPath() {
    static const char* path = omk::envStr("OMK_WINDUMP");
    return path;
}
static bool winDumpArmed = false;
static void writeWindowDump(const std::vector<unsigned char>& pic, int w, int h, const char* api) {
    winDumpArmed = false;
    if (!omk::safeOutputPath(winDumpPath())) return;
    std::ofstream o(winDumpPath(), std::ios::binary);
    o.write(reinterpret_cast<const char*>(pic.data()), static_cast<std::streamsize>(pic.size()));
    std::printf("window dump (%s): %dx%d RGBA8 -> %s\n", api, w, h, winDumpPath());
}
// after a present: `world` says it was the present pass's (`vy`, `vh`), else `surf`'s
static void windowDumpAfterPresent(omk::Renderer* r, bool world, int vy, int vh, const omk::Surface* surf) {
    if (!winDumpArmed || !r) return;
    std::vector<unsigned char> pic;
    int w = 0, h = 0;
#if defined(OMK_D3D12)
    if (onD3d12(r)) {
        if (omk::d3d12WindowPicture(r, pic, w, h)) writeWindowDump(pic, w, h, "d3d12 back buffer");
        return;
    }
#endif
    if (world && surf && omk::vulkanWorldPicture(r, vy, vh, pic) &&
        pic.size() == static_cast<std::size_t>(surf->w) * static_cast<std::size_t>(surf->h) * 4) {
        writeWindowDump(pic, surf->w, surf->h, "vulkan present image");
    } else if (!world && surf) {
        const unsigned char* lut = omk::expand565Rgba();
        pic.resize(surf->px.size() * 4);
        for (std::size_t i = 0; i < surf->px.size(); ++i) std::memcpy(&pic[4 * i], lut + 4 * surf->px[i], 4);
        writeWindowDump(pic, surf->w, surf->h, "vulkan uploaded frame");
    }
}

bool PlayState::gpuPresentSurface(const omk::Surface& pic) {
    if (vkRen) {
        gpuPresentSurfaceOn(vkRen, pic);
        windowDumpAfterPresent(vkRen, false, 0, 0, &pic);
        return true;
    }
    return false;
}

void PlayState::gpuPresentVerify(bool&) {
    if (!winDumpPath() || !vkRen || !frames || frameNo + 1 < frames) return;
    winDumpArmed = true;
#if defined(OMK_D3D12)
    if (onD3d12(vkRen)) omk::d3d12CaptureWindow(vkRen);
#endif
}

// COLOUR DEPTH 32 ON DIRECT3D 12 (`todo/enhancements.md` 21): a frame with an
// interface over the world - a soft gate (`gpuOverlayDecision`) - is the GLES
// window's overlay frame here too: the 2D composed over the KEY, its planes
// resolved (`omk::resolveOverlayPlanes`, as GL1, GLES and the 3DS do) and
// blended on the GPU over the present pass's full-precision world - so the
// HUD, the subtitles, the fades and the screens no longer pull the 3D back
// through a 565 readback. Vulkan keeps the CPU composite (its present is a
// `vkCmdBlitImage`; an overlay pass there is not written).
void PlayState::gpuPresentOverlay(bool& presentedWorld) {
#if defined(OMK_D3D12)
    if (!overlayFrame || presentedWorld || !onD3d12(vkRen)) return;
    static std::vector<std::uint8_t> ovMask, ovMaskRow;
    static std::vector<std::uint64_t> ovMaskBlocks;
    omk::resolveOverlayPlanes(fb.px.data(), fb.w, fb.h, g_ov, ovMask, ovMaskRow, ovMaskBlocks);
    presentedWorld = omk::d3d12PresentOverlay(vkRen, fb, ovMask.data(), ovFade, gpuVy, gpuVh);
    if (!presentedWorld) {
        static bool told = false;
        if (!told) std::printf("colour depth: the Direct3D 12 overlay present failed - the frame on the CPU\n");
        told = true;
    }
    else windowDumpAfterPresent(vkRen, true, gpuVy, gpuVh, &fb);
#else
    (void)presentedWorld;
#endif
}

void PlayState::gpuPresentWorld(bool& presentedWorld) {
    if (vkRen) {
        presentedWorld = gpuPresentWorldOn(vkRen, gpuVy, gpuVh);
        if (presentedWorld) windowDumpAfterPresent(vkRen, true, gpuVy, gpuVh, &fb);
    }
}

bool PlayState::gpuWorldOnWindow() {
    omk::Renderer& world = *world_;
    bool onVulkan = false;
    onVulkan = (vkRen && &world == vkRen) ||
               (verifyGpuPresent && worldVk && &world == worldVk);
    return onVulkan;
}

void PlayState::gpuOverlayDecision(const char*& keep) {
#if defined(OMK_D3D12)
    // the soft gates' frames go out as an OVERLAY on the Direct3D 12 window at
    // colour depth 32 (`gpuPresentOverlay`); at 16 the CPU composite, as before
    static const bool noOverlay = omk::envFlag("OMK_NO_OVERLAY");
    overlayFrame = softGate && !noOverlay && !verifyGpuPresent && colourDepth == 32 &&
                   onD3d12(vkRen) && world_ == vkRen;
    if (overlayFrame) {
        static std::string overlayWhy;
        overlayWhy = std::string("overlay (") + keep + ")";
        keep = overlayWhy.c_str();
    }
#else
    (void)keep;
#endif
}

void PlayState::gpuResize(int nw, int nh, bool& ok) {
    if (vkRen) ok = gpuResizeOn(vkRen, nw, nh);
    if (ok && worldVk) ok = gpuResizeOn(worldVk, nw, nh);
}

bool PlayState::gpuDriverRow(std::vector<std::string>& drivers) {
    if (vkRen) {
        drivers.push_back(std::string(onD3d12(vkRen) ? "Direct3D 12 - " : "Vulkan - ") + gpuDeviceName(vkRen));
        return true;
    }
    return false;
}

void PlayState::gpuReportTimings() {}
void PlayState::gpuSlowFrameReport() {}
void PlayState::gpuFinishReport() {}

void PlayState::gpuVerifyWorldPicture() {
    omk::Renderer& world = *world_;
    if (gpuFrame && verifyGpuPresent) {
        // the GPU's picture against the CPU frame, as the upload would
        // expand it - every byte of every pixel
        static std::vector<unsigned char> gpuPic;
        static long compared = 0, differing = 0, badPixels = 0;
        if (gpuWorldPictureOn(&world, gpuVy, gpuVh, gpuPic) &&
            gpuPic.size() == fb.px.size() * 4) {
            const unsigned char* lut = omk::expand565Rgba();
            long diff = 0;
            for (std::size_t i = 0; i < fb.px.size(); ++i)
                if (std::memcmp(lut + 4 * static_cast<std::size_t>(fb.px[i]), &gpuPic[4 * i], 4) != 0) ++diff;
            ++compared;
            if (diff) {
                ++differing; badPixels += diff;
                // at once, so a run with only a few GPU frames still says so
                std::printf("gpu present verify: frame %ld DIFFERS in %ld pixels\n", frameNo, diff);
            }
        }
        if (compared > 0 && compared % 30 == 0)
            std::printf("gpu present verify: frame %ld, %ld frames compared, %ld differ (%ld pixels)\n",
                        frameNo, compared, differing, badPixels);
    }
}

// ---- the native window handle, LAST (see the declaration above) -----------
#if defined(OMK_D3D12)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  if defined(OMK_SDL3)
static void* nativeWindowHandle(SDL_Window* w) {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}
#  else
#    include <SDL_syswm.h>
static void* nativeWindowHandle(SDL_Window* w) {
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_WINDOWS) return nullptr;
    return info.info.win.window;
}
#  endif
#else
static void* nativeWindowHandle(SDL_Window*) { return nullptr; }
#endif
