// SPDX-License-Identifier: GPL-3.0-or-later
// THE SECOND LIVE RENDERER - Direct3D 12, Windows only, beside Vulkan.
//
// `backends/vulkan/vkrender.cpp` is this file's SPECIFICATION, and it is meant
// literally: every decision below is that file's, turned into D3D12 calls, and
// where the comment there explains a choice the code here only names it. A2's
// rule holds unchanged - **a backend receives decisions and turns them into API
// calls; it never makes one** - so the ORDER is the order given, the TEXTURE is
// `drawTextureSlot`, the BLEND is `blendRule`, the CUTOUT a colour key in alpha,
// the BACK-FACE CULL a root constant per run of `cornerCull` and a discard on
// `SV_IsFrontFace`, the FOG the bucket key's two exclusions, the DEPTH TIE
// `o3de/depthtie.h`'s, the projection `cameraBasis`'s.
//
// What makes the pair ONE renderer twice rather than two renderers:
//
//   * THE SAME SHADERS. The GLSL in `backends/vulkan/shaders/` is compiled to
//     SPIR-V, translated to HLSL (spirv-cross, shader model 6.0) and compiled
//     to DXIL (dxc) by the `Makefile`; nothing here is a second transcription
//     of `raster.cpp`'s inner loop that could drift from the first. The vertex
//     shaders are translated Y-FLIPPED (`--flip-vert-y`), Vulkan's clip space
//     being Y-down and D3D's Y-up, so a vertex lands on the same pixel and a
//     triangle keeps its screen winding - `kCullSign` is Vulkan's, measured
//     again here (`engine: d3d12` differences the two pictures).
//   * THE SAME BYTES TO THEM. The push-constant block is 26 ROOT CONSTANTS at
//     `b0, space7` (the register the `Makefile` gives the one block GLSL leaves
//     unbound), the per-frame uniforms are root CBVs at Vulkan's set 1
//     bindings, the pose buffer a root SRV at set 2 - its dynamic offset is the
//     root descriptor's address. `Push`, `ShadowUbo`, `LightUbo` are the
//     Vulkan structs verbatim; spirv-cross emits `packoffset`s that reproduce
//     std140/std430, so a field cannot move between the two backends.
//   * THE SAME READBACK. R8G8B8A8 target, 565 quantised in SOFTWARE by the
//     very loop `VulkanRenderer::readback` runs (`PORTING` A3: one rule, one
//     place), the supersample resolve on the 8-bit side before it.
//
// **The frame, and the seam a later UPSCALER goes into.** Two sizes are kept
// apart throughout: the RENDER size `rw_ x rh_` (`w_ * ss_`, or the upscaler's
// answer for its quality), which the scene,
// its depth and SSAO run at, and the DISPLAY size `w_ x h_`, which `readback`,
// the present pass and the swapchain answer with. The scene pass draws into
// `msColour_` (MSAA) or `colour_`; `end()` resolves into `colour_`, runs SSAO
// over the depth and multiplies it in, and from then on `colour_` (render
// size, R8G8B8A8, SRV at `kSlotColour`) IS "the scene's resolved colour" and
// `depth_` (render size, D32S8, ALWAYS created sampleable - SRV at
// `kSlotDepth`) the scene's depth. The display half - `runPresentPass`,
// `readback`, `presentImage` - reads only `upscaleSource()` and the two sizes.
// THE UPSCALER (`upscale.h`, `todo/enhancements.md` 18, off unless asked) is
// the one pass between the two: the scene pass then also writes `motion_`
// (render size, R16G16_FLOAT - the shaders built -DOMK_MOTION, `PushMv`) over
// a per-frame jitter, `upscale()` hands colour, depth and motion to FSR or
// DLSS, which write the display-size `upOut_`, and `upscaleSource()` returns
// that instead of `colour_` - the display half is otherwise unchanged.
//
// A8: optional and Windows-only. The `Makefile` builds this into the Vulkan
// variant's `omk-play` only on Windows with glslc, spirv-cross and dxc found;
// `--d3d12` (a window) and `--world-d3d12` (the offscreen harness) choose it,
// Vulkan stays the default. `OMK_D3D12_DEBUG=1` turns the D3D12 debug layer on
// and prints every message it raises (`d3d12 debug [ERROR] ...`).
//
// mingw-w64's headers declare COM methods returning a struct (a descriptor
// heap's start handle, a resource's description) as returning it by value,
// which is NOT the MSVC member-function ABI the runtime was built with: the
// explicit-aggregate-return wrappers below are the correct calling convention.
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include "d3d12render.h"
#include "upscale.h"

#include "platform/threads.h"
#include "platform/profile.h"
#include "o3de/renderer.h"
#include "o3de/depthtie.h"
#include "o3de/drawcore.h"
#include "o3de/vertexlight.h"
#include "o3de/postfx.h"
#include "o3de/framegen.h"
#include "o3de/rtscene.h"
#include "o3de/texpack.h"
#include "o3de/colourgrade.h"
#include "platform/env.h"
#include "platform/events.h"
#include "platform/datafs.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

// THE SHADERS, as DXIL - `build/shaders/dx/*.h`, made from the Vulkan GLSL.
#include "dx/scene.vert.h"
#include "dx/scene.frag.h"
#include "dx/fsq.vert.h"
#include "dx/fsq.frag.h"
#include "dx/shadow.vert.h"
#include "dx/present.frag.h"
#include "dx/overlay.frag.h"
#include "dx/bloom.frag.h"
#include "dx/post.frag.h"
#include "dx/post_ms.frag.h"
#include "dx/blit_hdr.frag.h"
#include "dx/ssao.frag.h"
#include "dx/ssao_blur.frag.h"
#include "dx/ssao_ms.frag.h"
#include "dx/ssao_blur_ms.frag.h"
#include "dx/blit.frag.h"
// ...SOFT PARTICLES' scene pass (`scene.frag` -DOMK_SOFT) and VOLUMETRIC LAMP
// LIGHT's two passes (`todo/enhancements.md` 23, 24)
#include "dx/scene_soft.frag.h"
#include "dx/scene_soft_mv.frag.h"
#include "dx/vol.frag.h"
#include "dx/vol_ms.frag.h"
#include "dx/volmix.frag.h"
#include "dx/volmix_ms.frag.h"
// ...and the UPSCALERS' scene pass, the same two files built -DOMK_MOTION
#include "dx/scene_mv.vert.h"
#include "dx/scene_mv.frag.h"
// ...and HYBRID RAY TRACING's (`todo/enhancements.md` 19): `scene.frag` and
// `ssao.frag` built -DOMK_RT, shader model 6.5 (`backends/vulkan/shaders/rt.glsl`)
#include "dx/scene_rt.frag.h"
#include "dx/scene_rt_mv.frag.h"
#include "dx/scene_rtc.frag.h"
#include "dx/scene_rtc_mv.frag.h"
#include "dx/ssao_rt.frag.h"
#include "dx/ssao_rt_ms.frag.h"
// ...and FRAME GENERATION's three (`todo/enhancements.md` 27): the capture at
// the frame's end, the splat and the resolve of the in-between picture
#include "dx/fg_cap.frag.h"
#include "dx/fg_splat.frag.h"
#include "dx/fg_resolve.frag.h"

// ---- the blocks the shaders read, VERBATIM from `vkrender.cpp` ------------
struct PresentPush { int32_t vy, vh, w, dither, ss, depth, rcas, rcasCon, raw; };   // (the last three: the remaster's upscale, Vulkan's - zero here)
// the overlay pass's (`overlay.frag`, colour depth 32): the colour fade
struct OverlayPush { float fade[4]; };
struct SsaoPush { float tanH, tanV, A, B, vw, vh, radius, strength; };
// ...the RAY-TRACED AO's (`ssao.frag` -DOMK_RT): the camera in the world, after it
struct SsaoRtPush { SsaoPush a; float eye[4], s[4], u[4], f[4]; };
// `scene.frag`'s `RtFrame` (-DOMK_RT, set 3 binding 2: a root CBV b2 space3), shared
// with the Vulkan backend (`o3de/rtscene.h`)
using RtFrame = omk::rt::Frame;
// `blit.frag`'s, this backend's own: the swapchain's scaled copy
struct BlitPush { float dst[4]; float srcSize[4]; int32_t linear; float white, peak; };
// THE POST CHAIN's (`bloom.frag`, `post.frag`, todo/enhancements.md 22)
using omk::postfx::BloomPush;   // `o3de/postfx.h`: shared with the Vulkan backend
using omk::postfx::PostPush;
// VOLUMETRIC LAMP LIGHT's (`vol.frag`, `volmix.frag`, todo/enhancements.md 24)
using omk::postfx::VolPush;     // `o3de/postfx.h`: shared with the Vulkan backend
using omk::postfx::VolMixPush;
using omk::postfx::kMaxVolLamps;
using omk::postfx::VolUbo;
// FRAME GENERATION's (`fg_cap.frag`; `fg_splat.frag`, `fg_resolve.frag`)
using FgCapPush = omk::framegen::CapPush;   // `o3de/framegen.h`: shared with the Vulkan window
using FgPush = omk::framegen::Push;

constexpr UINT kShadowSide = 2048;
constexpr int kShadowLayers = 3;

struct GpuLightStd140 {
    float posA[4];      // xyz position, w outer radius
    float dirB[4];      // xyz direction, w inner radius
    float colourI[4];   // rgb colour 0..1, a intensity
};
struct LightUbo {
    GpuLightStd140 l[omk::View::kMaxGpuLights];   // 64 (the remaster's tiled point lights raised it)
    int32_t count;
    float   base;      // View::litBase
    int32_t grey;      // View::grey, the greyscale bank (ops 150/151)
    int32_t tiles;     // the TILED lights module's - never bound here, 0
    float   eye[4];    // the camera's position (vehicle shine's view vector, the water's Fresnel); w the NIGHT (View::setDim)
    float   fx[4];     // x the relief (none here); y, z, w the MIST's strength, ground and height
    float   soft[4];   // SOFT PARTICLES (`scene.frag` -DOMK_SOFT): A, B, 1 / the fade's range
};
struct ShadowUbo {
    float lightMvp[kShadowLayers][16];
    float strength[4];
    float lightPos[kShadowLayers][4];
    float params[kShadowLayers][4];
    float texel;
    float bias;
    float shimmer;     // THE SHIMMER's clock, the frame's uniform (see vkrender.cpp)
    float pad;
};

constexpr float kTieLoserPhase = 8192.0f;

struct GpuVert {
    float x, y, z;
    float u, v;
    float r, g, b;
    float nx, ny, nz;
    float phase;
    float mesh;
};

// `scene.vert` / `scene.frag`'s push block, byte for byte - and the reason it
// is ordered so is Vulkan's (`fogColour` 16-byte aligned at 80). spirv-cross
// carries the same offsets into the HLSL as `packoffset`s.
struct Push {
    float mvp[16];     // 0
    int32_t cutout;    // 64
    float fogStart;    // 68
    float fogEnd;      // 72
    int32_t caster;    // 76
    float fogColour[3];// 80
    int32_t lit;       // 92
    int32_t cull;      // 96
    int32_t posed;     // 100
};
static_assert(sizeof(Push) == 104, "the push block's common part is 104 bytes");
// ...and what `scene.frag` -DOMK_DX adds after it: THE TEXTURE PACK's replacement (the remaster's,
// `o3de/texpack.h`) is `su` x `sv` times the original, the UVs staying in the original's texels (1, 1 on
// every shipped texture). This backend has no bloom, no water pass: nothing else rides.
struct PushSc {
    Push p;
    std::uint32_t texScale;   // 104: (u - 1) | (v - 1) << 4, `scene.frag`'s `texScale`
};
static_assert(sizeof(PushSc) == 108, "the plain push block is 108 bytes");
constexpr UINT kPushDwords = sizeof(PushSc) / 4;
// ...and with an UPSCALER on (`scene.vert` built -DOMK_MOTION), the same 104
// bytes and then the JITTER at 104 (clip units) and the PREVIOUS frame's
// view-projection at 112, the pack's scale after them at 176 - 45 root
// constants, inside the 64 a root signature holds with the rest of it.
struct PushMv {
    Push p;
    float jitter[2];      // 104
    float prevMvp[16];    // 112
    std::uint32_t texScale;   // 176
};
static_assert(sizeof(PushMv) == 180, "the motion push block is 180 bytes");
constexpr UINT kPushMvDwords = sizeof(PushMv) / 4;
// Vulkan's `kCullSign`, and the same value is right here because the
// Y-flipped vertex stage puts every triangle on the same pixels with the same
// screen winding, and `FrontCounterClockwise` is Vulkan's CCW front face.
// Measured, as Vulkan's was: the wrong sign turns the room inside out.
constexpr int32_t kCullSign = 1;

// The root signatures' registers, as the `Makefile` makes them deterministic.
constexpr UINT kPushSpace = 7;   // the push block, `register(b0, space7)`

// The shader-visible descriptor heap's FIXED slots - the frame's own views,
// written again in place when the frame is remade (`resize`). Depth and the
// AO image sit side by side because SSAO binds them as ONE table (t0, t1).
constexpr UINT kSlotColour = 0;   // `colour_`, the present pass's and the blit's source
constexpr UINT kSlotDepth = 1;    // `depth_`'s depth plane (SSAO; an upscaler)
constexpr UINT kSlotAo = 2;       // the AO factor
constexpr UINT kSlotShadow = 3;   // the shadow map's three layers
constexpr UINT kSlotPres = 4;     // the present pass's output, for the blit
constexpr UINT kSlotUpOut = 5;    // the UPSCALER's display-size output
constexpr UINT kSlotOvPic = 6;    // the OVERLAY's 565 colour plane (colour depth 32)...
constexpr UINT kSlotOvMask = 7;   // ...and its mask plane, the next slot (one table)
// THE POST CHAIN (todo/enhancements.md 22): `post.frag`'s ONE table - the
// source, bloom level 0, the depth, its stencil plane, the grade's LUT - then the chain's output
// and the levels
constexpr UINT kSlotPostSrc = 8;
constexpr UINT kSlotPostBloom = 9;
constexpr UINT kSlotPostDepth = 10;
constexpr UINT kSlotPostStencil = 11;
constexpr UINT kSlotPostLut = 12;   // the REMASTER's city grade (`setGrade`): the table's fifth, a null view without one
constexpr UINT kSlotPostOut = 13;
constexpr UINT kSlotBloom0 = 14;
// the stencil value a TRANSLUCENT BODY's pixels take for depth of field
constexpr UINT kSubjectStencil = omk::postfx::kSubjectStencil;
constexpr int kBloomLevels = omk::postfx::kBloomLevels;   // half the display size down to 1/64
// VOLUMETRIC LAMP LIGHT (todo/enhancements.md 24): its ONE table - the depth,
// then the half-size light
constexpr UINT kSlotVolDepth = kSlotBloom0 + kBloomLevels;
constexpr UINT kSlotVol = kSlotVolDepth + 1;
// FRAME GENERATION (todo/enhancements.md 27): the capture's ONE table - the
// display source, the depth, the motion - then the in-between passes' - the
// two frames' pictures and their motion-and-depth - then the two splat heads
// (UAVs, one table) and the in-between picture
constexpr UINT kSlotFgSrc = kSlotVol + 1;
constexpr UINT kSlotFgIn = kSlotFgSrc + 3;
constexpr UINT kSlotFgHead = kSlotFgIn + 4;
constexpr UINT kSlotFgMid = kSlotFgHead + 2;
constexpr UINT kSlotFirstFree = kSlotFgMid + 1;
constexpr UINT kSrvHeapSize = 4096;

#define DXCHECK(x, what)                                                        \
    do {                                                                        \
        const HRESULT hr_ = (x);                                                \
        if (FAILED(hr_)) {                                                      \
            std::fprintf(stderr, "d3d12: %s -> 0x%08lx\n", what,                \
                         static_cast<unsigned long>(hr_));                      \
            return false;                                                       \
        }                                                                       \
    } while (0)

template <class T> void release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

constexpr UINT64 alignUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

// A descriptor heap, allocated one slot at a time with a free list. The
// shader-visible one holds every view a shader reads; nothing is allocated
// per draw, so there is no ring to overrun.
struct DescHeap {
    ID3D12DescriptorHeap* heap = nullptr;
    UINT inc = 0, cap = 0, next = 0;
    std::vector<UINT> freed;
    bool make(ID3D12Device* dev, D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, bool visible, UINT first) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = type; d.NumDescriptors = n;
        d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)))) return false;
        inc = dev->GetDescriptorHandleIncrementSize(type);
        cap = n; next = first;
        return true;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT i) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(i) * inc;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT i) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(i) * inc;
        return h;
    }
    UINT alloc() {
        if (!freed.empty()) { const UINT i = freed.back(); freed.pop_back(); return i; }
        return next < cap ? next++ : UINT(-1);
    }
    void free(UINT i) { if (i != UINT(-1)) freed.push_back(i); }
};

// One resource and the state it is in. D3D12 has no layouts that a render
// pass moves for you, so every image here says where it is and `to` emits the
// barrier the next use needs - the explicit counterpart of Vulkan's
// `initialLayout`/`finalLayout` pairs.
struct Res {
    ID3D12Resource* r = nullptr;
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COMMON;
    void to(ID3D12GraphicsCommandList* l, D3D12_RESOURCE_STATES want) {
        if (!r || st == want) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = st;
        b.Transition.StateAfter = want;
        l->ResourceBarrier(1, &b);
        st = want;
    }
};

class D3D12Renderer : public omk::Renderer {
public:
    bool init(int w, int h) override;
    void setTextures(std::span<const omk::Texture> t) override;
    void begin(const omk::View& v) override;
    void shadowPass(const omk::View& v, std::span<const omk::Draw> casters) override;
    bool drawsPixelLights() const override { return true; }
    bool drawsShadowMap() const override { return true; }
    // BODIES POSED BY THE RENDERER: set 2's pose buffer, a root SRV here.
    bool posesBodies() const override { return posePtr_ != nullptr; }
    int maxVertexLights() const override { return posePtr_ ? kPoseLights : 0; }
    bool drawMirrorScene(const omk::View& v, const omk::View& refl,
                         std::span<const omk::Draw> scene,
                         std::span<const omk::Draw> sceneClipped,
                         std::span<const omk::Draw> mirror) override;
    void pushView(const omk::View& v);
    void submit(const omk::Draw& d) override;
    void end() override;
    const omk::Surface& readback() override;
    omk::RasterStats stats() const override { return st_; }
    const char* name() const override { return "d3d12"; }
    D3D12Renderer() {
        releaseOn_ = !omk::envFlag("OMK_NO_GEOMETRY_RELEASE");
        omk::addGeometryListener(&D3D12Renderer::geometryGone, this);
    }
    ~D3D12Renderer() override;

    // The enhancements (renderer.h): requests before `init`, honoured in
    // `makeTarget` where the device's own limits get the last word.
    bool setSsao(bool on) override {
        if (dev_) return false;
        wantSsao_ = on;
        return true;
    }
    // HYBRID RAY TRACING (`todo/enhancements.md` 19): honoured in `init` when
    // the device has DXR 1.1 (`rtInit`)
    bool setRaytracing(bool on) override {
        if (dev_) return false;
        wantRt_ = on;
        return true;
    }
    bool raytracingLive() const override { return rtOn_; }
    // RAY-TRACED REFLECTIONS (`todo/enhancements.md` 26): honoured in `init`
    // only when the ray tracing came up
    bool setRtReflections(bool on) override {
        if (dev_) return false;
        wantRtRefl_ = on;
        return true;
    }
    bool rtReflectionsLive() const override { return rtReflOn_; }
    bool setMultisample(int samples) override {
        if (dev_) return false;
        wantSamples_ = samples < 2 ? 1 : samples < 4 ? 2 : samples < 8 ? 4 : 8;
        return true;
    }
    bool setTextureFilter(int mode) override {
        if (dev_) return false;
        filter_ = mode < 1 ? 0 : mode < 2 ? 1 : 2;
        return true;
    }
    bool setAnisotropy(int n) override {
        if (dev_) return false;
        aniso_ = n < 1 ? 1 : n > 16 ? 16 : n;
        return true;
    }
    bool setSupersample(int n) override {
        if (dev_) return false;
        ss_ = n < 2 ? 1 : n < 4 ? 2 : 4;
        return true;
    }
    bool bakeDepthTie(const omk::Geometry* g, std::span<const omk::Draw> order) override;
    // THE UPSCALER (`upscale.h`, `todo/enhancements.md` 18): asked for here,
    // made in `init` once the device exists - DLSS falling back to FSR, FSR
    // to the native frame, each with a line saying why.
    bool setUpscaler(int kind, int quality) override {
        if (dev_) return false;
        upKind_ = kind < 0 ? 0 : kind > 2 ? 2 : kind;
        upQuality_ = quality < 0 ? 0 : quality > 4 ? 4 : quality;
        return true;
    }

    // THE POST CHAIN (`todo/enhancements.md` 22): asked for here, its passes
    // made on first use (`makePost`), HDR decided with the swapchain
    bool setPostProcess(const PostProcess& p) override {
        if (dev_) return false;
        postReq_ = p;
        postReq_.grade = p.grade < 0 ? 0 : p.grade > 4 ? 4 : p.grade;
        postWant_ = p.bloom || p.dof || postReq_.grade > 0 || p.hdr;
        softWant_ = p.softParticles;   // decided in `makeTarget` (not under MSAA)
        volWant_ = p.volumetrics;
        return true;
    }
    // THE REMASTER'S CITY GRADE (`todo/remaster.md` step 2; the shower's overcast rides on it too): a 3D
    // colour LUT of n^3 RGB entries, red fastest (`Renderer::setGrade`), applied FIRST in the post chain
    // (`post.frag` -DOMK_DX, before this backend's own `colorgrade`, as Vulkan's remaster pass runs
    // before its chain). Asked before `init` with a null table it only WANTS the chain (the grades
    // arrive per set, later); null or n < 2 afterwards is off.
    bool setGrade(const float* rgb, int n) override;
    // what this backend can draw of the remaster's requests (`ui/omkoptions.h` offers only those)
    unsigned takes() const override {
        return omk::kTakesMultisample | omk::kTakesTextureFilter | omk::kTakesSupersample |
               omk::kTakesAnisotropy | omk::kTakesShadowMap | omk::kTakesPixelLights | omk::kTakesGrade |
               omk::kTakesBodyTint | omk::kTakesMist | omk::kTakesRain3d | omk::kTakesTrilinear;
    }
    // the body light: `Draw::lightTint` rides in the pose buffer's tint vec4 (`writePose`, `scene.vert`)
    bool setBodyTint(bool) override { return true; }
    bool drawsBodyTint() const override { return true; }
    bool softParticlesLive() const override { return softOn_; }
    bool volumetricsLive() const override { return volOn_; }
    // FRAME GENERATION (`todo/enhancements.md` 27): asked for here, decided in
    // `init` (a window, one sample a pixel), its passes made on first use
    bool setFrameGeneration(bool on) override {
        if (dev_) return false;
        fgWant_ = on;
        return true;
    }

    int samples() const { return static_cast<int>(samples_); }
    int maxSamples() const;
    const char* device() const { return devName_.c_str(); }

    // ---- the window (optional; nothing here runs offscreen) ----
    void attachWindow(HWND w) { hwnd_ = w; }
    bool presentDirect();
    bool presentSurface(const omk::Surface& s);
    bool presentWorld(int vy, int vh);
    bool worldPicture(int vy, int vh, std::vector<unsigned char>& rgba);
    bool presentOverlay(const omk::Surface& s, const std::uint8_t* mask, const float fade[4], int vy, int vh);
    // THE WINDOW'S OWN PICTURE (`OMK_WINDUMP`): the next present copies
    // the swapchain's back buffer - what the window shows - out before it goes
    bool windowPicture(std::vector<unsigned char>& rgba, int& w, int& h);
    void captureWindow() { winCapture_ = true; }
    bool resize(int w, int h);

private:
    bool pickDevice();
    bool makeTarget();
    void killTarget();
    bool makePipelines();
    bool makeSwapchain();
    void killSwapchainBuffers();
    bool swapchainBuffers();
    void flush();                       // signal, wait, report the debug layer
    void flushQueue(ID3D12CommandQueue* q);
    void drainDebug();
    ID3D12GraphicsCommandList* oneShotBegin();
    void oneShotEnd(ID3D12CommandQueue* q = nullptr);
    void openFrameList();
    ID3D12Resource* makeBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, const char* tag);
    ID3D12Resource* makeImage(DXGI_FORMAT fmt, UINT w, UINT h, UINT16 layers, UINT16 mips,
                              UINT samples, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES init,
                              const D3D12_CLEAR_VALUE* clear, const char* tag);
    void freeRes(ID3D12Resource*& r) {
        if (!r) return;
        OMK_GPU_FREE(omk::prof::gpuKey(omk::prof::kD3dResource, r));
        r->Release();
        r = nullptr;
    }
    ID3D12RootSignature* makeRootSig(const D3D12_ROOT_SIGNATURE_DESC& d, const char* what);
    ID3D12PipelineState* makePso(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, const char* what);
    bool uploadGeometry(const omk::Geometry* g);
    void resolveTies(const omk::Draw& d);
    void setViewport(const omk::View& view);
    bool writePose(const omk::Draw& d, UINT64& off);
    omk::Draw cpuPose(const omk::Draw& d);
    void submitOne(const omk::Draw& d, bool posed, UINT64 poseOff);
    bool makeSsao();
    void runSsao();
    // HYBRID RAY TRACING ("HYBRID RAY TRACING" below)
    struct RtBlas;
    ID3D12Resource* rtBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, bool uav, D3D12_RESOURCE_STATES st,
                             const char* tag);
    bool rtMapped(UINT64 bytes, ID3D12Resource*& r, void*& p, UINT64& cap);
    bool rtInit();
    void rtBind(const omk::View& view);
    // what goes into a BLAS is made by `o3de/rtscene.h`, for both backends
    using RtTri = omk::rt::Tri;
    using RtRun = omk::rt::Run;
    using RtBodyNote = omk::rt::BodyNote;
    bool rtFill(RtBlas& b, const std::vector<RtTri>& tri);
    RtBlas* rtSetFor(const omk::Geometry* g);
    void rtNote(const omk::Draw& d);
    void rtDrop(RtBlas& b);
    void rtTallyCopy(ID3D12GraphicsCommandList* l);
    void rtTallyRead();
    void rtBuild();
    void rtAfter();
    void rtKill();
    bool makePresentPass();
    // (`over`: FRAME GENERATION's in-between picture in the display source's place)
    bool runPresentPass(int vy, int vh, Res* over = nullptr);
    // (`capture`: a real frame, which `OMK_WINDUMP`'s picture is taken from)
    bool presentImage(Res& src, UINT srv, int sw, int sh, int texW, int texH, bool capture = true);
    void recordBlit(ID3D12GraphicsCommandList* l, Res& src, UINT srv, int sw, int sh, int texW, int texH, UINT idx);
    void ovDraw(const float fade[4]);
    // FRAME GENERATION ("FRAME GENERATION" below)
    bool fgStart();
    bool makeFg();
    void fgCapture();
    void fgTick();
    bool fgMidFrame();
    void fgCpuFrame();
    void fgInterpolate(int n, int p, float k);
    void fgProbe(int n, int p, int m);
    bool fgRead(Res& img, std::vector<unsigned char>& out);
    bool fgPresentReal(bool mid);
    void fgSettle();
    void fgThreadMain();
    void fgBlitPresent();
    void fgReport();
    bool uploadToColour(const unsigned char* rgba);   // w_ x h_ RGBA8 into the display image's top-left
    // THE UPSCALER SEAM (the header): the image the display half reads - the
    // upscaler's display-size output when one runs, else the scene's colour
    // at the render size - with its view, its size and the supersample the
    // display half still has to fold.
    Res& upscaleSource() { return up_ ? upOut_ : colour_; }
    UINT sourceSlot() const { return up_ ? kSlotUpOut : kSlotColour; }
    int sourceW() const { return up_ ? w_ : rw_; }
    int sourceH() const { return up_ ? h_ : rh_; }
    int sourceSs() const { return up_ ? 1 : ss_; }
    // a display-size length at the RENDER size (the letterbox, SSAO's viewport)
    int toRenderX(int v) const { return w_ > 0 ? v * rw_ / w_ : v; }
    int toRenderY(int v) const { return h_ > 0 ? v * rh_ / h_ : v; }
    bool makeUpscaler();
    void upscale();
    void motionStats();
    // THE POST CHAIN: its passes and images, the chain itself (on the frame's
    // list, after the upscaler), and what the display half reads once it ran
    bool makePost();
    void runPost();
    // SOFT PARTICLES: the scene pass's depth bound read-only (and readable) or
    // writable again; VOLUMETRIC LAMP LIGHT: its passes, made on first use
    void depthReadOnly(bool ro);
    bool makeVol();
    void runVol();
    bool outputIsHdr(float& peakNits);
    Res& displaySource() { return postLive_ ? postImg_ : upscaleSource(); }
    UINT displaySlot() const { return postLive_ ? kSlotPostOut : sourceSlot(); }
    int displaySs() const { return postLive_ ? 1 : sourceSs(); }

    static void geometryGone(void* self, const omk::Geometry* g);

    // ---- the device ----
    IDXGIFactory4*             factory_ = nullptr;
    IDXGIAdapter1*             adapter_ = nullptr;
    ID3D12Device*              dev_ = nullptr;
    ID3D12CommandQueue*        queue_ = nullptr;
    // FRAME GENERATION's PRESENT QUEUE: the swapchain's, high priority, so a
    // deferred real frame's blit need not wait behind the next frame's work
    ID3D12CommandQueue*        presQ_ = nullptr;
    ID3D12CommandQueue* swapQueue() const { return presQ_ ? presQ_ : queue_; }
    ID3D12CommandAllocator*    alloc_ = nullptr;     // the frame's list
    ID3D12GraphicsCommandList* list_ = nullptr;
    ID3D12CommandAllocator*    oneAlloc_ = nullptr;  // one-shot work: uploads, copies, presents
    ID3D12GraphicsCommandList* oneList_ = nullptr;
    ID3D12Fence*               fence_ = nullptr;
    UINT64                     fenceVal_ = 0;
    HANDLE                     fenceEvent_ = nullptr;
    ID3D12InfoQueue*           info_ = nullptr;      // OMK_D3D12_DEBUG only
    long                       dbgErrors_ = 0, dbgWarnings_ = 0;
    std::string                devName_;
    DescHeap                   srv_, rtv_, dsv_;

    // ---- the sizes (the header's "two sizes") ----
    int w_ = 0, h_ = 0;      // DISPLAY: what `readback()` and the present answer with
    int rw_ = 0, rh_ = 0;    // RENDER: w_ * ss_ by h_ * ss_
    int ss_ = 1;
    int srcW_ = 0, srcH_ = 0;   // the live region of `colour_` for the blit (vkrender.cpp)
    int wantSamples_ = 1;
    UINT samples_ = 1;
    int filter_ = 0, aniso_ = 1;
    bool anisoOn_ = false;
    bool resizing_ = false;

    // ---- the frame's targets ----
    Res colour_;             // single-sample, every consumer reads it
    Res msColour_;           // MSAA: what the pipelines draw into, resolved into colour_
    Res depth_;              // D32S8, sampleable (SSAO, an upscaler)
    UINT rtvColour_ = 0, rtvMs_ = 1, rtvAo_ = 2, rtvPres_ = 3, rtvMotion_ = 4;   // fixed RTV slots
    UINT rtvPost_ = 5, rtvBloom0_ = 6;                           // the post chain's (6 levels)
    static constexpr UINT kRtvSwap = 6 + kBloomLevels;           // ...and the swapchain's, after them
    UINT dsvDepth_ = 0;                                          // DSV 0; the shadow layers 1..3
    ID3D12Resource* readBuf_ = nullptr;
    UINT64 readPitch_ = 0;
    ID3D12Resource* upBuf_ = nullptr;   // a w_ x h_ picture going up (presentSurface)
    UINT64 upPitch_ = 0;

    // ---- the pipelines ----
    ID3D12RootSignature* sceneRs_ = nullptr;
    ID3D12PipelineState* pipe_[3] = {};
    // DEPTH OF FIELD'S TRANSLUCENT SUBJECT: a blended body (a hologram, a ghost)
    // writes no depth, so the post pass saw the wall behind it and blurred it.
    // On a frame that will take depth of field, the blended pipelines' twins
    // mark such a body's pixels in the STENCIL (`kSubjectStencil`) - the picture
    // and the depth untouched - and `post.frag` keeps them at the focus.
    ID3D12PipelineState* pipeSubj_[3] = {};
    ID3D12PipelineState* pipeCountSubj_[3] = {};
    bool curBody_ = false;             // the draw being submitted is a posed body's
    // The mirror's stencil states, made as Vulkan makes them (`pipeStencil_`,
    // `pipeDepthReset_`, `pipeRefl_`). `drawMirrorScene` draws the ORIGINAL's
    // way - reflected pass, then the real one over it, depth confining - as
    // Vulkan's does, so like Vulkan's these are held for the stencil
    // construction rather than used by it; the hooks are `forcePso_` and
    // `reflStencil_`, which nothing sets today, exactly as there.
    ID3D12PipelineState* pipeStencil_ = nullptr;
    ID3D12PipelineState* pipeDepthReset_ = nullptr;
    ID3D12PipelineState* pipeRefl_[3] = {};
    ID3D12PipelineState* forcePso_ = nullptr;
    bool reflStencil_ = false;
    ID3D12PipelineState* bound_ = nullptr;   // the PSO last set on the frame's list

    // ---- the MAPPED shadow (`todo/enhancements.md` 6) ----
    Res shadow_;
    ID3D12RootSignature* shRs_ = nullptr;
    ID3D12PipelineState* shPipe_ = nullptr;
    ID3D12Resource* shUbo_ = nullptr;  void* shUboPtr_ = nullptr;
    ID3D12Resource* litUbo_ = nullptr; void* litUboPtr_ = nullptr;
    bool listOpen_ = false;            // the frame's list, opened by shadowPass or begin
    bool shadowLive_ = false;
    int litCount_ = 0;
    float shimmerClock_ = 0.0f;
    std::uint32_t texScale_ = 0;   // the bound texture's pack scale, for the push block
    bool enhOff_ = false;          // `View::enhOff`: the original's look shows - ours left out
    bool dither_ = true;
    int colourDepth_ = 16;   // the View's colour depth: 32 = the present pass leaves the picture 8-bit

    // ---- THE POST CHAIN (`todo/enhancements.md` 22) ----
    PostProcess postReq_{};
    bool postWant_ = false;            // anything asked at all
    bool lutWant_ = false;             // the remaster's grade is coming (`setGrade` before `init`)
    int lutN_ = 0;                     // the LUT's side, 0 none
    ID3D12Resource* lutImg_ = nullptr;
    bool postLive_ = false;            // the chain ran on THIS frame: the display half reads `postImg_`
    // (`omk::postfx::Focus`: the frame's picture rectangle, cinematic or not, the
    // depth of field's focus - the nearest body, the View's named subjects -
    // shared with the Vulkan backend)
    omk::postfx::Focus foc_;
    Res postImg_;                      // display size, `presFmt_`
    UINT64 postPitch_ = 0;
    Res bloom_[kBloomLevels];          // R11G11B10_FLOAT, half size and down
    int bloomW_[kBloomLevels] = {}, bloomH_[kBloomLevels] = {};
    ID3D12RootSignature* bloomRs_ = nullptr;
    ID3D12RootSignature* postRs_ = nullptr;
    ID3D12PipelineState* bloomDownPipe_ = nullptr;
    ID3D12PipelineState* bloomUpPipe_ = nullptr;
    ID3D12PipelineState* postPipe_ = nullptr;
    // HDR OUTPUT: an scRGB swapchain (FP16, linear, BT.709) when the display reports HDR
    bool hdrOn_ = false;
    float hdrPeak_ = 1000.0f;          // the display's MaxLuminance, nits
    static constexpr float kPaperWhite = 200.0f;   // nits: where the SDR picture's white goes
    DXGI_FORMAT swapFmt_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT presFmt_ = DXGI_FORMAT_R8G8B8A8_UNORM;   // the present image's, the overlay's, the chain's
    // `OMK_POST_TIMING`: two timestamps around the chain, read after the frame
    ID3D12QueryHeap* postQ_ = nullptr;
    ID3D12Resource* postQBuf_ = nullptr;
    double postMsSum_ = 0.0;
    int postMsN_ = 0;
    bool postTimed_ = false;

    // ---- SOFT PARTICLES (`todo/enhancements.md` 23; off unless asked) ----
    bool softWant_ = false, softOn_ = false;
    bool depthRo_ = false;             // the scene pass's depth is bound read-only now
    ID3D12PipelineState* pipeSoft_[3] = {};   // [1] additive, [2] multiply: `scene.frag` -DOMK_SOFT
    UINT softParam_ = 0;               // the scene root's table holding the depth (t0 space6)
    static constexpr UINT kDsvDepthRo = 1 + kShadowLayers;   // the depth's READ-ONLY view
    float softRange_ = 24.0f;          // the fade's depth range, world units (~0.6 m)
    long softDraws_ = 0;               // sprite batches drawn soft this frame
    bool softTold_ = false;

    // ---- VOLUMETRIC LAMP LIGHT (`todo/enhancements.md` 24; off unless asked) ----
    bool volWant_ = false, volOn_ = false;
    Res vol_;                          // half the render size, RGBA16F: the light, the depth / 1000
    int volW_ = 0, volH_ = 0;
    ID3D12RootSignature* volRs_ = nullptr;
    ID3D12PipelineState* volPipe_ = nullptr;
    ID3D12PipelineState* volMixPipe_ = nullptr;
    ID3D12Resource* volUbo_ = nullptr; void* volUboPtr_ = nullptr;
    int volCount_ = 0;                 // this frame's lamps
    long volFrame_ = 0;
    float volEye_[3] = {}, volS_[3] = {}, volU_[3] = {}, volF_[3] = {};
    float volTanH_ = 1.0f, volTanV_ = 1.0f;
    float volDensity_ = 0.0011f;       // the air's density
    float volCore_ = 0.05f;            // a lamp's core radius, as a share of its reach
    float volReach_ = 0.5f;            // ...and its glow's reach, likewise
    bool volTold_ = false;
    float volSig_ = -1.0f;             // the lamps and camera last said (`vol.lamp` events)
    ID3D12QueryHeap* volQ_ = nullptr;  // `OMK_VOL_TIMING`
    ID3D12Resource* volQBuf_ = nullptr;
    double volMsSum_ = 0.0;
    int volMsN_ = 0;
    bool volTimed_ = false;

    // ---- the textures ----
    // `scale`: THE TEXTURE PACK's k (`o3de/texpack.h`) - the image is k x the
    // original, the UVs stay in the original's units (`scene.frag`, `rt.glsl`)
    struct Tex { ID3D12Resource* img = nullptr; UINT srv = UINT(-1); float su = 1.0f, sv = 1.0f; bool packed = false; };
    std::vector<Tex> tex_;
    Tex white_;
    struct Uploaded { Tex t; omk::PixelBuffer idx, pal; bool used = false; };
    std::map<std::tuple<const std::uint8_t*, const std::uint8_t*, int, int>, Uploaded> uploaded_;
    // a slot's texture word for a ray-tracing record: the heap index, and the
    // pack's k - 1 in bits 24.. (`rt.glsl` `rtTexel`; the heap is 4096)
    UINT rtTexWord(std::size_t slot) const {
        if (slot >= tex_.size() || tex_[slot].srv == UINT(-1)) return white_.srv;
        return tex_[slot].srv | omk::rtTexScaleBits(tex_[slot].su, tex_[slot].sv);
    }

    // ---- the geometry: upload-heap buffers, mapped for their life ----
    struct Vbo { ID3D12Resource* res = nullptr; GpuVert* ptr = nullptr; std::size_t cap = 0; };
    std::map<const omk::Geometry*, Vbo> vbo_;
    std::map<const omk::Geometry*, std::size_t> vboN_;
    std::map<const omk::Geometry*, std::uint64_t> vboRev_;
    std::vector<ID3D12Resource*> deadBufs_;
    bool releaseOn_ = true;
    long released_ = 0;
    std::map<const omk::Geometry*, omk::DepthTie> tie_;
    std::map<const omk::Geometry*, std::vector<std::uint8_t>> baked_;
    GpuVert vertOf(const omk::Geometry& g, std::size_t i, const std::vector<std::uint8_t>* lose) const {
        const auto& c = g.corners[i];
        const float mesh = g.cornerMesh.size() == g.corners.size() ? static_cast<float>(g.cornerMesh[i]) : -1.0f;
        GpuVert v{c.x, c.y, c.z, c.u, c.v, c.r, c.g, c.b, c.nx, c.ny, c.nz, c.phase, mesh};
        if (lose && i < lose->size() && (*lose)[i]) v.phase -= kTieLoserPhase;
        return v;
    }
    const std::vector<std::uint8_t>* bakedOf(const omk::Geometry* g) const {
        const auto it = baked_.find(g);
        return it == baked_.end() ? nullptr : &it->second;
    }

    // ---- THE POSE BUFFER (Vulkan's set 2): the same blocks, a root SRV ----
    static constexpr int kPoseLights = 16, kPoseMaxMeshes = 256;
    static constexpr UINT64 kPoseBytes = 8u << 20;
    static constexpr UINT64 kPoseRange = (1 + 2 * kPoseLights + 3 * kPoseMaxMeshes + 1) * 16;   // (+ the tint)
    static constexpr UINT64 kPoseAlign = 256;
    ID3D12Resource* poseBuf_ = nullptr;
    void* posePtr_ = nullptr;
    UINT64 poseUsed_ = 0;
    std::map<std::pair<const float*, const float*>, UINT64> poseCache_;
    std::map<std::pair<const omk::Geometry*, int>, omk::Geometry> cpuPosed_;
    std::map<std::pair<const float*, const omk::Geometry*>, int> cpuPoseSlot_;
    std::map<const omk::Geometry*, int> cpuPoseCount_;
    std::uint64_t cpuPoseRev_ = 0;

    // ---- SSAO (`todo/enhancements.md` 15) ----
    bool wantSsao_ = false, ssaoOn_ = false;
    Res ao_;
    ID3D12RootSignature* aoRs_ = nullptr;
    ID3D12PipelineState* aoPipe_ = nullptr;
    ID3D12PipelineState* aoBlurPipe_ = nullptr;
    float viewTanH_ = 1.0f, viewTanV_ = 1.0f;
    float aoTanH_ = 1.0f, aoTanV_ = 1.0f;
    int aoVw_ = 0, aoVh_ = 0;

    // ---- HYBRID RAY TRACING (`todo/enhancements.md` 19; off unless asked) ----
    struct RtBlas {
        ID3D12Resource* as = nullptr;  UINT64 asCap = 0;                                     // the BLAS
        ID3D12Resource* pos = nullptr; float* posPtr = nullptr; UINT64 posCap = 0;           // corners, float3
        ID3D12Resource* rec = nullptr; std::uint32_t* recPtr = nullptr; UINT64 recCap = 0;   // 16 words a triangle
        UINT recSrv = UINT(-1);                     // the records' raw view, in the heap
        std::uint32_t nOpaque = 0, nCut = 0;        // opaque triangles first, then the cutout ones
        std::uint64_t rev = ~0ull;                  // a set's: the geometry's revision it was made from
        std::vector<std::int32_t> triOf;            // a set's: corner / 3 -> its record, -1 none
        std::map<std::size_t, std::pair<std::size_t, UINT>> texAt;   // draw start -> count, texture written
        std::uint64_t texGen = 0;
        long seen = 0;                              // the frame it was last drawn
        bool built = false;
    };
    bool wantRt_ = false, rtOn_ = false;
    bool wantRtRefl_ = false, rtReflOn_ = false;   // RAY-TRACED REFLECTIONS (row 26)
    long rtFloorDraws_ = 0;                        // ...the ground draws this frame, for the log
    bool rtBodyDraw_ = false;                       // the draw going to `submitOne` is a body
    bool rtCounting_ = false;                       // this frame draws `pipeCount_` (one in 60)
    long rtCounted_ = 0;                            // ...frames so drawn
    ID3D12PipelineState* pipeCount_[3] = {};
    ID3D12Device5* dev5_ = nullptr;
    ID3D12CommandAllocator* rtAlloc_ = nullptr;     // the builds' own list, run just before the frame's
    ID3D12GraphicsCommandList4* rtList_ = nullptr;
    bool rtRecorded_ = false;
    std::map<const omk::Geometry*, RtBlas> rtSet_;
    RtBlas rtBodyAll_;                              // EVERY body of the frame, one BLAS
    omk::rt::BodyNotes rtNotes_;
    static constexpr UINT kRtMaxInst = 2048;
    ID3D12Resource* rtTlas_ = nullptr;
    UINT64 rtTlasScratch_ = 0;
    ID3D12Resource* rtInstDesc_ = nullptr; void* rtInstDescPtr_ = nullptr;
    ID3D12Resource* rtInstTab_ = nullptr;  void* rtInstTabPtr_ = nullptr;
    ID3D12Resource* rtScratch_ = nullptr;  UINT64 rtScratchCap_ = 0;
    ID3D12Resource* rtFrameCb_ = nullptr;  void* rtFramePtr_ = nullptr;
    ID3D12Resource* rtCount_ = nullptr;
    ID3D12Resource* rtCountRead_ = nullptr;
    std::uint64_t texGen_ = 1;                      // bumped when a texture's view goes
    long rtFrame_ = 0;
    float rtEye_[3] = {}, rtS_[3] = {}, rtU_[3] = {}, rtF_[3] = {};
    int rtInstances_ = 0, rtSets_ = 0, rtBodies_ = 0, rtBodyCount_ = 0, rtToldInst_ = -1, rtTold_ = 0;
    long rtSetBuilds_ = 0, rtBodyBuilds_ = 0, rtBodyRefits_ = 0, rtBodyRebuilt_ = 0;
    std::uint64_t rtBodySig_ = 0;
    long long rtSetTris_ = 0, rtBodyTris_ = 0, rtAoRays_ = 0;
    std::uint32_t rtCounts_[4] = {};
    double rtCpuMs_ = 0.0;

    // ---- THE PRESENT PASS (todo/optimization.md step 4b) ----
    Res presImg_;
    ID3D12RootSignature* presRs_ = nullptr;
    ID3D12PipelineState* presPipe_ = nullptr;
    ID3D12Resource* presRead_ = nullptr;
    UINT64 presPitch_ = 0;
    // ---- THE OVERLAY at colour depth 32 (`overlay.frag`, todo/enhancements.md 21) ----
    Res ovPic_, ovMask_;               // R16_UINT (the 565 words), R8_UNORM
    ID3D12Resource* ovBuf_ = nullptr;  // both planes going up, at the copy's row pitch
    UINT64 ovPicPitch_ = 0, ovMaskPitch_ = 0, ovMaskOff_ = 0;
    ID3D12RootSignature* ovRs_ = nullptr;
    ID3D12PipelineState* ovPipe_ = nullptr;
    bool makeOverlayPass();
    // ---- the window's picture, read back on request (`captureWindow`) ----
    bool winCapture_ = false;
    std::vector<unsigned char> winPic_;
    int winPicW_ = 0, winPicH_ = 0;

    // ---- the swapchain ----
    HWND hwnd_ = nullptr;
    IDXGISwapChain3* swap_ = nullptr;
    static constexpr UINT kSwapBuffers = 3;
    Res swapBuf_[kSwapBuffers];
    UINT swapW_ = 0, swapH_ = 0;
    ID3D12RootSignature* blitRs_ = nullptr;
    ID3D12PipelineState* blitPipe_ = nullptr;

    // ---- the frame ----
    Push push_{};
    bool fog_ = false;
    float fogStart_ = 0.0f, fogEnd_ = 0.0f;
    float fogColour_[3] = {0, 0, 0};
    float clearColour_[3] = {0, 0, 0};
    omk::RasterStats st_;
    omk::Surface fb_{1, 1, 0};
    bool recording_ = false;
    bool dirty_ = false;
    bool copyOwed_ = false;
    bool flipX_ = false;

    // ---- THE UPSCALER (`upscale.h`; off unless asked) ----
    int upKind_ = 0, upQuality_ = 1;          // asked: 0 off, 1 FSR, 2 DLSS
    std::unique_ptr<omk::d3dup::Upscaler> up_;   // ...and running
    Res motion_;               // render size, R16G16_FLOAT: the scene pass's second target
    Res upOut_;                // display size, R8G8B8A8, UAV: the upscaler's output
    PushMv pm_{};              // the push block with the jitter and the previous camera
    float curMain_[16] = {};   // this frame's main view-projection (unjittered)
    float prevMain_[16] = {};  // ...and the last frame's
    bool havePrev_ = false;
    float frameEye_[3] = {}, prevEye_[3] = {}, prevFwd_[3] = {};
    float mainS_[3] = {}, mainU_[3] = {}, mainF_[3] = {};   // the frame's camera basis
    float mainA_ = 1.0f, mainB_ = 0.0f;
    int jitterIndex_ = 0;
    float jitterPx_[2] = {};   // this frame's, in render pixels
    bool resetOwed_ = true;    // a camera cut, a new area, a resize: drop the history
    long upFrames_ = 0;
    // a POSED body's affines, by its `meshPose` pointer (stable frame to
    // frame): this frame's, and the last frame's that `writePose` puts after
    // the current ones for `scene.vert`'s previous position
    std::map<const float*, std::vector<float>> curAffines_, prevAffines_;
    // `OMK_MOTION_STATS`: the motion target read back, for a check
    ID3D12Resource* motionRead_ = nullptr;
    UINT64 motionPitch_ = 0;
    bool mvOn_ = false;        // the scene pass writes `motion_`: an upscaler, or FRAME GENERATION

    // ---- FRAME GENERATION (`todo/enhancements.md` 27; off unless asked) ----
    bool fgWant_ = false, fgOn_ = false;
    static constexpr int kFgMaxSlots = 3;
    static constexpr UINT kRtvFg = kRtvSwap + kSwapBuffers + 1;   // the kept frames' RTVs, then the in-between's
    int fgSlots_ = 2;                  // the frames kept (3 for the probe's N-2/N pair)
    Res fgCol_[kFgMaxSlots];           // display size, `presFmt_`: a frame's finished world picture
    Res fgAux_[kFgMaxSlots];           // display size, RGBA16F: its motion (display px), view depth / 4
    Res fgHead_[2];                    // display size, R32_UINT, UAVs: the splat heads, frame N's and N-1's
    Res fgMid_;                        // display size, `presFmt_`: the in-between picture
    bool fgCut_[kFgMaxSlots] = {};     // the frame was a camera cut (no motion to its predecessor)
    int fgPicW_[kFgMaxSlots] = {}, fgPicH_[kFgMaxSlots] = {};
    long fgSeq_ = 0, fgShownSeq_ = 0;  // frames captured; the newest a present has used
    ID3D12RootSignature* fgRs_ = nullptr;
    ID3D12PipelineState* fgCapPipe_ = nullptr;
    ID3D12PipelineState* fgSplatPipe_ = nullptr;
    ID3D12PipelineState* fgResolvePipe_ = nullptr;
    // the PACING: the interval between presents, and the real frame deferred to
    // half of it on a thread of its own (its own list and fence; the frame's
    // thread waits for it - `fgSettle` - before it touches the swapchain again)
    double fgLastCall_ = 0.0, fgInterval_ = 0.0, fgLatSum_ = 0.0;
    double fgLateSum_ = 0.0;           // ...the deferred blit's lateness against its due time
    // THE DISPLAY'S ROOM: the frame's own work (from the last present's return
    // to this present's call - pacer, simulation, drawing; not the presents),
    // and whether it spans enough refreshes for an in-between frame to have
    // one of its own. Without the room a v-synced in-between present only
    // blocks, and slows the real frames (`fgMidFrame`).
    double fgCallAt_ = 0.0, fgPresentDone_ = 0.0, fgWork_ = 0.0;
    bool fgRoom_ = false;
    bool fgAlways_ = false;            // `OMK_FRAMEGEN_ALWAYS`: the harness's, whatever the room
    long fgNoRoom_ = 0;
    long fgOnTime_ = 0;
    long fgReal_ = 0, fgMids_ = 0, fgCuts_ = 0, fgStale_ = 0, fgCpu_ = 0, fgDeferred_ = 0, fgHurried_ = 0;
    int fgRefresh_ = 0;
    std::thread fgThread_;
    std::mutex fgMu_;
    std::condition_variable fgCv_;
    bool fgPending_ = false, fgHurry_ = false, fgQuit_ = false;
    double fgDue_ = 0.0;
    ID3D12CommandAllocator* fgAlloc_ = nullptr;
    ID3D12GraphicsCommandList* fgList_ = nullptr;
    ID3D12Fence* fgFence_ = nullptr;
    UINT64 fgFenceVal_ = 0;
    HANDLE fgEvent_ = nullptr;
    // `OMK_FRAMEGEN_TIMING`: timestamps round the in-between passes [0, 1] and the capture [2, 3]
    ID3D12QueryHeap* fgQ_ = nullptr;
    ID3D12Resource* fgQBuf_ = nullptr;
    double fgMsSum_ = 0.0, fgCapMsSum_ = 0.0;
    int fgMsN_ = 0;
    bool fgCapTimed_ = false;
    const char* fgProbe_ = nullptr;    // `OMK_FRAMEGEN_PROBE`: the files' prefix
};

// ============================================================ the device

ID3D12Resource* D3D12Renderer::makeBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, const char* tag) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 1;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // an UPLOAD heap must be created readable, a READBACK heap writable
    const D3D12_RESOURCE_STATES st = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                   : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                      : D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) {
        std::fprintf(stderr, "d3d12: buffer of %llu bytes (%s) failed\n",
                     static_cast<unsigned long long>(bytes), tag);
        return nullptr;
    }
    OMK_GPU_ALLOC(tag, omk::prof::gpuKey(omk::prof::kD3dResource, r), bytes);
    return r;
}

ID3D12Resource* D3D12Renderer::makeImage(DXGI_FORMAT fmt, UINT w, UINT h, UINT16 layers, UINT16 mips,
                                         UINT samples, D3D12_RESOURCE_FLAGS flags,
                                         D3D12_RESOURCE_STATES init, const D3D12_CLEAR_VALUE* clear,
                                         const char* tag) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = layers; d.MipLevels = mips;
    d.Format = fmt;
    d.SampleDesc.Count = samples;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, init, clear, IID_PPV_ARGS(&r)))) {
        std::fprintf(stderr, "d3d12: image %ux%u (%s) failed\n", w, h, tag);
        return nullptr;
    }
    [[maybe_unused]] const D3D12_RESOURCE_ALLOCATION_INFO ai = dev_->GetResourceAllocationInfo(0, 1, &d);
    OMK_GPU_ALLOC(tag, omk::prof::gpuKey(omk::prof::kD3dResource, r), ai.SizeInBytes);
    return r;
}

// Every list is submitted and WAITED, as every Vulkan submit here is: the
// frame is synchronous, which is what lets the CPU write the vertex, pose and
// uniform buffers in place at the next `begin` with nothing in flight.
void D3D12Renderer::flushQueue(ID3D12CommandQueue* q) {
    ++fenceVal_;
    q->Signal(fence_, fenceVal_);
    if (fence_->GetCompletedValue() < fenceVal_) {
        fence_->SetEventOnCompletion(fenceVal_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void D3D12Renderer::flush() {
    if (presQ_) flushQueue(presQ_);
    flushQueue(queue_);
    const HRESULT why = dev_->GetDeviceRemovedReason();
    if (FAILED(why)) {
        static bool said = false;
        if (!said) std::fprintf(stderr, "d3d12: DEVICE REMOVED - 0x%08lx\n", static_cast<unsigned long>(why));
        said = true;
    }
    drainDebug();
}

// `OMK_D3D12_DEBUG=1`: every message the debug layer stored, printed and
// counted - the D3D12 counterpart of running under the validation layers.
void D3D12Renderer::drainDebug() {
    if (!info_) return;
    const UINT64 n = info_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        info_->GetMessage(i, nullptr, &len);
        std::vector<unsigned char> buf(len);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(info_->GetMessage(i, m, &len))) continue;
        const char* sev = m->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION"
                        : m->Severity == D3D12_MESSAGE_SEVERITY_ERROR      ? "ERROR"
                        : m->Severity == D3D12_MESSAGE_SEVERITY_WARNING    ? "WARNING"
                        : m->Severity == D3D12_MESSAGE_SEVERITY_INFO       ? "INFO" : "MESSAGE";
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++dbgErrors_;
        else if (m->Severity == D3D12_MESSAGE_SEVERITY_WARNING) ++dbgWarnings_;
        else continue;   // INFO and MESSAGE: chatter, not findings
        std::fprintf(stderr, "d3d12 debug [%s] (id %d) %.*s\n", sev, static_cast<int>(m->ID),
                     static_cast<int>(m->DescriptionByteLength), m->pDescription);
    }
    info_->ClearStoredMessages();
}

ID3D12GraphicsCommandList* D3D12Renderer::oneShotBegin() {
    oneAlloc_->Reset();
    oneList_->Reset(oneAlloc_, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    oneList_->SetDescriptorHeaps(1, heaps);
    return oneList_;
}

void D3D12Renderer::oneShotEnd(ID3D12CommandQueue* q) {
    oneList_->Close();
    ID3D12CommandList* l[] = {oneList_};
    (q ? q : queue_)->ExecuteCommandLists(1, l);
    if (q && q != queue_) {
        flushQueue(q);
        drainDebug();
    } else {
        flush();
    }
}

// The frame's list is opened ONCE, by whichever of `shadowPass` and `begin`
// runs first - the depth pass from the light is recorded before the frame's
// own pass, as Vulkan records its render passes.
void D3D12Renderer::openFrameList() {
    if (listOpen_) return;
    alloc_->Reset();
    list_->Reset(alloc_, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    list_->SetDescriptorHeaps(1, heaps);
    bound_ = nullptr;
    listOpen_ = true;
}

bool D3D12Renderer::pickDevice() {
    // THE DEBUG LAYER, only when asked: it costs, and it must be enabled
    // before the device exists.
    const bool debug = omk::envFlag("OMK_D3D12_DEBUG");
    if (debug) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            dbg->Release();
            std::printf("d3d12: debug layer ON (OMK_D3D12_DEBUG)\n");
        } else {
            std::fprintf(stderr, "d3d12: OMK_D3D12_DEBUG set but no debug layer (Graphics Tools)\n");
        }
    }
    DXCHECK(CreateDXGIFactory2(debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_)),
            "CreateDXGIFactory2");
    // The HIGH-PERFORMANCE adapter first (a laptop's discrete GPU), never the
    // software rasteriser: a WARP frame would be a third renderer, not this one.
    IDXGIFactory6* f6 = nullptr;
    factory_->QueryInterface(IID_PPV_ARGS(&f6));
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        const HRESULT hr = f6 ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                IID_PPV_ARGS(&a))
                              : factory_->EnumAdapters1(i, &a);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 ad{};
        a->GetDesc1(&ad);
        HRESULT made = E_FAIL;
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            made = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_));
            // a hardware adapter passed over is said, with why: which GPU drew
            // the frame is the first thing a reader of a log needs
            if (FAILED(made))
                std::fprintf(stderr, "d3d12: adapter %u (%ls) passed over - D3D12CreateDevice 0x%08lx\n", i,
                             ad.Description, static_cast<unsigned long>(made));
        }
        if (SUCCEEDED(made)) {
            adapter_ = a;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof name, nullptr, nullptr);
            devName_ = name;
            break;
        }
        a->Release();
    }
    release(f6);
    if (!dev_) { std::fprintf(stderr, "d3d12: no hardware D3D12 device\n"); return false; }
    if (debug && SUCCEEDED(dev_->QueryInterface(IID_PPV_ARGS(&info_)))) {
        info_->SetMuteDebugOutput(TRUE);   // ours prints them; no duplicate on the debugger
        // ONE message is a performance HINT about a decision that is not
        // ours: the frame is cleared to the scene's FOG colour, which the
        // clock moves (`View::clearColour`), so no optimised clear value can
        // be given at creation and the layer says so on every clear. Denied
        // by its id, and nothing else is.
        D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = deny;
        info_->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    DXCHECK(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
    DXCHECK(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)), "allocator");
    DXCHECK(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&oneAlloc_)), "allocator");
    DXCHECK(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_, nullptr, IID_PPV_ARGS(&list_)),
            "CreateCommandList");
    DXCHECK(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, oneAlloc_, nullptr,
                                    IID_PPV_ARGS(&oneList_)), "CreateCommandList");
    list_->Close();
    oneList_->Close();
    DXCHECK(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent_) return false;
    if (!srv_.make(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, true, kSlotFirstFree) ||
        !rtv_.make(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvFg + 2 * kFgMaxSlots + 1, false, 0) ||
        !dsv_.make(dev_, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvDepthRo + 1, false, 0)) {
        std::fprintf(stderr, "d3d12: descriptor heaps\n");
        return false;
    }
    // Anisotropy, as Vulkan asks for its feature: only with the trilinear
    // mip chain, which is what it filters along.
    anisoOn_ = aniso_ > 1 && filter_ >= 2;
    if (aniso_ > 1 && !anisoOn_)
        std::fprintf(stderr, "d3d12: anisotropy %d ignored - it needs trilinear filtering (a mip chain)\n",
                     aniso_);
    return true;
}

int D3D12Renderer::maxSamples() const {
    if (!dev_) return 1;
    for (UINT n = 64; n > 1; n >>= 1) {
        bool ok = true;
        for (DXGI_FORMAT f : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT}) {
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
            q.Format = f; q.SampleCount = n;
            if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof q)) ||
                q.NumQualityLevels == 0)
                ok = false;
        }
        if (ok) return static_cast<int>(n);
    }
    return 1;
}

bool D3D12Renderer::makeTarget() {
    // THE SAMPLE COUNT, cut to what the colour AND depth-stencil formats both
    // allow (the mirror's stencil needs the same count). 1 unless asked.
    samples_ = 1;
    if (wantSamples_ > 1) {
        const int most = maxSamples();
        for (int n = wantSamples_; n > 1; n >>= 1)
            if (n <= most) { samples_ = static_cast<UINT>(n); break; }
        if (static_cast<int>(samples_) != wantSamples_ && !resizing_)
            std::fprintf(stderr, "d3d12: %dx MSAA is not supported here - using %ux\n", wantSamples_, samples_);
    }
    const bool msaa = samples_ > 1;
    const UINT rw = static_cast<UINT>(rw_), rh = static_cast<UINT>(rh_);

    // R8G8B8A8 rather than a 565 format - the readback quantises in software,
    // one rule in one place (`PORTING` A3). No optimised clear value: the
    // clear is the scene's FOG colour and changes with the clock.
    colour_.r = makeImage(DXGI_FORMAT_R8G8B8A8_UNORM, rw, rh, 1, 1, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE,
                          nullptr, "render targets");
    colour_.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!colour_.r) return false;
    if (msaa) {
        msColour_.r = makeImage(DXGI_FORMAT_R8G8B8A8_UNORM, rw, rh, 1, 1, samples_,
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                nullptr, "render targets");
        msColour_.st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (!msColour_.r) return false;
    }
    // A depth+STENCIL format, for the mirror's stencil (Vulkan's D32S8 first
    // choice; every D3D12 device has it), TYPELESS so the depth plane can be
    // read as R32_FLOAT. It is created SAMPLEABLE whether or not SSAO is on:
    // an upscaler reads the scene's depth too (the header's seam).
    D3D12_CLEAR_VALUE dc{};
    dc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dc.DepthStencil.Depth = 1.0f;
    depth_.r = makeImage(DXGI_FORMAT_R32G8X24_TYPELESS, rw, rh, 1, 1, samples_,
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                         &dc, "render targets");
    depth_.st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (!depth_.r) return false;
    // SOFT PARTICLES read the depth beside the test: a single-sample depth, so
    // not under MSAA (an upscaler turns MSAA off, so FSR and DLSS have them)
    softOn_ = softWant_ && !msaa;
    if (softWant_ && !resizing_) {
        if (softOn_) {
            const char* r = omk::envStr("OMK_SOFT_RANGE");
            if (r && std::atof(r) > 0.0) softRange_ = static_cast<float>(std::atof(r));
            std::printf("d3d12: soft particles on - an ENHANCEMENT: the effect sprites fade over %.0f units "
                        "where they meet the opaque depth\n", static_cast<double>(softRange_));
        } else {
            std::printf("d3d12: soft particles need a single-sample depth - off under %ux MSAA\n", samples_);
        }
        omk::event("softparticles", {{"on", softOn_ ? 1 : 0}, {"samples", static_cast<int>(samples_)}});
    }
    volOn_ = volWant_;
    if (volWant_ && !resizing_) {
        omk::postfx::VolTune vt{volDensity_, volCore_, volReach_};
        vt.fromEnv();
        volDensity_ = vt.density; volCore_ = vt.core; volReach_ = vt.reach;
        std::printf("d3d12: volumetric lamp light on - an ENHANCEMENT: the lamps' light scattered in the air, "
                    "half-size, added before the post chain (density %g, core %g and glow %g of the reach)\n",
                    static_cast<double>(volDensity_), static_cast<double>(volCore_),
                    static_cast<double>(volReach_));
    }
    // (ray tracing runs the AO pass too, its rays in place of the hemisphere)
    ssaoOn_ = wantSsao_ || rtOn_;
    if (wantSsao_ && !rtOn_ && !resizing_)
        std::printf("d3d12: SSAO on - an ENHANCEMENT: ambient occlusion from the depth buffer, "
                    "multiplied over the frame%s\n", msaa ? " (multisampled depth)" : "");

    {   // the views, into their FIXED slots
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(colour_.r, &rv, rtv_.cpu(rtvColour_));
        if (msaa) {
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            dev_->CreateRenderTargetView(msColour_.r, &rv, rtv_.cpu(rtvMs_));
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        dv.ViewDimension = msaa ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(depth_.r, &dv, dsv_.cpu(dsvDepth_));
        // ...and its READ-ONLY twin, which SOFT PARTICLES test against while
        // their shader reads the same depth through `kSlotDepth`
        dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        dev_->CreateDepthStencilView(depth_.r, &dv, dsv_.cpu(kDsvDepthRo));
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(colour_.r, &sv, srv_.cpu(kSlotColour));
        sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sv.ViewDimension = msaa ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        dev_->CreateShaderResourceView(depth_.r, &sv, srv_.cpu(kSlotDepth));
    }

    // ---- THE UPSCALER's two targets (`upscale.h`), only with one running:
    // the MOTION VECTORS at the render size - the scene pass's second target,
    // cleared to no motion - and the display-size OUTPUT the upscaler writes
    // through an unordered-access view, which the display half then reads in
    // place of `colour_` (`upscaleSource`).
    // (the motion vectors with FRAME GENERATION too, `mvOn_`, upscaler or not)
    if (mvOn_) {
        D3D12_CLEAR_VALUE mc{};
        mc.Format = DXGI_FORMAT_R16G16_FLOAT;
        motion_.r = makeImage(DXGI_FORMAT_R16G16_FLOAT, rw, rh, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, &mc, "render targets");
        motion_.st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (!motion_.r) return false;
        dev_->CreateRenderTargetView(motion_.r, nullptr, rtv_.cpu(rtvMotion_));
    }
    if (up_) {
        upOut_.r = makeImage(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, 1, 1,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE,
                             nullptr, "render targets");
        upOut_.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (!upOut_.r) return false;
        dev_->CreateShaderResourceView(upOut_.r, nullptr, srv_.cpu(kSlotUpOut));
    }

    // the readback, at the copy's own row pitch (256-aligned), of the image
    // the display half reads (`upscaleSource`)
    {
        D3D12_RESOURCE_DESC cd = upscaleSource().r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev_->GetCopyableFootprints(&cd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        readPitch_ = fp.Footprint.RowPitch;
        readBuf_ = makeBuffer(total, D3D12_HEAP_TYPE_READBACK, "readback");
        if (!readBuf_) return false;
    }
    fb_ = omk::Surface(w_, h_, 0);
    if (resizing_) return true;   // what follows is sized once and kept

    // ---- THE SHADOW MAP's own target: three layers, one per light ----
    D3D12_CLEAR_VALUE sc{};
    sc.Format = DXGI_FORMAT_D32_FLOAT;
    sc.DepthStencil.Depth = 1.0f;
    shadow_.r = makeImage(DXGI_FORMAT_R32_TYPELESS, kShadowSide, kShadowSide, kShadowLayers, 1, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                          &sc, "shadow map");
    shadow_.st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (shadow_.r) {
        for (int L = 0; L < kShadowLayers; ++L) {
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = DXGI_FORMAT_D32_FLOAT;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dv.Texture2DArray.FirstArraySlice = static_cast<UINT>(L);
            dv.Texture2DArray.ArraySize = 1;
            dev_->CreateDepthStencilView(shadow_.r, &dv, dsv_.cpu(1 + static_cast<UINT>(L)));
        }
        // the SAMPLED view spans every layer (`sampler2DArray`)
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = 1;
        sv.Texture2DArray.ArraySize = kShadowLayers;
        dev_->CreateShaderResourceView(shadow_.r, &sv, srv_.cpu(kSlotShadow));
    } else {
        std::fprintf(stderr, "d3d12: no shadow map here - `--shadow-quality mapped` will draw nothing\n");
    }
    // the frame's two uniform blocks, root CBVs (256-byte aligned), mapped
    shUbo_ = makeBuffer(alignUp(sizeof(ShadowUbo), 256), D3D12_HEAP_TYPE_UPLOAD, "uniforms");
    litUbo_ = makeBuffer(alignUp(sizeof(LightUbo), 256), D3D12_HEAP_TYPE_UPLOAD, "uniforms");
    if (!shUbo_ || !litUbo_) return false;
    // VOLUMETRICS: the air's lamps, a root CBV of the march
    if (volOn_) {
        volUbo_ = makeBuffer(alignUp(sizeof(VolUbo), 256), D3D12_HEAP_TYPE_UPLOAD, "uniforms");
        D3D12_RANGE nr{0, 0};
        if (!volUbo_ || FAILED(volUbo_->Map(0, &nr, &volUboPtr_))) volUboPtr_ = nullptr;
        if (!volUboPtr_) { volOn_ = false; std::fprintf(stderr, "d3d12: no lamp buffer - volumetrics off\n"); }
    }
    D3D12_RANGE none{0, 0};
    shUbo_->Map(0, &none, &shUboPtr_);
    litUbo_->Map(0, &none, &litUboPtr_);
    if (shUboPtr_) std::memset(shUboPtr_, 0, sizeof(ShadowUbo));
    if (litUboPtr_) std::memset(litUboPtr_, 0, sizeof(LightUbo));
    return true;
}

// The frame-sized half of `makeTarget`, released for a resize. The PSOs stay:
// the viewport is dynamic and the formats and sample count do not change.
void D3D12Renderer::killTarget() {
    freeRes(colour_.r);
    freeRes(msColour_.r);
    freeRes(depth_.r);
    freeRes(readBuf_);
    freeRes(upBuf_);
    freeRes(ao_.r);          // remade at the new size on first use (`makeSsao`)
    freeRes(presImg_.r);     // ...and the present pass's (`makePresentPass`)
    freeRes(presRead_);
    freeRes(ovPic_.r);       // ...and the overlay's planes (`makeOverlayPass`)
    freeRes(ovMask_.r);
    freeRes(ovBuf_);
    freeRes(motion_.r);
    freeRes(upOut_.r);
    freeRes(motionRead_);
    freeRes(postImg_.r);     // ...and the post chain's (`makePost`)
    freeRes(vol_.r);         // ...and the volumetrics' (`makeVol`)
    for (Res& b : bloom_) freeRes(b.r);
    postLive_ = false;
    for (Res& f : fgCol_) freeRes(f.r);   // ...and frame generation's (`makeFg`)
    for (Res& f : fgAux_) freeRes(f.r);
    for (Res& f : fgHead_) freeRes(f.r);
    freeRes(fgMid_.r);
}

ID3D12RootSignature* D3D12Renderer::makeRootSig(const D3D12_ROOT_SIGNATURE_DESC& d, const char* what) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
        std::fprintf(stderr, "d3d12: root signature %s: %s\n", what,
                     err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        release(err);
        return nullptr;
    }
    release(err);
    ID3D12RootSignature* rs = nullptr;
    if (FAILED(dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                         IID_PPV_ARGS(&rs))))
        std::fprintf(stderr, "d3d12: CreateRootSignature %s\n", what);
    release(blob);
    return rs;
}

ID3D12PipelineState* D3D12Renderer::makePso(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, const char* what) {
    ID3D12PipelineState* p = nullptr;
    const HRESULT hr = dev_->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p));
    if (FAILED(hr)) {
        std::fprintf(stderr, "d3d12: pipeline %s -> 0x%08lx\n", what, static_cast<unsigned long>(hr));
        drainDebug();
        return nullptr;
    }
    return p;
}

namespace rs {
D3D12_ROOT_PARAMETER constants(UINT reg, UINT space, UINT n, D3D12_SHADER_VISIBILITY vis) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p.Constants.ShaderRegister = reg; p.Constants.RegisterSpace = space; p.Constants.Num32BitValues = n;
    p.ShaderVisibility = vis;
    return p;
}
D3D12_ROOT_PARAMETER table(const D3D12_DESCRIPTOR_RANGE* r, D3D12_SHADER_VISIBILITY vis) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 1; p.DescriptorTable.pDescriptorRanges = r;
    p.ShaderVisibility = vis;
    return p;
}
D3D12_ROOT_PARAMETER view(D3D12_ROOT_PARAMETER_TYPE t, UINT reg, UINT space, D3D12_SHADER_VISIBILITY vis) {
    D3D12_ROOT_PARAMETER p{};
    p.ParameterType = t;
    p.Descriptor.ShaderRegister = reg; p.Descriptor.RegisterSpace = space;
    p.ShaderVisibility = vis;
    return p;
}
D3D12_DESCRIPTOR_RANGE srvs(UINT reg, UINT space, UINT n) {
    D3D12_DESCRIPTOR_RANGE r{};
    r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r.NumDescriptors = n; r.BaseShaderRegister = reg; r.RegisterSpace = space;
    r.OffsetInDescriptorsFromTableStart = 0;
    return r;
}
D3D12_STATIC_SAMPLER_DESC sampler(UINT reg, UINT space, D3D12_FILTER f, D3D12_TEXTURE_ADDRESS_MODE a,
                                  float maxLod, UINT aniso = 1) {
    D3D12_STATIC_SAMPLER_DESC s{};
    s.Filter = f;
    s.AddressU = s.AddressV = s.AddressW = a;
    s.MaxAnisotropy = aniso;
    s.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    s.MinLOD = 0.0f; s.MaxLOD = maxLod;
    s.ShaderRegister = reg; s.RegisterSpace = space;
    s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return s;
}
D3D12_SHADER_BYTECODE code(const unsigned char* p, std::size_t n) { return {p, n}; }
}  // namespace rs

bool D3D12Renderer::makePipelines() {
    // ---- THE SCENE's root signature: Vulkan's three sets and its push block,
    // at the registers spirv-cross gave them (set S binding B -> spaceS, B):
    //   0  the push block, 26 root constants, b0 space7, both stages
    //   1  set 0 binding 0, the material's texture, t0 space0 (a table)
    //   2  set 1 binding 0, the Shadow block, a root CBV b0 space1
    //   3  set 1 binding 1, the shadow map's layers, t1 space1 (a table)
    //   4  set 1 binding 2, the Lights block, a root CBV b2 space1
    //   5  set 2 binding 0, the POSE buffer, a root SRV t0 space2 - Vulkan's
    //      dynamic offset becomes the root descriptor's address
    // and the two samplers static: the texture's (s0 space0) is fixed at init
    // by the filter enhancement, the shadow map's (s1 space1) is NEAREST.
    {
        const D3D12_DESCRIPTOR_RANGE texRange = rs::srvs(0, 0, 1);
        const D3D12_DESCRIPTOR_RANGE shRange = rs::srvs(1, 1, 1);
        // (44 root constants with an upscaler or frame generation: `PushMv`)
        D3D12_ROOT_PARAMETER p[13] = {
            rs::constants(0, kPushSpace, mvOn_ ? kPushMvDwords : kPushDwords, D3D12_SHADER_VISIBILITY_ALL),
            rs::table(&texRange, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_CBV, 0, 1, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&shRange, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_CBV, 2, 1, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 2, D3D12_SHADER_VISIBILITY_VERTEX)};
        // HYBRID RAY TRACING's six (`rt.glsl`, `scene.frag` -DOMK_RT): 6 the TLAS
        // (t0 space3), 7 the instance table (t1), 8 `RtFrame` (b2), 9 the rays'
        // tally (u4), and two BINDLESS tables over the whole heap - 10 the
        // records (t0 space4), 11 the textures (t0 space5) - 62 of the 64
        // dwords with an upscaler's push block
        UINT np = 6;
        const D3D12_DESCRIPTOR_RANGE recAll = rs::srvs(0, 4, UINT_MAX), texAll = rs::srvs(0, 5, UINT_MAX);
        if (rtOn_) {
            p[np++] = rs::view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 3, D3D12_SHADER_VISIBILITY_PIXEL);
            p[np++] = rs::view(D3D12_ROOT_PARAMETER_TYPE_SRV, 1, 3, D3D12_SHADER_VISIBILITY_PIXEL);
            p[np++] = rs::view(D3D12_ROOT_PARAMETER_TYPE_CBV, 2, 3, D3D12_SHADER_VISIBILITY_PIXEL);
            p[np++] = rs::view(D3D12_ROOT_PARAMETER_TYPE_UAV, 4, 3, D3D12_SHADER_VISIBILITY_PIXEL);
            p[np++] = rs::table(&recAll, D3D12_SHADER_VISIBILITY_PIXEL);
            p[np++] = rs::table(&texAll, D3D12_SHADER_VISIBILITY_PIXEL);
        }
        // NEAREST and WRAP (the software rasterizer's `% t.width`), unless the
        // filtering ENHANCEMENT asked for bilinear or the trilinear chain.
        // Max LOD 0 below trilinear: one level, sampled exactly (Vulkan's
        // value-initialised `maxLod`).
        const D3D12_FILTER f = anisoOn_ ? D3D12_FILTER_ANISOTROPIC
                             : filter_ >= 2 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
                             : filter_ >= 1 ? D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT
                                            : D3D12_FILTER_MIN_MAG_MIP_POINT;
        D3D12_STATIC_SAMPLER_DESC ss[3] = {
            rs::sampler(0, 0, f, D3D12_TEXTURE_ADDRESS_MODE_WRAP, filter_ >= 2 ? D3D12_FLOAT32_MAX : 0.0f,
                        anisoOn_ ? static_cast<UINT>(aniso_) : 1),
            // NEAREST: the fragment shader compares depths itself
            rs::sampler(1, 1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            // the rays' texel (s3 space3): NEAREST and WRAP, the original's
            rs::sampler(3, 3, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f)};
        // SOFT PARTICLES' depth (`scene.frag` -DOMK_SOFT): t0 space6, one table -
        // 63 of the 64 dwords with an upscaler and ray tracing - and its
        // sampler (s0 space6; the shader fetches texels)
        UINT nss = rtOn_ ? 3 : 2;
        D3D12_STATIC_SAMPLER_DESC ssAll[4] = {ss[0], ss[1], ss[2], {}};
        const D3D12_DESCRIPTOR_RANGE softRange = rs::srvs(0, 6, 1);
        if (softOn_) {
            softParam_ = np;
            p[np++] = rs::table(&softRange, D3D12_SHADER_VISIBILITY_PIXEL);
            ssAll[nss++] = rs::sampler(0, 6, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f);
        }
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = np; d.pParameters = p;
        d.NumStaticSamplers = nss; d.pStaticSamplers = ssAll;
        d.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        sceneRs_ = makeRootSig(d, "scene");
        if (!sceneRs_) return false;
    }

    // `scene.vert`'s six inputs, TEXCOORDn = location n (spirv-cross's rule)
    const D3D12_INPUT_ELEMENT_DESC il[6] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVert, x), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(GpuVert, u), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVert, r), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(GpuVert, nx), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 4, DXGI_FORMAT_R32_FLOAT, 0, offsetof(GpuVert, phase), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 5, DXGI_FORMAT_R32_FLOAT, 0, offsetof(GpuVert, mesh), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC base{};
    base.pRootSignature = sceneRs_;
    base.VS = rs::code(kDx_scene_vert, sizeof kDx_scene_vert);
    base.PS = rtOn_ ? rs::code(kDx_scene_rt_frag, sizeof kDx_scene_rt_frag)
                    : rs::code(kDx_scene_frag, sizeof kDx_scene_frag);
    base.SampleMask = UINT_MAX;
    // D3DCULL_NONE - `Raster_DrawTriangles` sets it; the engine's own cull is
    // in software, before the device: `push_.cull` and scene.frag's discard.
    base.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    base.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    base.RasterizerState.FrontCounterClockwise = TRUE;
    base.RasterizerState.DepthClipEnable = TRUE;
    base.RasterizerState.MultisampleEnable = samples_ > 1 ? TRUE : FALSE;
    base.InputLayout = {il, 6};
    base.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    base.NumRenderTargets = 1;
    base.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    // THE UPSCALER's scene pass: the -DOMK_MOTION build of the same shaders and
    // the motion vectors as a second target. Only OPAQUE geometry writes them
    // (the mask, below): an additive glow or a multiply decal over a wall
    // leaves the wall's motion, which is what the history should follow.
    // FRAME GENERATION warps along the same vectors, with or without an upscaler.
    if (mvOn_) {
        base.VS = rs::code(kDx_scene_mv_vert, sizeof kDx_scene_mv_vert);
        base.PS = rtOn_ ? rs::code(kDx_scene_rt_mv_frag, sizeof kDx_scene_rt_mv_frag)
                        : rs::code(kDx_scene_mv_frag, sizeof kDx_scene_mv_frag);
        base.NumRenderTargets = 2;
        base.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        base.BlendState.IndependentBlendEnable = TRUE;
    }
    base.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    base.SampleDesc.Count = samples_;
    base.DepthStencilState.DepthEnable = TRUE;
    base.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    // the three blend modes by `blendRule` (o3de/drawcore.h): opaque, additive
    // and multiply (`dst * (1 - src)`); only an OPAQUE pass owns the depth.
    const auto dxFactor = [](omk::BlendFactor f) {
        return f == omk::BlendFactor::Zero ? D3D12_BLEND_ZERO
             : f == omk::BlendFactor::One  ? D3D12_BLEND_ONE
                                           : D3D12_BLEND_INV_SRC_COLOR;
    };
    const omk::Blend kModes[3] = {omk::Blend::Opaque, omk::Blend::Add, omk::Blend::Mul};
    const auto blendState = [&](int k) {
        const omk::BlendRule rule = omk::blendRule(kModes[k]);
        D3D12_RENDER_TARGET_BLEND_DESC b{};
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        b.BlendEnable = rule.blend ? TRUE : FALSE;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ZERO; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        if (rule.blend) {
            b.SrcBlend = dxFactor(rule.src);
            b.DestBlend = dxFactor(rule.dst);
            b.DestBlendAlpha = kModes[k] == omk::Blend::Add ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
        }
        return b;
    };
    const auto stencilOp = [](D3D12_STENCIL_OP pass, D3D12_COMPARISON_FUNC cmp) {
        D3D12_DEPTH_STENCILOP_DESC o{};
        o.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        o.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        o.StencilPassOp = pass;
        o.StencilFunc = cmp;
        return o;
    };
    const auto markSubject = [&](D3D12_GRAPHICS_PIPELINE_STATE_DESC& x) {
        x.DepthStencilState.StencilEnable = TRUE;
        x.DepthStencilState.StencilReadMask = 0xFF;
        x.DepthStencilState.StencilWriteMask = 0xFF;
        x.DepthStencilState.FrontFace = x.DepthStencilState.BackFace =
            stencilOp(D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS);
    };
    for (int k = 0; k < 3; ++k) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base;
        d.BlendState.RenderTarget[0] = blendState(k);
        d.BlendState.RenderTarget[1].RenderTargetWriteMask =
            k == 0 ? (D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN) : 0;
        d.DepthStencilState.DepthWriteMask = omk::blendRule(kModes[k]).depthWrite
                                                 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pipe_[k] = makePso(d, "scene");
        if (!pipe_[k]) return false;
        // SOFT PARTICLES' twin of the two blended pipelines: the -DOMK_SOFT
        // build (the plain one even with ray tracing: a sprite takes none of
        // its terms, and the RT root is a superset of what it binds)
        if (softOn_ && k > 0) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC ds = d;
            ds.PS = mvOn_ ? rs::code(kDx_scene_soft_mv_frag, sizeof kDx_scene_soft_mv_frag)
                        : rs::code(kDx_scene_soft_frag, sizeof kDx_scene_soft_frag);
            pipeSoft_[k] = makePso(ds, "scene, soft");
            if (!pipeSoft_[k]) return false;
        }
        // RAY TRACING's counting twin (-DOMK_RT_COUNT), drawn one frame in 60
        if (rtOn_) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dc = d;
            dc.PS = mvOn_ ? rs::code(kDx_scene_rtc_mv_frag, sizeof kDx_scene_rtc_mv_frag)
                        : rs::code(kDx_scene_rtc_frag, sizeof kDx_scene_rtc_frag);
            pipeCount_[k] = makePso(dc, "scene, counting rays");
            if (!pipeCount_[k]) return false;
            if (k > 0) {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC dcs = dc;
                markSubject(dcs);
                pipeCountSubj_[k] = makePso(dcs, "scene, counting rays, subject");
            }
        }
        if (k > 0) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC dss = d;
            markSubject(dss);
            pipeSubj_[k] = makePso(dss, "scene, subject");
        }
        // ...and the reflection's: the same with the stencil test EQUAL 1
        d.DepthStencilState.StencilEnable = TRUE;
        d.DepthStencilState.StencilReadMask = 0xFF;
        d.DepthStencilState.StencilWriteMask = 0;
        d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace =
            stencilOp(D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL);
        pipeRefl_[k] = makePso(d, "reflection");
    }
    {   // the mirror's marker: colour off, depth write off, stencil := 1
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base;
        d.BlendState.RenderTarget[0] = blendState(0);
        d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.StencilEnable = TRUE;
        d.DepthStencilState.StencilReadMask = 0xFF;
        d.DepthStencilState.StencilWriteMask = 0xFF;
        d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace =
            stencilOp(D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS);
        pipeStencil_ = makePso(d, "stencil");
        // ...and the depth reset inside it: a full-screen triangle at z = 1
        d.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        d.PS = rs::code(kDx_fsq_frag, sizeof kDx_fsq_frag);
        d.InputLayout = {nullptr, 0};
        d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.DepthStencilState.StencilWriteMask = 0;
        d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace =
            stencilOp(D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_EQUAL);
        pipeDepthReset_ = makePso(d, "depth reset");
    }

    // ---- the POSE BUFFER (set 2). Without it every body is posed on the CPU.
    poseBuf_ = makeBuffer(kPoseBytes, D3D12_HEAP_TYPE_UPLOAD, "uniforms");
    if (poseBuf_) {
        D3D12_RANGE none{0, 0};
        if (FAILED(poseBuf_->Map(0, &none, &posePtr_))) posePtr_ = nullptr;
    }
    if (!posePtr_) std::fprintf(stderr, "d3d12: no pose buffer - every body posed on the CPU\n");

    // ---- the SHADOW pipeline: depth only, a depth bias against the ground
    // shadowing itself in stripes. Vulkan's constant factor is 1.5; D3D12's
    // is an integer count of the format's steps, so 2.
    if (shadow_.r) {
        D3D12_ROOT_PARAMETER p = rs::constants(0, kPushSpace, 16, D3D12_SHADER_VISIBILITY_VERTEX);
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 1; d.pParameters = &p;
        d.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        shRs_ = makeRootSig(d, "shadow");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC sd{};
        sd.pRootSignature = shRs_;
        sd.VS = rs::code(kDx_shadow_vert, sizeof kDx_shadow_vert);
        sd.SampleMask = UINT_MAX;
        sd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // NO culling: a character model is not a closed solid (vkrender.cpp)
        sd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        sd.RasterizerState.FrontCounterClockwise = TRUE;
        sd.RasterizerState.DepthClipEnable = TRUE;
        sd.RasterizerState.DepthBias = 2;
        sd.RasterizerState.SlopeScaledDepthBias = 2.5f;
        sd.InputLayout = {il, 1};
        sd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        sd.NumRenderTargets = 0;
        sd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        sd.SampleDesc.Count = 1;
        sd.DepthStencilState.DepthEnable = TRUE;
        sd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        sd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        if (shRs_) shPipe_ = makePso(sd, "shadow");
        if (!shPipe_) std::fprintf(stderr, "d3d12: no shadow pipeline - mapped shadows off\n");
    }
    return true;
}

// ============================================================ the window

bool D3D12Renderer::swapchainBuffers() {
    for (UINT i = 0; i < kSwapBuffers; ++i) {
        if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&swapBuf_[i].r)))) return false;
        swapBuf_[i].st = D3D12_RESOURCE_STATE_PRESENT;
        dev_->CreateRenderTargetView(swapBuf_[i].r, nullptr, rtv_.cpu(kRtvSwap + i));
    }
    DXGI_SWAP_CHAIN_DESC1 d{};
    swap_->GetDesc1(&d);
    swapW_ = d.Width; swapH_ = d.Height;
    return true;
}

void D3D12Renderer::killSwapchainBuffers() {
    for (auto& b : swapBuf_) release(b.r);
}

// A FLIP-MODEL swapchain on the window, and the finished frame is DRAWN into
// it (`presentImage`) rather than rendered there - the reason is Vulkan's:
// `readback()` must keep working from the same offscreen target, and the
// letterboxed picture is placed into a window-sized image.
bool D3D12Renderer::makeSwapchain() {
    // HDR OUTPUT (`hdr = on`, todo/enhancements.md 22): only when the output
    // the window is on reports an HDR colour space (ST.2084) - Windows' HDR
    // switch on, an HDR display - else a line and the SDR swapchain.
    // `OMK_HDR_FORCE=1` takes the HDR path on any display, to exercise it.
    hdrOn_ = false;
    if (postReq_.hdr) {
        float peak = 0.0f;
        const bool hdr = outputIsHdr(peak);
        if (peak > 0.0f) hdrPeak_ = peak;
        if (hdr || omk::envFlag("OMK_HDR_FORCE")) hdrOn_ = true;
        else std::printf("hdr: the display reports no HDR (IDXGIOutput6::GetDesc1: not ST.2084) - "
                         "the picture stays SDR\n");
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        swapFmt_ = hdrOn_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = 0; d.Height = 0;            // the window's client area
        d.Format = swapFmt_;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = kSwapBuffers;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        IDXGISwapChain1* sc1 = nullptr;
        DXCHECK(factory_->CreateSwapChainForHwnd(swapQueue(), hwnd_, &d, nullptr, nullptr, &sc1),
                "CreateSwapChainForHwnd");
        const HRESULT hr = sc1->QueryInterface(IID_PPV_ARGS(&swap_));
        sc1->Release();
        DXCHECK(hr, "IDXGISwapChain3");
        if (!hdrOn_) break;
        // scRGB: linear, BT.709 primaries, 1.0 = 80 nits - the DWM composes it
        // onto an HDR10 display itself (DXGI's "HDR and wide colour gamut")
        UINT support = 0;
        const DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
        if (SUCCEEDED(swap_->CheckColorSpaceSupport(cs, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) && SUCCEEDED(swap_->SetColorSpace1(cs))) {
            std::printf("hdr: on - an ENHANCEMENT: an scRGB (FP16) swapchain, paper white %.0f nits, the "
                        "display's peak %.0f nits\n", static_cast<double>(kPaperWhite),
                        static_cast<double>(hdrPeak_));
            break;
        }
        std::printf("hdr: the swapchain refuses the scRGB colour space - the picture stays SDR\n");
        release(swap_);
        hdrOn_ = false;
    }
    presFmt_ = hdrOn_ ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    // ALT+ENTER is the window layer's (F11, SDL's borderless fullscreen), not DXGI's
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (!swapchainBuffers()) return false;

    // the blit: `fsq.vert` + `blit.frag`, a linear clamped sampler
    const D3D12_DESCRIPTOR_RANGE r = rs::srvs(0, 0, 1);
    D3D12_ROOT_PARAMETER p[2] = {
        rs::constants(0, kPushSpace, sizeof(BlitPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
        rs::table(&r, D3D12_SHADER_VISIBILITY_PIXEL)};
    D3D12_STATIC_SAMPLER_DESC s = rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                              D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f);
    D3D12_ROOT_SIGNATURE_DESC rd{};
    rd.NumParameters = 2; rd.pParameters = p;
    rd.NumStaticSamplers = 1; rd.pStaticSamplers = &s;
    blitRs_ = makeRootSig(rd, "blit");
    if (!blitRs_) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = blitRs_;
    pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
    pd.PS = hdrOn_ ? rs::code(kDx_blit_hdr_frag, sizeof kDx_blit_hdr_frag)
                   : rs::code(kDx_blit_frag, sizeof kDx_blit_frag);
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = swapFmt_;
    pd.SampleDesc.Count = 1;
    blitPipe_ = makePso(pd, "blit");
    if (!blitPipe_) return false;
    std::printf("d3d12: swapchain %ux%u, %u buffers, flip model\n", swapW_, swapH_, kSwapBuffers);
    return true;
}

bool D3D12Renderer::resize(int w, int h) {
    if (!dev_ || w <= 0 || h <= 0) return false;
    fgSettle();
    flush();
    killTarget();
    w_ = w; h_ = h;
    rw_ = w_ * ss_; rh_ = h_ * ss_;
    // the upscaler is made for ONE display size: made again at the new one,
    // and its history dropped (a resize is a discontinuity like a cut)
    if (up_) {
        up_.reset();
        if (!makeUpscaler()) std::printf("upscaler: off after the resize - the native frame\n");
    }
    srcW_ = sourceW(); srcH_ = sourceH();
    resizing_ = true;
    const bool ok = makeTarget();
    resizing_ = false;
    if (!ok) return false;
    std::printf("d3d12: frame target remade at %dx%d\n", w_, h_);
    return true;   // the swapchain follows the WINDOW, at the next present
}

bool D3D12Renderer::presentDirect() {
    fgSettle();
    return presentImage(upscaleSource(), sourceSlot(), srcW_ ? srcW_ : sourceW(), srcH_ ? srcH_ : sourceH(),
                        sourceW(), sourceH());
}

// The swapchain half of every present: `src`'s top-left `sw x sh`, fitted
// into the window at the frame's own aspect over black - Vulkan's
// `presentImage` rule, NEAREST unless supersampled (`blit.frag`).
// The blit itself, `src` scaled into back buffer `idx` - recorded on the
// frame's thread's one-shot list, or on frame generation's own (`fgBlitPresent`).
void D3D12Renderer::recordBlit(ID3D12GraphicsCommandList* l, Res& src, UINT srv, int sw, int sh, int texW,
                               int texH, UINT idx) {
    Res& bb = swapBuf_[idx];
    bb.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    src.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_.cpu(kRtvSwap + idx);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    l->ClearRenderTargetView(rtv, black, 0, nullptr);
    const int sW = static_cast<int>(swapW_), sH = static_cast<int>(swapH_);
    int dW = sW, dH = sW * h_ / w_;
    if (dH > sH) { dH = sH; dW = sH * w_ / h_; }
    if (sW == w_ && h_ <= sH) { dW = w_; dH = h_; }
    const int x0 = (sW - dW) / 2, y0 = (sH - dH) / 2;
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{static_cast<float>(x0), static_cast<float>(y0),
                            static_cast<float>(dW), static_cast<float>(dH), 0.0f, 1.0f};
    const D3D12_RECT sr{x0, y0, x0 + dW, y0 + dH};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(blitRs_);
    l->SetPipelineState(blitPipe_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const BlitPush bp{{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(dW), static_cast<float>(dH)},
                      {static_cast<float>(sw), static_cast<float>(sh), static_cast<float>(texW), static_cast<float>(texH)},
                      // LINEAR only when there is something to average
                      ss_ > 1 ? 1 : 0,
                      // HDR (`blit.frag` -DOMK_HDR): paper white and the peak in scRGB
                      kPaperWhite / 80.0f, hdrPeak_ / 80.0f};
    l->SetGraphicsRoot32BitConstants(0, sizeof bp / 4, &bp, 0);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(srv));
    l->DrawInstanced(3, 1, 0, 0);
}

bool D3D12Renderer::presentImage(Res& src, UINT srv, int sw, int sh, int texW, int texH, bool capture) {
    if (!swap_ || !blitPipe_) return false;
    // THE WINDOW MAY HAVE CHANGED SIZE - fullscreen, a resize
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT cw = static_cast<UINT>(rc.right - rc.left), ch = static_cast<UINT>(rc.bottom - rc.top);
    if (cw == 0 || ch == 0) return false;   // minimised
    if (cw != swapW_ || ch != swapH_) {
        flush();
        killSwapchainBuffers();
        if (FAILED(swap_->ResizeBuffers(kSwapBuffers, cw, ch, swapFmt_, 0)) || !swapchainBuffers())
            return false;
    }
    const UINT idx = swap_->GetCurrentBackBufferIndex();
    Res& bb = swapBuf_[idx];
    ID3D12GraphicsCommandList* l = oneShotBegin();
    recordBlit(l, src, srv, sw, sh, texW, texH, idx);
    const int sW = static_cast<int>(swapW_), sH = static_cast<int>(swapH_);
    // THE WINDOW'S PICTURE, when asked (`captureWindow`): the back buffer as
    // it will be shown, copied out before it is handed to the swapchain - a
    // REAL frame's (frame generation's in-between ones are never the dump)
    ID3D12Resource* capBuf = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT capFp{};
    if (winCapture_ && capture) {
        D3D12_RESOURCE_DESC cd = bb.r->GetDesc();
        UINT64 total = 0;
        dev_->GetCopyableFootprints(&cd, 0, 1, 0, &capFp, nullptr, nullptr, &total);
        capBuf = makeBuffer(total, D3D12_HEAP_TYPE_READBACK, "readback");
        if (capBuf) {
            bb.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{}, srcL{};
            srcL.pResource = bb.r;
            srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = capBuf;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = capFp;
            l->CopyTextureRegion(&dst, 0, 0, 0, &srcL, nullptr);
        }
    }
    bb.to(l, D3D12_RESOURCE_STATE_PRESENT);
    src.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    oneShotEnd(swapQueue());
    if (capBuf) {
        winCapture_ = false;
        winPicW_ = sW; winPicH_ = sH;
        winPic_.resize(static_cast<std::size_t>(sW) * static_cast<std::size_t>(sH) * 4);
        void* p = nullptr;
        if (SUCCEEDED(capBuf->Map(0, nullptr, &p))) {
            for (int y = 0; y < sH; ++y) {
                const unsigned char* row = static_cast<const unsigned char*>(p) + capFp.Offset +
                                           static_cast<UINT64>(y) * capFp.Footprint.RowPitch;
                unsigned char* to = winPic_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(sW) * 4;
                if (!hdrOn_) {
                    std::memcpy(to, row, static_cast<std::size_t>(sW) * 4);
                    continue;
                }
                // HDR: scRGB halves back to gamma 2.2 at paper white, clipped
                const auto* h = reinterpret_cast<const std::uint16_t*>(row);
                for (int x = 0; x < sW; ++x)
                    for (int k = 0; k < 4; ++k) {
                        const std::uint16_t v = h[4 * x + k];
                        const std::uint32_t e = (v >> 10) & 31u, m = v & 1023u;
                        float f = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                                         : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
                        if (v >> 15) f = 0.0f;
                        f = k == 3 ? 1.0f : std::pow(std::min(f * 80.0f / kPaperWhite, 1.0f), 1.0f / 2.2f);
                        to[4 * x + k] = static_cast<unsigned char>(f * 255.0f + 0.5f);
                    }
            }
            D3D12_RANGE none{0, 0};
            capBuf->Unmap(0, &none);
        }
        freeRes(capBuf);
    }
    // FIFO, v-synced - what a 30 Hz game wants (Vulkan's FIFO)
    swap_->Present(1, 0);
    return true;
}

bool D3D12Renderer::windowPicture(std::vector<unsigned char>& rgba, int& w, int& h) {
    fgSettle();
    if (winPic_.empty()) return false;
    rgba = winPic_;
    w = winPicW_; h = winPicH_;
    return true;
}

// `w_ x h_` RGBA8 into `colour_`'s top-left, through a buffer at the copy's
// 256-byte row pitch.
bool D3D12Renderer::uploadToColour(const unsigned char* rgba) {
    if (!upBuf_) {
        upPitch_ = alignUp(static_cast<UINT64>(w_) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        upBuf_ = makeBuffer(upPitch_ * static_cast<UINT64>(h_), D3D12_HEAP_TYPE_UPLOAD, "staging");
        if (!upBuf_) return false;
    }
    void* p = nullptr;
    if (FAILED(upBuf_->Map(0, nullptr, &p))) return false;
    for (int y = 0; y < h_; ++y)
        std::memcpy(static_cast<unsigned char*>(p) + static_cast<UINT64>(y) * upPitch_,
                    rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) * 4,
                    static_cast<std::size_t>(w_) * 4);
    upBuf_->Unmap(0, nullptr);
    // into the DISPLAY image - `colour_` natively, the upscaler's output
    // (display-sized, where `colour_` is smaller) with one on
    Res& img = upscaleSource();
    ID3D12GraphicsCommandList* l = oneShotBegin();
    img.to(l, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{}, srcL{};
    dst.pResource = img.r;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    srcL.pResource = upBuf_;
    srcL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcL.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcL.PlacedFootprint.Footprint.Width = static_cast<UINT>(w_);
    srcL.PlacedFootprint.Footprint.Height = static_cast<UINT>(h_);
    srcL.PlacedFootprint.Footprint.Depth = 1;
    srcL.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(upPitch_);
    l->CopyTextureRegion(&dst, 0, 0, 0, &srcL, nullptr);
    img.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    oneShotEnd();
    return true;
}

// Present a CPU-composited frame (the 2D over the world, the CPU mirror).
bool D3D12Renderer::presentSurface(const omk::Surface& s) {
    fgSettle();
    if (!swap_) return false;
    fgCpuFrame();   // FRAME GENERATION: a frame composed on the CPU goes out as it is
    static std::vector<unsigned char> rgba;
    rgba.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4, 0);
    const int n = std::min<int>(w_ * h_, static_cast<int>(s.px.size()));
    const unsigned char* lut = omk::expand565Rgba();
    for (int i = 0; i < n; ++i)
        std::memcpy(rgba.data() + 4 * static_cast<std::size_t>(i),
                    lut + 4 * static_cast<std::size_t>(s.px[static_cast<std::size_t>(i)]), 4);
    if (!uploadToColour(rgba.data())) return false;
    srcW_ = w_; srcH_ = h_;   // only the top-left holds the uploaded picture
    return presentDirect();
}

// ---- THE PRESENT PASS (todo/optimization.md step 4b) ----------------------

bool D3D12Renderer::makePresentPass() {
    if (!presRs_) {
        const D3D12_DESCRIPTOR_RANGE r = rs::srvs(0, 0, 1);
        D3D12_ROOT_PARAMETER p[2] = {
            rs::constants(0, kPushSpace, sizeof(PresentPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r, D3D12_SHADER_VISIBILITY_PIXEL)};
        D3D12_STATIC_SAMPLER_DESC s = rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                                                  D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f);
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 2; d.pParameters = p;
        d.NumStaticSamplers = 1; d.pStaticSamplers = &s;
        presRs_ = makeRootSig(d, "present");
        if (!presRs_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = presRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = rs::code(kDx_present_frag, sizeof kDx_present_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = presFmt_;
        pd.SampleDesc.Count = 1;
        presPipe_ = makePso(pd, "present");
        if (!presPipe_) return false;
    }
    if (presImg_.r) return true;
    // the DISPLAY-size picture, which the swapchain blit and the
    // verification copy both read
    presImg_.r = makeImage(presFmt_, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, 1, 1,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE,
                           nullptr, "present image");
    presImg_.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!presImg_.r) return false;
    dev_->CreateRenderTargetView(presImg_.r, nullptr, rtv_.cpu(rtvPres_));
    dev_->CreateShaderResourceView(presImg_.r, nullptr, srv_.cpu(kSlotPres));
    D3D12_RESOURCE_DESC cd = presImg_.r->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    dev_->GetCopyableFootprints(&cd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    presPitch_ = fp.Footprint.RowPitch;
    presRead_ = makeBuffer(total, D3D12_HEAP_TYPE_READBACK, "readback");
    return presRead_ != nullptr;
}

// `colour_` through the present shader into `presImg_` - the display half
// reads `upscaleSource()` at the RENDER size, `ss_` folding the supersample.
bool D3D12Renderer::runPresentPass(int vy, int vh, Res* over) {
    if (!makePresentPass()) return false;
    // the POST CHAIN's picture when it ran this frame (display size, folded) -
    // or FRAME GENERATION's in-between picture (display size)
    Res& src = over ? *over : displaySource();
    ID3D12GraphicsCommandList* l = oneShotBegin();
    src.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    presImg_.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_.cpu(rtvPres_);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, w_, h_};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(presRs_);
    l->SetPipelineState(presPipe_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // (HDR: the picture passed through - its highlights run past 1.0)
    const PresentPush pp{vy, vh, w_, dither_ ? 1 : 0, over ? 1 : displaySs(), hdrOn_ ? 32 : colourDepth_, 0, 0, 0};
    l->SetGraphicsRoot32BitConstants(0, sizeof pp / 4, &pp, 0);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(over ? kSlotFgMid : displaySlot()));
    l->DrawInstanced(3, 1, 0, 0);
    presImg_.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    src.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    oneShotEnd();
    return true;
}

bool D3D12Renderer::presentWorld(int vy, int vh) {
    fgSettle();
    if (!swap_) return false;
    // FRAME GENERATION: the in-between picture first, through the same present
    // pass, then the real frame - at once, or half an interval later
    const bool mid = fgMidFrame();
    if (mid && runPresentPass(vy, vh, &fgMid_)) presentImage(presImg_, kSlotPres, w_, h_, w_, h_, false);
    if (!runPresentPass(vy, vh)) return false;
    return fgPresentReal(mid);
}

// ---- THE OVERLAY AT COLOUR DEPTH 32 (`todo/enhancements.md` 21) -----------
//
// The GLES window's overlay present (`GlesRenderer::presentOverlay`,
// `todo/vita-port.md` G6 step 2) on this backend: the present pass writes the
// world into `presImg_` at full precision, then `overlay.frag` blends the
// interface's two planes over it - C (the 565 frame, R16_UINT) and M (R8) -
// as `C + world * M` with the colour fade, and the result goes to the window
// like any present. No readback, no 565 world under the HUD.

bool D3D12Renderer::makeOverlayPass() {
    if (!ovRs_) {
        const D3D12_DESCRIPTOR_RANGE r = rs::srvs(0, 0, 2);   // t0 C, t1 M
        D3D12_ROOT_PARAMETER p[2] = {
            rs::constants(0, kPushSpace, sizeof(OverlayPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r, D3D12_SHADER_VISIBILITY_PIXEL)};
        // (both read by texelFetch; the samplers spirv-cross declares for them)
        D3D12_STATIC_SAMPLER_DESC s[2] = {
            rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(1, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f)};
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 2; d.pParameters = p;
        d.NumStaticSamplers = 2; d.pStaticSamplers = s;
        ovRs_ = makeRootSig(d, "overlay");
        if (!ovRs_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = ovRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = rs::code(kDx_overlay_frag, sizeof kDx_overlay_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        // new = overlay + world x weight (GL_ONE, GL_SRC_ALPHA); alpha stays the world's
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE;
        b.DestBlend = D3D12_BLEND_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = presFmt_;
        pd.SampleDesc.Count = 1;
        ovPipe_ = makePso(pd, "overlay");
        if (!ovPipe_) return false;
    }
    if (ovPic_.r) return true;
    ovPic_.r = makeImage(DXGI_FORMAT_R16_UINT, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, 1, 1,
                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "overlay colour");
    ovPic_.st = D3D12_RESOURCE_STATE_COPY_DEST;
    ovMask_.r = makeImage(DXGI_FORMAT_R8_UNORM, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, 1, 1,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "overlay mask");
    ovMask_.st = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!ovPic_.r || !ovMask_.r) return false;
    dev_->CreateShaderResourceView(ovPic_.r, nullptr, srv_.cpu(kSlotOvPic));
    dev_->CreateShaderResourceView(ovMask_.r, nullptr, srv_.cpu(kSlotOvMask));
    ovPicPitch_ = alignUp(static_cast<UINT64>(w_) * 2, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ovMaskPitch_ = alignUp(static_cast<UINT64>(w_), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    ovMaskOff_ = alignUp(ovPicPitch_ * static_cast<UINT64>(h_), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    ovBuf_ = makeBuffer(ovMaskOff_ + ovMaskPitch_ * static_cast<UINT64>(h_), D3D12_HEAP_TYPE_UPLOAD, "staging");
    return ovBuf_ != nullptr;
}

bool D3D12Renderer::presentOverlay(const omk::Surface& s, const std::uint8_t* mask, const float fade[4],
                                   int vy, int vh) {
    fgSettle();
    if (!swap_ || s.w != w_ || s.h != h_ || !mask) return false;
    if (!makeOverlayPass()) return false;
    // FRAME GENERATION: an in-between world, if one is due, under the SAME
    // interface (the 2D is never interpolated: this frame's, over both)
    const bool mid = fgMidFrame();
    // both planes up, whole: ~3 bytes a pixel against the CPU path's readback,
    // dither and RGBA upload of the entire frame
    void* p = nullptr;
    if (FAILED(ovBuf_->Map(0, nullptr, &p))) return false;
    unsigned char* up = static_cast<unsigned char*>(p);
    for (int y = 0; y < h_; ++y) {
        std::memcpy(up + static_cast<UINT64>(y) * ovPicPitch_,
                    s.px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_),
                    static_cast<std::size_t>(w_) * 2);
        std::memcpy(up + ovMaskOff_ + static_cast<UINT64>(y) * ovMaskPitch_,
                    mask + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_),
                    static_cast<std::size_t>(w_));
    }
    ovBuf_->Unmap(0, nullptr);
    ID3D12GraphicsCommandList* l = oneShotBegin();
    const auto copy = [&](Res& img, DXGI_FORMAT f, UINT64 off, UINT64 pitch) {
        img.to(l, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, srcL{};
        dst.pResource = img.r;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcL.pResource = ovBuf_;
        srcL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcL.PlacedFootprint.Offset = off;
        srcL.PlacedFootprint.Footprint = {f, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, static_cast<UINT>(pitch)};
        l->CopyTextureRegion(&dst, 0, 0, 0, &srcL, nullptr);
        img.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    copy(ovPic_, DXGI_FORMAT_R16_UINT, 0, ovPicPitch_);
    copy(ovMask_, DXGI_FORMAT_R8_UNORM, ovMaskOff_, ovMaskPitch_);
    oneShotEnd();
    if (mid && runPresentPass(vy, vh, &fgMid_)) {
        ovDraw(fade);
        presentImage(presImg_, kSlotPres, w_, h_, w_, h_, false);
    }
    if (!runPresentPass(vy, vh)) return false;
    ovDraw(fade);
    return fgPresentReal(mid);
}

// The overlay's planes blended over the present pass's picture in `presImg_`.
void D3D12Renderer::ovDraw(const float fade[4]) {
    ID3D12GraphicsCommandList* l = oneShotBegin();
    presImg_.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_.cpu(rtvPres_);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, w_, h_};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(ovRs_);
    l->SetPipelineState(ovPipe_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const OverlayPush op{{fade[0], fade[1], fade[2], fade[3]}};
    l->SetGraphicsRoot32BitConstants(0, sizeof op / 4, &op, 0);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotOvPic));
    l->DrawInstanced(3, 1, 0, 0);
    presImg_.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    oneShotEnd();
}

bool D3D12Renderer::worldPicture(int vy, int vh, std::vector<unsigned char>& rgba) {
    fgSettle();
    if (hdrOn_) return false;   // an FP16 present image: not this RGBA8 copy's
    if (!runPresentPass(vy, vh)) return false;
    ID3D12GraphicsCommandList* l = oneShotBegin();
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    src.pResource = presImg_.r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = presRead_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(w_), static_cast<UINT>(h_),
                                     1, static_cast<UINT>(presPitch_)};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    oneShotEnd();
    rgba.resize(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4);
    void* p = nullptr;
    if (FAILED(presRead_->Map(0, nullptr, &p))) return false;
    for (int y = 0; y < h_; ++y)
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) * 4,
                    static_cast<const unsigned char*>(p) + static_cast<UINT64>(y) * presPitch_,
                    static_cast<std::size_t>(w_) * 4);
    D3D12_RANGE none{0, 0};
    presRead_->Unmap(0, &none);
    return true;
}

// ============================================================ init, textures

bool D3D12Renderer::init(int w, int h) {
    w_ = w; h_ = h;
    rw_ = w_ * ss_; rh_ = h_ * ss_;
    if (!pickDevice()) return false;
    // HYBRID RAY TRACING, before the pipelines it changes (`rtInit`)
    if (wantRt_) rtInit();
    // ...and its REFLECTIONS on the ground (`todo/enhancements.md` 26)
    if (wantRtRefl_) {
        rtReflOn_ = rtOn_;
        if (rtOn_) {
            std::printf("d3d12: RT REFLECTIONS on - an ENHANCEMENT: the ground wet in patches, one traced "
                        "ray a pixel mirroring the set, the bodies and the neon\n");
            omk::event("rt_refl", {{"on", 1}});
        } else {
            std::printf("d3d12: rt reflections asked for, but the ray tracing is not live - off\n");
            omk::event("rt_refl", {{"on", 0}});
        }
    }
    // THE UPSCALER, once there is a device to ask - it decides the render size
    if (upKind_ != omk::d3dup::kOff) makeUpscaler();
    // FRAME GENERATION (`todo/enhancements.md` 27): a way of PRESENTING, so the
    // window's alone; it warps along single-sample motion vectors, so not under
    // MSAA (an upscaler has turned MSAA off by now)
    if (fgWant_) {
        if (!hwnd_)
            std::printf("framegen: a way of presenting - no window here (the offscreen harness), off\n");
        else if (wantSamples_ > 1)
            std::printf("framegen: it warps along single-sample motion vectors - off under %dx MSAA "
                        "(aa = off, or an upscaler, lets it run)\n", wantSamples_);
        else
            fgOn_ = true;
        if (!fgOn_) omk::event("framegen", {{"on", 0}, {"refresh", 0}});
    }
    mvOn_ = up_ != nullptr || fgOn_;
    if (fgOn_) {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        if (FAILED(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&presQ_)))) {
            qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
            if (FAILED(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&presQ_)))) presQ_ = nullptr;
        }
    }
    srcW_ = sourceW(); srcH_ = sourceH();
    if (!makeTarget()) return false;
    if (!makePipelines()) return false;
    if (hwnd_ && !makeSwapchain()) return false;
    if (fgOn_ && !fgStart()) fgOn_ = false;
    return true;
}

void D3D12Renderer::setTextures(std::span<const omk::Texture> t) {
    // THE POOL, UPLOADED ONLY WHERE IT IS NEW - Vulkan's rule and its reason
    // (a per-frame call must not re-send the pool): a texture already on the
    // device from an earlier pool, by its pixel storage, palette and size, is
    // kept; what no slot of the new pool uses is released, after a wait for
    // the device only then. All the new ones go up in ONE submission.
    struct Pending { Tex* out; std::vector<unsigned char> rgba; int w, h; };
    std::vector<Pending> pend;
    // `fill` writes tw*th RGBA texels, alpha the COLOUR KEY (`toRgbaKeyed`)
    const auto queue = [&](auto&& fill, int tw, int th, Tex& out) {
        Pending p{&out, std::vector<unsigned char>(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4), tw, th};
        fill(p.rgba.data());
        pend.push_back(std::move(p));
    };
    if (!white_.img)
        queue([](unsigned char* d) { d[0] = d[1] = d[2] = d[3] = 255; }, 1, 1, white_);
    const auto keyOf = [](const omk::Texture& x) {
        return std::make_tuple(x.idx.data(), x.pal.data(), x.width, x.height);
    };
    // THE REMASTER's TEXTURE PACK (`o3de/texpack.h`, the project's one: `[Remaster] texturepack`, content-keyed
    // PNGs): consulted only when one is open and shown. What the pool brings that the device does not hold
    // yet is decoded together on the thread pool, as Vulkan's pool does; this backend takes the PNGs'
    // colours alone (no block formats, light maps or relief here), at the pack's size - the UVs stay in the
    // original's texels (`Tex::su`/`sv`, the push block's `texScale*`)
    const omk::TexturePack& pack = omk::texturePack();
    std::vector<omk::PackImage> reps;
    if (pack.shown()) {
        std::vector<const omk::Texture*> toDecode(t.size(), nullptr);
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i].hasPixels() && !uploaded_.count(keyOf(t[i]))) toDecode[i] = &t[i];
        reps = pack.replacements(toDecode, true, false);
    }
    for (auto& [key, u] : uploaded_) u.used = false;
    for (const auto& x : t)
        if (x.hasPixels())
            if (auto hit = uploaded_.find(keyOf(x)); hit != uploaded_.end()) hit->second.used = true;
    bool waited = false;
    for (auto it = uploaded_.begin(); it != uploaded_.end();) {
        if (it->second.used) { ++it; continue; }
        if (!waited) { flush(); waited = true; }
        freeRes(it->second.t.img);
        srv_.free(it->second.t.srv);
        ++texGen_;   // no ray-tracing record may name the view now
        it = uploaded_.erase(it);
    }
    tex_.assign(t.size(), Tex{});
    std::vector<std::pair<std::size_t, decltype(uploaded_)::key_type>> fresh;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const omk::Texture& x = t[i];
        if (!x.hasPixels()) continue;
        auto hit = uploaded_.find(keyOf(x));
        if (hit == uploaded_.end()) {
            Uploaded u;
            u.idx = x.idx; u.pal = x.pal; u.used = true;
            hit = uploaded_.emplace(keyOf(x), std::move(u)).first;
            if (pack.shown() && i < reps.size() && reps[i].ok && !reps[i].rgba.empty()) {
                // the pack's picture, already keyed as `toRgbaKeyed` keys
                const omk::PackImage& rep = reps[i];
                hit->second.t.su = static_cast<float>(rep.w) / static_cast<float>(x.width);
                hit->second.t.sv = static_cast<float>(rep.h) / static_cast<float>(x.height);
                hit->second.t.packed = true;
                queue([&](unsigned char* d) { std::memcpy(d, rep.rgba.data(), rep.rgba.size()); },
                      rep.w, rep.h, hit->second.t);
            } else {
                queue([&](unsigned char* d) { x.toRgbaKeyed(d); }, x.width, x.height, hit->second.t);
            }
        }
        fresh.push_back({i, hit->first});
    }

    if (!pend.empty()) {
        // THE MIP CHAIN - the trilinear enhancement's. Vulkan blits a ladder
        // of half-size LINEAR copies on the GPU; D3D12 has no blit, so the
        // same ladder is averaged here, 2x2 texels rounded, which is what a
        // linear blit at exactly half size computes. The (0,0,0,0) key texels
        // keep every level premultiplied, as there.
        std::vector<ID3D12Resource*> staging;
        ID3D12GraphicsCommandList* l = oneShotBegin();
        for (Pending& p : pend) {
            UINT16 levels = 1;
            if (filter_ >= 2)
                for (int m = std::max(p.w, p.h); m > 1; m >>= 1) ++levels;
            p.out->img = makeImage(DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(p.w), static_cast<UINT>(p.h),
                                   1, levels, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                   nullptr, "textures");
            if (!p.out->img) continue;
            D3D12_RESOURCE_DESC rd = p.out->img->GetDesc();
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(levels);
            UINT64 total = 0;
            dev_->GetCopyableFootprints(&rd, 0, levels, 0, fp.data(), nullptr, nullptr, &total);
            ID3D12Resource* sb = makeBuffer(total, D3D12_HEAP_TYPE_UPLOAD, "staging");
            if (!sb) continue;
            staging.push_back(sb);
            unsigned char* dst = nullptr;
            sb->Map(0, nullptr, reinterpret_cast<void**>(&dst));
            std::vector<unsigned char> cur = p.rgba, nxt;
            int pw = p.w, ph = p.h;
            for (UINT16 lv = 0; lv < levels; ++lv) {
                if (lv > 0) {
                    const int nw = std::max(pw / 2, 1), nh = std::max(ph / 2, 1);
                    nxt.assign(static_cast<std::size_t>(nw) * static_cast<std::size_t>(nh) * 4, 0);
                    for (int y = 0; y < nh; ++y)
                        for (int x = 0; x < nw; ++x) {
                            const int x0 = std::min(2 * x, pw - 1), x1 = std::min(2 * x + 1, pw - 1);
                            const int y0 = std::min(2 * y, ph - 1), y1 = std::min(2 * y + 1, ph - 1);
                            for (int c = 0; c < 4; ++c) {
                                const auto at = [&](int xx, int yy) {
                                    return static_cast<int>(cur[(static_cast<std::size_t>(yy) * static_cast<std::size_t>(pw) +
                                                                 static_cast<std::size_t>(xx)) * 4 + static_cast<std::size_t>(c)]);
                                };
                                nxt[(static_cast<std::size_t>(y) * static_cast<std::size_t>(nw) + static_cast<std::size_t>(x)) * 4 +
                                    static_cast<std::size_t>(c)] =
                                    static_cast<unsigned char>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
                            }
                        }
                    cur.swap(nxt);
                    pw = nw; ph = nh;
                }
                for (int y = 0; y < ph; ++y)
                    std::memcpy(dst + fp[lv].Offset + static_cast<UINT64>(y) * fp[lv].Footprint.RowPitch,
                                cur.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(pw) * 4,
                                static_cast<std::size_t>(pw) * 4);
                D3D12_TEXTURE_COPY_LOCATION td{}, sl{};
                td.pResource = p.out->img;
                td.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                td.SubresourceIndex = lv;
                sl.pResource = sb;
                sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                sl.PlacedFootprint = fp[lv];
                l->CopyTextureRegion(&td, 0, 0, 0, &sl, nullptr);
            }
            sb->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = p.out->img;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            l->ResourceBarrier(1, &b);
            p.out->srv = srv_.alloc();
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = levels;
            if (p.out->srv != UINT(-1)) dev_->CreateShaderResourceView(p.out->img, &sv, srv_.cpu(p.out->srv));
        }
        oneShotEnd();
        for (ID3D12Resource*& sb : staging) freeRes(sb);
    }
    for (const auto& [i, key] : fresh) tex_[i] = uploaded_[key].t;
}

// ============================================================ geometry

bool D3D12Renderer::uploadGeometry(const omk::Geometry* g) {
    // Cached by pointer, but only while the CONTENT has not moved
    // (`Geometry::revision`) - vkrender.cpp has the bug this prevents.
    const auto seen = vboRev_.find(g);
    const auto have = vbo_.find(g);
    if (have != vbo_.end() && seen != vboRev_.end() && seen->second == g->revision) return true;
    bool grow = false;
    if (have != vbo_.end()) {
        const auto it = vboN_.find(g);
        if (it != vboN_.end() && have->second.cap >= g->corners.size()) {
            // refilled in place: the previous frame has been waited on. Only
            // the corners that moved, when the geometry says which
            // (`Geometry::dirtyCorners`, todo/optimization.md step 8).
            static const bool noDirty = omk::envFlag("OMK_NO_DIRTY");
            const bool partial = !noDirty && seen != vboRev_.end() && g->dirtyTo != 0 &&
                                 g->dirtyTo == g->revision && seen->second == g->dirtyFrom &&
                                 it->second == g->corners.size();
            GpuVert* dst = have->second.ptr;
            const auto* lose = bakedOf(g);
            if (partial) {
                for (const std::uint32_t i : g->dirtyCorners)
                    if (i < g->corners.size()) dst[i] = vertOf(*g, i, lose);
            } else {
                for (std::size_t i = 0; i < g->corners.size(); ++i) dst[i] = vertOf(*g, i, lose);
                tie_[g].vboReplaced();
            }
            vboRev_[g] = g->revision;
            vboN_[g] = g->corners.size();
            return true;
        }
        deadBufs_.push_back(have->second.res);   // in flight in THIS frame's list, maybe
        vbo_.erase(have); vboN_.erase(g); vboRev_.erase(g);
        grow = true;
    }
    const std::size_t n = g->corners.size();
    if (!n) return false;
    // a first buffer is exact; one that outgrew its buffer gets half again
    const std::size_t cap = grow ? n + n / 2 : n;
    Vbo b;
    b.res = makeBuffer(cap * sizeof(GpuVert), D3D12_HEAP_TYPE_UPLOAD, "vertex buffers");
    if (!b.res) return false;
    D3D12_RANGE none{0, 0};
    if (FAILED(b.res->Map(0, &none, reinterpret_cast<void**>(&b.ptr)))) { freeRes(b.res); return false; }
    b.cap = cap;
    const auto* lose = bakedOf(g);
    for (std::size_t i = 0; i < n; ++i) b.ptr[i] = vertOf(*g, i, lose);
    vbo_[g] = b;
    g->resident.mark();
    vboN_[g] = n;
    vboRev_[g] = g->revision;
    tie_[g].vboReplaced();
    return true;
}

// Vulkan's `bakeDepthTie`, unchanged: the losers marked in every upload and
// drawn two depth steps back by `scene.vert`.
bool D3D12Renderer::bakeDepthTie(const omk::Geometry* g, std::span<const omk::Draw> order) {
    if (omk::tieBakeOff() || !g) return false;
    const std::vector<std::size_t> tris = omk::bakeTieLosers(*g, order);
    std::vector<std::uint8_t> lose(g->corners.size(), 0);
    for (const std::size_t tri : tris)
        for (std::size_t k = 3 * tri; k < 3 * tri + 3 && k < lose.size(); ++k) lose[k] = 1;
    baked_[g] = std::move(lose);
    tie_.erase(g);
    vboRev_.erase(g);
    static const bool tieLog = omk::envFlag("OMK_TIE_LOG");
    if (tieLog)
        std::printf("[tie] d3d12: baked %zu losers over %zu draws on geometry %p\n",
                    tris.size(), order.size(), static_cast<const void*>(g));
    return true;
}

// Vulkan's `resolveTies`, unchanged: a loser is DEGENERATED in the vertex
// buffer, a face no longer losing put back.
void D3D12Renderer::resolveTies(const omk::Draw& d) {
    if (omk::tieOff()) return;
    const omk::Geometry* g = d.geo;
    if (baked_.count(g)) return;
    auto& t = tie_[g];
    std::vector<std::size_t> losers, restore;
    t.resolve(*g, d.start, d.count, d.blend == omk::Blend::Opaque, losers, restore);
    if (losers.empty() && restore.empty()) return;
    static const bool tieLog = omk::envFlag("OMK_TIE_LOG");
    if (tieLog)
        for (const std::size_t tri : losers)
            std::printf("[tie] triangle %zu mesh %d at %.1f %.1f %.1f (draw start %zu count %zu)\n",
                        tri, 3 * tri < g->cornerMesh.size() ? g->cornerMesh[3 * tri] : -1,
                        g->corners[3 * tri].x, g->corners[3 * tri].y, g->corners[3 * tri].z,
                        d.start, d.count);
    const auto vb = vbo_.find(g);
    const auto vn = vboN_.find(g);
    if (vb == vbo_.end() || vn == vboN_.end()) return;
    GpuVert* v = vb->second.ptr;
    for (const std::size_t tri : restore) {
        const std::size_t c = 3 * tri;
        if (c + 2 >= vn->second || c + 2 >= g->corners.size()) continue;
        for (std::size_t k = c; k < c + 3; ++k) {
            const auto& s = g->corners[k];
            v[k] = {s.x, s.y, s.z, s.u, s.v, s.r, s.g, s.b, s.nx, s.ny, s.nz, s.phase, 0.0f};   // mesh 0, as Vulkan's
        }
    }
    for (const std::size_t tri : losers) {
        const std::size_t c = 3 * tri;
        if (c + 2 >= vn->second) continue;
        v[c + 1].x = v[c + 2].x = v[c].x;
        v[c + 1].y = v[c + 2].y = v[c].y;
        v[c + 1].z = v[c + 2].z = v[c].z;
    }
    t.dropped += static_cast<long>(losers.size());
    t.touched = true;
}

// ============================================================ the frame

void D3D12Renderer::setViewport(const omk::View& view) {
    // ...in RENDER pixels, so the letterbox's strip scales with the frame -
    // by the supersample, or by the upscaler's ratio (`toRenderX` is `* ss_`
    // exactly when there is no upscaler)
    const int vw = toRenderX(view.letterboxed() ? view.vw : w_);
    const int vh = toRenderY(view.letterboxed() ? view.vh : h_);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, vw, vh};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sc);
}

// Vulkan's `pushView`, line for line: the view-projection from the SOFTWARE
// rasterizer's own basis, column-major, Z in [0,1] - D3D's depth range too.
// The Y-down sense Vulkan's clip space happens to share with the game is
// undone by the vertex shader's translation (`--flip-vert-y`), not here, so
// this matrix and the shadow's are byte-identical to Vulkan's.
void D3D12Renderer::pushView(const omk::View& view) {
    omk::RCamera cam = view.cam;
    flipX_ = cam.flipX;
    cam.w = view.letterboxed() ? view.vw : w_;
    cam.h = view.letterboxed() ? view.vh : h_;
    float s[3], u[3], f[3], tanH = 0, tanV = 0;
    omk::cameraBasis(cam, s, u, f, tanH, tanV);
    viewTanH_ = tanH; viewTanV_ = tanV;
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const float A = farP / (farP - nearP), B = -farP * nearP / (farP - nearP);
    const float e[3] = {cam.eye[0], cam.eye[1], cam.eye[2]};
#if OMK_VR
    const float lx = cam.lensX, ly = cam.lensY;
    const float fe0 = f[0] * e[0] + f[1] * e[1] + f[2] * e[2];
    const float r0[4] = {(s[0] - lx * f[0]) / tanH, (s[1] - lx * f[1]) / tanH,
                         (s[2] - lx * f[2]) / tanH,
                         (-(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]) + lx * fe0) / tanH};
    const float r1[4] = {-(u[0] - ly * f[0]) / tanV, -(u[1] - ly * f[1]) / tanV,
                         -(u[2] - ly * f[2]) / tanV,
                         ((u[0] * e[0] + u[1] * e[1] + u[2] * e[2]) - ly * fe0) / tanV};
#else
    const float r0[4] = {s[0] / tanH, s[1] / tanH, s[2] / tanH,
                         -(s[0] * e[0] + s[1] * e[1] + s[2] * e[2]) / tanH};
    const float r1[4] = {-u[0] / tanV, -u[1] / tanV, -u[2] / tanV,
                         (u[0] * e[0] + u[1] * e[1] + u[2] * e[2]) / tanV};
#endif
    const float r2[4] = {A * f[0], A * f[1], A * f[2],
                         -A * (f[0] * e[0] + f[1] * e[1] + f[2] * e[2]) + B};
    const float r3[4] = {f[0], f[1], f[2], -(f[0] * e[0] + f[1] * e[1] + f[2] * e[2])};
    const float* rows[4] = {r0, r1, r2, r3};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            push_.mvp[c * 4 + r] = rows[r][c];
    // THE UPSCALER's previous camera for the draws that follow. The frame's
    // MAIN view (its eye is `begin`'s) moves from where the last frame's main
    // view was; a mirror's reflected view is given no camera motion - its
    // previous matrix is its current one - which only the reflection's own
    // pixels carry.
    if (mvOn_) {
        const bool mainView = cam.eye[0] == frameEye_[0] && cam.eye[1] == frameEye_[1] &&
                              cam.eye[2] == frameEye_[2];
        if (mainView) {
            for (int k = 0; k < 3; ++k) { mainS_[k] = s[k]; mainU_[k] = u[k]; mainF_[k] = f[k]; }
            mainA_ = A; mainB_ = B;
        }
        std::memcpy(pm_.prevMvp, mainView && havePrev_ ? prevMain_ : push_.mvp, sizeof pm_.prevMvp);
    }
}

// THE DEPTH PASS FROM THE LIGHT - Vulkan's `shadowPass`, its matrices
// unchanged (the same matrix writes the map and reads it, so whatever a clip
// space does to the handedness cancels between the two - and the Y-flipped
// `shadow.vert` keeps row 0 of the map where `scene.frag` looks for it).
void D3D12Renderer::shadowPass(const omk::View& v, std::span<const omk::Draw> casters) {
    if (!shPipe_ || !v.shadow.on || casters.empty()) return;
    constexpr float kLampSize = 30.0f;
    constexpr float kOrthoSpread = 0.06f;
    const auto lightMatrix = [](const omk::View::ShadowLight& sl, float m[16],
                                float lpos[4], float prm[4]) -> bool {
        const float R = sl.radius > 1.0f ? sl.radius : 1.0f;
        float f[3] = {sl.dir[0], sl.dir[1], sl.dir[2]};
        float D = 0.0f;
        if (sl.point) {
            float t[3] = {sl.centre[0] - sl.pos[0], sl.centre[1] - sl.pos[1], sl.centre[2] - sl.pos[2]};
            D = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (D > 40.0f) for (int k = 0; k < 3; ++k) f[k] = t[k];
            else D = 0.0f;
        }
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (fl < 1e-6f) return false;
        for (float& c : f) c /= fl;
        float up[3] = {0.0f, -1.0f, 0.0f};
        if (std::fabs(f[0] * up[0] + f[1] * up[1] + f[2] * up[2]) > 0.99f) {
            up[0] = 1.0f; up[1] = 0.0f; up[2] = 0.0f;
        }
        float r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2],
                      up[0] * f[1] - up[1] * f[0]};
        const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        for (float& c : r) c /= rl;
        float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2],
                      f[0] * r[1] - f[1] * r[0]};
        for (int k = 0; k < 16; ++k) m[k] = 0.0f;
        const float* ax[3] = {r, u, f};
        if (D > 0.0f) {
            const float n = std::max((D - R) * 0.9f, 8.0f), fa = D + R * 4.0f;
            const float sn = std::min(R / D, 0.866f);
            const float s = std::sqrt(1.0f - sn * sn) / sn;
            const float A = fa / (fa - n), B = -fa * n / (fa - n);
            float d[3];
            for (int row = 0; row < 3; ++row) {
                d[row] = 0.0f;
                for (int c = 0; c < 3; ++c) d[row] += ax[row][c] * sl.pos[c];
            }
            for (int c = 0; c < 3; ++c) {
                m[c * 4 + 0] = r[c] * s;
                m[c * 4 + 1] = u[c] * s;
                m[c * 4 + 2] = f[c] * A;
                m[c * 4 + 3] = f[c];
            }
            m[12] = -d[0] * s; m[13] = -d[1] * s;
            m[14] = -d[2] * A + B; m[15] = -d[2];
            for (int k = 0; k < 3; ++k) lpos[k] = sl.pos[k];
            lpos[3] = 1.0f;
            prm[0] = n; prm[1] = fa; prm[2] = kLampSize * s * 0.5f; prm[3] = 1.0f;
            return true;
        }
        const float back = R * 2.0f;
        float eye[3];
        for (int k = 0; k < 3; ++k) eye[k] = sl.centre[k] - f[k] * back;
        const float farD = back + R * 2.0f;   // `far` is a <windows.h> macro
        const float sc[3] = {1.0f / R, 1.0f / R, 1.0f / farD};
        for (int row = 0; row < 3; ++row) {
            float d = 0.0f;
            for (int c = 0; c < 3; ++c) {
                m[c * 4 + row] = ax[row][c] * sc[row];
                d += ax[row][c] * eye[c];
            }
            m[12 + row] = -d * sc[row];
        }
        m[15] = 1.0f;
        for (int k = 0; k < 3; ++k) lpos[k] = f[k];
        lpos[3] = 0.0f;
        prm[0] = 0.0f; prm[1] = farD; prm[2] = kOrthoSpread / (2.0f * R); prm[3] = 0.0f;
        return true;
    };
    const omk::View::ShadowLight* lights[kShadowLayers] = {
        &v.shadow, &v.extraShadows[0], &v.extraShadows[1]};
    float mats[kShadowLayers][16] = {};
    float lposs[kShadowLayers][4] = {}, prms[kShadowLayers][4] = {};
    bool live[kShadowLayers] = {};
    for (int L = 0; L < kShadowLayers; ++L)
        live[L] = lights[L]->on && lights[L]->strength > 0.0f &&
                  lightMatrix(*lights[L], mats[L], lposs[L], prms[L]);
    if (!live[0]) return;

    openFrameList();
    // a posed caster is placed on the CPU: the depth-only pipeline reads no pose
    std::vector<omk::Draw> placed;
    placed.reserve(casters.size());
    for (const auto& d : casters)
        placed.push_back(d.meshPose && d.meshPoses && d.geo &&
                                 d.geo->cornerMesh.size() == d.geo->corners.size()
                             ? cpuPose(d) : d);
    for (const auto& d : placed) if (d.geo) uploadGeometry(d.geo);

    shadow_.to(list_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    list_->SetGraphicsRootSignature(shRs_);
    list_->SetPipelineState(shPipe_);
    bound_ = shPipe_;
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(kShadowSide), static_cast<float>(kShadowSide), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, static_cast<LONG>(kShadowSide), static_cast<LONG>(kShadowSide)};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sr);
    // EVERY layer cleared each frame - an unlit one only cleared
    for (int L = 0; L < kShadowLayers; ++L) {
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_.cpu(1 + static_cast<UINT>(L));
        list_->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        if (!live[L]) continue;
        list_->SetGraphicsRoot32BitConstants(0, 16, mats[L], 0);
        for (const auto& d : placed) {
            if (!d.geo || !d.count) continue;
            const auto it = vbo_.find(d.geo);
            if (it == vbo_.end()) continue;
            const D3D12_VERTEX_BUFFER_VIEW vbv{it->second.res->GetGPUVirtualAddress(),
                                               static_cast<UINT>(it->second.cap * sizeof(GpuVert)),
                                               sizeof(GpuVert)};
            list_->IASetVertexBuffers(0, 1, &vbv);
            list_->DrawInstanced(static_cast<UINT>(d.count), 1, static_cast<UINT>(d.start), 0);
        }
    }
    shadow_.to(list_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (shUboPtr_) {
        ShadowUbo ub{};
        for (int L = 0; L < kShadowLayers; ++L) {
            std::memcpy(ub.lightMvp[L], mats[L], sizeof mats[L]);
            ub.strength[L] = live[L] ? lights[L]->strength : 0.0f;
            std::memcpy(ub.lightPos[L], lposs[L], sizeof lposs[L]);
            std::memcpy(ub.params[L], prms[L], sizeof prms[L]);
        }
        ub.texel = 1.0f / static_cast<float>(kShadowSide);
        ub.bias = 1.5f;
        ub.shimmer = shimmerClock_;
        std::memcpy(shUboPtr_, &ub, sizeof ub);
    }
    shadowLive_ = true;
}

void D3D12Renderer::begin(const omk::View& view) {
    st_ = omk::RasterStats{};
    poseUsed_ = 0;
    poseCache_.clear();
    for (ID3D12Resource*& r : deadBufs_) freeRes(r);   // the frame that used them was waited for
    deadBufs_.clear();
    fog_ = view.fog;
    fogStart_ = view.fogStart;
    fogEnd_ = view.fogEnd;
    for (int i = 0; i < 3; ++i) fogColour_[i] = static_cast<float>(view.fogColour[i]) / 255.0f;
    for (int i = 0; i < 3; ++i) clearColour_[i] = static_cast<float>(view.clearColour[i]) / 255.0f;
    for (int i = 0; i < 3; ++i) frameEye_[i] = view.cam.eye[i];   // the MAIN view's (`pushView`)
    pushView(view);
    shimmerClock_ = view.shimmerClock;
    enhOff_ = view.enhOff;
    dither_ = view.dither;
    colourDepth_ = view.colourDepth;
    foc_.begin(view, w_, h_, postReq_.dof);   // the post chain's view and depth of field's focus
    litCount_ = 0;
    if (litUboPtr_) {
        LightUbo ub{};
        const int n = static_cast<int>(std::min<std::size_t>(
            view.lights.size(), static_cast<std::size_t>(omk::View::kMaxGpuLights)));
        for (int i = 0; i < n; ++i) {
            const auto& l = view.lights[static_cast<std::size_t>(i)];
            for (int k = 0; k < 3; ++k) {
                ub.l[i].posA[k] = l.pos[k];
                ub.l[i].dirB[k] = l.dir[k];
                ub.l[i].colourI[k] = l.colour[k];
            }
            ub.l[i].posA[3] = l.radiusA;
            ub.l[i].dirB[3] = l.radiusB;
            ub.l[i].colourI[3] = l.intensity;
        }
        ub.count = n;
        ub.base = view.litBase;
        ub.grey = view.grey ? 1 : 0;
        for (int k = 0; k < 3; ++k) ub.eye[k] = view.cam.eye[k];
        ub.eye[3] = view.setDim;   // the remaster's night (1: a multiply by one)
        ub.fx[1] = view.mist; ub.fx[2] = view.mistGround; ub.fx[3] = view.mistHeight;   // THE MIST
        if (softOn_) {
            const float nearP = omk::kNearCut, farP = 200000.0f;
            ub.soft[0] = farP / (farP - nearP);
            ub.soft[1] = -farP * nearP / (farP - nearP);
            ub.soft[2] = 1.0f / softRange_;
        }
        litCount_ = n;
        std::memcpy(litUboPtr_, &ub, sizeof ub);
    }
    // VOLUMETRIC LAMP LIGHT: the air's lamps (`View::airLights`) and the main
    // view's camera, for `runVol` (`o3de/postfx.h`: one build for both backends)
    volCount_ = 0;
    if (volOn_ && volUboPtr_) {
        omk::postfx::VolFrame vf;
        omk::postfx::buildVolFrame(view, w_, h_, omk::postfx::VolTune{volDensity_, volCore_, volReach_}, vf);
        std::memcpy(volUboPtr_, &vf.ubo, sizeof vf.ubo);
        volCount_ = vf.count;
        for (int k = 0; k < 3; ++k) { volEye_[k] = vf.eye[k]; volS_[k] = vf.s[k]; volU_[k] = vf.u[k]; volF_[k] = vf.f[k]; }
        volTanH_ = vf.tanH; volTanV_ = vf.tanV;
        omk::postfx::volLampEvents(vf, volSig_, volFrame_);
    }
    softDraws_ = 0;
    depthRo_ = false;
    openFrameList();
    // a frame with no shadow pass says so through the uniform
    if (!shadowLive_ && shUboPtr_) {
        ShadowUbo ub{};
        ub.texel = 1.0f / static_cast<float>(kShadowSide);
        ub.shimmer = shimmerClock_;
        std::memcpy(shUboPtr_, &ub, sizeof ub);
    }
    Res& rt = samples_ > 1 ? msColour_ : colour_;
    rt.to(list_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    depth_.to(list_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_.cpu(samples_ > 1 ? rtvMs_ : rtvColour_);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_.cpu(dsvDepth_);
    // with an upscaler, the MOTION VECTORS beside the colour, cleared to none
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv, rtv_.cpu(rtvMotion_)};
    if (mvOn_) {
        motion_.to(list_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        list_->ClearRenderTargetView(rtvs[1], zero, 0, nullptr);
    }
    list_->OMSetRenderTargets(mvOn_ ? 2 : 1, rtvs, FALSE, &dsv);
    // the WHOLE render target, at the RENDER size (vkrender.cpp: a smaller
    // area would leave the supersample resolve averaging garbage); the engine
    // clears to the scene's fog colour (`View::clearColour`)
    const float cc[4] = {clearColour_[0], clearColour_[1], clearColour_[2], 1.0f};
    list_->ClearRenderTargetView(rtv, cc, 0, nullptr);
    list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    setViewport(view);
    list_->OMSetStencilRef(1);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->SetGraphicsRootSignature(sceneRs_);
    list_->SetGraphicsRootConstantBufferView(2, shUbo_->GetGPUVirtualAddress());
    list_->SetGraphicsRootDescriptorTable(3, srv_.gpu(kSlotShadow));
    list_->SetGraphicsRootConstantBufferView(4, litUbo_->GetGPUVirtualAddress());
    if (poseBuf_) list_->SetGraphicsRootShaderResourceView(5, poseBuf_->GetGPUVirtualAddress());
    if (softOn_) list_->SetGraphicsRootDescriptorTable(softParam_, srv_.gpu(kSlotDepth));
    if (rtOn_) rtBind(view);
    bound_ = nullptr;
    recording_ = true;
    aoTanH_ = viewTanH_; aoTanV_ = viewTanV_;
    aoVw_ = toRenderX(view.letterboxed() ? view.vw : w_);
    aoVh_ = toRenderY(view.letterboxed() ? view.vh : h_);
    // ---- THE UPSCALER's per-frame half (`upscale.h`) - and FRAME GENERATION's,
    // which needs the same motion, cuts and previous poses (not the jitter) ----
    if (mvOn_) {
        std::memcpy(curMain_, push_.mvp, sizeof curMain_);
        // THE CAMERA CUT: the engine's camera jumps - a cutscene's editing, an
        // area load, a scripted placement - and the history from before the
        // jump would smear the old picture over the new. More than five
        // metres (200 inches) in a frame, or a turn of more than 30 degrees,
        // is a jump; a first frame, a resize or a new set are too.
        if (havePrev_) {
            const float turn = mainF_[0] * prevFwd_[0] + mainF_[1] * prevFwd_[1] + mainF_[2] * prevFwd_[2];
            if (omk::framegen::cameraCut(frameEye_, prevEye_, turn)) resetOwed_ = true;
        } else {
            resetOwed_ = true;
        }
        // across a cut the vectors say "no motion": the history is dropped anyway
        if (resetOwed_) std::memcpy(pm_.prevMvp, push_.mvp, sizeof pm_.prevMvp);
        // THE JITTER: the SDK's sequence, one step a frame, in render pixels;
        // into clip units over the VIEWPORT (the letterbox's strip, when there
        // is one), Y in Vulkan's down-pointing clip space (`upscale.h`)
        if (up_) {
            const int phases = up_->jitterPhases(rw_, w_);
            up_->jitter(jitterIndex_ % phases, phases, jitterPx_[0], jitterPx_[1]);
            ++jitterIndex_;
            pm_.jitter[0] = 2.0f * jitterPx_[0] / static_cast<float>(aoVw_ > 0 ? aoVw_ : rw_);
            pm_.jitter[1] = 2.0f * jitterPx_[1] / static_cast<float>(aoVh_ > 0 ? aoVh_ : rh_);
        }
        // the bodies' affines: last frame's become the previous ones
        prevAffines_.swap(curAffines_);
        curAffines_.clear();
    }
}

// A POSED body's block in the pose buffer - Vulkan's `writePose`, the offset
// now a root SRV's address rather than a dynamic offset.
bool D3D12Renderer::writePose(const omk::Draw& d, UINT64& off) {
    const auto key = std::make_pair(d.meshPose, d.vertexLights);
    if (const auto it = poseCache_.find(key); it != poseCache_.end()) { off = it->second; return true; }
    const UINT64 at = alignUp(poseUsed_, kPoseAlign);
    if (!posePtr_ || at + (mvOn_ ? 2 : 1) * kPoseRange > kPoseBytes || d.meshPoses > kPoseMaxMeshes) return false;
    float* f = reinterpret_cast<float*>(static_cast<unsigned char*>(posePtr_) + at);
    const int nl = d.vertexLights ? std::min(d.vertexLightCount, kPoseLights) : 0;
    f[0] = static_cast<float>(nl);
    f[1] = d.lightsFromBlack ? 1.0f : 0.0f;
    f[2] = d.lightBase;
    f[3] = static_cast<float>(d.meshPoses);
    for (int i = 0; i < nl; ++i) std::memcpy(f + 4 + 8 * i, d.vertexLights + 8 * i, 8 * sizeof(float));
    float* a = f + 4 + 8 * kPoseLights;
    std::memcpy(a, d.meshPose, d.meshPoses * 12 * sizeof(float));
    // ...then the vec4 after the affines: the body light's tint (`scene.vert`; the remaster's A5,
    // 1, 1, 1 unless it is on)
    float* t = a + 12 * d.meshPoses;
    t[0] = d.lightTint[0]; t[1] = d.lightTint[1]; t[2] = d.lightTint[2]; t[3] = 1.0f;
    poseUsed_ = at + (1 + 2 * kPoseLights + 3 * d.meshPoses + 1) * 16;
    // ...and with an upscaler, the body's PREVIOUS affines right after them
    // (`scene.vert` -DOMK_MOTION), from the last frame's block of the same
    // `meshPose` - or the current ones again for a body new this frame, which
    // then shows only the camera's motion rather than a wrong one of its own
    if (mvOn_) {
        const std::size_t n = d.meshPoses * 12;
        const auto prev = prevAffines_.find(d.meshPose);
        // (and across a cut, where nothing has a previous position)
        const float* from = !resetOwed_ && prev != prevAffines_.end() && prev->second.size() == n
                                ? prev->second.data() : d.meshPose;
        std::memcpy(a + n + 4, from, n * sizeof(float));
        curAffines_[d.meshPose].assign(d.meshPose, d.meshPose + n);
        poseUsed_ += 3 * d.meshPoses * 16;
    }
    off = at;
    poseCache_.emplace(key, off);
    return true;
}

// Vulkan's `cpuPose`: a posed draw placed and lit on the CPU, for what the
// pose buffer does not reach and for every caster of the shadow map.
omk::Draw D3D12Renderer::cpuPose(const omk::Draw& d) {
    auto slot = cpuPoseSlot_.find({d.meshPose, d.geo});
    if (slot == cpuPoseSlot_.end())
        slot = cpuPoseSlot_.emplace(std::make_pair(d.meshPose, d.geo), cpuPoseCount_[d.geo]++).first;
    omk::Geometry& cp = cpuPosed_[{d.geo, slot->second}];
    omk::poseGeometry(d, cp, omk::PoseParts{true, true, kPoseLights});
    cp.revision = d.geo->revision + (++cpuPoseRev_ << 32);
    omk::Draw out = d;
    out.geo = &cp;
    out.meshPose = nullptr;
    out.meshPoses = 0;
    out.vertexLights = nullptr;
    out.vertexLightCount = 0;
    out.lightsFromBlack = false;
    return out;
}

void D3D12Renderer::submit(const omk::Draw& d) {
    if (!recording_ || !d.geo || !d.count) return;
    foc_.note(d);
    curBody_ = (d.meshPose && d.meshPoses) || d.castsShadow;
    // HYBRID RAY TRACING: the draw into the frame's acceleration structures,
    // and a posed body takes the character's traced terms even with the
    // game's shadows off (`caster` - no shadow then receives anyway)
    if (rtOn_) {
        rtNote(d);
        rtBodyDraw_ = omk::rt::isBodyDraw(d);
        rtFloorDraws_ += (d.lit & omk::kDrawFloor) != 0;
    }
    if (d.meshPose && d.meshPoses && d.geo->cornerMesh.size() == d.geo->corners.size()) {
        UINT64 off = 0;
        if (posePtr_ && writePose(d, off)) submitOne(d, true, off);
        else submitOne(cpuPose(d), false, 0);
        return;
    }
    submitOne(d, false, 0);
}

void D3D12Renderer::submitOne(const omk::Draw& d, bool posed, UINT64 poseOff) {
    if (!uploadGeometry(d.geo)) return;
    resolveTies(d);
    st_.triangles += static_cast<long>(d.count / 3);

    const int k = d.blend == omk::Blend::Opaque ? 0 : d.blend == omk::Blend::Add ? 1 : 2;
    // SOFT PARTICLES: an effect sprite (`kDrawSoft`) is drawn by the soft
    // twin with the depth bound READ-ONLY - so its shader can read the depth it
    // is tested against - and the next draw that writes depth binds it
    // writable again (the engine's order puts the sprites after the opaque)
    const bool soft = softOn_ && !enhOff_ && k > 0 && (d.lit & omk::kDrawSoft) && !forcePso_ && !reflStencil_;
    if (soft && !depthRo_) depthReadOnly(true);
    else if (depthRo_ && (k == 0 || forcePso_)) depthReadOnly(false);
    softDraws_ += soft;
    ID3D12PipelineState* use = forcePso_ ? forcePso_ : reflStencil_ ? pipeRefl_[k]
                             : soft ? pipeSoft_[k] : rtCounting_ ? pipeCount_[k] : pipe_[k];
    // a translucent body on a frame with depth of field: its twin, which marks the stencil
    const bool subj = foc_.subjStencil && k > 0 && curBody_ && !soft && !forcePso_ && !reflStencil_ &&
                      (rtCounting_ ? pipeCountSubj_[k] : pipeSubj_[k]) != nullptr;
    if (subj) use = rtCounting_ ? pipeCountSubj_[k] : pipeSubj_[k];
    if (use != bound_) { list_->SetPipelineState(use); bound_ = use; }
    if (subj) list_->OMSetStencilRef(kSubjectStencil);

    // The texture is the key's LOW SIX BITS and nothing else (ASSETS 4b) -
    // or the pool's full index above them, past 64 (`drawTextureSlot`).
    const std::size_t slot = omk::drawTextureSlot(d);
    const UINT srv = (slot < tex_.size() && tex_[slot].srv != UINT(-1)) ? tex_[slot].srv : white_.srv;
    list_->SetGraphicsRootDescriptorTable(1, srv_.gpu(srv));
    // set 2, the pose buffer - bound for every draw, at the body's block for
    // a posed one
    if (poseBuf_) list_->SetGraphicsRootShaderResourceView(5, poseBuf_->GetGPUVirtualAddress() + poseOff);
    push_.posed = posed ? 1 : 0;
    push_.cutout = d.cutout ? 1 : 0;
    {
        const Tex& bound = (slot < tex_.size() && tex_[slot].srv != UINT(-1)) ? tex_[slot] : white_;
        const auto k = [](float s) { const long r = static_cast<long>(s + 0.5f); return static_cast<std::uint32_t>(r < 1 ? 0 : r > 16 ? 15 : r - 1); };
        texScale_ = bound.packed ? (k(bound.su) | (k(bound.sv) << 4)) : 0u;
    }
    push_.caster = (d.castsShadow || (rtOn_ && rtBodyDraw_)) ? 1 : 0;
    push_.lit = litCount_ > 0 ? d.lit : (d.lit & (omk::kDrawShine | omk::kDrawFloor));
    if (!rtReflOn_) push_.lit &= ~omk::kDrawFloor;
    // THE FOG's two exclusions, the same rule `renderer.cpp` applies for the
    // software loop and Vulkan applies in `submitOne`: key bits 0x2080 not
    // fogged, key bit 0x800 doubles both ends.
    push_.fogStart = 0.0f;
    push_.fogEnd = 0.0f;
    if (fog_ && !(d.bucketKey & 0x2080u)) {
        const float fogScale = (d.bucketKey & 0x800u) ? 2.0f : 1.0f;
        push_.fogStart = fogStart_ * fogScale;
        push_.fogEnd = fogEnd_ * fogScale;
        for (int i = 0; i < 3; ++i) push_.fogColour[i] = fogColour_[i];
    }
    const auto it = vbo_.find(d.geo);
    const D3D12_VERTEX_BUFFER_VIEW vbv{it->second.res->GetGPUVirtualAddress(),
                                       static_cast<UINT>(it->second.cap * sizeof(GpuVert)), sizeof(GpuVert)};
    list_->IASetVertexBuffers(0, 1, &vbv);
    // THE BACK-FACE CULL, one set of root constants per run of `cornerCull`;
    // the mirror's screen-X flip reverses the sense, as it reverses the area
    // `raster.cpp` tests.
    const omk::Geometry* g = d.geo;
    const bool haveCull = g->cornerCull.size() == g->corners.size();
    std::size_t i = d.start;
    const std::size_t e = d.start + d.count;
    while (i < e) {
        const std::uint8_t c = haveCull ? g->cornerCull[i] : 0u;
        std::size_t j = i;
        while (j < e && (haveCull ? g->cornerCull[j] : 0u) == c) ++j;
        push_.cull = c ? (flipX_ ? -kCullSign : kCullSign) : 0;
        if (mvOn_) {
            pm_.p = push_;
            pm_.texScale = texScale_;
            list_->SetGraphicsRoot32BitConstants(0, kPushMvDwords, &pm_, 0);
        } else {
            const PushSc ps{push_, texScale_};
            list_->SetGraphicsRoot32BitConstants(0, kPushDwords, &ps, 0);
        }
        list_->DrawInstanced(static_cast<UINT>(j - i), 1, static_cast<UINT>(i), 0);
        i = j;
    }
    if (subj) list_->OMSetStencilRef(1);
    st_.drawn += static_cast<long>(d.count / 3);
}

// The NATIVE mirror pass, the original's two passes (`sub_440D90`) exactly as
// Vulkan's `drawMirrorScene` draws them: the scene through the reflected
// camera first, then the real one over it with nothing cleared between, the
// mirror's own faces blended last.
bool D3D12Renderer::drawMirrorScene(const omk::View& v, const omk::View& refl,
                                    std::span<const omk::Draw> scene,
                                    std::span<const omk::Draw> sceneClipped,
                                    std::span<const omk::Draw> mirror) {
    if (mirror.empty()) return false;
    begin(v);
    pushView(refl);
    for (const auto& d : sceneClipped) submit(d);
    pushView(v);
    for (const auto& d : scene) submit(d);
    for (const auto& d : mirror) submit(d);
    end();
    return true;
}

// ---- SSAO, `todo/enhancements.md` 15 ---------------------------------------
//
// Vulkan's two full-screen passes over the frame's depth: `ssao.frag` into an
// R8 target, then `ssao_blur.frag` MULTIPLIED into `colour_` through the
// blend. Made on first use at the render size; the image goes on a resize.
bool D3D12Renderer::makeSsao() {
    if (!ssaoOn_) return false;
    if (!aoRs_) {
        const D3D12_DESCRIPTOR_RANGE r = rs::srvs(0, 0, 2);   // depth (t0), the AO (t1)
        // ...and with RAY TRACING (`ssao.frag` -DOMK_RT) the camera in the push
        // block, the TLAS, the instance table and the two bindless tables
        const D3D12_DESCRIPTOR_RANGE recAll = rs::srvs(0, 4, UINT_MAX), texAll = rs::srvs(0, 5, UINT_MAX);
        D3D12_ROOT_PARAMETER p[6] = {
            rs::constants(0, kPushSpace, (rtOn_ ? sizeof(SsaoRtPush) : sizeof(SsaoPush)) / 4,
                          D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_SRV, 0, 3, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_SRV, 1, 3, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&recAll, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&texAll, D3D12_SHADER_VISIBILITY_PIXEL)};
        D3D12_STATIC_SAMPLER_DESC s[3] = {
            rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(1, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(3, 3, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0.0f)};
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = rtOn_ ? 6 : 2; d.pParameters = p;
        d.NumStaticSamplers = rtOn_ ? 3 : 2; d.pStaticSamplers = s;
        aoRs_ = makeRootSig(d, "ssao");
        if (!aoRs_) return false;
        const bool ms = samples_ > 1;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = aoRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = rtOn_ ? (ms ? rs::code(kDx_ssao_rt_ms_frag, sizeof kDx_ssao_rt_ms_frag)
                            : rs::code(kDx_ssao_rt_frag, sizeof kDx_ssao_rt_frag))
                      : ms ? rs::code(kDx_ssao_ms_frag, sizeof kDx_ssao_ms_frag)
                           : rs::code(kDx_ssao_frag, sizeof kDx_ssao_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        pd.SampleDesc.Count = 1;
        aoPipe_ = makePso(pd, "ssao");
        pd.PS = ms ? rs::code(kDx_ssao_blur_ms_frag, sizeof kDx_ssao_blur_ms_frag)
                   : rs::code(kDx_ssao_blur_frag, sizeof kDx_ssao_blur_frag);
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
        // `OMK_SSAO_VIEW=1`: the occlusion ITSELF written over the frame
        static const bool aoView = omk::envFlag("OMK_SSAO_VIEW");
        if (!aoView) {
            // dst * src: the grey darkens, white leaves the frame as it was
            b.BlendEnable = TRUE;
            b.SrcBlend = D3D12_BLEND_ZERO; b.DestBlend = D3D12_BLEND_SRC_COLOR; b.BlendOp = D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_ONE;
            b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            b.LogicOp = D3D12_LOGIC_OP_NOOP;
        }
        aoBlurPipe_ = makePso(pd, "ssao blur");
    }
    if (!aoPipe_ || !aoBlurPipe_) return false;
    if (!ao_.r) {
        ao_.r = makeImage(DXGI_FORMAT_R8_UNORM, static_cast<UINT>(rw_), static_cast<UINT>(rh_), 1, 1, 1,
                          D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          nullptr, "ssao");
        ao_.st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (!ao_.r) return false;
        dev_->CreateRenderTargetView(ao_.r, nullptr, rtv_.cpu(rtvAo_));
        dev_->CreateShaderResourceView(ao_.r, nullptr, srv_.cpu(kSlotAo));
    }
    return true;
}

void D3D12Renderer::runSsao() {
    if (enhOff_) return;
    if (!makeSsao() || aoVw_ <= 0 || aoVh_ <= 0) return;
    // the frame's depth, read where it was written
    depth_.to(list_, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ao_.to(list_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const SsaoPush push{aoTanH_, aoTanV_, farP / (farP - nearP), -farP * nearP / (farP - nearP),
                        static_cast<float>(aoVw_), static_cast<float>(aoVh_), 80.0f, 1.0f};
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(rw_), static_cast<float>(rh_), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, rw_, rh_};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sr);
    list_->SetGraphicsRootSignature(aoRs_);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // RAY-TRACED: the same push block and the camera in the world after it,
    // and the scene the rays see (`rtBind`'s, at the AO root's slots)
    const float aoMask = 255.0f;   // every instance: the set and the bodies occlude
    SsaoRtPush rp{push, {rtEye_[0], rtEye_[1], rtEye_[2], aoMask}, {rtS_[0], rtS_[1], rtS_[2], 0.0f},
                  {rtU_[0], rtU_[1], rtU_[2], 0.0f}, {rtF_[0], rtF_[1], rtF_[2], 0.0f}};
    if (rtOn_) {
        list_->SetGraphicsRootShaderResourceView(2, rtTlas_->GetGPUVirtualAddress());
        list_->SetGraphicsRootShaderResourceView(3, rtInstTab_->GetGPUVirtualAddress());
        list_->SetGraphicsRootDescriptorTable(4, srv_.gpu(0));
        list_->SetGraphicsRootDescriptorTable(5, srv_.gpu(0));
        rtAoRays_ += 4LL * aoVw_ * aoVh_;   // at most: a sky pixel casts none
    }
    const auto run = [&](D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12PipelineState* pl) {
        list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list_->SetPipelineState(pl);
        if (rtOn_) list_->SetGraphicsRoot32BitConstants(0, sizeof rp / 4, &rp, 0);
        else list_->SetGraphicsRoot32BitConstants(0, sizeof push / 4, &push, 0);
        list_->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotDepth));
        list_->DrawInstanced(3, 1, 0, 0);
    };
    run(rtv_.cpu(rtvAo_), aoPipe_);
    ao_.to(list_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    colour_.to(list_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    run(rtv_.cpu(rtvColour_), aoBlurPipe_);
    bound_ = nullptr;
}

// ---- HYBRID RAY TRACING, `todo/enhancements.md` 19 ------------------------
//
// AN ENHANCEMENT, off unless asked (`raytracing = on`, `--raytracing`), and
// this backend's alone: DXR 1.1's INLINE ray queries (tier 1.1, shader model
// 6.5) cast from the raster passes - no ray-tracing pipeline, no shader table.
// Three effects, in `backends/vulkan/shaders/rt.glsl` and the -DOMK_RT arms of
// `scene.frag` and `ssao.frag`:
//   1. a CHARACTER LIT BY ITS SURROUNDINGS - six cosine-weighted rays into the
//      set from each of its fragments, a hit returning what the eye sees there
//      (the baked vertex colour times the texel), a miss the sky; the mean is
//      added to its lamps' light and the near hits occlude its ambient;
//   2. SHADOW RAYS for the lamps that light a character: a lamp the set hides
//      gives it nothing (the mapped shadow still darkens the ground alone);
//   3. RAY-TRACED AO in SSAO's place - the same pass, 4x4 blur and multiply,
//      four short rays a pixel, the bodies occluding too.
// The set keeps its baked light, the artists' own: only the AO term touches it.
//
// The scene the rays see is made HERE, from what the frame submits:
//   * a SET geometry (no pose, not a caster) gets a BLAS once, from all of its
//     opaque and cutout BATCHES - what is culled from the view still shades a
//     character - remade when its revision moves; it stays in the TLAS while
//     it was drawn in the last 120 frames;
//   * the BODIES (posed by the pose buffer or on the CPU) share ONE BLAS rebuilt
//     every frame from their drawn opaque runs, posed here on the CPU (on the
//     shared threads) by the very affines `scene.vert` applies;
//   * one TLAS a frame over them, identity transforms (every corner is in world
//     space already), the sets mask 1, the bodies 2.
// The builds go on the one-shot list, executed and waited for in `end()` before
// the frame's list, which reads the TLAS through a root SRV at a fixed address.
ID3D12Resource* D3D12Renderer::rtBuffer(UINT64 bytes, D3D12_HEAP_TYPE type, bool uav,
                                        D3D12_RESOURCE_STATES st, const char* tag) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes ? bytes : 1;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r)))) {
        std::fprintf(stderr, "d3d12: ray tracing buffer of %llu bytes (%s) failed\n",
                     static_cast<unsigned long long>(bytes), tag);
        return nullptr;
    }
    OMK_GPU_ALLOC(tag, omk::prof::gpuKey(omk::prof::kD3dResource, r), bytes);
    return r;
}

// an UPLOAD buffer of at least `bytes`, mapped for its life; a smaller one is
// retired (the frame that used it has been waited for)
bool D3D12Renderer::rtMapped(UINT64 bytes, ID3D12Resource*& r, void*& p, UINT64& cap) {
    if (r && cap >= bytes) return true;
    if (r) deadBufs_.push_back(r);
    r = nullptr; p = nullptr; cap = 0;
    const UINT64 want = alignUp(bytes + bytes / 4, 256);
    r = rtBuffer(want, D3D12_HEAP_TYPE_UPLOAD, false, D3D12_RESOURCE_STATE_GENERIC_READ, "ray tracing");
    if (!r) return false;
    D3D12_RANGE none{0, 0};
    if (FAILED(r->Map(0, &none, &p))) { freeRes(r); return false; }
    cap = want;
    return true;
}

bool D3D12Renderer::rtInit() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    const bool tier = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)) &&
                      o5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_5};
    const bool model = SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)) &&
                       sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
    if (!tier || !model) {
        std::printf("d3d12: ray tracing asked for, but this device has no DXR 1.1 inline ray queries "
                    "(raytracing tier 0x%x, shader model 0x%x) - off\n",
                    static_cast<unsigned>(o5.RaytracingTier), static_cast<unsigned>(sm.HighestShaderModel));
        return false;
    }
    if (FAILED(dev_->QueryInterface(IID_PPV_ARGS(&dev5_))) ||
        FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rtAlloc_))) ||
        FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rtAlloc_, nullptr,
                                       IID_PPV_ARGS(&rtList_)))) {
        std::printf("d3d12: ray tracing asked for, but the device gives no ID3D12Device5 - off\n");
        release(rtList_);
        release(rtAlloc_);
        release(dev5_);
        return false;
    }
    rtList_->Close();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.NumDescs = kRtMaxInst;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
    dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
    rtTlasScratch_ = alignUp(pi.ScratchDataSizeInBytes, 256);
    rtTlas_ = rtBuffer(alignUp(pi.ResultDataMaxSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, true,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "ray tracing");
    UINT64 c0 = 0, c1 = 0, c2 = 0;
    const bool ok = rtTlas_ &&
        rtMapped(kRtMaxInst * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), rtInstDesc_, rtInstDescPtr_, c0) &&
        rtMapped(kRtMaxInst * 16, rtInstTab_, rtInstTabPtr_, c1) &&
        rtMapped(256, rtFrameCb_, rtFramePtr_, c2) &&
        (rtCount_ = rtBuffer(256, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON, "ray tracing")) &&
        (rtCountRead_ = rtBuffer(256, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST, "ray tracing"));
    if (!ok) {
        std::printf("d3d12: ray tracing - its buffers could not be made - off\n");
        rtKill();
        return false;
    }
    rtOn_ = true;
    std::printf("d3d12: RAY TRACING on - an ENHANCEMENT: DXR 1.1 inline ray queries (tier 0x%x, shader "
                "model 6.5): characters lit by the set around them (no ray-traced shadow on them), "
                "ray-traced AO in place of SSAO\n", static_cast<unsigned>(o5.RaytracingTier));
    omk::event("rt_on", {{"tier", static_cast<unsigned>(o5.RaytracingTier)}});
    return true;
}

// The frame's constants and the scene pass's six ray-tracing roots (bound
// after the scene's root signature in `begin`).
void D3D12Renderer::rtBind(const omk::View& view) {
    ++rtFrame_;
    rtCounting_ = rtFrame_ % 60 == 2;
    rtCounted_ += rtCounting_;
    rtNotes_.clear();
    omk::RCamera cam = view.cam;
    cam.w = view.letterboxed() ? view.vw : w_;
    cam.h = view.letterboxed() ? view.vh : h_;
    float tH = 0.0f, tV = 0.0f;
    omk::cameraBasis(cam, rtS_, rtU_, rtF_, tH, tV);
    for (int k = 0; k < 3; ++k) rtEye_[k] = cam.eye[k];
    // the constants, numbers and `OMK_RT_*` tunings alike, are `o3de/rtscene.cpp`'s
    RtFrame f{};
    omk::rt::fillFrame(fog_ ? fogColour_ : clearColour_, rtEye_, view.skyMean, rtReflOn_, f);
    if (view.enhOff) { f.sky[3] = 0.0f; f.refl[0] = 0.0f; }   // the original's look: no traced light, no reflections
    std::memcpy(rtFramePtr_, &f, sizeof f);
    list_->SetGraphicsRootShaderResourceView(6, rtTlas_->GetGPUVirtualAddress());
    list_->SetGraphicsRootShaderResourceView(7, rtInstTab_->GetGPUVirtualAddress());
    list_->SetGraphicsRootConstantBufferView(8, rtFrameCb_->GetGPUVirtualAddress());
    list_->SetGraphicsRootUnorderedAccessView(9, rtCount_->GetGPUVirtualAddress());
    list_->SetGraphicsRootDescriptorTable(10, srv_.gpu(0));
    list_->SetGraphicsRootDescriptorTable(11, srv_.gpu(0));
}

// A BLAS's triangles - opaque ones first - into its two upload buffers: the
// corners' positions (posed by the triangle's affines when it has them, the
// rows of a 3x4 a mesh as `scene.vert` reads them) and the records. On the
// shared threads: a frame's bodies are tens of thousands of triangles.
bool D3D12Renderer::rtFill(RtBlas& b, const std::vector<RtTri>& tri) {
    const std::size_t n = tri.size();
    void* pp = b.posPtr;
    void* rp = b.recPtr;
    ID3D12Resource* recWas = b.rec;
    if (!rtMapped(n * 36, b.pos, pp, b.posCap) || !rtMapped(n * 64, b.rec, rp, b.recCap)) return false;
    b.posPtr = static_cast<float*>(pp);
    b.recPtr = static_cast<std::uint32_t*>(rp);
    if (b.recSrv == UINT(-1)) b.recSrv = srv_.alloc();
    if (b.recSrv == UINT(-1)) return false;
    if (b.rec != recWas) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = static_cast<UINT>(b.recCap / 4);
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        dev_->CreateShaderResourceView(b.rec, &v, srv_.cpu(b.recSrv));
    }
    omk::rt::fillRecords(tri, b.posPtr, b.recPtr);
    return true;
}

// A SET geometry's BLAS inputs, made on first sight and again when its corners
// move: every opaque and cutout batch (a blended one is a glow or a decal, not
// a wall). -> null when it has none.
D3D12Renderer::RtBlas* D3D12Renderer::rtSetFor(const omk::Geometry* g) {
    const auto it = rtSet_.find(g);
    if (it != rtSet_.end() && it->second.rev == g->revision)
        return it->second.nOpaque + it->second.nCut ? &it->second : nullptr;
    RtBlas& b = rtSet_[g];
    b.rev = g->revision;
    b.built = false;
    b.texAt.clear();
    b.texGen = texGen_;
    std::vector<std::uint32_t> opq;
    omk::rt::setCorners(*g, opq, b.nOpaque, b.nCut);
    std::vector<RtTri> tris;
    tris.reserve(opq.size());
    for (const std::uint32_t c : opq) tris.push_back(RtTri{g, nullptr, 0, c, white_.srv});
    if (opq.empty() || !rtFill(b, tris)) { b.nOpaque = b.nCut = 0; return nullptr; }
    b.triOf.assign(g->corners.size() / 3, -1);
    for (std::size_t t = 0; t < opq.size(); ++t) b.triOf[opq[t] / 3] = static_cast<std::int32_t>(t);
    return &b;
}

// Every opaque draw, as it is submitted: a body's run noted for this frame's
// BLAS (its affines copied while the pointer is good), a set's texture slots
// written into its records when they changed.
void D3D12Renderer::rtNote(const omk::Draw& d) {
    if (d.blend != omk::Blend::Opaque || d.start + d.count > d.geo->corners.size()) return;
    const std::size_t slot = omk::drawTextureSlot(d);
    const UINT srv = rtTexWord(slot);   // the heap index, with a pack's k
    const bool posed = d.meshPose && d.meshPoses && d.geo->cornerMesh.size() == d.geo->corners.size();
    if (posed || d.castsShadow || (d.lit & ~(omk::kDrawSoft | omk::kDrawFloor)) != 0) {
        RtBodyNote& n = rtNotes_[{posed ? d.meshPose : nullptr, d.geo}];
        n.geo = d.geo;
        if (posed && n.aff.empty()) n.aff.assign(d.meshPose, d.meshPose + 12 * d.meshPoses);
        n.runs[d.start] = RtRun{d.count, d.cutout != 0, srv};
        return;
    }
    RtBlas* b = rtSetFor(d.geo);
    if (!b) return;
    b->seen = rtFrame_;
    if (b->texGen != texGen_) {   // a texture went: no record may name its slot
        b->texGen = texGen_;
        b->texAt.clear();
        for (std::uint32_t t = 0; t < b->nOpaque + b->nCut; ++t) b->recPtr[16 * t + 15] = white_.srv;
    }
    const auto at = b->texAt.find(d.start);
    if (at != b->texAt.end() && at->second.first == d.count && at->second.second == srv) return;
    b->texAt[d.start] = {d.count, srv};
    for (std::size_t c = d.start; c + 3 <= d.start + d.count; c += 3) {
        const std::int32_t t = c / 3 < b->triOf.size() ? b->triOf[c / 3] : -1;
        if (t >= 0) b->recPtr[16 * t + 15] = srv;
    }
}

void D3D12Renderer::rtDrop(RtBlas& b) {
    if (b.as) deadBufs_.push_back(b.as);
    if (b.pos) deadBufs_.push_back(b.pos);
    if (b.rec) deadBufs_.push_back(b.rec);
    srv_.free(b.recSrv);
    b = RtBlas{};
}

// the rays' tally (`scene.frag`'s RtCount) copied out on a one-shot list. A
// buffer rests in COMMON between lists and is PROMOTED to what a list does
// with it (a copy source here, a UAV in the scene pass, the builds' scratch),
// decaying back at the list's end - so no barrier is written for it.
void D3D12Renderer::rtTallyCopy(ID3D12GraphicsCommandList* l) {
    l->CopyBufferRegion(rtCountRead_, 0, rtCount_, 0, 16);
}
void D3D12Renderer::rtTallyRead() {
    void* p = nullptr;
    const D3D12_RANGE rr{0, 16};
    if (FAILED(rtCountRead_->Map(0, &rr, &p))) return;
    std::memcpy(rtCounts_, p, sizeof rtCounts_);
    const D3D12_RANGE none{0, 0};
    rtCountRead_->Unmap(0, &none);
}

// The frame's acceleration structures, built and waited for before the
// frame's list runs: the bodies' BLAS (rebuilt), any set BLAS not built yet,
// then the TLAS over the sets drawn lately and this frame's bodies.
void D3D12Renderer::rtBuild() {
    struct Job {
        RtBlas* b;
        bool body;
        bool refit;
        D3D12_RAYTRACING_GEOMETRY_DESC gd[2];
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi;
    };
    std::vector<Job> jobs;
    rtRecorded_ = false;
    const auto tStart = std::chrono::steady_clock::now();
    // 1. this frame's bodies, posed and recorded
    // ...ALL of them in ONE BLAS: a build has a fixed cost, and a street's
    // crowd is a couple of hundred bodies of a few hundred triangles
    {
        std::vector<RtTri> tri;
        int bodies = 0;
        std::uint64_t sig = 0;
        RtBlas& b = rtBodyAll_;
        omk::rt::gatherBodies(rtNotes_, rtEye_, tri, b.nOpaque, b.nCut, bodies, sig);
        b.built = false;
        rtBodyCount_ = bodies;
        ID3D12Resource* const posWas = b.pos;
        if (!tri.empty() && rtFill(b, tri)) {
            b.seen = rtFrame_;
            // REFITTED in place while the same runs of the same bodies make it
            // (only their poses moved), rebuilt when that changes and every
            // 30 frames - a refit's tree loosens as the limbs travel
            const bool refit = b.as && sig == rtBodySig_ && b.pos == posWas && rtFrame_ - rtBodyRebuilt_ < 30;
            if (!refit) { rtBodySig_ = sig; rtBodyRebuilt_ = rtFrame_; }
            jobs.push_back(Job{&b, true, refit, {}, {}, {}});
        } else {
            b.nOpaque = b.nCut = 0;
        }
    }
    // 2. a set's, once
    for (auto& [g, b] : rtSet_)
        if (!b.built && b.nOpaque + b.nCut && rtFrame_ - b.seen <= 120) jobs.push_back(Job{&b, false, false, {}, {}, {}});
    // their inputs and sizes, and one scratch for all of them and the TLAS
    UINT64 scratch = rtTlasScratch_;
    for (Job& j : jobs) {
        RtBlas& b = *j.b;
        const D3D12_GPU_VIRTUAL_ADDRESS va = b.pos->GetGPUVirtualAddress();
        UINT ng = 0;
        const auto geom = [&](D3D12_GPU_VIRTUAL_ADDRESS at, std::uint32_t tris, bool opaque) {
            D3D12_RAYTRACING_GEOMETRY_DESC& g = j.gd[ng++];
            g = D3D12_RAYTRACING_GEOMETRY_DESC{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            // a CUTOUT geometry is non-opaque: `rtTrace` reads its colour key
            g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
            g.Triangles.VertexCount = 3 * tris;
            g.Triangles.VertexBuffer.StartAddress = at;
            g.Triangles.VertexBuffer.StrideInBytes = 12;
        };
        if (b.nOpaque) geom(va, b.nOpaque, true);
        if (b.nCut) geom(va + 36ull * b.nOpaque, b.nCut, false);
        j.in = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{};
        j.in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        j.in.Flags = j.body ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                                  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
                            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        if (j.refit) j.in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        j.in.NumDescs = ng;
        j.in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        j.in.pGeometryDescs = j.gd;
        dev5_->GetRaytracingAccelerationStructurePrebuildInfo(&j.in, &j.pi);
        scratch += alignUp(j.refit ? j.pi.UpdateScratchDataSizeInBytes : j.pi.ScratchDataSizeInBytes, 256);
        const UINT64 need = alignUp(j.pi.ResultDataMaxSizeInBytes, 256);
        if (!j.refit && (!b.as || b.asCap < need)) {
            if (b.as) deadBufs_.push_back(b.as);
            const UINT64 cap = j.body ? alignUp(need + need / 4, 256) : need;
            b.as = rtBuffer(cap, D3D12_HEAP_TYPE_DEFAULT, true,
                            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, "ray tracing");
            b.asCap = b.as ? cap : 0;
        }
    }
    if (rtScratchCap_ < scratch) {
        if (rtScratch_) deadBufs_.push_back(rtScratch_);
        const UINT64 cap = alignUp(scratch + scratch / 4, 256);
        rtScratch_ = rtBuffer(cap, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_COMMON,
                              "ray tracing");
        rtScratchCap_ = rtScratch_ ? cap : 0;
        if (!rtScratch_) return;
    }
    rtAlloc_->Reset();
    rtList_->Reset(rtAlloc_, nullptr);
    ID3D12GraphicsCommandList4* l = rtList_;
    const D3D12_GPU_VIRTUAL_ADDRESS sva = rtScratch_->GetGPUVirtualAddress();
    UINT64 so = 0;
    for (Job& j : jobs) {
        if (!j.b->as) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd{};
        bd.Inputs = j.in;
        bd.DestAccelerationStructureData = j.b->as->GetGPUVirtualAddress();
        bd.ScratchAccelerationStructureData = sva + so;
        if (j.refit) bd.SourceAccelerationStructureData = bd.DestAccelerationStructureData;
        l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        so += alignUp(j.refit ? j.pi.UpdateScratchDataSizeInBytes : j.pi.ScratchDataSizeInBytes, 256);
        j.b->built = true;
        ++(j.refit ? rtBodyRefits_ : j.body ? rtBodyBuilds_ : rtSetBuilds_);
    }
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1, &uav);
    // 3. the TLAS: an instance a BLAS, its custom index the row of the table
    auto* inst = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(rtInstDescPtr_);
    auto* tab = static_cast<std::uint32_t*>(rtInstTabPtr_);
    UINT ni = 0;
    int sets = 0, bodies = 0;
    long long setTris = 0, bodyTris = 0;
    const auto add = [&](const RtBlas& b, UINT mask) {
        if (ni >= kRtMaxInst || !b.built || !b.as) return false;
        D3D12_RAYTRACING_INSTANCE_DESC& d = inst[ni];
        std::memset(&d, 0, sizeof d);
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1.0f;
        d.InstanceID = ni;
        d.InstanceMask = mask;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        d.AccelerationStructure = b.as->GetGPUVirtualAddress();
        tab[4 * ni] = b.recSrv;
        tab[4 * ni + 1] = b.nOpaque;
        tab[4 * ni + 2] = mask;
        tab[4 * ni + 3] = 0;
        ++ni;
        return true;
    };
    for (const auto& [g, b] : rtSet_)
        if (rtFrame_ - b.seen <= 120 && add(b, 1)) { ++sets; setTris += b.nOpaque + b.nCut; }
    if (rtBodyAll_.seen == rtFrame_ && add(rtBodyAll_, 2)) {
        bodies = rtBodyCount_;
        bodyTris = rtBodyAll_.nOpaque + rtBodyAll_.nCut;
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td{};
    td.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    td.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    td.Inputs.NumDescs = ni;
    td.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    td.Inputs.InstanceDescs = rtInstDesc_->GetGPUVirtualAddress();
    td.DestAccelerationStructureData = rtTlas_->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = sva + so;
    l->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    l->ResourceBarrier(1, &uav);
    l->Close();
    rtRecorded_ = true;
    // the CPU's share (the bodies posed, the inputs recorded), for the log
    rtCpuMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
    rtInstances_ = static_cast<int>(ni); rtSets_ = sets; rtBodies_ = bodies;
    rtSetTris_ = setTris; rtBodyTris_ = bodyTris;
}

// After the frame ran: the rays' tally read back (copied at the end of the
// frame's list), and the log.
void D3D12Renderer::rtAfter() {
    rtTallyRead();
    const UINT ni = static_cast<UINT>(rtInstances_);
    const int sets = rtSets_, bodies = rtBodies_;
    const long long setTris = rtSetTris_, bodyTris = rtBodyTris_;
    if ((static_cast<int>(ni) != rtToldInst_ && rtTold_ < 6) || rtFrame_ % 60 == 30) {
        rtToldInst_ = static_cast<int>(ni);
        ++rtTold_;
        // the tally covers every COUNTING frame so far (`rtBind`)
        const long cf = std::max(1L, rtCounted_);
        const unsigned indirectRays = static_cast<unsigned>(rtCounts_[1] / cf);
        std::printf("d3d12: ray tracing live (frame %ld) - TLAS of %u instances: %d set BLAS (%lld triangles), "
                    "%d bodies in one BLAS (%lld triangles); ~%u indirect rays a frame; "
                    "builds %.2f ms CPU a frame, the bodies' BLAS built %ld times and refitted %ld\n", rtFrame_,
                    ni, sets, setTris, bodies, bodyTris,
                    indirectRays, rtCpuMs_ / rtFrame_, rtBodyBuilds_, rtBodyRefits_);
        omk::event("rt_tlas", {{"frame", rtFrame_}, {"instances", static_cast<unsigned>(ni)}, {"sets", sets},
                               {"bodies", bodies}, {"set_tris", static_cast<long>(setTris)},
                               {"body_tris", static_cast<long>(bodyTris)},
                               {"indirect_rays", indirectRays}});
        if (rtReflOn_) {
            const unsigned reflRays = static_cast<unsigned>(rtCounts_[2] / cf);
            std::printf("d3d12: rt reflections (frame %ld) - %ld ground draws, ~%u reflection rays a frame\n",
                        rtFrame_, rtFloorDraws_, reflRays);
            omk::event("rt_refl.frame", {{"frame", rtFrame_}, {"floor_draws", rtFloorDraws_},
                                         {"refl_rays", reflRays}});
        }
    }
    rtFloorDraws_ = 0;
}

void D3D12Renderer::rtKill() {
    if (rtOn_ && rtCount_ && rtCountRead_ && queue_) {
        // the last frames' tally, for the summary
        rtTallyCopy(oneShotBegin());
        oneShotEnd();
        rtTallyRead();
        std::printf("d3d12: ray tracing - %ld frames; the last TLAS %d instances (%d set BLAS, %d bodies); "
                    "%ld set BLAS builds, %ld body BLAS builds; ~%u indirect rays in %ld "
                    "counted frames, %lld AO rays\n", rtFrame_, rtInstances_, rtSets_, rtBodies_, rtSetBuilds_, rtBodyBuilds_,
                    rtCounts_[1], rtCounted_, rtAoRays_);
        omk::event("rt_summary", {{"frames", rtFrame_}, {"instances", rtInstances_}, {"sets", rtSets_},
                                  {"bodies", rtBodies_}, {"set_builds", rtSetBuilds_},
                                  {"body_builds", rtBodyBuilds_},
                                  {"indirect_rays", rtCounts_[1]}, {"ao_rays", static_cast<long>(rtAoRays_)}});
    }
    for (auto& [g, b] : rtSet_) rtDrop(b);
    rtDrop(rtBodyAll_);
    rtSet_.clear();
    rtNotes_.clear();
    for (ID3D12Resource** r : {&rtTlas_, &rtInstDesc_, &rtInstTab_, &rtScratch_, &rtFrameCb_, &rtCount_,
                               &rtCountRead_})
        freeRes(*r);
    rtInstDescPtr_ = rtInstTabPtr_ = rtFramePtr_ = nullptr;
    release(rtList_);
    release(rtAlloc_);
    release(dev5_);
    rtOn_ = false;
}

// ---- THE UPSCALER (`upscale.h`, `todo/enhancements.md` 18) ----------------
//
// Made once the device exists, for the display size: DLSS when asked and the
// GPU runs it, else FSR, else nothing - each step said. With one running the
// RENDER size is the SDK's answer for the quality, supersampling gives way
// (both decide the render size; the upscaler is the one asked for), and so
// does MSAA: the SDKs take a single-sampled colour and depth, and a temporal
// upscaler's own accumulation IS the anti-aliasing.
bool D3D12Renderer::makeUpscaler() {
    namespace up = omk::d3dup;
    if (upKind_ == up::kDlss) {
        up_ = up::makeDlss(dev_, adapter_, w_, h_, upQuality_);
        if (!up_) std::printf("upscaler: DLSS not available - trying FSR\n");
    }
    if (!up_) up_ = up::makeFsr(dev_, w_, h_, upQuality_);
    if (!up_) {
        std::printf("upscaler: none available - the native frame\n");
        rw_ = w_ * ss_; rh_ = h_ * ss_;
        return false;
    }
    if (ss_ > 1) {
        std::printf("upscaler: supersampling x%d ignored - the upscaler decides the render size\n", ss_);
        ss_ = 1;
    }
    if (wantSamples_ > 1) {
        std::printf("upscaler: %dx MSAA off - the upscaler takes one sample a pixel, and its temporal "
                    "resolve is the anti-aliasing\n", wantSamples_);
        wantSamples_ = 1;
    }
    up_->renderSize(rw_, rh_);
    rw_ = std::clamp(rw_, 1, w_);
    rh_ = std::clamp(rh_, 1, h_);
    std::printf("upscaler: %s %s - render %dx%d -> display %dx%d (ratio %.2f, %s)\n", up_->name(),
                up_->version().c_str(), rw_, rh_, w_, h_, static_cast<double>(w_) / rw_,
                up::qualityName(upQuality_));
    omk::event("upscaler", {{"sdk", up_->name()}, {"version", up_->version()}, {"rw", rw_}, {"rh", rh_},
                            {"dw", w_}, {"dh", h_}, {"quality", up::qualityName(upQuality_)}});
    jitterIndex_ = 0;
    havePrev_ = false;
    resetOwed_ = true;
    return true;
}

// The upscale itself, recorded on the frame's list after SSAO: the colour,
// depth and motion at the render size in, the display-size image out.
void D3D12Renderer::upscale() {
    const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    colour_.to(list_, in);
    depth_.to(list_, in);
    motion_.to(list_, in);
    upOut_.to(list_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    omk::d3dup::Frame f;
    f.list = list_;
    f.colour = colour_.r; f.depth = depth_.r; f.motion = motion_.r; f.output = upOut_.r;
    f.inputState = in;
    f.outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    f.rw = rw_; f.rh = rh_; f.dw = w_; f.dh = h_;
    f.jitterX = jitterPx_[0]; f.jitterY = jitterPx_[1];
    f.reset = resetOwed_;
    f.frameMs = 1000.0f / 30.0f;   // the game's tick
    // THE CAMERA, for the SDK that wants it (DLSS), in D3D's Y-UP clip space:
    // this engine's matrices are Vulkan's Y-down ones, whose second row the
    // vertex translation negates (`--flip-vert-y`), so it is negated here too.
    omk::d3dup::Camera& c = f.cam;
    c.nearP = omk::kNearCut;
    c.farP = 200000.0f;
    c.fovV = 2.0f * std::atan(aoTanV_);
    c.aspect = aoTanH_ / aoTanV_;
    const float* ax[3] = {mainS_, mainU_, mainF_};
    std::memset(c.worldToView, 0, sizeof c.worldToView);
    for (int r = 0; r < 3; ++r) {
        float d = 0.0f;
        for (int k = 0; k < 3; ++k) { c.worldToView[k * 4 + r] = ax[r][k]; d += ax[r][k] * frameEye_[k]; }
        c.worldToView[12 + r] = -d;
    }
    c.worldToView[15] = 1.0f;
    std::memset(c.viewToClip, 0, sizeof c.viewToClip);
    c.viewToClip[0] = 1.0f / aoTanH_;
    c.viewToClip[5] = 1.0f / aoTanV_;   // -(-1 / tanV): the flip
    c.viewToClip[10] = mainA_;
    c.viewToClip[14] = mainB_;
    c.viewToClip[11] = 1.0f;
    for (int i = 0; i < 16; ++i) {
        const bool row1 = i % 4 == 1;
        c.worldToClip[i] = row1 ? -curMain_[i] : curMain_[i];
        const float pv = havePrev_ && !resetOwed_ ? prevMain_[i] : curMain_[i];
        c.prevWorldToClip[i] = row1 ? -pv : pv;
    }
    for (int k = 0; k < 3; ++k) {
        c.pos[k] = frameEye_[k]; c.right[k] = mainS_[k]; c.up[k] = mainU_[k]; c.fwd[k] = mainF_[k];
    }
    up_->dispatch(f);
    // the SDK records with its own heaps and root signature: ours back
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    list_->SetDescriptorHeaps(1, heaps);
    bound_ = nullptr;
    upOut_.to(list_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ++upFrames_;
}

// `OMK_MOTION_STATS=1`: the frame's motion target read back and summed up -
// how much of the picture moves more than half a render pixel, the largest
// and the mean vector - as a line and an `upscaler.motion` event, which is
// what `verify.py: engine: upscaler` reads (the set standing still under a
// still camera, the crowd walking across it).
void D3D12Renderer::motionStats() {
    if (!motion_.r) return;
    if (!motionRead_) {
        D3D12_RESOURCE_DESC md = motion_.r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev_->GetCopyableFootprints(&md, 0, 1, 0, &fp, nullptr, nullptr, &total);
        motionPitch_ = fp.Footprint.RowPitch;
        motionRead_ = makeBuffer(total, D3D12_HEAP_TYPE_READBACK, "readback");
        if (!motionRead_) return;
    }
    ID3D12GraphicsCommandList* l = oneShotBegin();
    motion_.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    src.pResource = motion_.r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = motionRead_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R16G16_FLOAT, static_cast<UINT>(rw_), static_cast<UINT>(rh_), 1,
                                     static_cast<UINT>(motionPitch_)};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    oneShotEnd();
    void* p = nullptr;
    if (FAILED(motionRead_->Map(0, nullptr, &p))) return;
    const auto half = [](std::uint16_t h) {
        const std::uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
        float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
        return s ? -v : v;
    };
    long moving = 0;
    double sum = 0.0, most = 0.0;
    for (int y = 0; y < rh_; ++y) {
        const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const unsigned char*>(p) +
                                                                 static_cast<UINT64>(y) * motionPitch_);
        for (int x = 0; x < rw_; ++x) {
            const double mx = half(row[2 * x]) * rw_, my = half(row[2 * x + 1]) * rh_;
            const double len = std::sqrt(mx * mx + my * my);
            sum += len;
            most = std::max(most, len);
            moving += len > 0.5;
        }
    }
    D3D12_RANGE none{0, 0};
    motionRead_->Unmap(0, &none);
    const long n = static_cast<long>(rw_) * rh_;
    const double share = n ? static_cast<double>(moving) / n : 0.0;
    std::printf("d3d12 motion: frame %ld - %.2f%% of pixels move more than half a pixel, largest %.1f px, "
                "mean %.3f px%s\n", upFrames_, 100.0 * share, most, n ? sum / n : 0.0,
                resetOwed_ ? " (a cut: no motion)" : "");
    omk::event("upscaler.motion", {{"frame", upFrames_}, {"moving", share}, {"max", most},
                                   {"mean", n ? sum / n : 0.0}, {"reset", resetOwed_}});
}

void D3D12Renderer::end() {
    cpuPoseSlot_.clear();
    cpuPoseCount_.clear();
    // The depth-tie pass's report, once per geometry with losers.
    for (auto& [g, t] : tie_)
        if (t.touched && !t.logged && t.dropped > 0 && g->corners.size() > 3000) {
            t.logged = true;
            std::printf("d3d12: depth tie - %ld of %ld triangles coincide with an earlier face "
                        "and are degenerated so the first drawn wins, as the engine's strict test "
                        "decides\n", t.dropped, static_cast<long>(g->corners.size() / 3));
        }
    if (!recording_) return;
    // MSAA: the resolve Vulkan's render pass does at its end
    if (samples_ > 1) {
        msColour_.to(list_, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        colour_.to(list_, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        list_->ResolveSubresource(colour_.r, 0, msColour_.r, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    if (ssaoOn_) runSsao();
    // VOLUMETRIC LAMP LIGHT (todo/enhancements.md 24): added over the scene,
    // so the upscaler and the post chain take it with the rest
    if (volOn_) runVol();
    if (softOn_ && softDraws_ > 0 && !softTold_) {
        softTold_ = true;
        std::printf("d3d12: soft particles - %ld sprite batches faded against the depth on this frame\n",
                    softDraws_);
        omk::event("softparticles.drawn", {{"batches", static_cast<int>(softDraws_)}});
    }
    // From here `colour_` is the scene's resolved colour at the RENDER size
    // and `depth_` its depth: the input the upscaler takes (the header), and
    // the HUD and the rest of the 2D are composited over its OUTPUT later, at
    // the display size, on the CPU as ever.
    if (up_) upscale();
    // THE POST CHAIN (todo/enhancements.md 22), display size, when asked
    runPost();
    // FRAME GENERATION (todo/enhancements.md 27): the finished world picture
    // kept, with its motion and depth at the display size
    if (fgOn_) fgCapture();
    // It rests in COPY_SOURCE, where the readback copy and the present pass
    // expect it. The readback COPY itself is not recorded: a frame the GPU
    // presents never reads it (`readback()` does it when asked).
    colour_.to(list_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copyOwed_ = true;
    srcW_ = sourceW(); srcH_ = sourceH();
    // HYBRID RAY TRACING: the frame's acceleration structures, on their own
    // list submitted just before the frame's (which reads them; the list ends
    // on a UAV barrier), and the rays' tally copied out at the frame's end
    if (rtOn_) {
        rtBuild();
        rtTallyCopy(list_);
    }
    list_->Close();
    if (rtRecorded_) {
        ID3D12CommandList* b[] = {rtList_};
        queue_->ExecuteCommandLists(1, b);
    }
    ID3D12CommandList* l[] = {list_};
    queue_->ExecuteCommandLists(1, l);
    flush();
    if (fgCapTimed_) {
        fgCapTimed_ = false;
        void* q = nullptr;
        UINT64 hz = 0;
        if (SUCCEEDED(queue_->GetTimestampFrequency(&hz)) && hz && SUCCEEDED(fgQBuf_->Map(0, nullptr, &q))) {
            const auto* t = static_cast<const UINT64*>(q);
            fgCapMsSum_ += static_cast<double>(t[3] - t[2]) * 1000.0 / static_cast<double>(hz);
            D3D12_RANGE none{0, 0};
            fgQBuf_->Unmap(0, &none);
        }
    }
    if (postTimed_) {
        postTimed_ = false;
        void* q = nullptr;
        UINT64 hz = 0;
        if (SUCCEEDED(queue_->GetTimestampFrequency(&hz)) && hz && SUCCEEDED(postQBuf_->Map(0, nullptr, &q))) {
            const auto* t = static_cast<const UINT64*>(q);
            postMsSum_ += static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(hz);
            D3D12_RANGE none{0, 0};
            postQBuf_->Unmap(0, &none);
            if (++postMsN_ == 60) {
                std::printf("post: %.3f ms on the GPU a frame (mean of 60, %dx%d)\n", postMsSum_ / 60.0, w_, h_);
                omk::event("post.timing", {{"ms", postMsSum_ / 60.0}, {"w", w_}, {"h", h_}});
                postMsSum_ = 0.0;
                postMsN_ = 0;
            }
        }
    }
    if (volTimed_) {
        volTimed_ = false;
        void* q = nullptr;
        UINT64 hz = 0;
        if (SUCCEEDED(queue_->GetTimestampFrequency(&hz)) && hz && SUCCEEDED(volQBuf_->Map(0, nullptr, &q))) {
            const auto* t = static_cast<const UINT64*>(q);
            volMsSum_ += static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(hz);
            D3D12_RANGE none{0, 0};
            volQBuf_->Unmap(0, &none);
            if (++volMsN_ == 60) {
                std::printf("volumetrics: %.3f ms on the GPU a frame (mean of 60, render %dx%d)\n",
                            volMsSum_ / 60.0, rw_, rh_);
                omk::event("vol.timing", {{"ms", volMsSum_ / 60.0}, {"w", rw_}, {"h", rh_}});
                volMsSum_ = 0.0;
                volMsN_ = 0;
            }
        }
    }
    if (rtRecorded_) {
        rtRecorded_ = false;
        rtAfter();
    }
    recording_ = false;
    listOpen_ = false;
    shadowLive_ = false;
    dirty_ = true;
    if (mvOn_) {
        static const bool stats = omk::envFlag("OMK_MOTION_STATS");
        if (stats) motionStats();
        // this frame's camera is the next one's previous
        std::memcpy(prevMain_, curMain_, sizeof prevMain_);
        for (int k = 0; k < 3; ++k) { prevEye_[k] = frameEye_[k]; prevFwd_[k] = mainF_[k]; }
        havePrev_ = true;
        resetOwed_ = false;
    }
}

// ---- SOFT PARTICLES (`todo/enhancements.md` 23) ---------------------------
//
// The depth is bound READ-ONLY (`kDsvDepthRo`, both planes) and in the read
// state the shader's view needs too - the one state in which D3D12 lets a pass
// test against a depth and sample it at once - for the run of soft sprites;
// writable again before anything that writes it.
void D3D12Renderer::depthReadOnly(bool ro) {
    depth_.to(list_, ro ? D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                        : D3D12_RESOURCE_STATE_DEPTH_WRITE);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv_.cpu(samples_ > 1 ? rtvMs_ : rtvColour_), rtv_.cpu(rtvMotion_)};
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_.cpu(ro ? kDsvDepthRo : dsvDepth_);
    list_->OMSetRenderTargets(mvOn_ ? 2 : 1, rtvs, FALSE, &dsv);
    depthRo_ = ro;
}

// ---- VOLUMETRIC LAMP LIGHT (`todo/enhancements.md` 24) --------------------
//
// AN ENHANCEMENT, off unless asked. Two full-screen passes on the frame's list
// after the MSAA resolve and SSAO, before the upscaler: `vol.frag` marches the
// eye rays through the reach of the frame's lamps in the air (`View::airLights`)
// at HALF the render size into an RGBA16F image, and `volmix.frag` brings it
// up with a depth-aware filter and ADDS it over `colour_`. The upscaler and the
// post chain then take the light with the rest of the frame. A frame with no
// lamp in the air runs neither and is untouched.
bool D3D12Renderer::makeVol() {
    if (!volRs_) {
        // the depth (t0) and the half-size light (t1) as ONE table, the lamps a
        // root CBV (b2), and two point samplers (s0, s1) the fetches ignore
        const D3D12_DESCRIPTOR_RANGE r = rs::srvs(0, 0, 2);
        D3D12_ROOT_PARAMETER p[3] = {
            rs::constants(0, kPushSpace, sizeof(VolPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::view(D3D12_ROOT_PARAMETER_TYPE_CBV, 2, 0, D3D12_SHADER_VISIBILITY_PIXEL)};
        D3D12_STATIC_SAMPLER_DESC s[2] = {
            rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(1, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f)};
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 3; d.pParameters = p;
        d.NumStaticSamplers = 2; d.pStaticSamplers = s;
        volRs_ = makeRootSig(d, "volumetrics");
        if (!volRs_) return false;
        const bool ms = samples_ > 1;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = volRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = ms ? rs::code(kDx_vol_ms_frag, sizeof kDx_vol_ms_frag) : rs::code(kDx_vol_frag, sizeof kDx_vol_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        volPipe_ = makePso(pd, "volumetrics");
        // the composite: ADDED to the scene's colour, its alpha left alone
        pd.PS = ms ? rs::code(kDx_volmix_ms_frag, sizeof kDx_volmix_ms_frag)
                   : rs::code(kDx_volmix_frag, sizeof kDx_volmix_frag);
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ONE; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_ONE; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        volMixPipe_ = makePso(pd, "volumetrics composite");
        if (!volPipe_ || !volMixPipe_) return false;
        if (omk::envFlag("OMK_VOL_TIMING")) {
            D3D12_QUERY_HEAP_DESC qd{};
            qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qd.Count = 2;
            if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&volQ_)))) volQ_ = nullptr;
            volQBuf_ = makeBuffer(2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, "readback");
        }
    }
    if (!volPipe_ || !volMixPipe_) return false;
    if (vol_.r) return true;
    volW_ = (rw_ + 1) / 2;
    volH_ = (rh_ + 1) / 2;
    vol_.r = makeImage(DXGI_FORMAT_R16G16B16A16_FLOAT, static_cast<UINT>(volW_), static_cast<UINT>(volH_), 1, 1, 1,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       nullptr, "volumetrics");
    vol_.st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!vol_.r) return false;
    dev_->CreateRenderTargetView(vol_.r, nullptr, rtv_.cpu(kRtvSwap + kSwapBuffers));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sv.ViewDimension = samples_ > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(depth_.r, &sv, srv_.cpu(kSlotVolDepth));
    dev_->CreateShaderResourceView(vol_.r, nullptr, srv_.cpu(kSlotVol));
    return true;
}

void D3D12Renderer::runVol() {
    if (enhOff_) return;
    ++volFrame_;
    if (volCount_ <= 0 || aoVw_ <= 0 || aoVh_ <= 0) return;
    if (!makeVol()) {
        static bool told = false;
        if (!told) std::fprintf(stderr, "volumetrics: the passes could not be made - the frame without them\n");
        told = true;
        return;
    }
    ID3D12GraphicsCommandList* l = list_;
    volTimed_ = volQ_ && volQBuf_;
    if (volTimed_) l->EndQuery(volQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    depth_.to(l, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    vol_.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const int hw = (aoVw_ + 1) / 2, hh = (aoVh_ + 1) / 2;
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const float A = farP / (farP - nearP), B = -farP * nearP / (farP - nearP);
    VolPush vp{{volTanH_, volTanV_, A, B},
               {static_cast<float>(aoVw_), static_cast<float>(aoVh_), static_cast<float>(volFrame_ % 64), nearP},
               {volEye_[0], volEye_[1], volEye_[2], volDensity_},
               {volS_[0], volS_[1], volS_[2], fog_ ? fogStart_ : 0.0f},
               {volU_[0], volU_[1], volU_[2], fog_ ? fogEnd_ : 0.0f},
               {volF_[0], volF_[1], volF_[2], 40000.0f}};
    l->SetGraphicsRootSignature(volRs_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const auto pass = [&](D3D12_CPU_DESCRIPTOR_HANDLE rtv, ID3D12PipelineState* pl, int vw, int vh,
                          const void* push, UINT dwords) {
        const D3D12_VIEWPORT v{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
        const D3D12_RECT sr{0, 0, vw, vh};
        l->RSSetViewports(1, &v);
        l->RSSetScissorRects(1, &sr);
        l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        l->SetPipelineState(pl);
        l->SetGraphicsRoot32BitConstants(0, dwords, push, 0);
        l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotVolDepth));
        l->SetGraphicsRootConstantBufferView(2, volUbo_->GetGPUVirtualAddress());
        l->DrawInstanced(3, 1, 0, 0);
    };
    pass(rtv_.cpu(kRtvSwap + kSwapBuffers), volPipe_, hw, hh, &vp, sizeof vp / 4);
    vol_.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    colour_.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const VolMixPush mp{{volTanH_, volTanV_, A, B},
                        {static_cast<float>(aoVw_), static_cast<float>(aoVh_), static_cast<float>(hw),
                         static_cast<float>(hh)}};
    pass(rtv_.cpu(rtvColour_), volMixPipe_, aoVw_, aoVh_, &mp, sizeof mp / 4);
    if (volTimed_) {
        l->EndQuery(volQ_, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(volQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, volQBuf_, 0);
    }
    bound_ = nullptr;
    if (!volTold_) {
        volTold_ = true;
        std::printf("volumetrics: %d lamps in the air on this frame, marched at %dx%d\n", volCount_, hw, hh);
        omk::event("vol", {{"lamps", volCount_}, {"w", hw}, {"h", hh}});
    }
}

// ---- THE POST-PROCESSING CHAIN (`todo/enhancements.md` 22) ----------------
//
// AN ENHANCEMENT, every part off unless asked: `bloom`, `dof`, `colorgrade`,
// `hdr`. It runs on the frame's list after the scene, SSAO and the upscaler,
// at the DISPLAY size: the 3D frame in (`upscaleSource`, the supersample
// folded), one display-size picture out (`postImg_`), which everything the
// display half reads from then on reads instead - the present pass (and its
// colour-depth rule, `present.frag`), the overlay pass over it, and the
// `readback()` a CPU composite starts from. So the 2D interface is laid over
// the PROCESSED world and is never bloomed, blurred or graded. With nothing
// asked - or only depth of field, on a frame that is not cinematic - the chain
// does not run and the frame is byte-identical to one without it.
//
// The scene target stays RGBA8, deliberately: an FP16 one would change every
// scene pipeline, the MSAA resolve, the upscaler's input contract and the
// readback for light the scene shader never produces (the game's lighting
// saturates at 1 like its device did). The headroom is made where it is used:
// `expand` (`bloom.frag`, `post.frag`) maps the 8-bit picture into linear
// light with the tone curve's inverse, so near-white neon stands for several
// times a mid grey, bloom is added there, and the curve maps back - exactly
// onto the original picture wherever nothing was added.


// Which output the window is on, and whether it reports HDR: the output whose
// desktop rectangle overlaps the window most, over every adapter (a laptop's
// panel can hang off the other GPU). DXGI's `IDXGIOutput6::GetDesc1`.
// -1: no output found, 0: SDR, 1: HDR (ST.2084); `peakNits` its MaxLuminance
static int outputHdrOf(IDXGIFactory1* factory, HWND hwnd, float& peakNits) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    long best = -1;
    bool hdr = false;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIOutput* o = nullptr;
        for (UINT j = 0; a->EnumOutputs(j, &o) != DXGI_ERROR_NOT_FOUND; ++j) {
            IDXGIOutput6* o6 = nullptr;
            if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6)))) {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(o6->GetDesc1(&d))) {
                    const RECT& r = d.DesktopCoordinates;
                    const long ix = std::max(0L, std::min(wr.right, r.right) - std::max(wr.left, r.left));
                    const long iy = std::max(0L, std::min(wr.bottom, r.bottom) - std::max(wr.top, r.top));
                    if (ix * iy > best) {
                        best = ix * iy;
                        hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                        peakNits = d.MaxLuminance;
                    }
                }
                o6->Release();
            }
            o->Release();
        }
        a->Release();
    }
    return best < 0 ? -1 : hdr ? 1 : 0;
}

bool D3D12Renderer::outputIsHdr(float& peakNits) {
    return outputHdrOf(factory_, hwnd_, peakNits) == 1;
}

bool D3D12Renderer::makePost() {
    if (!bloomRs_) {
        // the bloom passes: one input (t0), a linear clamped sampler
        const D3D12_DESCRIPTOR_RANGE r1 = rs::srvs(0, 0, 1);
        D3D12_ROOT_PARAMETER p1[2] = {
            rs::constants(0, kPushSpace, sizeof(BloomPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r1, D3D12_SHADER_VISIBILITY_PIXEL)};
        D3D12_STATIC_SAMPLER_DESC s[5] = {
            rs::sampler(0, 0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(1, 0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(2, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(3, 0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f),
            rs::sampler(4, 0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0.0f)};
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 2; d.pParameters = p1;
        d.NumStaticSamplers = 1; d.pStaticSamplers = s;
        bloomRs_ = makeRootSig(d, "bloom");
        // the final pass: ONE table of five (source, bloom level 0, depth, its stencil, the grade's LUT)
        const D3D12_DESCRIPTOR_RANGE r3 = rs::srvs(0, 0, 5);
        D3D12_ROOT_PARAMETER p3[2] = {
            rs::constants(0, kPushSpace, sizeof(PostPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&r3, D3D12_SHADER_VISIBILITY_PIXEL)};
        d.pParameters = p3;
        d.NumStaticSamplers = 5;
        postRs_ = makeRootSig(d, "post");
        if (!bloomRs_ || !postRs_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = bloomRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = rs::code(kDx_bloom_frag, sizeof kDx_bloom_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R11G11B10_FLOAT;
        pd.SampleDesc.Count = 1;
        bloomDownPipe_ = makePso(pd, "bloom down");
        // back up the chain: added onto the larger level
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_ONE; b.DestBlend = D3D12_BLEND_ONE; b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO; b.DestBlendAlpha = D3D12_BLEND_ONE; b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.LogicOp = D3D12_LOGIC_OP_NOOP;
        bloomUpPipe_ = makePso(pd, "bloom up");
        b = D3D12_RENDER_TARGET_BLEND_DESC{};
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.pRootSignature = postRs_;
        pd.PS = samples_ > 1 ? rs::code(kDx_post_ms_frag, sizeof kDx_post_ms_frag)
                             : rs::code(kDx_post_frag, sizeof kDx_post_frag);
        pd.RTVFormats[0] = presFmt_;
        postPipe_ = makePso(pd, "post");
        if (!bloomDownPipe_ || !bloomUpPipe_ || !postPipe_) return false;
        if (omk::envFlag("OMK_POST_TIMING")) {
            D3D12_QUERY_HEAP_DESC qd{};
            qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qd.Count = 2;
            if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&postQ_)))) postQ_ = nullptr;
            postQBuf_ = makeBuffer(2 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, "readback");
        }
    }
    if (postImg_.r) return true;
    postImg_.r = makeImage(presFmt_, static_cast<UINT>(w_), static_cast<UINT>(h_), 1, 1, 1,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE,
                           nullptr, "post");
    postImg_.st = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!postImg_.r) return false;
    dev_->CreateRenderTargetView(postImg_.r, nullptr, rtv_.cpu(rtvPost_));
    dev_->CreateShaderResourceView(postImg_.r, nullptr, srv_.cpu(kSlotPostOut));
    {
        D3D12_RESOURCE_DESC cd = postImg_.r->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT64 total = 0;
        dev_->GetCopyableFootprints(&cd, 0, 1, 0, &fp, nullptr, nullptr, &total);
        postPitch_ = fp.Footprint.RowPitch;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (postReq_.bloom) {
        int bw = w_, bh = h_;
        for (int i = 0; i < kBloomLevels; ++i) {
            bw = std::max(1, (bw + 1) / 2);
            bh = std::max(1, (bh + 1) / 2);
            bloomW_[i] = bw; bloomH_[i] = bh;
            bloom_[i].r = makeImage(DXGI_FORMAT_R11G11B10_FLOAT, static_cast<UINT>(bw), static_cast<UINT>(bh), 1, 1,
                                    1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, "post");
            bloom_[i].st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            if (!bloom_[i].r) return false;
            dev_->CreateRenderTargetView(bloom_[i].r, nullptr, rtv_.cpu(rtvBloom0_ + static_cast<UINT>(i)));
            dev_->CreateShaderResourceView(bloom_[i].r, nullptr, srv_.cpu(kSlotBloom0 + static_cast<UINT>(i)));
        }
    }
    // the final pass's table: the source (the upscaler's output or the
    // scene's colour), bloom level 0 (a null view without bloom), the depth
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dev_->CreateShaderResourceView(upscaleSource().r, &sv, srv_.cpu(kSlotPostSrc));
    sv.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    dev_->CreateShaderResourceView(postReq_.bloom ? bloom_[0].r : nullptr, &sv, srv_.cpu(kSlotPostBloom));
    sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sv.ViewDimension = samples_ > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    dev_->CreateShaderResourceView(depth_.r, &sv, srv_.cpu(kSlotPostDepth));
    // ...and the depth image's STENCIL plane (a translucent body's mark)
    sv.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
    if (samples_ <= 1) sv.Texture2D.PlaneSlice = 1;
    dev_->CreateShaderResourceView(depth_.r, &sv, srv_.cpu(kSlotPostStencil));
    // ...and the grade's LUT strip (a null view while none: the shader reads it only when asked)
    D3D12_SHADER_RESOURCE_VIEW_DESC lv{};
    lv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    lv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    lv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(lutImg_, &lv, srv_.cpu(kSlotPostLut));
    return true;
}

// `Renderer::setGrade`, see the class: the strip (`o3de/colourgrade.h` lutStrip) uploaded into an image of
// its own, the post table's fifth view pointed at it
bool D3D12Renderer::setGrade(const float* rgb, int n) {
    if (!dev_) {   // before `init`: the grades are coming - the chain is wanted, whatever the request holds
        lutWant_ = true;
        return true;
    }
    flush();   // the last frame may still read the old image
    freeRes(lutImg_);
    lutN_ = 0;
    if (rgb && n >= 2) {
        std::vector<unsigned char> strip;
        omk::lutStrip(rgb, n, strip);
        const UINT w = static_cast<UINT>(n * n), h = static_cast<UINT>(n);
        lutImg_ = makeImage(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, "grade");
        if (lutImg_) {
            D3D12_RESOURCE_DESC rd = lutImg_->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
            UINT64 total = 0;
            dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
            ID3D12Resource* sb = makeBuffer(total, D3D12_HEAP_TYPE_UPLOAD, "staging");
            if (sb) {
                unsigned char* dst = nullptr;
                sb->Map(0, nullptr, reinterpret_cast<void**>(&dst));
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(dst + fp.Offset + static_cast<UINT64>(y) * fp.Footprint.RowPitch,
                                strip.data() + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
                sb->Unmap(0, nullptr);
                ID3D12GraphicsCommandList* l = oneShotBegin();
                D3D12_TEXTURE_COPY_LOCATION td{}, sl{};
                td.pResource = lutImg_; td.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                sl.pResource = sb; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; sl.PlacedFootprint = fp;
                l->CopyTextureRegion(&td, 0, 0, 0, &sl, nullptr);
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = lutImg_;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                l->ResourceBarrier(1, &b);
                oneShotEnd();
                freeRes(sb);
                lutN_ = n;
            } else freeRes(lutImg_);
        }
    }
    if (bloomRs_) {   // the post table exists: its fifth view follows the image
        D3D12_SHADER_RESOURCE_VIEW_DESC lv{};
        lv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(lutImg_, &lv, srv_.cpu(kSlotPostLut));
    }
    return lutN_ > 1 || !(rgb && n >= 2);
}

void D3D12Renderer::runPost() {
    postLive_ = false;
    if (enhOff_) return;   // the original's look shows
    if (!postWant_ && !lutWant_) return;
    // DEPTH OF FIELD on cinematic frames only; the rest of the chain always
    const bool dof = foc_.dofLive(postReq_.dof);
    if (!(postReq_.bloom || postReq_.grade > 0 || hdrOn_ || dof || lutN_ > 1)) return;
    if (!makePost()) {
        static bool told = false;
        if (!told) std::fprintf(stderr, "post: the chain could not be made - the frame without it\n");
        told = true;
        return;
    }
    ID3D12GraphicsCommandList* l = list_;
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    l->SetDescriptorHeaps(1, heaps);
    postTimed_ = postQ_ && postQBuf_;
    if (postTimed_) l->EndQuery(postQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    Res& src = upscaleSource();
    src.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float uvMax[2] = {static_cast<float>(foc_.vw) / static_cast<float>(w_),
                            static_cast<float>(foc_.vh) / static_cast<float>(h_)};
    const auto target = [&](Res& img, UINT rtv, int tw, int th) {
        img.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_.cpu(rtv);
        l->OMSetRenderTargets(1, &h, FALSE, nullptr);
        const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th), 0.0f, 1.0f};
        const D3D12_RECT sr{0, 0, tw, th};
        l->RSSetViewports(1, &vp);
        l->RSSetScissorRects(1, &sr);
    };
    if (postReq_.bloom) {
        l->SetGraphicsRootSignature(bloomRs_);
        // DOWN: the bright pass into level 0 from the frame, then level by level
        l->SetPipelineState(bloomDownPipe_);
        for (int i = 0; i < kBloomLevels; ++i) {
            const float iw = i == 0 ? static_cast<float>(w_) : static_cast<float>(bloomW_[i - 1]);
            const float ih = i == 0 ? static_cast<float>(h_) : static_cast<float>(bloomH_[i - 1]);
            if (i > 0) bloom_[i - 1].to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            target(bloom_[i], rtvBloom0_ + static_cast<UINT>(i), bloomW_[i], bloomH_[i]);
            const BloomPush bp = omk::postfx::bloomDown(i, iw, ih, bloomW_[i], bloomH_[i], uvMax);
            l->SetGraphicsRoot32BitConstants(0, sizeof bp / 4, &bp, 0);
            l->SetGraphicsRootDescriptorTable(1, srv_.gpu(i == 0 ? kSlotPostSrc : kSlotBloom0 + static_cast<UINT>(i - 1)));
            l->DrawInstanced(3, 1, 0, 0);
        }
        // UP: each level's tent added onto the next larger
        l->SetPipelineState(bloomUpPipe_);
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            bloom_[i + 1].to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            target(bloom_[i], rtvBloom0_ + static_cast<UINT>(i), bloomW_[i], bloomH_[i]);
            const BloomPush bp = omk::postfx::bloomUp(bloomW_[i + 1], bloomH_[i + 1], bloomW_[i], bloomH_[i], uvMax);
            l->SetGraphicsRoot32BitConstants(0, sizeof bp / 4, &bp, 0);
            l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotBloom0 + static_cast<UINT>(i + 1)));
            l->DrawInstanced(3, 1, 0, 0);
        }
        bloom_[0].to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // THE FINAL PASS
    depth_.to(l, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    target(postImg_, rtvPost_, w_, h_);
    l->SetGraphicsRootSignature(postRs_);
    l->SetPipelineState(postPipe_);
    PostPush pp = omk::postfx::buildPost(postReq_, foc_, dof, w_, h_, rw_, rh_, sourceSs(),
                                               hdrOn_ ? hdrPeak_ : 0.0f);
    pp.lut[0] = lutN_ > 1 ? static_cast<float>(lutN_) : 0.0f;
    l->SetGraphicsRoot32BitConstants(0, sizeof pp / 4, &pp, 0);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotPostSrc));
    l->DrawInstanced(3, 1, 0, 0);
    postImg_.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (up_) upOut_.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (postTimed_) {
        l->EndQuery(postQ_, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(postQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, postQBuf_, 0);
    }
    bound_ = nullptr;
    postLive_ = true;
    static bool told = false;
    if (!told) {
        told = true;
        std::printf("post: the chain ran - bloom %s, depth of field %s, grade %s, %s\n",
                    postReq_.bloom ? "on" : "off", postReq_.dof ? "on (cinematic frames)" : "off",
                    omk::postfx::kGradeNames[postReq_.grade], hdrOn_ ? "HDR (scRGB)" : "SDR");
        omk::event("post", {{"bloom", postReq_.bloom ? 1 : 0}, {"dof", postReq_.dof ? 1 : 0},
                            {"grade", omk::postfx::kGradeNames[postReq_.grade]}, {"hdr", hdrOn_ ? 1 : 0}});
    }
}

// ---- FRAME GENERATION (`todo/enhancements.md` 27) -------------------------
//
// AN ENHANCEMENT, off unless asked, the Direct3D 12 WINDOW's: between every two
// rendered frames an IN-BETWEEN picture, presented half an interval before the
// second, so the window shows about twice the frames the game renders.
//
//   * THE CAPTURE (`fg_cap.frag`, on the frame's list after the post chain):
//     the finished world picture (`displaySource`, before the present pass's
//     quantisation and before the 2D) kept at the display size, and beside it
//     the scene's motion vector (`motion_`, which the scene pass now writes for
//     frame generation as for an upscaler - `mvOn_`; no jitter without one) in
//     display pixels and its view depth. Two frames are kept, N-1 and N.
//   * THE IN-BETWEEN (`fg_splat.frag`, `fg_resolve.frag`, at the present):
//     both frames' pixels pushed half-way along their motion - N's back along
//     its own vector, N-1's forward along its own turned round - into two
//     32-bit heads with a depth test (`imageAtomicMin` of depth and source),
//     and resolved: the front-most surface wins, blended with the other frame
//     where that frame shows the same surface there, alone where it is hidden
//     there (a disocclusion), cracks filled from the farthest neighbour.
//   * THE PRESENT: the in-between picture through the SAME present pass and
//     the SAME overlay (this frame's 2D - the interface is never interpolated;
//     a fade is part of the world picture or of the overlay's colour fade) and
//     out at once; the real frame composited behind it and presented half the
//     measured interval later by a thread of the backend's own (its own list
//     and fence; the frame's thread settles it before it touches the swapchain
//     again). The swapchain is then made on a HIGH-PRIORITY queue of its own
//     (`presQ_`), so that blit need not wait behind the next frame's work on
//     the frame's queue (4K, GPU-bound: the loop's interval 48 ms against 79). One frame of the pair is always late by that half interval - the
//     latency frame generation costs.
//   * NOT INTERPOLATED: a first frame, a pair with a CAMERA CUT on either side
//     (the upscaler's own test - a jump of 5 m or 30 degrees, a set released, a
//     resize - kept per frame), a present with no new frame rendered since the
//     last, a frame composed on the CPU. `--frames N` still renders and
//     presents N real frames; the in-between ones are presents only, and the
//     window dump (`OMK_WINDUMP`) is always a real frame's.

static double qpcNow() {
    static LARGE_INTEGER hz{};
    if (!hz.QuadPart) QueryPerformanceFrequency(&hz);
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(hz.QuadPart);
}

bool D3D12Renderer::fgStart() {
    fgAlways_ = omk::envFlag("OMK_FRAMEGEN_ALWAYS");
    fgProbe_ = omk::envStr("OMK_FRAMEGEN_PROBE");
    if (fgProbe_ && !*fgProbe_) fgProbe_ = nullptr;
    fgSlots_ = fgProbe_ ? 3 : 2;
    if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fgAlloc_))) ||
        FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fgAlloc_, nullptr, IID_PPV_ARGS(&fgList_))) ||
        FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fgFence_)))) {
        std::fprintf(stderr, "framegen: no command list for the deferred present - off\n");
        return false;
    }
    fgList_->Close();
    fgEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fgEvent_) return false;
    // THE DISPLAY's refresh rate, for the line: what the in-between frames are
    // shown at (a v-synced swapchain shows at most one picture a refresh)
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (mon && GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        fgRefresh_ = static_cast<int>(dm.dmDisplayFrequency);
    fgThread_ = std::thread([this] { fgThreadMain(); });
    std::printf("framegen: on - an ENHANCEMENT: an in-between picture warped along the motion vectors before "
                "every rendered frame (%s); the display refreshes at %d Hz. The real frame goes out half an "
                "interval after its in-between one - that half interval is the latency it adds - and it pays "
                "only when the display can show about twice the rendered rate (or the GPU's frame is long); "
                "the simulation is unchanged. An in-between frame goes out only while a frame's work spans "
                "about two refreshes (%.0f ms here) - with less, the display has no refresh to show it in and "
                "a v-synced present would only slow the real frames\n",
                up_ ? "the upscaler's vectors" : "motion vectors on for it", fgRefresh_,
                fgRefresh_ > 0 ? 1900.0 / fgRefresh_ : 0.0);
    omk::event("framegen", {{"on", 1}, {"refresh", fgRefresh_}});
    return true;
}

bool D3D12Renderer::makeFg() {
    if (!fgRs_) {
        // ONE root signature for the three: the push block, four pictures (t0..t3)
        // and the two heads (u4, u5); the shaders fetch, so no sampler
        const D3D12_DESCRIPTOR_RANGE srvR = rs::srvs(0, 0, 4);
        D3D12_DESCRIPTOR_RANGE uavR{};
        uavR.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavR.NumDescriptors = 2;
        uavR.BaseShaderRegister = 4;
        D3D12_ROOT_PARAMETER p[3] = {
            rs::constants(0, kPushSpace, sizeof(FgCapPush) / 4, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&srvR, D3D12_SHADER_VISIBILITY_PIXEL),
            rs::table(&uavR, D3D12_SHADER_VISIBILITY_PIXEL)};
        D3D12_ROOT_SIGNATURE_DESC d{};
        d.NumParameters = 3; d.pParameters = p;
        fgRs_ = makeRootSig(d, "framegen");
        if (!fgRs_) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = fgRs_;
        pd.VS = rs::code(kDx_fsq_vert, sizeof kDx_fsq_vert);
        pd.PS = rs::code(kDx_fg_cap_frag, sizeof kDx_fg_cap_frag);
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.IndependentBlendEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = presFmt_;
        pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.SampleDesc.Count = 1;
        fgCapPipe_ = makePso(pd, "framegen capture");
        // the splat (and the heads' clear): no target, the heads are UAVs
        pd.PS = rs::code(kDx_fg_splat_frag, sizeof kDx_fg_splat_frag);
        pd.NumRenderTargets = 0;
        pd.RTVFormats[0] = pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        fgSplatPipe_ = makePso(pd, "framegen splat");
        pd.PS = rs::code(kDx_fg_resolve_frag, sizeof kDx_fg_resolve_frag);
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = presFmt_;
        fgResolvePipe_ = makePso(pd, "framegen resolve");
        if (!fgCapPipe_ || !fgSplatPipe_ || !fgResolvePipe_) return false;
        if (omk::envFlag("OMK_FRAMEGEN_TIMING")) {
            D3D12_QUERY_HEAP_DESC qd{};
            qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            qd.Count = 4;
            if (FAILED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&fgQ_)))) fgQ_ = nullptr;
            fgQBuf_ = makeBuffer(4 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK, "readback");
        }
    }
    if (fgMid_.r) return true;
    const UINT w = static_cast<UINT>(w_), h = static_cast<UINT>(h_);
    for (int i = 0; i < fgSlots_; ++i) {
        fgCol_[i].r = makeImage(presFmt_, w, h, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, "framegen");
        fgAux_[i].r = makeImage(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, "framegen");
        fgCol_[i].st = fgAux_[i].st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (!fgCol_[i].r || !fgAux_[i].r) return false;
        dev_->CreateRenderTargetView(fgCol_[i].r, nullptr, rtv_.cpu(kRtvFg + static_cast<UINT>(i)));
        dev_->CreateRenderTargetView(fgAux_[i].r, nullptr, rtv_.cpu(kRtvFg + kFgMaxSlots + static_cast<UINT>(i)));
    }
    for (int i = 0; i < 2; ++i) {
        fgHead_[i].r = makeImage(DXGI_FORMAT_R32_UINT, w, h, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, "framegen");
        fgHead_[i].st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!fgHead_[i].r) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = DXGI_FORMAT_R32_UINT;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev_->CreateUnorderedAccessView(fgHead_[i].r, nullptr, &uv, srv_.cpu(kSlotFgHead + static_cast<UINT>(i)));
    }
    fgMid_.r = makeImage(presFmt_, w, h, 1, 1, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, "framegen");
    fgMid_.st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!fgMid_.r) return false;
    dev_->CreateRenderTargetView(fgMid_.r, nullptr, rtv_.cpu(kRtvFg + 2 * kFgMaxSlots));
    dev_->CreateShaderResourceView(fgMid_.r, nullptr, srv_.cpu(kSlotFgMid));
    // the capture's depth and motion, beside its source (written per frame)
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(depth_.r, &sv, srv_.cpu(kSlotFgSrc + 1));
    dev_->CreateShaderResourceView(motion_.r, nullptr, srv_.cpu(kSlotFgSrc + 2));
    fgSeq_ = fgShownSeq_ = 0;   // (re)made: nothing kept to pair with
    return true;
}

// On the frame's list, after the post chain: the world picture, its motion
// and depth, into the next slot.
void D3D12Renderer::fgCapture() {
    if (!motion_.r || !makeFg()) {
        std::fprintf(stderr, "framegen: its targets could not be made - off\n");
        fgOn_ = false;
        return;
    }
    const int s = static_cast<int>(fgSeq_ % fgSlots_);
    ID3D12GraphicsCommandList* l = list_;
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    l->SetDescriptorHeaps(1, heaps);
    fgCapTimed_ = fgQ_ && fgQBuf_;
    if (fgCapTimed_) l->EndQuery(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    Res& src = displaySource();
    dev_->CreateShaderResourceView(src.r, nullptr, srv_.cpu(kSlotFgSrc));
    src.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    depth_.to(l, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    motion_.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fgCol_[s].to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    fgAux_[s].to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {rtv_.cpu(kRtvFg + static_cast<UINT>(s)),
                                                 rtv_.cpu(kRtvFg + kFgMaxSlots + static_cast<UINT>(s))};
    l->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, w_, h_};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(fgRs_);
    l->SetPipelineState(fgCapPipe_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float pw = static_cast<float>(std::max(foc_.vw, 1)), ph = static_cast<float>(std::max(foc_.vh, 1));
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const FgCapPush cp{{pw, ph, static_cast<float>(displaySs()), 0.0f},
                       {static_cast<float>(aoVw_ > 0 ? aoVw_ : rw_) / pw, static_cast<float>(aoVh_ > 0 ? aoVh_ : rh_) / ph,
                        0.0f, 0.0f},
                       {farP / (farP - nearP), -farP * nearP / (farP - nearP), 0.0f, 0.0f}};
    l->SetGraphicsRoot32BitConstants(0, sizeof cp / 4, &cp, 0);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotFgSrc));
    l->DrawInstanced(3, 1, 0, 0);
    fgCol_[s].to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fgAux_[s].to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    src.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);   // where the display half expects it
    if (fgCapTimed_) {
        l->EndQuery(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 3);
        l->ResolveQueryData(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 2, 2, fgQBuf_, 2 * sizeof(UINT64));
    }
    bound_ = nullptr;
    fgCut_[s] = resetOwed_;
    fgPicW_[s] = foc_.vw;
    fgPicH_[s] = foc_.vh;
    ++fgSeq_;
}

// the interval between presents, smoothed: what "half an interval" is
void D3D12Renderer::fgTick() {
    const double now = qpcNow();
    if (fgLastCall_ > 0.0) {
        const double dt = std::min(now - fgLastCall_, 0.1);
        fgInterval_ = fgInterval_ > 0.0 ? 0.8 * fgInterval_ + 0.2 * dt : dt;
    }
    if (fgPresentDone_ > 0.0) {
        const double w = std::min(now - fgPresentDone_, 0.1);
        fgWork_ = fgWork_ > 0.0 ? 0.8 * fgWork_ + 0.2 * w : w;
    }
    fgLastCall_ = fgCallAt_ = now;
}

// At a world present: whether an in-between frame goes out first - and if so
// it is in `fgMid_`.
bool D3D12Renderer::fgMidFrame() {
    if (enhOff_) return false;
    if (!fgOn_) return false;
    fgTick();
    ++fgReal_;
    bool mid = false;
    if (fgSeq_ == fgShownSeq_) {
        ++fgStale_;   // nothing rendered since the last present: nothing to go between
    } else {
        fgShownSeq_ = fgSeq_;
        const int S = fgSlots_;
        const auto pairs = [&](int a, int b) {
            return !fgCut_[a] && !fgCut_[b] && fgPicW_[a] == fgPicW_[b] && fgPicH_[a] == fgPicH_[b];
        };
        const int n = static_cast<int>((fgSeq_ - 1) % S), p = static_cast<int>((fgSeq_ + S - 2) % S);
        // THE DISPLAY'S ROOM, with a margin each way so it does not flicker on
        // and off: an in-between frame only when the frame's work spans about
        // two refreshes (a 60 Hz display: 33 ms, 30 real fps or fewer; 144 Hz:
        // 14 ms). Measured (2026-10-19, 4K, 60 Hz, ~21 ms of work): without it
        // every in-between present blocked 22-25 ms on the v-sync and the loop
        // went from 21 ms to 57 ms - the real frames at 17 a second, the
        // player, whom the eye follows, visibly stepping.
        fgRoom_ = omk::framegen::roomFor(fgRoom_, fgWork_, fgRefresh_, fgAlways_);
        if (fgSeq_ < 2 || !fgMid_.r || !pairs(n, p)) {
            ++fgCuts_;   // a first frame, a cut on either side, a new picture size
        } else if (!fgRoom_) {
            ++fgNoRoom_;   // the display could not show it without slowing the real frames
        } else {
            // THE PROBE: frames N-2 and N with the vectors doubled, against the real N-1
            if (fgProbe_ && S == 3 && fgSeq_ >= 3) {
                const int m = static_cast<int>((fgSeq_ - 3) % S);
                if (pairs(p, m)) fgProbe(n, p, m);
            }
            fgInterpolate(n, p, 1.0f);
            ++fgMids_;
            mid = true;
        }
    }
    fgReport();
    return mid;
}

// A frame composed on the CPU (`presentSurface`): no world picture of its own
// to warp, so it goes out as it is.
void D3D12Renderer::fgCpuFrame() {
    if (!fgOn_) return;
    fgTick();
    ++fgReal_;
    ++fgCpu_;
    fgShownSeq_ = fgSeq_;
    fgPresentDone_ = qpcNow() + 0.002;   // (its present follows, ~2 ms)
    static bool told = false;
    if (!told) {
        told = true;
        std::printf("framegen: a frame composed on the CPU (the 2D over the world at colour depth 16) goes out "
                    "as it is - colordepth = 32 lets the in-between frames run under the interface\n");
    }
    fgReport();
}

// The in-between picture of kept frames `n` (the later) and `p`, vectors x `k`.
void D3D12Renderer::fgInterpolate(int n, int p, float k) {
    // the passes' table, in the shaders' order: the two pictures, then the two
    // motion-and-depth images (rewritten here: the frames before were waited for)
    Res* in[4] = {&fgCol_[n], &fgCol_[p], &fgAux_[n], &fgAux_[p]};
    for (int i = 0; i < 4; ++i) dev_->CreateShaderResourceView(in[i]->r, nullptr, srv_.cpu(kSlotFgIn + static_cast<UINT>(i)));
    ID3D12GraphicsCommandList* l = oneShotBegin();
    const bool timed = fgQ_ && fgQBuf_;
    if (timed) l->EndQuery(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    for (Res* r : in) r->to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fgHead_[0].to(l, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fgHead_[1].to(l, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
    const D3D12_RECT sr{0, 0, w_, h_};
    l->RSSetViewports(1, &vp);
    l->RSSetScissorRects(1, &sr);
    l->SetGraphicsRootSignature(fgRs_);
    l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    l->SetGraphicsRootDescriptorTable(1, srv_.gpu(kSlotFgIn));
    l->SetGraphicsRootDescriptorTable(2, srv_.gpu(kSlotFgHead));
    D3D12_RESOURCE_BARRIER ub[2]{};
    for (int i = 0; i < 2; ++i) {
        ub[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        ub[i].UAV.pResource = fgHead_[i].r;
    }
    FgPush fp{{static_cast<float>(fgPicW_[n]), static_cast<float>(fgPicH_[n]), 0.0f, 0.0f}, {k, 1.0f, 0.0f, 0.0f}};
    l->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    l->SetPipelineState(fgSplatPipe_);
    l->SetGraphicsRoot32BitConstants(0, sizeof fp / 4, &fp, 0);
    l->DrawInstanced(3, 1, 0, 0);                    // the heads emptied
    l->ResourceBarrier(2, ub);
    fp.k[1] = 0.0f;
    l->SetGraphicsRoot32BitConstants(0, sizeof fp / 4, &fp, 0);
    l->DrawInstanced(3, 1, 0, 0);                    // both frames splatted
    l->ResourceBarrier(2, ub);
    fgMid_.to(l, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_.cpu(kRtvFg + 2 * kFgMaxSlots);
    l->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    l->SetPipelineState(fgResolvePipe_);
    l->DrawInstanced(3, 1, 0, 0);                    // resolved
    fgMid_.to(l, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (timed) {
        l->EndQuery(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(fgQ_, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, fgQBuf_, 0);
    }
    oneShotEnd();
    if (!timed) return;
    void* q = nullptr;
    UINT64 hz = 0;
    if (FAILED(queue_->GetTimestampFrequency(&hz)) || !hz || FAILED(fgQBuf_->Map(0, nullptr, &q))) return;
    const auto* t = static_cast<const UINT64*>(q);
    fgMsSum_ += static_cast<double>(t[1] - t[0]) * 1000.0 / static_cast<double>(hz);
    D3D12_RANGE none{0, 0};
    fgQBuf_->Unmap(0, &none);
    if (++fgMsN_ == 60) {
        std::printf("framegen: %.3f ms on the GPU an in-between frame (clear, splat, resolve) and %.3f ms a frame "
                    "for the capture (mean of 60, %dx%d)\n", fgMsSum_ / 60.0, fgCapMsSum_ / 60.0, w_, h_);
        omk::event("framegen.timing", {{"ms", fgMsSum_ / 60.0}, {"capture", fgCapMsSum_ / 60.0}, {"w", w_}, {"h", h_}});
        fgMsSum_ = fgCapMsSum_ = 0.0;
        fgMsN_ = 0;
    }
}

// a display-size RGBA8 image read back (the probe's)
bool D3D12Renderer::fgRead(Res& img, std::vector<unsigned char>& out) {
    if (hdrOn_ || !makePresentPass()) return false;
    ID3D12GraphicsCommandList* l = oneShotBegin();
    const D3D12_RESOURCE_STATES was = img.st;
    img.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    src.pResource = img.r;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = presRead_;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(w_), static_cast<UINT>(h_), 1,
                                     static_cast<UINT>(presPitch_)};
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    img.to(l, was);
    oneShotEnd();
    out.resize(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4);
    void* p = nullptr;
    if (FAILED(presRead_->Map(0, nullptr, &p))) return false;
    for (int y = 0; y < h_; ++y)
        std::memcpy(out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) * 4,
                    static_cast<const unsigned char*>(p) + static_cast<UINT64>(y) * presPitch_,
                    static_cast<std::size_t>(w_) * 4);
    D3D12_RANGE none{0, 0};
    presRead_->Unmap(0, &none);
    return true;
}

// `OMK_FRAMEGEN_PROBE=<prefix>`: THE CHECK's instrument. Frames N-2 (`m`) and N
// (`n`) interpolated with the vectors doubled, and laid against the frame that
// really was rendered between them, N-1 (`p`) - beside the plain half-and-half
// blend of the two and frame N repeated. The mean error over the picture (a
// channel, 0..255) and over its MOVING part (where N-2 and N differ), a line and
// a `framegen.probe` event a frame; the last frame's three pictures written to
// `<prefix>warp.rgba`, `real.rgba`, `blend.rgba` (RGBA8, the display size).
void D3D12Renderer::fgProbe(int n, int p, int m) {
    fgInterpolate(n, m, 2.0f);
    static std::vector<unsigned char> warp, real, a, b;
    if (!fgRead(fgMid_, warp) || !fgRead(fgCol_[p], real) || !fgRead(fgCol_[n], b) || !fgRead(fgCol_[m], a)) return;
    omk::framegen::probeCompare(warp, real, a, b, fgPicW_[n], fgPicH_[n], w_, fgSeq_, fgProbe_);
}

// The real frame in `presImg_`: presented now - or, after an in-between one,
// half the interval later by the backend's thread.
bool D3D12Renderer::fgPresentReal(bool mid) {
    // at once without an in-between frame, or when the window's picture is
    // asked for (`OMK_WINDUMP`: the real frame's, taken by the present itself)
    if (!mid || winCapture_ || !fgThread_.joinable()) {
        const bool ok = presentImage(presImg_, kSlotPres, w_, h_, w_, h_);
        fgPresentDone_ = qpcNow();
        return ok;
    }
    // half the interval after the CALL - when the frame was ready - not after
    // the in-between frame's present returned (a blocked present would push
    // the real frame onto the next one's heels)
    const double half = std::clamp(0.5 * fgInterval_, 0.0, 0.05);
    {
        std::lock_guard<std::mutex> lk(fgMu_);
        fgDue_ = std::max(fgCallAt_ + half, qpcNow() + 0.001);
        fgPending_ = true;
        fgHurry_ = false;
    }
    fgCv_.notify_all();
    ++fgDeferred_;
    fgLatSum_ += half;
    fgPresentDone_ = qpcNow();
    return true;
}

// The frame's thread waits for a deferred real frame to be out - early, if it
// is not yet due - before it touches the swapchain or `presImg_` again.
void D3D12Renderer::fgSettle() {
    if (!fgThread_.joinable()) return;
    std::unique_lock<std::mutex> lk(fgMu_);
    if (!fgPending_) return;
    fgHurry_ = true;
    fgCv_.notify_all();
    fgCv_.wait(lk, [this] { return !fgPending_; });
}

void D3D12Renderer::fgThreadMain() {
    std::unique_lock<std::mutex> lk(fgMu_);
    for (;;) {
        fgCv_.wait(lk, [this] { return fgPending_ || fgQuit_; });
        if (!fgPending_) return;   // asked to stop, nothing owed
        // to the due time: a timed wait to ~1.5 ms short of it, then yields (the pacer's way)
        while (!fgHurry_) {
            const double left = fgDue_ - qpcNow();
            if (left <= 0.0) break;
            if (left > 0.002) {
                fgCv_.wait_for(lk, std::chrono::microseconds(static_cast<long long>((left - 0.0015) * 1e6)));
            } else {
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
            }
        }
        if (fgHurry_) ++fgHurried_;   // the next frame came before it was due
        lk.unlock();
        fgBlitPresent();
        lk.lock();
        fgPending_ = false;
        fgHurry_ = false;
        fgCv_.notify_all();
    }
}

// The deferred present, on this thread's own list and fence. A window whose
// size changed meanwhile is left to the frame's thread (`presentImage` remakes
// the buffers); this frame is then not shown.
void D3D12Renderer::fgBlitPresent() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT cw = static_cast<UINT>(rc.right - rc.left), ch = static_cast<UINT>(rc.bottom - rc.top);
    if (cw == 0 || ch == 0 || cw != swapW_ || ch != swapH_) return;
    const UINT idx = swap_->GetCurrentBackBufferIndex();
    fgAlloc_->Reset();
    fgList_->Reset(fgAlloc_, nullptr);
    ID3D12DescriptorHeap* heaps[] = {srv_.heap};
    fgList_->SetDescriptorHeaps(1, heaps);
    recordBlit(fgList_, presImg_, kSlotPres, w_, h_, w_, h_, idx);
    swapBuf_[idx].to(fgList_, D3D12_RESOURCE_STATE_PRESENT);
    presImg_.to(fgList_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    fgList_->Close();
    ID3D12CommandList* ls[] = {fgList_};
    swapQueue()->ExecuteCommandLists(1, ls);
    swap_->Present(1, 0);
    ++fgFenceVal_;
    swapQueue()->Signal(fgFence_, fgFenceVal_);
    if (fgFence_->GetCompletedValue() < fgFenceVal_) {
        fgFence_->SetEventOnCompletion(fgFenceVal_, fgEvent_);
        WaitForSingleObject(fgEvent_, INFINITE);
    }
    // how late it reached the screen's queue against when it was due
    // (hurried ones, sent early by the next frame, are not counted)
    std::lock_guard<std::mutex> lk(fgMu_);
    if (!fgHurry_) {
        fgLateSum_ += qpcNow() - fgDue_;
        ++fgOnTime_;
    }
}

// THE COUNTERS, after every real present: an event (what `verify.py: engine:
// framegen` reads) and, every 300, a line.
void D3D12Renderer::fgReport() {
    long hurried = 0;
    double late = 0.0;
    {
        std::lock_guard<std::mutex> lk(fgMu_);
        hurried = fgHurried_;
        late = fgOnTime_ ? 1000.0 * fgLateSum_ / static_cast<double>(fgOnTime_) : 0.0;
    }
    const double lat = fgDeferred_ ? 1000.0 * fgLatSum_ / static_cast<double>(fgDeferred_) : 0.0;
    omk::event("framegen.stats", {{"real", fgReal_}, {"interp", fgMids_}, {"cut", fgCuts_}, {"stale", fgStale_},
                                  {"cpu", fgCpu_}, {"hurried", hurried}, {"interval", 1000.0 * fgInterval_},
                                  {"latency", lat}, {"late", late}, {"noroom", fgNoRoom_},
                                  {"work", 1000.0 * fgWork_}});
    if (fgReal_ % 300 != 0) return;
    std::printf("framegen: %ld real frames presented, %ld in-between ones (%ld firsts or cuts, %ld with nothing new "
                "rendered, %ld composed on the CPU, %ld with no refresh to spare - %.1f ms of work a frame); a frame every %.1f ms, the real one shown %.1f ms after its "
                "in-between one (the latency added; the blit done %.1f ms after it was due), %ld sent early by the "
                "next\n", fgReal_, fgMids_, fgCuts_, fgStale_, fgCpu_, fgNoRoom_, 1000.0 * fgWork_,
                1000.0 * fgInterval_, lat, late, hurried);
}

const omk::Surface& D3D12Renderer::readback() {
    // IDEMPOTENT, and that is a contract (renderer.h): converted once a frame
    if (!dirty_) return fb_;
    dirty_ = false;
    if (copyOwed_) {
        copyOwed_ = false;
        ID3D12GraphicsCommandList* l = oneShotBegin();
        // the scene's colour, or the upscaler's output - or THE POST CHAIN's
        // display-size picture when it ran (an SDR one: HDR's is FP16, and
        // HDR's frames go out on the GPU, `colour depth 32`'s overlay)
        const bool post = postLive_ && !hdrOn_;
        Res& img = post ? postImg_ : upscaleSource();
        img.to(l, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        src.pResource = img.r;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readBuf_;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(post ? w_ : sourceW()),
                                         static_cast<UINT>(post ? h_ : sourceH()), 1,
                                         static_cast<UINT>(post ? postPitch_ : readPitch_)};
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        oneShotEnd();
    }
    void* p = nullptr;
    if (FAILED(readBuf_->Map(0, nullptr, &p))) return fb_;
    const auto* base = static_cast<const unsigned char*>(p);
    const bool post = postLive_ && !hdrOn_;
    const std::size_t pitch = static_cast<std::size_t>(post ? postPitch_ : readPitch_);
    const int ss = post ? 1 : ss_;   // the chain's picture is folded already
    // 888 -> 565 HERE, in software, and the SUPERSAMPLE RESOLVE on the 8-bit
    // side before it - `VulkanRenderer::readback`'s loop, rows at the copy's
    // pitch, rows in parallel (bit-identical to one core).
    omk::Threads::shared().parallelFor(0, static_cast<std::size_t>(h_), 16,
                                       [&](std::size_t rowFrom, std::size_t rowTo) {
        const int y0 = static_cast<int>(rowFrom), y1 = static_cast<int>(rowTo);
        if (ss <= 1 && dither_) {
            for (int y = y0; y < y1; ++y)
                omk::quantise888DitherRow(base + static_cast<std::size_t>(y) * pitch, w_, y,
                                          fb_.px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_));
        } else if (ss <= 1) {
            for (int y = y0; y < y1; ++y) {
                const unsigned char* row = base + static_cast<std::size_t>(y) * pitch;
                for (int x = 0; x < w_; ++x)
                    fb_.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)] =
                        omk::rgb565(row[4 * x], row[4 * x + 1], row[4 * x + 2]);
            }
        } else {
            const int n = ss * ss;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w_; ++x) {
                    int acc[3] = {0, 0, 0};
                    for (int sy = 0; sy < ss; ++sy)
                        for (int sx = 0; sx < ss; ++sx) {
                            const std::size_t o = static_cast<std::size_t>(y * ss + sy) * pitch +
                                                  static_cast<std::size_t>(x * ss + sx) * 4;
                            acc[0] += base[o]; acc[1] += base[o + 1]; acc[2] += base[o + 2];
                        }
                    const int ar = (acc[0] + n / 2) / n, ag = (acc[1] + n / 2) / n, ab = (acc[2] + n / 2) / n;
                    fb_.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x)] =
                        dither_ ? omk::quantise888Dither(ar, ag, ab, x, y)
                                : omk::rgb565(static_cast<unsigned char>(ar), static_cast<unsigned char>(ag),
                                              static_cast<unsigned char>(ab));
                }
        }
    });
    D3D12_RANGE none{0, 0};
    readBuf_->Unmap(0, &none);
    return fb_;
}

void D3D12Renderer::geometryGone(void* self, const omk::Geometry* g) {
    auto* r = static_cast<D3D12Renderer*>(self);
    // the RAY TRACING scene keys on the pointer too, released or not: a new
    // geometry at the same address must not inherit this one's BLAS
    if (auto it = r->rtSet_.find(g); it != r->rtSet_.end()) { r->rtDrop(it->second); r->rtSet_.erase(it); }
    for (auto it = r->rtNotes_.begin(); it != r->rtNotes_.end();)
        it = it->first.second == g ? r->rtNotes_.erase(it) : std::next(it);
    if (!r->releaseOn_) return;
    bool had = false;
    if (auto it = r->vbo_.find(g); it != r->vbo_.end()) {
        r->deadBufs_.push_back(it->second.res);
        r->vbo_.erase(it);
        had = true;
    }
    r->vboN_.erase(g);
    r->vboRev_.erase(g);
    had |= r->tie_.erase(g) > 0;
    had |= r->baked_.erase(g) > 0;
    // a SET going (an area left) is a cut for the upscaler's history
    if (had && g->corners.size() > 30000) r->resetOwed_ = true;
    if (had) ++r->released_;
}

D3D12Renderer::~D3D12Renderer() {
    omk::removeGeometryListener(&D3D12Renderer::geometryGone, this);
    if (!dev_) { release(adapter_); release(factory_); return; }
    // FRAME GENERATION's thread: its last frame out, then stopped
    if (fgThread_.joinable()) {
        fgSettle();
        {
            std::lock_guard<std::mutex> lk(fgMu_);
            fgQuit_ = true;
        }
        fgCv_.notify_all();
        fgThread_.join();
    }
    if (queue_ && fence_) flush();
    up_.reset();   // the SDK's context before the device it was made on
    freeRes(motionRead_);
    if (info_)
        std::printf("d3d12: debug layer - %ld errors, %ld warnings\n", dbgErrors_, dbgWarnings_);
    rtKill();
    for (ID3D12Resource*& b : deadBufs_) freeRes(b);
    for (auto& [g, b] : vbo_) freeRes(b.res);
    for (auto& [key, u] : uploaded_) freeRes(u.t.img);
    freeRes(lutImg_);
    freeRes(white_.img);
    killTarget();
    freeRes(shadow_.r);
    freeRes(shUbo_);
    freeRes(litUbo_);
    freeRes(poseBuf_);
    killSwapchainBuffers();
    release(swap_);
    for (auto*& p : pipe_) release(p);
    for (auto*& p : pipeRefl_) release(p);
    for (auto*& p : pipeCount_) release(p);
    release(pipeStencil_);
    release(pipeDepthReset_);
    release(shPipe_);
    release(aoPipe_);
    release(aoBlurPipe_);
    release(presPipe_);
    release(ovPipe_);
    release(bloomDownPipe_);
    release(bloomUpPipe_);
    release(postPipe_);
    release(bloomRs_);
    release(postRs_);
    release(postQ_);
    release(postQBuf_);
    for (auto*& p : pipeSoft_) release(p);
    release(volPipe_);
    release(volMixPipe_);
    release(volRs_);
    release(volQ_);
    freeRes(volQBuf_);
    freeRes(volUbo_);
    release(blitPipe_);
    release(fgCapPipe_);
    release(fgSplatPipe_);
    release(fgResolvePipe_);
    release(fgRs_);
    release(fgQ_);
    freeRes(fgQBuf_);
    release(fgList_);
    release(fgAlloc_);
    release(fgFence_);
    if (fgEvent_) CloseHandle(fgEvent_);
    release(sceneRs_);
    release(shRs_);
    release(aoRs_);
    release(presRs_);
    release(ovRs_);
    release(blitRs_);
    release(srv_.heap);
    release(rtv_.heap);
    release(dsv_.heap);
    release(list_);
    release(oneList_);
    release(alloc_);
    release(oneAlloc_);
    release(fence_);
    if (fenceEvent_) CloseHandle(fenceEvent_);
    release(presQ_);
    release(queue_);
    release(info_);
    release(dev_);
    release(adapter_);
    release(factory_);
}

}  // namespace

namespace omk {

// The factory, and the window layer's seam (`d3d12render.h`).
Renderer* makeD3d12Renderer() { return new D3D12Renderer(); }

const char* d3d12DeviceName(Renderer* r) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d ? d->device() : "";
}
bool d3d12AttachWindow(Renderer* r, void* hwnd) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    if (!d || !hwnd) return false;
    d->attachWindow(static_cast<HWND>(hwnd));
    return true;
}
bool d3d12Present(Renderer* r) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->presentDirect();
}
bool d3d12PresentSurface(Renderer* r, const Surface& s) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->presentSurface(s);
}
bool d3d12PresentWorld(Renderer* r, int vy, int vh) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->presentWorld(vy, vh);
}
bool d3d12WorldPicture(Renderer* r, int vy, int vh, std::vector<unsigned char>& rgba) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->worldPicture(vy, vh, rgba);
}
// the same look at the output a window is on, for a window of ANOTHER backend (the
// Vulkan one: DXGI is the one place Windows says whether the display is in HDR
// and what its peak is)
int d3d12OutputHdr(void* hwnd, float& peakNits) {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return -1;
    const int r = outputHdrOf(f, static_cast<HWND>(hwnd), peakNits);
    f->Release();
    return r;
}

bool d3d12PresentOverlay(Renderer* r, const Surface& s, const std::uint8_t* mask, const float fade[4],
                         int vy, int vh) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->presentOverlay(s, mask, fade, vy, vh);
}
void d3d12CaptureWindow(Renderer* r) {
    if (auto* d = dynamic_cast<D3D12Renderer*>(r)) d->captureWindow();
}
bool d3d12WindowPicture(Renderer* r, std::vector<unsigned char>& rgba, int& w, int& h) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->windowPicture(rgba, w, h);
}
bool d3d12Resize(Renderer* r, int w, int h) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d && d->resize(w, h);
}
int d3d12Samples(Renderer* r) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d ? d->samples() : 1;
}
int d3d12MaxSamples(Renderer* r) {
    auto* d = dynamic_cast<D3D12Renderer*>(r);
    return d ? d->maxSamples() : 1;
}

}  // namespace omk
