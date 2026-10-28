// SPDX-License-Identifier: GPL-3.0-or-later
// THE LIVE RENDERER - Vulkan, through MoltenVK on macOS.
//
// `PORTING` A1 puts two implementations behind one boundary: a software
// rasterizer that `verify.py` checks, and this, which makes the replica
// playable. `PORTING` A2 puts that boundary at the DECISION level, and the
// rule it states is the one this file is written against - **a backend
// receives decisions and turns them into API calls; it never makes one.**
//
// What that means concretely, and each of these is a decision arriving from
// `Draw` rather than a choice made here:
//
//   * the ORDER. Submissions are drawn in the order given, which is
//     `Render_FlushBuckets` walking 0x4000 buckets ascending - opaque, then
//     additive, then multiply, by material within each. Nothing here sorts by
//     depth, texture or state, however much a GPU would prefer it. The Anekbah
//     signs are the standing proof that this data has coincident faces whose
//     tie-break IS that order.
//   * the TEXTURE. `bucketKey & 0x3F` - the key's low six bits are the
//     material's runtime slot (ASSETS 4b). This binds what that says and does
//     not resolve a texture any other way.
//   * the BLEND. Three pipelines, one per `Blend`, built from the mesh flags
//     the loader decoded: `0x1000|0x2000` additive, `0x1000|0x4000` multiply.
//   * the CUTOUT. Flag `0x800`, a COLOUR KEY on black (`SetRenderState(27,1)`),
//     passed to the shader as a flag and discarded there. Not alpha.
//   * the BACK-FACE CULL. `Raster_DrawTriangles` sets D3DCULL_NONE, so the
//     rasterizer state says `VK_CULL_MODE_NONE` - but the engine has already
//     dropped every single-sided face seen from behind, in software, in
//     `Render_SubmitMesh` (`geom3do.h` `kTwoSided`). Here that is a push
//     constant per run of `cornerCull` and a discard on `gl_FrontFacing`, so
//     no pipeline is doubled.
//   * the CONVENTIONS. The view-projection is built from `cameraBasis`, the
//     software rasterizer's own - world up is (0,-1,0) because the game's Y
//     points DOWN, `hfovDeg` is HORIZONTAL with the vertical following from
//     the frame's shape. Those are the two errors that laying a wireframe over
//     a real screenshot corrected, and re-deriving them here would be a fresh
//     chance to get either wrong where the only symptom is a plausible
//     picture.
//
// **It renders OFFSCREEN and can read back.** No swapchain: the target is a
// VkImage, and `readback()` copies it to an RGB565 `Surface` - the same type
// the software renderer returns. That is what makes the pair differenceable
// rather than merely parallel, and this repo has spent two sessions learning
// what an unverifiable render costs. Presentation is the frontend's job; it
// already uploads a `Surface`.
//
// **What must NOT be claimed of a GPU frame.** It will never be pixel-equal to
// the software loop and is not meant to be: the fill rule, the interpolation
// precision and the texture filter are the driver's. That is `PORTING` B5's
// argument about a captured frame, and it applies here for the same reason -
// so the two are compared by SILHOUETTE and COVERAGE, with the same instrument
// `verify.py: engine silhouette` uses against the engine's own captures.
//
// A8: this is a system dependency and it is OPTIONAL. `make` and the whole
// suite must pass on a machine with no Vulkan SDK, so nothing under `src/`
// includes this and the Makefile builds it only when pkg-config finds vulkan.
// Every Vulkan create-info here is written `VkXxxInfo x{VK_STRUCTURE_TYPE_...}`:
// the type set, every other field value-initialised to zero - the API's own
// idiom, which `-Wmissing-field-initializers` reports once per struct.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "platform/threads.h"
#include <cmath>
#include <vulkan/vulkan.h>
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
#include "o3de/lighttiles.h"
#include "o3de/fsrcon.h"
#include "upscale_vk.h"
#include "platform/events.h"
#include "platform/env.h"
#include "platform/events.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <map>
#include <tuple>
#include <span>
#include <string>
#include <vector>

namespace {

const uint32_t kVert[] =
#include "scene.vert.inc"
;
const uint32_t kFrag[] =
#include "scene.frag.inc"
;
// SOFT PARTICLES' variant of the scene fragment shader (-DOMK_SOFT, the depth as set 3)
const uint32_t kFragSoft[] =
#include "scene_soft.frag.inc"
;
// VOLUMETRIC LAMP LIGHT's march and composite (one GLSL source with Direct3D 12), and their
// multisampled-depth builds
const uint32_t kVolFrag[] =
#include "vol.frag.inc"
;
const uint32_t kVolMsFrag[] =
#include "vol_ms.frag.inc"
;
const uint32_t kVolMixFrag[] =
#include "volmix.frag.inc"
;
const uint32_t kVolMixMsFrag[] =
#include "volmix_ms.frag.inc"
;
// FRAME GENERATION's scene pass (-DOMK_MOTION: the motion vectors) and its three passes
const uint32_t kVertMv[] =
#include "scene_mv.vert.inc"
;
const uint32_t kFragMv[] =
#include "scene_mv.frag.inc"
;
const uint32_t kFragSoftMv[] =
#include "scene_soft_mv.frag.inc"
;
// HYBRID RAY TRACING's (`todo/enhancements.md` 19): the scene pass and the AO pass built -DOMK_RT
// (`rt.glsl`: VK_KHR_ray_query; the rays' three sets as 4, 5, 6 here and 1, 2, 3 for the AO pass),
// the scene pass's motion build and its COUNTING builds (the rays' tally), the AO's multisampled one
const uint32_t kFragRt[] =
#include "scene_rt.frag.inc"
;
const uint32_t kFragRtMv[] =
#include "scene_rt_mv.frag.inc"
;
const uint32_t kFragRtc[] =
#include "scene_rtc.frag.inc"
;
const uint32_t kFragRtcMv[] =
#include "scene_rtc_mv.frag.inc"
;
const uint32_t kSsaoRtFrag[] =
#include "ssao_rt.frag.inc"
;
const uint32_t kSsaoRtMsFrag[] =
#include "ssao_rt_ms.frag.inc"
;
const uint32_t kFgCapFrag[] =
#include "fg_cap.frag.inc"
;
const uint32_t kFgSplatFrag[] =
#include "fg_splat.frag.inc"
;
const uint32_t kFgResolveFrag[] =
#include "fg_resolve.frag.inc"
;
// ...and with the TILED point lights (`o3de/lighttiles.h`): its own module,
// bound only on a frame whose tiles are on (`scene.frag`, -DOMK_TILED)
const uint32_t kFragTiled[] =
#include "scene_tiled.frag.inc"
;
// ...and both with the CUTOUT's late depth (`scene.frag` OMK_LATE_DEPTH): the
// opaque cutout draws' modules
const uint32_t kFragCut[] =
#include "scene_cut.frag.inc"
;
const uint32_t kFragTiledCut[] =
#include "scene_tiled_cut.frag.inc"
;
const uint32_t kFsqVert[] =
#include "fsq.vert.inc"
;
// the temporal upscaler's depth copy (`depthcopy.frag`)
const uint32_t kDepthCopyFrag[] =
#include "depthcopy.frag.inc"
;
const uint32_t kFsqFrag[] =
#include "fsq.frag.inc"
;
// The present pass: the adventure frame's dither and 565 expansion on the GPU
// (todo/optimization.md step 4b).
const uint32_t kPresentFrag[] =
#include "present.frag.inc"
;
struct PresentPush { int32_t vy, vh, w, dither, ss, depth, rcas; uint32_t rcasCon; int32_t raw; };
// THE INTERFACE AS AN OVERLAY at colour depth 32 (`overlay.frag`, shared with
// the Direct3D 12 backend; `todo/enhancements.md` 21): C and M over the world
const uint32_t kOverlayFrag[] =
#include "overlay.frag.inc"
;
struct OverlayPush { float fade[4]; };
// HDR OUTPUT (`hdr = on`, `todo/enhancements.md` 22): `blit.frag` built with
// -DOMK_HDR - the picture (gamma 2.2, 1.0 = paper white, its highlights past
// 1.0) to scRGB's linear light - drawn into an FP16 swapchain. The Direct3D 12
// backend's blit, from the same GLSL.
const uint32_t kBlitHdrFrag[] =
#include "blit_hdr.frag.inc"
;
struct BlitPush { float dst[4]; float srcSize[4]; int32_t linear; float white, peak; };
// THE POST CHAIN (`todo/enhancements.md` 22): `bloom.frag`'s three jobs and
// `post.frag`'s final pass, the Direct3D 12 backend's own GLSL (the
// multisampled-depth build for a frame whose depth is read per sample)
const uint32_t kBloomFrag[] =
#include "bloom.frag.inc"
;
const uint32_t kPostFrag[] =
#include "post.frag.inc"
;
const uint32_t kPostMsFrag[] =
#include "post_ms.frag.inc"
;
// THE UPSCALE (`todo/remaster.md` A6): AMD FSR 1's EASU, vendored (MIT,
// `third_party/ffx_fsr1.h`); its RCAS is in the present pass above
const uint32_t kEasuFrag[] =
#include "easu.frag.inc"
;
// THE REMASTER'S POST-PASS (`todo/remaster.md` step 2): bloom and grade.
const uint32_t kRmPostFrag[] =
#include "rmpost.frag.inc"
;
struct RmPostPush { float dstInv[2], srcInv[2], dir[2]; float strength; int32_t mode; int32_t lutSize; int32_t ao; };
// THE AMBIENT OCCLUSION (`todo/remaster.md` A4): from the frame's depth, half
// size, plain and multisampled
const uint32_t kAoFrag[] =
#include "ao.frag.inc"
;
const uint32_t kAoMsFrag[] =
#include "ao_ms.frag.inc"
;
constexpr float kAoRadius = 60.0f;   // the occlusion's reach, game units (~60 cm): contact, not shade
struct AoPush { float view[4], depth[4]; float radius, strength; int32_t mode, pad; float srcInv[2]; };
// The DEPTH-ONLY pass of the mapped shadow (`todo/enhancements.md` 6).
const uint32_t kShadowVert[] =
#include "shadow.vert.inc"
;
// SSAO (`todo/enhancements.md` 15): the occlusion, then its blur multiplied in.
const uint32_t kSsaoFrag[] =
#include "ssao.frag.inc"
;
const uint32_t kSsaoBlurFrag[] =
#include "ssao_blur.frag.inc"
;
const uint32_t kSsaoMsFrag[] =        // the same two over a MULTISAMPLED depth
#include "ssao_ms.frag.inc"
;
const uint32_t kSsaoBlurMsFrag[] =
#include "ssao_blur_ms.frag.inc"
;
struct SsaoPush { float tanH, tanV, A, B, vw, vh, radius, strength; };
// ...the RAY-TRACED AO's (`ssao.frag` -DOMK_RT): the camera in the world after it
struct SsaoRtPush { SsaoPush a; float eye[4], s[4], u[4], f[4]; };
// `scene.frag`'s `RtFrame` block (-DOMK_RT), shared with the Direct3D 12 backend
using RtFrame = omk::rt::Frame;

// The map's side. 1024 is a shadow of a handful of CHARACTERS over a slab a
// few metres across, so a texel is centimetres.
constexpr uint32_t kShadowSide = 2048;

// Set 1, binding 0 - per FRAME, and it must match `scene.frag`'s block.
// Set 1, binding 2 - the LIGHTS, per frame. std140: an array of structs
// aligns every member to 16 bytes, which three vec4s already are.
struct GpuLightStd140 {
    float posA[4];      // xyz position, w outer radius
    float dirB[4];      // xyz direction, w inner radius
    float colourI[4];   // rgb colour 0..1, a intensity
};
struct LightUbo {
    GpuLightStd140 l[omk::View::kMaxGpuLights];   // 8 the set's, the rest the remaster's points
    int32_t count;
    float   base;      // View::litBase, in the std140 padding that was here
    int32_t grey;      // View::grey, the greyscale bank (ops 150/151) - padding too
    int32_t tiles;     // 1: the TILED point lights (`o3de/lighttiles.h`) are bound - padding once
    // THE WATER's Fresnel (`todo/remaster.md` A3): the eye, at 3088. Written by
    // every `begin`, and the frame's own pass is the last one - the only one
    // that reads it (the reflection pass draws no water)
    float   eye[4];
    // THE RELIEF (`View::relief`), at 3104: read by the TILED module alone
    float   fx[4];
    // THE SAND'S DUST (`View::dust`, the restored weather's, the remaster's), at
    // 3120: strength, the drift's x and z, the distance scale; its colour at 3136
    float   dust[4];
    float   dustColour[4];
    // THE RAIN'S VEILS (`View::veil`), at 3152: strength, the fall's offset,
    // the distance scale; their colour at 3168
    float   veil[4];
    float   veilColour[4];
    float   soft[4];   // SOFT PARTICLES (`scene.frag` -DOMK_SOFT): A, B, 1 / the fade's range
};
// the offsets `scene.frag`'s block reads them at (std140: 64 lights of 48
// bytes, four scalars, then the two vec4s)
static_assert(offsetof(LightUbo, eye) == 3088 && offsetof(LightUbo, fx) == 3104 &&
              offsetof(LightUbo, dust) == 3120 && offsetof(LightUbo, dustColour) == 3136 &&
              offsetof(LightUbo, veil) == 3152 && offsetof(LightUbo, veilColour) == 3168,
              "the Lights block's layout is scene.frag's");

// Up to three mapped shadows a frame, one per LAYER of the shadow image: the
// strongest lights reaching the casters (`View::shadow` + `extraShadows`).
constexpr int kShadowLayers = 3;

struct ShadowUbo {
    float lightMvp[kShadowLayers][16];
    float strength[4]; // per layer; 0 = no shadow from it (the default)
    float lightPos[kShadowLayers][4]; // xyz = lamp position (w 1) or travel direction (w 0)
    float params[kShadowLayers][4];   // near, far, penumbra scale, 1 = perspective
    float texel;       // 1 / kShadowSide
    float bias;
    // THE SHIMMER's clock (`o3de/shimmer.h`), which is per FRAME like the rest
    // of this block and had a spare float sitting here for the std140
    // alignment. Not the shadow's business, but this is the frame's uniform.
    float shimmer;
    float pad;
};

#define VKCHECK(x, what)                                                     \
    do {                                                                     \
        const VkResult r_ = (x);                                             \
        if (r_ != VK_SUCCESS) {                                              \
            std::fprintf(stderr, "vulkan: %s -> %d\n", what, r_);            \
            return false;                                                    \
        }                                                                    \
    } while (0)

// how a baked tie loser's corner is marked; `scene.vert` decodes it
constexpr float kTieLoserPhase = 8192.0f;

// One vertex, matching `scene.vert`'s three inputs. The UVs stay in the
// material's own PIXEL units, as the shipped data stores them, and the shader
// divides by textureSize with the sampler set to REPEAT - which is the same
// wrap `raster.cpp`'s `sample()` does with `% t.width`.
struct GpuVert {
    float x, y, z;
    float u, v;
    float r, g, b;
    // THE NORMAL, for row 7's per-pixel lighting. It travels with the vertex
    // because a bone's rotation turns its normals with it - `applyPose` does
    // that already, for the crowd's per-vertex light.
    float nx, ny, nz;
    // THE SHIMMER's phase (`o3de/shimmer.h`), -1 on a mesh that does not.
    float phase;
    // THE CORNER'S MESH (`Geometry::cornerMesh`), -1 for none: which affine
    // a POSED draw places it by (`scene.vert`, `posesBodies`); every other
    // draw ignores it.
    float mesh;
};

// It MUST match `scene.frag`'s block byte for byte, and the ordering is what
// makes it match: a `vec3` in a push-constant block is 16-byte aligned, so
// `fogColour` sits at offset 80 and everything scalar has to come before it.
// Packed the obvious way - mvp, cutout, fogStart, fogEnd, fogColour - the
// colour arrived four bytes early and had done since the fog landed; it went
// unseen because the shipped fog colour is BLACK, so the misplaced bytes were
// zeros. See `scene.frag`.
struct Push {
    float mvp[16];     // 0
    int32_t cutout;    // 64  flag 0x800: a colour key on black, never alpha
    float fogStart;    // 68  0 = no fog for this batch; see renderer.h's View
    float fogEnd;      // 72
    int32_t caster;    // 76  this batch CASTS, so it must not RECEIVE
    float fogColour[3];// 80
    int32_t lit;       // 92  this batch is LIT per pixel (row 7)
    int32_t cull;      // 96  the BACK-FACE CULL: 0 both sides, +1 discard
                       //     where !gl_FrontFacing, -1 where gl_FrontFacing
    int32_t posed;     // 100 a body POSED HERE: set 2 holds its affines and lights
    // THE TEXTURE PACK (`o3de/texpack.h`, `todo/remaster.md` B4): the bound
    // texture is a replacement `texScaleU` x `texScaleV` the original's size,
    // and the UVs are still in the ORIGINAL's texels. 0 for every shipped
    // texture, and the shader then divides exactly as it did before.
    int32_t texScaled; // 104
    float texScaleU;   // 108
    float texScaleV;   // 112
    // THE BLOOM (`todo/remaster.md` step 2, `post.frag`): 0 off; 1 on - the
    // alpha written is the texel's LIGHT, from the pack's light map
    int32_t bloomMode; // 116
    // THE WATER REFLECTION (`todo/remaster.md` A3): a water draw adds the
    // reflection (binding 2) times the strength; 0 on every other draw. The
    // low four bits the mode; above them a WET floor's darkening, 0..255
    // (`Draw::wet`, `o3de/wetfloor.h`), and above that the rain's, 0..255 (its
    // ripples, `View::rain`)
    int32_t water;     // 120
    float waterStrength; // 124
};
// 128 is every device's guaranteed push-constant size: the block is full
static_assert(sizeof(Push) == 128, "the push block is 128 bytes");
// ...and with FRAME GENERATION on (`scene.vert` built -DOMK_MOTION), the same 128 bytes and then the
// JITTER at 128 (clip units - zero here: no upscaler) and the PREVIOUS frame's view-projection at 144
struct PushMv {
    Push p;
    float jitter[2];      // 128
    float pad_[2];        // 136 (the shader's mat4 sits on a 16)
    float prevMvp[16];    // 144
};
static_assert(sizeof(PushMv) == 208 && offsetof(PushMv, prevMvp) == 144, "the motion push block is 208 bytes");
// Which `gl_FrontFacing` is the software rasterizer's FRONT (a negative area in
// `raster.cpp`): this pipeline's `frontFace` and Vulkan's Y-down viewport.
// Measured, not derived - the wrong sign culls every front face and the room
// turns inside out, which `engine: back-face cull` and the renderer checks see.
constexpr int32_t kCullSign = 1;

class VulkanRenderer : public omk::Renderer {
public:
    bool init(int w, int h) override;
    void setTextures(std::span<const omk::Texture> t) override { poolTextures(t, false); }
    void setPreloadTextures(std::span<const omk::Texture> t) override;
    bool keepsPreload() const override { return true; }
    int preloadStep(int budget) override;
    // the pool's textures uploaded where new; `preload`: ONLY that, pinned -
    // nothing evicted, the frame's pool (`tex_`) untouched
    void poolTextures(std::span<const omk::Texture> t, bool preload);
    void begin(const omk::View& v) override;
    void shadowPass(const omk::View& v, std::span<const omk::Draw> casters) override;
    bool drawsPixelLights() const override { return true; }
    bool drawsShadowMap() const override { return true; }
    // BODIES POSED BY THE RENDERER (`renderer.h` `Draw::meshPose`), as GLES,
    // GL1 and the 3DS do, for consistency across the backends: a posed draw's rest corners, one affine a mesh and
    // the lights that reach the body go to set 2, and `scene.vert` places,
    // turns and lights each corner by the GLES shader's rule. A draw that will
    // not fit the frame's pose buffer, and every caster of the shadow map, is
    // posed on the CPU instead (`cpuPose`) - so no path draws it wrong.
    bool posesBodies() const override { return poseDs_ != VK_NULL_HANDLE; }
    int maxVertexLights() const override { return poseDs_ != VK_NULL_HANDLE ? kPoseLights : 0; }
    bool setWater(float) override { return true; }   // nothing to build ahead
    bool setBodyTint(bool) override { return true; }  // the pose block always carries it
    bool drawsBodyTint() const override { return true; }
    // scene.frag takes a point light always; ON says the frame will send the
    // emitters' lights, and only then are the tiled lights' buffers made (32 MB)
    bool setEmitterLights(bool on) override { emitters_ = on; return true; }
    bool drawsEmitterLights() const override { return true; }
    bool takesPack() const override { return true; }
    bool drawsRelief() const override { return true; }
    int maxPixelLights() const override { return omk::View::kMaxGpuLights; }
    bool tilesLights() const override {
        return pointsBuf_ != VK_NULL_HANDLE && tilesBuf_ != VK_NULL_HANDLE && pipeTiled_[0] != VK_NULL_HANDLE;
    }
    bool drawsWater() const override { return true; }
    bool drawWaterScene(const omk::View& v, const omk::View& refl, std::span<const omk::Draw> scene,
                        std::span<const omk::Draw> above, std::span<const omk::Draw> water,
                        float strength, float planeY) override;
    bool drawMirrorScene(const omk::View& v, const omk::View& refl,
                         std::span<const omk::Draw> scene,
                         std::span<const omk::Draw> sceneClipped,
                         std::span<const omk::Draw> mirror) override;
    void pushView(const omk::View& v);   // recompute the mvp mid-frame
    void submit(const omk::Draw& d) override;
    void end() override;
    const omk::Surface& readback() override;
    omk::RasterStats stats() const override { return st_; }
    const char* name() const override { return "vulkan"; }
    // every request of the boundary (`Renderer::takes`)
    unsigned takes() const override {
        return omk::kTakesMultisample | omk::kTakesTextureFilter | omk::kTakesSupersample |
               omk::kTakesAnisotropy | omk::kTakesShadowMap | omk::kTakesPixelLights | omk::kTakesUpscale |
               omk::kTakesBloom | omk::kTakesGrade | omk::kTakesWater | omk::kTakesBodyTint |
               omk::kTakesEmitterLights | omk::kTakesAmbientOcclusion | omk::kTakesRelief | omk::kTakesMist | omk::kTakesRain3d |
               omk::kTakesTrilinear;
    }
    ~VulkanRenderer() override;
    // A geometry the renderer holds state for tells it when it is destroyed
    // (`omk::GpuResidency`, todo/optimization.md step 26): the maps are cleared
    // at once and the buffers destroyed at the next `begin`, when the previous
    // frame's fence has been waited on. `OMK_NO_GEOMETRY_RELEASE=1` keeps the
    // old behaviour, to compare.
    VulkanRenderer() {
        releaseOn_ = !omk::envFlag("OMK_NO_GEOMETRY_RELEASE");
        omk::addGeometryListener(&VulkanRenderer::geometryGone, this);
    }
    bool releaseOn_ = true;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> deadBufs_;
    long released_ = 0;
    static void geometryGone(void* self, const omk::Geometry* g);
    // The enhancement (renderer.h). Recorded here and honoured in makeTarget,
    // where the device's own limits get the last word.
    bool setSsao(bool on) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        wantSsao_ = on;
        return true;
    }
    // HYBRID RAY TRACING (`todo/enhancements.md` 19): honoured in `init` when the device has
    // VK_KHR_ray_query (`pickDevice`, `rtInit`) - the same contract as Direct3D 12's
    bool setRaytracing(bool on) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        rtWant_ = on;
        return true;
    }
    bool raytracingLive() const override { return rtOn_; }
    // ...and RAY-TRACED REFLECTIONS (`todo/enhancements.md` 26), only with the ray tracing live
    bool setRtReflections(bool on) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        rtWantRefl_ = on;
        return true;
    }
    bool rtReflectionsLive() const override { return rtReflOn_; }
    bool setMultisample(int samples) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        wantSamples_ = samples < 2 ? 1 : samples < 4 ? 2 : samples < 8 ? 4 : 8;
        return true;
    }
    // THE POST CHAIN (`todo/enhancements.md` 22): bloom, colour grading, depth
    // of field on cinematic frames - the Direct3D 12 backend's chain, the same
    // GLSL and the same numbers (`o3de/postfx.h`). Asked for here before
    // `init`, made on first use (`makePost`). Soft particles ride on it (the
    // scene pass itself), and volumetric lamp light (two passes before the chain).
    bool setPostProcess(const PostProcess& p) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        postReq_ = p;
        postReq_.grade = p.grade < 0 ? 0 : p.grade > 4 ? 4 : p.grade;
        softWant_ = p.softParticles;   // decided in `makeTarget` (not under MSAA)
        volWant_ = p.volumetrics;      // ...and this one there too (the depth must be sampleable)
        postWant_ = p.bloom || p.dof || postReq_.grade > 0 || p.hdr;
        return true;
    }
    bool softParticlesLive() const override { return softOn_; }
    // FRAME GENERATION (`todo/enhancements.md` 27): the window's alone; decided in `makeTarget`
    bool setFrameGeneration(bool on) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        fgWant_ = on;
        return true;
    }
    // the display's refresh rate, Hz, which the window layer knows (0: unknown)
    void setRefresh(int hz) { fgRefresh_ = hz; }
    bool volumetricsLive() const override { return volOn_; }
    // HDR OUTPUT: what the window layer knows of the display before `init`
    // (`known`: 1 reports HDR, 0 does not, -1 not asked - then the surface's
    // offer of scRGB is all there is) and its peak luminance in nits (0: unknown)
    void setHdrDisplay(int known, float peak) {
        hdrKnown_ = known;
        if (peak > 0.0f) hdrPeak_ = peak;
    }
    int samples() const { return static_cast<int>(samples_); }
    bool setTextureFilter(int mode) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        filter_ = mode < 1 ? 0 : mode < 2 ? 1 : 2;
        return true;
    }
    bool setAnisotropy(int n) override {
        if (dev_ != VK_NULL_HANDLE) return false;
        aniso_ = n < 1 ? 1 : n > 16 ? 16 : n;
        return true;
    }
    // THE BLOOM's strength, 0 off; any time - it changes no pipeline
    bool setBloom(float s) override { bloom_ = s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s; return true; }
    // THE AMBIENT OCCLUSION's strength, 0 off - turned ON before `init`: it
    // keeps the frame's depth readable, which the target is made with. Once
    // the target has it (`aoOk_`), the strength moves at any time - it is the
    // pass's push constant (OMK's options menu, `todo/graphics-menu.md` 3a).
    bool setAmbientOcclusion(float s) override {
        if (dev_ != VK_NULL_HANDLE && !aoOk_) return false;
        ao_ = s < 0.0f ? 0.0f : s > 4.0f ? 4.0f : s;
        return true;
    }
    // THE GRADE: `rgb` the LUT's N^3 entries, red fastest (a .cube's order),
    // 0..1; n 0 turns it off. Any time - uploaded before the next frame's pass.
    bool setGrade(const float* rgb, int n) override {
        if (!rgb || n < 2) { lutN_ = 0; return true; }
        omk::lutStrip(rgb, n, lutData_);
        lutN_ = n; lutDirty_ = true;
        return true;
    }
    bool setSupersample(int n) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        ss_ = n < 2 ? 1 : n < 4 ? 2 : 4;
        return true;
    }
    bool setUpscale(float f) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        up_ = f > 1.0f ? std::min(f, 3.0f) : 1.0f;
        return true;
    }
    // THE TEMPORAL UPSCALER (`upscale_vk.h`, `todo/enhancements.md` 18): FSR 3.1 or DLSS asked for here,
    // before `init`; it is made once the device exists (`makeTemporalUpscaler`) and decides the render size
    bool setUpscaler(int kind, int quality) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the device and the target exist
        tupKind_ = kind < 0 ? 0 : kind > 2 ? 2 : kind;
        tupQuality_ = quality < 0 ? 0 : quality > 4 ? 4 : quality;
        return true;
    }
    int supersample() const { return ss_; }
    float upscale() const { return up_; }
    int textureFilter() const { return filter_; }
    int anisotropy() const { return anisoOn_ ? aniso_ : 1; }
    // The largest count the device's colour, depth and stencil limits all
    // allow - so a probe can tell "the device cannot" from "the backend did
    // not", which a check that skips on the former must be able to do.
    int maxSamples() const {
        const VkSampleCountFlags ok = props_.limits.framebufferColorSampleCounts
                                    & props_.limits.framebufferDepthSampleCounts
                                    & props_.limits.framebufferStencilSampleCounts;
        for (int n = 64; n > 1; n >>= 1)
            if (ok & static_cast<VkSampleCountFlags>(n)) return n;
        return 1;
    }

    const char* device() const { return props_.deviceName; }

private:
    bool pickDevice();
    bool makeTarget();
    bool makePipelines();
    void makeShadowSet();
    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want) const;
    // every device memory released here, so the profiler's GPU table
    // (todo/debug-tools.md 5) loses it too
    void freeDeviceMemory(VkDeviceMemory m) {
        OMK_GPU_FREE(omk::prof::gpuKey(omk::prof::kVkMemory, m));
        vkFreeMemory(dev_, m, nullptr);
    }
    bool makeBuffer(VkDeviceSize n, VkBufferUsageFlags use,
                    VkMemoryPropertyFlags props, VkBuffer& b, VkDeviceMemory& m);
    bool makeImage2D(const VkImageCreateInfo& ii, VkImageAspectFlags aspect, const char* what,
                     VkImage& img, VkDeviceMemory& mem, VkImageView* view);
    VkCommandBuffer oneShotBegin();
    void oneShotEnd(VkCommandBuffer cb);
    // THE BATCHED UPLOAD (the remaster audit's performance item 2): a pool's
    // texture copies recorded into ONE command buffer (`uploadCb`), submitted
    // and waited on once (`flushUploads`), their bytes staged in one
    // persistent host-visible RING - not a staging buffer, an allocation, a
    // submit and a `vkQueueWaitIdle` a texture
    VkCommandBuffer uploadCb();
    bool stage(VkDeviceSize bytes, VkBuffer& buf, VkDeviceSize& off, unsigned char*& ptr);
    void flushUploads();
    static constexpr VkDeviceSize kRingBytes = VkDeviceSize{32} << 20;
    VkCommandBuffer batchCb_ = VK_NULL_HANDLE;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> batchOwn_;   // staging past the ring's size
    VkBuffer ring_ = VK_NULL_HANDLE; VkDeviceMemory ringMem_ = VK_NULL_HANDLE;
    unsigned char* ringPtr_ = nullptr;
    VkDeviceSize ringAt_ = 0;
    bool ringFailed_ = false;
    bool uploadGeometry(const omk::Geometry* g);

    VkInstance        inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    bool r8Ok_ = false;   // R8 blits and filters: the light maps one channel (`setTextures`)
    bool bcOk_ = false;   // BC7 and BC4 sampled and filtered: the pack's block files (`setTextures`)
    VkPhysicalDeviceMemoryProperties memProps_{};
    long long lazyBytes_ = 0; int lazyImages_ = 0;   // TRANSIENT attachments in lazily allocated memory
    VkDevice          dev_  = VK_NULL_HANDLE;
    VkQueue           queue_ = VK_NULL_HANDLE;
    uint32_t          qfam_ = 0;
    VkCommandPool     pool_ = VK_NULL_HANDLE;
    VkCommandBuffer   cb_   = VK_NULL_HANDLE;
    VkFence           fence_ = VK_NULL_HANDLE;

    int w_ = 0, h_ = 0;      // the OUTPUT size - what `readback()` answers with
    // ...and the RENDER size, `w_ * ss_` by `h_ * ss_`. Supersampling
    // (`todo/enhancements.md` 9) draws the frame larger and averages it down,
    // which is the only anti-aliasing that reaches a CUTOUT edge - MSAA looks
    // at triangle edges and a cutout's silhouette is a colour key inside one.
    int rw_ = 0, rh_ = 0;
    int ss_ = 1;             // 1 off, else 2 or 4
    // ...or THE UPSCALE (`todo/remaster.md` A6): the frame drawn `up_` times
    // SMALLER each way and brought up to the output by FSR 1 in the present
    // pass (`runPresentPass`); 1 off, and it takes the supersample's place
    float up_ = 1.0f;
    // a picture `v` output pixels long, in RENDER pixels - the one rule every
    // size the frame is drawn at goes through (EASU's viewport is the same)
    int rs(int v) const {
        if (tupOn_) return std::max(1, static_cast<int>(std::lround(static_cast<float>(v) / tupDiv_)));
        return up_ > 1.0f ? std::max(1, static_cast<int>(std::lround(static_cast<float>(v) / up_))) : v * ss_;
    }
    // ...and the supersample the display half folds: one, once the temporal upscaler has rebuilt the
    // frame at the display size in `colour_` (`upscale()`)
    int outSs() const { return tupOn_ ? 1 : ss_; }
    int picH_ = 0;           // the last frame's picture height, output pixels (`readback`'s upscale)
    // Which region of `colour_` the present blit should take. `end()` fills
    // the whole render target; `presentSurface` uploads a finished picture at
    // the OUTPUT size into its top-left, and blitting the whole thing then
    // would stretch a quarter of the frame over the window.
    int srcW_ = 0, srcH_ = 0;
    // `colour_` is the single-sample image every consumer reads - the
    // readback, the swapchain blit, the 2D upload. With MSAA on it becomes
    // the RESOLVE target and the pipelines rasterise into `msColour_`.
    int                   wantSamples_ = 1;
    int                   filter_ = 0;        // 0 nearest (the original), 1 bilinear, 2 trilinear
    int                   aniso_ = 1;         // asked; 1 is off
    bool                  anisoOn_ = false;   // the device feature was enabled for it
    bool                  anisoAvail_ = false; // ...the device has it (enabled whatever was asked)
    std::vector<VkSampler> retiredSamplers_;  // `retexture`'s old ones: the 1x1 stand-ins' sets keep them
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkImage        colour_ = VK_NULL_HANDLE;  VkDeviceMemory colourMem_ = VK_NULL_HANDLE;
    VkImageView    colourView_ = VK_NULL_HANDLE;
    VkImage        msColour_ = VK_NULL_HANDLE; VkDeviceMemory msColourMem_ = VK_NULL_HANDLE;
    VkImageView    msColourView_ = VK_NULL_HANDLE;
    VkImage        depth_ = VK_NULL_HANDLE;   VkDeviceMemory depthMem_ = VK_NULL_HANDLE;
    VkFormat       dsFmt_ = VK_FORMAT_UNDEFINED;
    VkImageView    depthView_ = VK_NULL_HANDLE;
    VkRenderPass   pass_ = VK_NULL_HANDLE;
    // SOFT PARTICLES (`todo/enhancements.md` 23; off unless asked): the frame's pass is cut where
    // the first effect sprite comes - the sprites are drawn in a CONTINUATION of the pass (LOAD, the
    // frame's final layouts as its own initial and final ones: any number of cuts) whose depth is
    // READ-ONLY, so the fragment shader can sample the depth it is tested against; the next draw
    // that writes depth opens a writable continuation. Single-sample only, like Direct3D 12's.
    bool softWant_ = false, softOn_ = false, softNoUbo_ = false;
    VkRenderPass passCont_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [0] writable depth, [1] read-only
    VkDescriptorSetLayout dsl3_ = VK_NULL_HANDLE;
    VkDescriptorPool softPool_ = VK_NULL_HANDLE;
    VkDescriptorSet softDs_ = VK_NULL_HANDLE;                       // set 3: the depth, sampled
    VkSampler softSampler_ = VK_NULL_HANDLE;
    VkPipeline pipeSoft_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};   // [1] additive, [2] multiply
    bool depthRo_ = false;             // the open pass segment's depth is read-only
    float softRange_ = 24.0f;          // the fade's depth range, world units (~0.6 m)
    long softDraws_ = 0;               // sprite batches drawn soft this frame
    bool softTold_ = false;
    void softSegment(bool readOnly);
    // ---- FRAME GENERATION (`todo/enhancements.md` 27; off unless asked) ----
    //
    // The scene pass writes MOTION VECTORS (`mvOn_`, `scene.vert/frag` built -DOMK_MOTION) beside the
    // colour; at each frame's end the finished world picture is kept with them and the depth
    // (`fg_cap.frag`); at the present an in-between picture is splatted from frames N-1 and N half-way
    // along their motion with a depth test (`fg_splat.frag`) and resolved (`fg_resolve.frag`), goes out
    // through the same present pass and overlay, and the real frame follows half an interval later - its
    // present (the blit is done) deferred to a thread of the backend's own on a second queue. One source
    // with the Direct3D 12 window; the pacing rules (`o3de/framegen.h`) are shared.
    bool fgWant_ = false, fgOn_ = false, fgFeat_ = false, mvOn_ = false;
    VkImage motion_ = VK_NULL_HANDLE; VkDeviceMemory motionMem_ = VK_NULL_HANDLE; VkImageView motionView_ = VK_NULL_HANDLE;
    VkQueue presQ_ = VK_NULL_HANDLE;   // a second queue of the family, for the deferred present
    PushMv pm_{};
    // ---- THE TEMPORAL UPSCALER (`upscale_vk.h`; off unless asked) ----
    //
    // FSR 3.1 or DLSS rebuilds the display-size picture from the scene's colour, depth and motion at a
    // RENDER size below it, over the sub-pixel jitter `begin` puts in the push block. `colour_` is then
    // the DISPLAY size (the scene pass draws into its top-left `rw_ x rh_`); `upscale()` records the SDK
    // from it into `tupOut_` and copies the result back over it, so every reader after it - the post
    // chain, the present pass, the readback, the frame-generation capture - finds an ordinary display-size
    // picture (`outSs()`: no supersample to fold).
    int tupKind_ = 0, tupQuality_ = 1;           // asked: 0 off, 1 FSR, 2 DLSS
    std::unique_ptr<omk::vkup::Upscaler> tup_;   // ...and running
    bool tupOn_ = false;
    float tupDiv_ = 1.0f;                        // the display-to-render ratio (`rs`)
    bool tupFeat_ = false;                       // the device feature the motion target's blend needs is enabled
    bool tupTold_ = false;
    VkImage tupOut_ = VK_NULL_HANDLE; VkDeviceMemory tupOutMem_ = VK_NULL_HANDLE; VkImageView tupOutView_ = VK_NULL_HANDLE;
    // the depth as the SDK takes it: a plain R32_SFLOAT image, `depth_`'s depth drawn into it (`depthcopy.frag`;
    // FidelityFX's Vulkan backend views a depth input as D32_SFLOAT and its barriers name the depth aspect alone,
    // neither legal on the depth+stencil image the mirror pass needs, and no copy converts between the two)
    VkImage tupDepth_ = VK_NULL_HANDLE; VkDeviceMemory tupDepthMem_ = VK_NULL_HANDLE; VkImageView tupDepthView_ = VK_NULL_HANDLE;
    VkRenderPass tdPass_ = VK_NULL_HANDLE; VkFramebuffer tdFb_ = VK_NULL_HANDLE;
    VkPipeline tdPipe_ = VK_NULL_HANDLE; VkPipelineLayout tdPlo_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tdDsl_ = VK_NULL_HANDLE; VkDescriptorPool tdPool_ = VK_NULL_HANDLE;
    VkDescriptorSet tdDs_ = VK_NULL_HANDLE; VkSampler tdSampler_ = VK_NULL_HANDLE;
    bool makeTupDepth();
    void killTupDepth();
    int jitterIndex_ = 0;
    float jitterPx_[2] = {};                     // this frame's, in render pixels
    long upFrames_ = 0;
    omk::vkup::DlssNeeds dlssNeeds_;             // what Streamline asked of the instance and device
    bool dlssPrepared_ = false, dlssFeat_ = false;
    uint32_t dlssQueueStart_ = 0;
    bool makeTemporalUpscaler();
    void upscale();
    void motionStats();
    float curMain_[16] = {}, prevMain_[16] = {};
    bool havePrev_ = false;
    float frameEye_[3] = {}, prevEye_[3] = {}, prevFwd_[3] = {};
    float mainS_[3] = {}, mainU_[3] = {}, mainF_[3] = {};
    bool resetOwed_ = true;            // a camera cut, a new area, a resize: no history
    std::map<const float*, std::vector<float>> curAffines_, prevAffines_;
    static constexpr int kFgMaxSlots = 3;
    int fgSlots_ = 2;                  // the frames kept (3 for the probe's N-2/N pair)
    VkImage fgColImg_[kFgMaxSlots] = {}, fgAuxImg_[kFgMaxSlots] = {}, fgHeadImg_[2] = {}, fgMidImg_ = VK_NULL_HANDLE;
    VkDeviceMemory fgColMem_[kFgMaxSlots] = {}, fgAuxMem_[kFgMaxSlots] = {}, fgHeadMem_[2] = {}, fgMidMem_ = VK_NULL_HANDLE;
    VkImageView fgColView_[kFgMaxSlots] = {}, fgAuxView_[kFgMaxSlots] = {}, fgHeadView_[2] = {}, fgMidView_ = VK_NULL_HANDLE;
    VkFramebuffer fgCapFbuf_[kFgMaxSlots] = {}, fgMidFbuf_ = VK_NULL_HANDLE, fgSplatFbuf_ = VK_NULL_HANDLE;
    VkRenderPass fgCapPass_ = VK_NULL_HANDLE, fgSplatPass_ = VK_NULL_HANDLE, fgResolvePass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout fgDsl_ = VK_NULL_HANDLE; VkDescriptorPool fgPool_ = VK_NULL_HANDLE;
    VkDescriptorSet fgCapDs_ = VK_NULL_HANDLE, fgIntDs_ = VK_NULL_HANDLE, fgMidDs_ = VK_NULL_HANDLE;
    VkPipelineLayout fgPlo_ = VK_NULL_HANDLE;
    VkPipeline fgCapPipe_ = VK_NULL_HANDLE, fgSplatPipe_ = VK_NULL_HANDLE, fgResolvePipe_ = VK_NULL_HANDLE;
    bool fgCut_[kFgMaxSlots] = {};     // the frame was a camera cut (no motion to its predecessor)
    int fgPicW_[kFgMaxSlots] = {}, fgPicH_[kFgMaxSlots] = {};
    long fgSeq_ = 0, fgShownSeq_ = 0;  // frames captured; the newest a present has used
    bool fgHeadsReady_ = false;
    // the PACING (the interval between presents; the real frame deferred to half of it) and the counters
    double fgLastCall_ = 0.0, fgInterval_ = 0.0, fgLatSum_ = 0.0, fgLateSum_ = 0.0;
    double fgCallAt_ = 0.0, fgPresentDone_ = 0.0, fgWork_ = 0.0;   // the display's room (`fgMidFrame`)
    bool fgRoom_ = false, fgAlways_ = false;
    long fgNoRoom_ = 0, fgOnTime_ = 0;
    long fgReal_ = 0, fgMids_ = 0, fgCuts_ = 0, fgStale_ = 0, fgCpu_ = 0, fgDeferred_ = 0, fgHurried_ = 0;
    int fgRefresh_ = 0;
    std::thread fgThread_;
    std::mutex fgMu_;
    std::condition_variable fgCv_;
    bool fgPending_ = false, fgHurry_ = false, fgQuit_ = false;
    double fgDue_ = 0.0, fgDeferDue_ = 0.0;
    uint32_t fgPendIdx_ = 0;           // the swapchain image whose present is deferred
    const char* fgProbe_ = nullptr;    // `OMK_FRAMEGEN_PROBE`: the files' prefix
    bool fgStart();
    bool makeFg();
    void killFg();
    void fgCapture();
    bool fgMidFrame();
    void fgCpuFrame();
    void fgInterpolate(int n, int p, float k);
    bool fgRead(VkImage img, std::vector<unsigned char>& out);
    void fgProbeRun(int n, int p, int m);
    bool fgPresentReal(bool mid);
    void fgSettle();
    void fgThreadMain();
    void fgTick();
    void fgReport();
    static double nowSec() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // VOLUMETRIC LAMP LIGHT (`todo/enhancements.md` 24; off unless asked): the half-size march of
    // the frame's lamps and the composite over `colour_`, after SSAO and before the post chain.
    bool volWant_ = false, volOn_ = false, volNoBuf_ = false;
    VkImage volImg_ = VK_NULL_HANDLE; VkDeviceMemory volMem_ = VK_NULL_HANDLE; VkImageView volView_ = VK_NULL_HANDLE;
    int volW_ = 0, volH_ = 0;
    VkRenderPass volPass_ = VK_NULL_HANDLE, volMixPass_ = VK_NULL_HANDLE;
    VkFramebuffer volFbuf_ = VK_NULL_HANDLE, volMixFbuf_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout volDsl_ = VK_NULL_HANDLE; VkDescriptorPool volPool_ = VK_NULL_HANDLE;
    VkDescriptorSet volDs_ = VK_NULL_HANDLE; VkSampler volSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout volPlo_ = VK_NULL_HANDLE;
    VkPipeline volPipe_ = VK_NULL_HANDLE, volMixPipe_ = VK_NULL_HANDLE;
    VkBuffer volUbo_ = VK_NULL_HANDLE; VkDeviceMemory volUboMem_ = VK_NULL_HANDLE; void* volUboPtr_ = nullptr;
    int volCount_ = 0;                 // this frame's lamps
    long volFrame_ = 0;
    float volEye_[3] = {}, volS_[3] = {}, volU_[3] = {}, volF_[3] = {};
    float volTanH_ = 1.0f, volTanV_ = 1.0f;
    omk::postfx::VolTune volTune_;     // density, core, glow (`OMK_VOL_TUNE`)
    bool volTold_ = false;
    float volSig_ = -1.0f;             // the lamps and camera last said (`vol.lamp` events)
    bool makeVol();
    void killVol();
    void runVol();
    void writeSoftSet();
    VkRenderPass   reflPass_ = VK_NULL_HANDLE;   // the water reflection's, under the AO (its depth not stored)
    VkFramebuffer  fbuf_ = VK_NULL_HANDLE;
    VkBuffer       readBuf_ = VK_NULL_HANDLE; VkDeviceMemory readMem_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool      dpool_ = VK_NULL_HANDLE;
    VkPipelineLayout      plo_ = VK_NULL_HANDLE;
    VkPipeline            pipe_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline            pipeTiled_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};   // the tiled module's
    // an OPAQUE CUTOUT draw's: its depth written by the shader, so a discarded
    // keyed texel writes none (`scene.frag` OMK_LATE_DEPTH) - plain and tiled
    VkPipeline            pipeCut_ = VK_NULL_HANDLE, pipeTiledCut_ = VK_NULL_HANDLE;
    // THE FLOORS' REFLECTION at ONE sample when the frame has MSAA: its own
    // render pass and the scene pipelines again at 1 sample (a pipeline's
    // count is fixed) - the reflection's per-fragment lights ran four times a
    // pixel for an image the relief blurs anyway. The water's keeps the MSAA.
    VkRenderPass          reflPass1_ = VK_NULL_HANDLE;
    VkPipeline            pipe1_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipeline            pipeTiled1_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool                  tilesNow_ = false;   // this frame's tiles are on: the tiled pipelines
    // The mirror pass's three extra states. `pipeStencil_` marks the mirror's
    // pixels, `pipeDepthReset_` clears depth inside that mark, and `pipeRefl_`
    // is `pipe_` again with the stencil TEST on so the reflection lands only
    // where the mirror is.
    VkPipeline            pipeStencil_ = VK_NULL_HANDLE;
    VkPipeline            pipeDepthReset_ = VK_NULL_HANDLE;
    VkPipeline            pipeRefl_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkSampler             sampler_ = VK_NULL_HANDLE;

    // ---- the MAPPED shadow, `todo/enhancements.md` 6 (an ENHANCEMENT) -----
    VkImage         shImg_ = VK_NULL_HANDLE;  VkDeviceMemory shMem_ = VK_NULL_HANDLE;
    VkImageView     shView_ = VK_NULL_HANDLE; VkSampler      shSampler_ = VK_NULL_HANDLE;
    VkFormat        shFmt_ = VK_FORMAT_UNDEFINED;
    VkRenderPass    shPass_ = VK_NULL_HANDLE;  VkFramebuffer shFbuf_ = VK_NULL_HANDLE;
    // one depth view and framebuffer per shadow layer; shFbuf_ is layer 0's
    VkImageView     shLayerView_[3] = {};      VkFramebuffer shLayerFbuf_[3] = {};
    VkPipeline      shPipe_ = VK_NULL_HANDLE;  VkPipelineLayout shPlo_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl1_ = VK_NULL_HANDLE;
    VkDescriptorSet       ds1_  = VK_NULL_HANDLE;
    VkDescriptorSet       ds1Refl_ = VK_NULL_HANDLE;   // ...the water reflection's: its own tiles
    omk::LightTiles       tilesRefl_;                  // ...binned for the reflected camera
    VkBuffer        shUbo_ = VK_NULL_HANDLE;   VkDeviceMemory shUboMem_ = VK_NULL_HANDLE;
    void*           shUboPtr_ = nullptr;
    VkBuffer        litUbo_ = VK_NULL_HANDLE;  VkDeviceMemory litUboMem_ = VK_NULL_HANDLE;
    void*           litUboPtr_ = nullptr;
    // THE TILED POINT LIGHTS (`o3de/lighttiles.h`), set 1's bindings 3 and 4:
    // every point light of the frame, then the tiles' lists - host-visible,
    // written each frame as the lights UBO is
    static constexpr VkDeviceSize kPointsBytes = omk::kMaxPoints * sizeof(GpuLightStd140);
    static constexpr VkDeviceSize kTileWordsMax = 1u << 22;   // 16 MB: 64 x 40-odd tiles x 16 slices x 48, and room
    VkBuffer        pointsBuf_ = VK_NULL_HANDLE; VkDeviceMemory pointsMem_ = VK_NULL_HANDLE;
    void*           pointsPtr_ = nullptr;
    VkBuffer        tilesBuf_ = VK_NULL_HANDLE;  VkDeviceMemory tilesMem_ = VK_NULL_HANDLE;
    void*           tilesPtr_ = nullptr;
    bool            emitters_ = false;   // `setEmitterLights`: the two buffers above made at init
    omk::LightTiles tiles_;
    bool            tilesTold_ = false, tilesOverTold_ = false;
    // The command buffer is opened by whichever of `shadowPass` and `begin`
    // runs first, and only once - the depth pass has to be recorded BEFORE the
    // frame's own render pass begins.
    bool            cbStarted_ = false;
    // THE GPU'S OWN TIME per pass (`OMK_GPU_TIMES=1`, an instrument - the
    // reader's 4K slowdown was GPU-bound and the CPU's phases could not say
    // where): a timestamp at each pass boundary of the frame's one command
    // buffer, OUTSIDE every render pass, read after the frame's fence (no
    // extra wait); the means per pass every 60 frames, as `gpu.time` too.
    VkQueryPool     tsPool_ = VK_NULL_HANDLE;
    static constexpr uint32_t kStamps = 8;
    uint32_t        tsCount_ = 0;
    const char*     tsLabel_[kStamps] = {};
    double          tsSum_[kStamps] = {};      // ms, by the label's slot in `kStampNames`
    int             tsFrames_ = 0;
    void stamp(const char* label) {
        if (!tsPool_ || tsCount_ >= kStamps) return;
        if (tsCount_ == 0) vkCmdResetQueryPool(cb_, tsPool_, 0, kStamps);
        vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, tsPool_, tsCount_);
        tsLabel_[tsCount_++] = label;
    }
    void readStamps();
    bool            shadowLive_ = false;   // a depth pass was recorded this frame
    int             litCount_ = 0;         // lights uploaded for this frame
    float           shimmerClock_ = 0.0f;  // the View's, held for the uniform
    float           rain_ = 0.0f;          // the View's rain, for the wet floors' ripples
    bool            enhOff_ = false;       // `View::enhOff`: the original's look shows - ours left out
    bool            dither_ = true;        // the engine's DITHERENABLE, see surface.h
    int             colourDepth_ = 16;           // the View's colour depth: 32 = no 565 in the present

    struct Tex {
        VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE; VkDescriptorSet ds = VK_NULL_HANDLE;
        int rt = -1;      // HYBRID RAY TRACING: its place in the bindless texture table
        // a TEXTURE PACK replacement's factor over the original (texpack.h)
        float su = 1.0f, sv = 1.0f;
        bool packed = false;
        // its LIGHT MAP, the pack's `<key>_e.png` (binding 1; `lightBlack_`
        // when there is none)
        VkImage emImg = VK_NULL_HANDLE; VkDeviceMemory emMem = VK_NULL_HANDLE;
        VkImageView emView = VK_NULL_HANDLE;
        // its NORMAL MAP, the pack's `<key>_n.png` (binding 3, the relief's;
        // `normalFlat_` when there is none)
        VkImage nmImg = VK_NULL_HANDLE; VkDeviceMemory nmMem = VK_NULL_HANDLE;
        VkImageView nmView = VK_NULL_HANDLE;
    };
    std::vector<Tex> tex_;                // per pool slot: handles `uploaded_` owns
    Tex              white_;              // a material with no texture
    Tex              lightBlack_;         // the light map of a texture that has none
    Tex              normalFlat_;         // the normal map of one that has none: flat, ALPHA 0 - "none"
    // the textures on the device, by their pixel storage (`setTextures`); the
    // entry holds that storage so its address is not reused while cached
    // `pinned`: a preloaded set's (`setPreloadTextures`) - kept while listed
    struct Uploaded { Tex t; omk::PixelBuffer idx, pal; bool used = false; bool pinned = false; };
    std::vector<omk::Texture> preQueue_;   // the preload's textures not yet on the device
    std::map<std::tuple<const std::uint8_t*, const std::uint8_t*, int, int>, Uploaded> uploaded_;

    std::map<const omk::Geometry*, std::pair<VkBuffer, VkDeviceMemory>> vbo_;
    // THE POSE BUFFER (set 2): host-visible, written from the start at every
    // `begin` (the frame before it was waited for), one block a posed body -
    // a header (lights, from-black, the base grey, the affines' count), the
    // lights (two vec4 each, `kPoseLights` places), then three vec4 an affine
    static constexpr int kPoseLights = 16, kPoseMaxMeshes = 256;
    // 2 MB: the Anekbah street walk's busiest frame writes 288 KB (25 staged
    // bodies and the crowd, measured 2026-10-20); a frame past it poses the
    // rest on the CPU (`writePose` refuses)
    static constexpr VkDeviceSize kPoseBytes = 2u << 20;
    static constexpr VkDeviceSize kPoseRange = (1 + 2 * kPoseLights + 3 * kPoseMaxMeshes + 1) * 16;   // (+ the body light's tint vec4)
    // ...and with motion vectors the previous affines follow the current ones and the tint
    static constexpr VkDeviceSize kPoseRangeMv = (1 + 2 * kPoseLights + 6 * kPoseMaxMeshes + 1) * 16;
    VkDeviceSize poseRange_ = kPoseRange;
    VkDescriptorSetLayout dsl2_ = VK_NULL_HANDLE;
    VkBuffer poseBuf_ = VK_NULL_HANDLE;
    VkDeviceMemory poseMem_ = VK_NULL_HANDLE;
    void* posePtr_ = nullptr;
    VkDescriptorSet poseDs_ = VK_NULL_HANDLE;
    VkDeviceSize poseUsed_ = 0, poseAlign_ = 256;
    std::map<std::pair<const float*, const float*>, uint32_t> poseCache_;   // a body's block this frame
    // `cpuPose`'s corners, one per BODY: keyed by the rest geometry AND the
    // body's ordinal among this frame's bodies sharing it. Keyed by the
    // geometry alone, every passer-by of one model shared a single posed copy
    // and only the last one posed cast a shadow (or drew, past its room).
    std::map<std::pair<const omk::Geometry*, int>, omk::Geometry> cpuPosed_;
    std::map<std::pair<const float*, const omk::Geometry*>, int> cpuPoseSlot_;   // (pose, geometry) -> ordinal, this frame
    std::map<const omk::Geometry*, int> cpuPoseCount_;           // bodies per geometry, this frame

    // SSAO (`todo/enhancements.md` 15). Asked for before init; LIVE when the
    // target allows it (single-sampled, a sampleable depth format). The main
    // pass then keeps its depth, `end()` runs the two passes over it.
    bool wantSsao_ = false, ssaoOn_ = false;
    VkImageView depthSampleView_ = VK_NULL_HANDLE;   // depth aspect alone
    VkImage aoImg_ = VK_NULL_HANDLE; VkDeviceMemory aoMem_ = VK_NULL_HANDLE;
    VkImageView aoView_ = VK_NULL_HANDLE;
    VkRenderPass aoPass_ = VK_NULL_HANDLE, aoApplyPass_ = VK_NULL_HANDLE;
    VkFramebuffer aoFbuf_ = VK_NULL_HANDLE, aoApplyFbuf_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout aoDsl_ = VK_NULL_HANDLE;
    VkDescriptorPool aoPool_ = VK_NULL_HANDLE; VkDescriptorSet aoDs_ = VK_NULL_HANDLE;
    VkSampler aoSampler_ = VK_NULL_HANDLE; VkPipelineLayout aoPlo_ = VK_NULL_HANDLE;
    VkPipeline aoPipe_ = VK_NULL_HANDLE, aoBlurPipe_ = VK_NULL_HANDLE;
    float viewTanH_ = 1.0f, viewTanV_ = 1.0f;        // the last pushView's
    float aoTanH_ = 1.0f, aoTanV_ = 1.0f;            // the frame's own (begin)
    int aoVw_ = 0, aoVh_ = 0;
    bool makeSsao();
    void killSsao();
    void runSsao();

    // ---- HYBRID RAY TRACING (`todo/enhancements.md` 19, 26; off unless asked) ----
    // VK_KHR_ray_query over VK_KHR_acceleration_structure, probed and enabled at the device
    // (`pickDevice`), made in `rtInit`; the methods and their notes are under "HYBRID RAY TRACING"
    // below. What goes into a BLAS is `o3de/rtscene.h`'s, shared with Direct3D 12.
    struct RtBuf {
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
        void* p = nullptr; VkDeviceSize cap = 0; VkDeviceAddress va = 0;
    };
    struct RtBlas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE; RtBuf asBuf; VkDeviceAddress asAddr = 0;   // the BLAS
        RtBuf pos;                                  // corners, float3
        RtBuf rec;                                  // 16 words a triangle
        int recSlot = -1;                           // the records' place in the bindless table
        std::uint32_t nOpaque = 0, nCut = 0;        // opaque triangles first, then the cutout ones
        std::uint64_t rev = ~0ull;                  // a set's: the geometry's revision it was made from
        std::vector<std::int32_t> triOf;            // a set's: corner / 3 -> its record, -1 none
        std::map<std::size_t, std::pair<std::size_t, std::uint32_t>> texAt;   // draw start -> count, texture written
        std::uint64_t texGen = 0;
        long seen = 0;                              // the frame it was last drawn
        bool built = false;
    };
    using RtTri = omk::rt::Tri;
    using RtRun = omk::rt::Run;
    using RtBodyNote = omk::rt::BodyNote;
    static constexpr int kRtTexSlots = 4096, kRtRecSlots = 2048;
    bool rtWant_ = false, rtFeat_ = false, rtOn_ = false, rtCountFeat_ = false;
    bool rtWantRefl_ = false, rtReflOn_ = false;   // RAY-TRACED REFLECTIONS (row 26)
    uint32_t instApi_ = VK_API_VERSION_1_1;        // the instance's API version
    VkDeviceSize rtScratchAlign_ = 256;
    PFN_vkCreateAccelerationStructureKHR rtCreateAs_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR rtDestroyAs_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR rtCmdBuild_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR rtBuildSizes_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR rtAsAddr_ = nullptr;
    // set A (the TLAS, the instance table, `RtFrame`, the sampler, the rays' tally), B (the records'
    // buffers) and C (the textures) - the last two bindless; `dslEmpty_` fills set 3 when soft
    // particles do not
    VkDescriptorSetLayout rtDslA_ = VK_NULL_HANDLE, rtDslB_ = VK_NULL_HANDLE, rtDslC_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslEmpty_ = VK_NULL_HANDLE;
    VkDescriptorPool rtPool_ = VK_NULL_HANDLE;
    VkDescriptorSet rtDsA_ = VK_NULL_HANDLE, rtDsB_ = VK_NULL_HANDLE, rtDsC_ = VK_NULL_HANDLE;
    VkSampler rtSampler_ = VK_NULL_HANDLE;
    std::vector<int> rtTexFree_, rtRecFree_;
    int rtTexNext_ = 0, rtRecNext_ = 0;
    VkAccelerationStructureKHR rtTlas_ = VK_NULL_HANDLE;
    VkDeviceSize rtTlasScratch_ = 0;
    RtBuf rtTlasBuf_, rtInstDesc_, rtInstTab_, rtScratch_, rtFrameBuf_, rtCountBuf_;
    std::vector<VkAccelerationStructureKHR> rtDeadAs_;   // freed at the next `begin`
    VkCommandBuffer rtCb_ = VK_NULL_HANDLE;      // the builds', submitted just before the frame's
    bool rtRecorded_ = false;
    std::map<const omk::Geometry*, RtBlas> rtSet_;
    RtBlas rtBodyAll_;                           // EVERY body of the frame, one BLAS
    omk::rt::BodyNotes rtNotes_;
    long rtFloorDraws_ = 0;                      // ...the ground draws this frame, for the log
    bool rtBodyDraw_ = false;                    // the draw going to `submitOne` is a body
    bool rtCounting_ = false;                    // this frame draws `pipeCount_` (one in 60)
    long rtCounted_ = 0;                         // ...frames so drawn
    VkPipeline pipeCount_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::uint64_t texGen_ = 1;                   // bumped when a texture's slot goes
    long rtFrame_ = 0;
    float rtEye_[3] = {}, rtS_[3] = {}, rtU_[3] = {}, rtF_[3] = {};
    int rtInstances_ = 0, rtSets_ = 0, rtBodies_ = 0, rtBodyCount_ = 0, rtToldInst_ = -1, rtTold_ = 0;
    long rtSetBuilds_ = 0, rtBodyBuilds_ = 0, rtBodyRefits_ = 0, rtBodyRebuilt_ = 0;
    std::uint64_t rtBodySig_ = 0;
    long long rtSetTris_ = 0, rtBodyTris_ = 0, rtAoRays_ = 0;
    std::uint32_t rtCounts_[4] = {};
    double rtCpuMs_ = 0.0;
    bool rtInit();
    void rtKill();
    bool rtBuffer(RtBuf& r, VkDeviceSize bytes, VkBufferUsageFlags use, bool host, const char* tag);
    void rtRetire(RtBuf& r);
    void rtFree(RtBuf& r);
    bool rtMapped(RtBuf& r, VkDeviceSize bytes, VkBufferUsageFlags use);
    void rtWriteTex(int slot, VkImageView view);
    void rtWriteRec(int slot, VkBuffer b);
    int rtAllocTex(VkImageView view);
    void rtFreeTex(int slot);
    std::uint32_t rtTexWord(std::size_t slot) const;
    void rtBind(const omk::View& view);
    bool rtFill(RtBlas& b, const std::vector<RtTri>& tri);
    RtBlas* rtSetFor(const omk::Geometry* g);
    void rtNote(const omk::Draw& d);
    void rtDrop(RtBlas& b);
    void rtBuild();
    void rtAfter();

    // ---- THE POST CHAIN (`todo/enhancements.md` 22) ----------------------
    //
    // A display-size chain after the scene and SSAO, before the present pass:
    // the 3D frame (`colour_`, the supersample folded in the shader) in, one
    // display-size picture out (`postImg_`), which the present pass and the
    // CPU readback read instead of `colour_` on a frame it ran. So the 2D is
    // laid over the PROCESSED world and never bloomed, blurred or graded. With
    // nothing asked - or only depth of field on a frame that is not cinematic -
    // it does not run and the frame is byte-identical to one without it.
    // Depth of field reads the scene's depth AND the stencil mark of a
    // translucent body (`pipeSubj_`), so the depth image is kept and
    // sampleable on a frame that may take it (`dofOn_`).
    Renderer::PostProcess postReq_{};
    bool postWant_ = false;            // anything asked at all
    bool postLive_ = false;            // the chain ran on THIS frame
    bool postFailed_ = false;          // `makePost` could not: the frame without it
    bool dofOn_ = false;               // the depth and stencil can be sampled
    // HDR OUTPUT (`hdr = on`): the HDR passes (an FP16 post picture and present
    // image, the shoulder's white at the display's peak over paper white) run
    // when `hdrOn_`; they reach the window through an FP16 scRGB swapchain
    // (`hdrSwap_`: VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, 1.0 = 80 nits, the
    // blit `blit.frag -DOMK_HDR` draws), or - `OMK_HDR_FORCE` on a surface that
    // offers no scRGB, a test of the passes - through a plain blit that clips
    // the picture to SDR.
    bool hdrOn_ = false, hdrSwap_ = false, hdrDecided_ = false, hdrMeta_ = false;
    int hdrKnown_ = -1;
    bool hdrToldSwap_ = false;
    float hdrPeak_ = 1000.0f;          // the display's peak luminance, nits
    static constexpr float kPaperWhite = omk::postfx::kPaperWhite;   // nits: where the SDR picture's white goes
    VkFormat presFmt_ = VK_FORMAT_R8G8B8A8_UNORM;   // the present image's, the overlay's, the chain's
    std::vector<VkImageView> swapViews_; std::vector<VkFramebuffer> swapFbufs_;
    VkRenderPass hdrPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blitDsl_ = VK_NULL_HANDLE; VkDescriptorPool blitPool_ = VK_NULL_HANDLE;
    VkDescriptorSet blitDs_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // [0] `colour_`, [1] `presImg_`
    VkSampler blitSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout blitPlo_ = VK_NULL_HANDLE; VkPipeline blitPipe_ = VK_NULL_HANDLE;
    bool makeHdrBlit();
    void killHdrBlit();
    bool presentImageHdr(VkImage src, int sw, int sh, uint32_t idx, int x0, int y0, int dW, int dH);
    omk::postfx::Focus foc_;           // the frame's picture rectangle and depth of field's focus
    bool curBody_ = false;             // the draw being submitted is a posed body's
    VkImageView stencilSampleView_ = VK_NULL_HANDLE;   // the depth image's stencil plane (a hologram's mark)
    VkPipeline pipeSubj_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};   // the blended pipelines' stencil-marking twins
    VkImage postImg_ = VK_NULL_HANDLE; VkDeviceMemory postMem_ = VK_NULL_HANDLE; VkImageView postView_ = VK_NULL_HANDLE;
    VkImage bloomImg_[omk::postfx::kBloomLevels] = {}; VkDeviceMemory bloomMem_[omk::postfx::kBloomLevels] = {};
    VkImageView bloomView_[omk::postfx::kBloomLevels] = {}; VkFramebuffer bloomFbuf_[omk::postfx::kBloomLevels] = {};
    int bloomW_[omk::postfx::kBloomLevels] = {}, bloomH_[omk::postfx::kBloomLevels] = {};
    VkImage dummyDepth_ = VK_NULL_HANDLE, dummyStencil_ = VK_NULL_HANDLE;   // bound where no depth of field samples
    VkDeviceMemory dummyDepthMem_ = VK_NULL_HANDLE, dummyStencilMem_ = VK_NULL_HANDLE;
    VkImageView dummyDepthView_ = VK_NULL_HANDLE, dummyStencilView_ = VK_NULL_HANDLE;
    VkRenderPass bloomDownPass_ = VK_NULL_HANDLE, bloomUpPass_ = VK_NULL_HANDLE, postPass_ = VK_NULL_HANDLE;
    VkFramebuffer postFbuf_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bloomDsl_ = VK_NULL_HANDLE, postDsl_ = VK_NULL_HANDLE;
    VkDescriptorPool postPool_ = VK_NULL_HANDLE;
    VkDescriptorSet bloomSrcDs_ = VK_NULL_HANDLE, bloomLvlDs_[omk::postfx::kBloomLevels] = {}, postDs_ = VK_NULL_HANDLE;
    VkDescriptorSet presDsPost_ = VK_NULL_HANDLE;      // the present pass reading `postImg_`
    VkSampler postLin_ = VK_NULL_HANDLE, postNear_ = VK_NULL_HANDLE;
    VkPipelineLayout bloomPlo_ = VK_NULL_HANDLE, postPlo_ = VK_NULL_HANDLE;
    VkPipeline bloomDownPipe_ = VK_NULL_HANDLE, bloomUpPipe_ = VK_NULL_HANDLE, postPipe_ = VK_NULL_HANDLE;
    bool makePost();
    void killPost();
    void runPost();
    std::uint64_t cpuPoseRev_ = 0;
    bool writePose(const omk::Draw& d, uint32_t& off);
    omk::Draw cpuPose(const omk::Draw& d);
    void submitOne(const omk::Draw& d, bool posed, uint32_t poseOff);
    std::map<const omk::Geometry*, std::size_t> vboN_;
    // ...and how many corners its buffer HOLDS, which a geometry that changes
    // size every frame (the particles) keeps above what it uses, so it refills
    // in place instead of freeing and allocating device memory every frame
    // (the same cost the GLES ring answers on the Vita)
    std::map<const omk::Geometry*, std::size_t> vboCap_;
    std::map<const omk::Geometry*, std::uint64_t> vboRev_;
    // THE DEPTH TIE, resolved at SUBMIT (raster.cpp `kDepthTie` has the
    // reading). Two faces on the same four vertices in opposite winding - the
    // two SIDES of a shop sign, 18 pairs in Anekbah - and the engine's strict
    // test on a quantised z-buffer keeps the FIRST drawn on every pixel. The
    // engine's software back-face cull keeps only one side of such a pair,
    // and the tie's key carries the face's side - `depthtie.cpp` faceClass -
    // so what is left here is coincident faces of ONE winding. The software rasterizer rejects a
    // depth within 2^-16 of the buffer's; a GPU compare cannot read the
    // buffer, a quantised `gl_FragDepth` still straddles its grid on ~1% of
    // pixels, and a per-draw depth bias through the non-linear projection is
    // hundreds of inches at street distance. So the tie is settled where it
    // is decidable exactly: a face whose position set an earlier DEPTH-WRITING
    // face of the same geometry already claimed, in draw order, can never win
    // a pixel from it, and is DEGENERATED in the vertex buffer (its three
    // vertices collapsed to one) so it rasterises nothing. Faces are walked in
    // the order the draws arrive, once per geometry revision; a quad is the
    // consecutive pair `buildGeometry` emits, (0,1,2)(0,2,3).
    // The DECISION lives in `o3de/depthtie.h` (todo/optimization.md
    // step 3), so it can be tested without a GPU; this backend keeps the vertex
    // buffer half below.
    std::map<const omk::Geometry*, omk::DepthTie> tie_;
    void resolveTies(const omk::Draw& d);
    // THE TIE DECIDED ONCE (`bakeDepthTie`, todo/optimization.md step 29): per
    // geometry, one byte a corner, 1 on a loser's. A baked geometry skips
    // `resolveTies` and its losers are MARKED in every upload (the phase moved
    // down by `kTieLoserPhase`), which `scene.vert` draws two 16-bit depth
    // steps back instead of collapsing them.
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
public:
    bool bakeDepthTie(const omk::Geometry* g, std::span<const omk::Draw> order) override;
private:

    Push             push_{};
    // the fog, held between begin() and each submit()
    bool             fog_ = false;
    float            fogStart_ = 0.0f, fogEnd_ = 0.0f;
    bool             mistOn_ = false;   // the frame has the mist (`View::mist`): the fogless batches marked for it
    float            fogColour_[3] = {0.0f, 0.0f, 0.0f};
    float            clearColour_[3] = {0.0f, 0.0f, 0.0f};   // View::clearColour
    omk::RasterStats st_;
    omk::Surface     fb_{1, 1, 0};
    bool             recording_ = false;
    bool             dirty_ = false;    // a new frame is waiting in readBuf_
    bool             copyOwed_ = false; // ...still to be copied there (`readback`)
    VkPipeline       forcePipeline_ = VK_NULL_HANDLE;  // the mirror pass's override
    bool             flipX_ = false;   // the current view's screen-X flip (mirror pass)
    bool             reflStencil_ = false;             // draw through pipeRefl_

    // ---- DIRECT PRESENTATION (optional; nothing here runs offscreen).
    //
    // The surface comes from the window layer, which is the only thing that
    // knows what a window is - `PORTING` A8 rule 2 keeps SDL in one file and
    // Vulkan in this one, so the two meet through opaque handles and a list of
    // extension names rather than by including each other's headers.
    std::vector<std::string> wantInstExts_;
    VkSurfaceKHR   surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swap_    = VK_NULL_HANDLE;
    VkFormat       swapFmt_ = VK_FORMAT_UNDEFINED;
    VkExtent2D     swapExt_{};
    std::vector<VkImage> swapImgs_;
    VkSemaphore    acquired_ = VK_NULL_HANDLE;
    VkSemaphore    drawn_    = VK_NULL_HANDLE;
    VkBuffer       upBuf_ = VK_NULL_HANDLE; VkDeviceMemory upMem_ = VK_NULL_HANDLE;
    VkCommandBuffer pcb_ = VK_NULL_HANDLE;
    VkFence        pfence_ = VK_NULL_HANDLE;
    uint32_t       presentFam_ = 0;

    // ---- THE PRESENT PASS (todo/optimization.md step 4b) ----------------------
    // `colour_` dithered to 565 and expanded back to RGBA8 at the letterbox's
    // offset into `presImg_`, which the swapchain blit then takes - the bytes
    // `readback` + `presentSurface` produce for a frame nothing is drawn over,
    // without either. Made on first use.
    VkImage          presImg_ = VK_NULL_HANDLE;  VkDeviceMemory presMem_ = VK_NULL_HANDLE;
    VkImageView      presView_ = VK_NULL_HANDLE;
    VkRenderPass     presPass_ = VK_NULL_HANDLE; VkFramebuffer presFbuf_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout presDsl_ = VK_NULL_HANDLE;
    VkDescriptorPool presPool_ = VK_NULL_HANDLE; VkDescriptorSet presDs_ = VK_NULL_HANDLE;
    VkPipelineLayout presPlo_ = VK_NULL_HANDLE;  VkPipeline presPipe_ = VK_NULL_HANDLE;
    VkSampler        presSampler_ = VK_NULL_HANDLE;
    VkBuffer         presRead_ = VK_NULL_HANDLE; VkDeviceMemory presReadMem_ = VK_NULL_HANDLE;
    bool makePresentPass();
    bool runPresentPass(int vy, int vh, bool mid = false, bool raw = false);   // `mid`: the in-between picture (frame generation); `raw`: the 8-bit value (the remaster's `readback`)
    bool presentImage(VkImage src, int sw, int sh, bool defer = false);   // `defer`: its present is the pacing thread's
    // a 2D colour image of the given format (device-local) with its view; and
    // the reverse
    bool makeImage2d(VkFormat f, int w, int h, VkImageUsageFlags use, VkImage& img, VkDeviceMemory& mem,
                     VkImageView& view, const char* label);
    void freeImage2d(VkImage& img, VkDeviceMemory& mem, VkImageView& view);
    VkShaderModule shaderModule(const uint32_t* code, size_t bytes);
    // ---- THE OVERLAY at colour depth 32 (`overlay.frag`, `todo/enhancements.md` 21) ----
    // C (the 565 words, R16_UINT) and M (R8_UNORM) up each frame, blended over
    // the present pass's full-precision world in `presImg_` as `C + world * M`
    // with the colour fade - the Direct3D 12 backend's `presentOverlay`, the
    // GLES window's before it. No readback, no 565 world under the HUD.
    VkImage ovPic_ = VK_NULL_HANDLE, ovMask_ = VK_NULL_HANDLE;
    VkDeviceMemory ovPicMem_ = VK_NULL_HANDLE, ovMaskMem_ = VK_NULL_HANDLE;
    VkImageView ovPicView_ = VK_NULL_HANDLE, ovMaskView_ = VK_NULL_HANDLE;
    VkBuffer ovBuf_ = VK_NULL_HANDLE; VkDeviceMemory ovBufMem_ = VK_NULL_HANDLE;
    VkDeviceSize ovMaskOff_ = 0;
    VkRenderPass ovPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ovDsl_ = VK_NULL_HANDLE;
    VkDescriptorPool ovPool_ = VK_NULL_HANDLE; VkDescriptorSet ovDs_ = VK_NULL_HANDLE;
    VkPipelineLayout ovPlo_ = VK_NULL_HANDLE; VkPipeline ovPipe_ = VK_NULL_HANDLE;
    VkSampler ovSampler_ = VK_NULL_HANDLE;
    bool makeOverlayPass();
    void killOverlay();
    void ovDraw(const float fade[4]);
    bool copyPresentOut(std::vector<unsigned char>& rgba);

    // ---- THE UPSCALE (`todo/remaster.md` A6) -----------------------------
    // EASU's output at the OUTPUT size (`easu.frag`), which the present pass
    // reads through `presUpDs_` and sharpens (RCAS); made on first use
    VkImage          easuImg_ = VK_NULL_HANDLE;  VkDeviceMemory easuMem_ = VK_NULL_HANDLE;
    VkImageView      easuView_ = VK_NULL_HANDLE;
    VkRenderPass     easuPass_ = VK_NULL_HANDLE; VkFramebuffer easuFb_ = VK_NULL_HANDLE;
    VkDescriptorPool easuPool_ = VK_NULL_HANDLE;
    VkDescriptorSet  easuDs_ = VK_NULL_HANDLE;   // `colour_`, LINEAR: EASU's gathers
    VkDescriptorSet  presUpDs_ = VK_NULL_HANDLE; // `easuView_`: the present pass's input
    VkPipelineLayout easuPlo_ = VK_NULL_HANDLE;  VkPipeline easuPipe_ = VK_NULL_HANDLE;
    VkSampler        easuSampler_ = VK_NULL_HANDLE;
    bool makeUpscale();
    void killUpscale();

    // ---- THE REMASTER'S POST-PASS (`todo/remaster.md` step 2) -----------
    // The BLOOM from the texture pack's light maps and the city's colour
    // GRADE, after the scene's render pass and before the readback and the
    // present pass, so every way a frame leaves sees them. The scene is drawn
    // into `postSrc_` (`fbufPost_`); two quarter-size half-float images take
    // the glow (down, blur, blur); one pass writes the finished frame into
    // `colour_`. Made on first use for the target's size; `resize` drops it.
    float            bloom_ = 0.0f;
    VkImage          postSrc_ = VK_NULL_HANDLE;  VkDeviceMemory postSrcMem_ = VK_NULL_HANDLE;
    VkImageView      postSrcView_ = VK_NULL_HANDLE;
    VkImage          glowImg_[2] = {};  VkDeviceMemory glowMem_[2] = {};
    VkImageView      glowView_[2] = {};
    VkRenderPass     glowPass_ = VK_NULL_HANDLE, finalPass_ = VK_NULL_HANDLE;
    VkFramebuffer    glowFb_[2] = {};   VkFramebuffer finalFb_ = VK_NULL_HANDLE;
    VkFramebuffer    fbufPost_ = VK_NULL_HANDLE;   // `pass_` into `postSrc_`: the scene when the post runs
    bool             postNow_ = false;             // this frame's post, decided by `begin`
    bool             poolLit_ = false;             // a texture of the pool has a light map (`setTextures`)
    bool             lightMapNow_ = false;         // a draw of the frame's pass bound one: else the glow is black
    VkDescriptorSetLayout rmPostDsl_ = VK_NULL_HANDLE;
    VkDescriptorPool rmPostPool_ = VK_NULL_HANDLE;
    VkDescriptorSet  rmPostDs_[8] = {};   // down, blur H, blur V, final; AO, AO blur H, AO blur V, AO depth
    VkSampler        postSampler_ = VK_NULL_HANDLE;
    VkPipelineLayout rmPostPlo_ = VK_NULL_HANDLE;
    VkPipeline       glowPipe_ = VK_NULL_HANDLE, finalPipe_ = VK_NULL_HANDLE;
    int              glowW_ = 0, glowH_ = 0;
    // THE AMBIENT OCCLUSION: the depth's DEPTH-aspect view (`depthView_` has
    // both aspects, which a sampler may not read), two half-size R8 images
    // (AO, blur, blur), and the camera the frame's pass was drawn with
    float            ao_ = 0.0f;
    bool             aoOk_ = false;      // the depth format can be sampled
    VkImageView      depthRead_ = VK_NULL_HANDLE;
    VkImage          rmAoImg_[2] = {};  VkDeviceMemory rmAoMem_[2] = {};
    VkImageView      rmAoView_[2] = {};
    VkFramebuffer    aoFb_[2] = {};
    VkRenderPass     rmAoPass_ = VK_NULL_HANDLE;
    VkPipeline       rmAoPipe_ = VK_NULL_HANDLE;
    // ...and the half-size VIEW Z every occlusion pass reads (R32F, its own pass)
    VkImage          zImg_ = VK_NULL_HANDLE;  VkDeviceMemory zMem_ = VK_NULL_HANDLE;
    VkImageView      zView_ = VK_NULL_HANDLE;
    VkFramebuffer    zFb_ = VK_NULL_HANDLE;
    VkRenderPass     zPass_ = VK_NULL_HANDLE;
    VkPipeline       zPipe_ = VK_NULL_HANDLE;
    int              aoW_ = 0, aoH_ = 0;
    float            aoView4_[4] = {1, 1, 1, 1};   // tanH, tanV, picture w, h (render pixels)
    // the GRADE: an N*N x N RGBA8 strip, N 0 for none
    int              lutN_ = 0;
    std::vector<unsigned char> lutData_;
    bool             lutDirty_ = false;
    VkImage          lutImg_ = VK_NULL_HANDLE;  VkDeviceMemory lutMem_ = VK_NULL_HANDLE;
    VkImageView      lutView_ = VK_NULL_HANDLE;
    // ---- THE WATER REFLECTION (`todo/remaster.md` A3) ----------------------
    // A second scene target the size of the first, drawn through the camera
    // reflected in the water plane by the SAME render pass (so every pipeline
    // fits it) into its own framebuffer, then sampled by the water draws of
    // the main pass. Made on first use; `resize` drops it.
    VkImage          reflColour_ = VK_NULL_HANDLE, reflMs_ = VK_NULL_HANDLE, reflDepth_ = VK_NULL_HANDLE;
    VkDeviceMemory   reflColourMem_ = VK_NULL_HANDLE, reflMsMem_ = VK_NULL_HANDLE, reflDepthMem_ = VK_NULL_HANDLE;
    VkImageView      reflColourView_ = VK_NULL_HANDLE, reflMsView_ = VK_NULL_HANDLE, reflDepthView_ = VK_NULL_HANDLE;
    VkFramebuffer    reflFbuf_ = VK_NULL_HANDLE;
    // ...and the floors' at one sample (`reflPass1_`): the same colour image,
    // a depth of its own at half size
    VkFramebuffer    reflFbuf1_ = VK_NULL_HANDLE;
    VkImage          reflDepth1_ = VK_NULL_HANDLE;
    VkDeviceMemory   reflDepth1Mem_ = VK_NULL_HANDLE;
    VkImageView      reflDepth1View_ = VK_NULL_HANDLE;
    VkFramebuffer    passFb_ = VK_NULL_HANDLE;   // `begin`'s target when set, `fbuf_` otherwise
    bool             waterNow_ = false;
    bool             reflectingNow_ = false;   // the reflection pass: clip below `waterY_`
    // the REFLECTING FLOORS' frame (no water draws): its reflection drawn at
    // half size into the image's top-left quarter, the floors sampling it
    // there (`scene.frag` mode 3) - one image and one binding for both
    bool             reflHalf_ = false;
    bool reflHalfNow() const { return reflHalf_ && passFb_ != VK_NULL_HANDLE; }
    float            waterStrength_ = 0.0f, waterY_ = 0.0f;
    bool makeReflection();
    void killReflection();
    void bindReflection(VkDescriptorSet ds);
    bool makeRmPost();
    bool makePostAt();
    int  postFailW_ = -1, postFailH_ = -1;   // the size `makePostAt` last failed at: not retried every frame
    void killRmPost();
    void killLut();
    bool uploadLut();
    void bindPostSets();
    void recordPost(VkCommandBuffer cb);

public:
    void needExtensions(const char* const* names, unsigned n) {
        wantInstExts_.assign(names, names + n);
    }
    void* createInstanceOnly();
    bool attachSurface(unsigned long long surf);
    bool makeSwapchain(int w, int h);
    // THE FRAME'S SIZE CHANGED (options row 2, `todo/options-menu.md`): the
    // frame-sized half of `makeTarget` is rebuilt - colour, depth, the pass
    // and framebuffer, the readback - with the present pass and the upload
    // buffer, which re-make themselves lazily. The pipelines stay: their
    // viewport is dynamic and the new pass is the same description, so it is
    // render-pass COMPATIBLE. The shadow map is a fixed size and is kept.
    bool resize(int w, int h);
    bool retarget(int samples, int ss, float up, float ao);   // ...at new enhancements (OMK's menu)
    bool makeSampler();                                        // the textures' sampler, by filter_/anisoOn_
    bool retexture(int filter, int aniso) override;            // ...again, the textures dropped
    // ...and the WINDOW's: fullscreen, or any resize, changes the surface's
    // extent and the swapchain is remade at it.
    bool remakeSwapchain();
    void killSwapchain();
    bool resizing_ = false;
    bool presentDirect();
    bool presentSurface(const omk::Surface& s);
    void setViewport(const omk::View& view);
    // The present pass's three uses: present the frame (a window only), read
    // the pass's output back (the verification), and load `colour_` with a
    // picture of the caller's (the exhaustive probe). All take the letterbox
    // as the picture's first row `vy` and height `vh` in a `w_ x h_` window.
    bool presentWorld(int vy, int vh);
    bool worldPicture(int vy, int vh, std::vector<unsigned char>& rgba);
    bool presentOverlay(const omk::Surface& s, const std::uint8_t* mask, const float fade[4], int vy, int vh);
    bool windowPicture(std::vector<unsigned char>& rgba) { return copyPresentOut(rgba); }
    bool probeUpload(const unsigned char* rgba);
    void setDitherForProbe(bool on) { dither_ = on; }
};

uint32_t VulkanRenderer::memType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (memProps_.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

bool VulkanRenderer::makeBuffer(VkDeviceSize n, VkBufferUsageFlags use,
                                VkMemoryPropertyFlags props, VkBuffer& b,
                                VkDeviceMemory& m) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = n; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(dev_, &bi, nullptr, &b), "vkCreateBuffer");
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(dev_, b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, props);
    // A READBACK buffer (the GPU writes it, the CPU reads it) wants CACHED system
    // memory. `memType` takes the first type that matches, and on a discrete card
    // with Resizable BAR that is VRAM mapped over PCIe, uncached: every CPU read
    // crosses the bus. On an RX 9070 XT (Windows, 2026-10-16) that made the
    // compose-on-the-CPU path - any frame with a subtitle, a fade or the HUD over
    // the world - take 0.7 to 1.7 s; unified-memory Macs never see it. So prefer
    // HOST_CACHED and not DEVICE_LOCAL, and keep the old choice when none exists.
    if ((props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && use == VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
        const VkMemoryPropertyFlags cached = props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        int best = -1;
        for (uint32_t i = 0; i < memProps_.memoryTypeCount && best < 0; ++i) {
            const VkMemoryPropertyFlags f = memProps_.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (f & cached) == cached &&
                !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                best = static_cast<int>(i);
        }
        for (uint32_t i = 0; i < memProps_.memoryTypeCount && best < 0; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps_.memoryTypes[i].propertyFlags & cached) == cached)
                best = static_cast<int>(i);
        if (best >= 0) ai.memoryTypeIndex = static_cast<uint32_t>(best);
    }
    VKCHECK(vkAllocateMemory(dev_, &ai, nullptr, &m), "vkAllocateMemory");
    // the profiler's GPU memory (todo/debug-tools.md 5): a buffer's category
    // from what it is for
    OMK_GPU_ALLOC((use & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ? "vertex buffers"
                  : (use & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ? "uniforms"
                  : (use & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ? "staging"
                  : (use & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ? "readback" : "buffers",
                  omk::prof::gpuKey(omk::prof::kVkMemory, m), ai.allocationSize);
    VKCHECK(vkBindBufferMemory(dev_, b, m, 0), "vkBindBufferMemory");
    return true;
}

// ONE IMAGE, CHECKED (the remaster audit's bug 4, 2026-10-23): `ii`'s image,
// device-local memory allocated and bound, and - where `view` is given - a 2D
// view of all its levels with `aspect`. Or NONE of them: a step that fails
// destroys what the ones before it made, leaves every handle null and says
// so (once per `what`, which is also the GPU profiler's category). The image
// sequences were written out by hand ten times, and the two texture uploads
// checked nothing - out of device memory (the 4x pack with its light and
// normal maps) they bound memory to a null image and copied into it. A
// texture that fails now has no image and no set, and draws white.
bool VulkanRenderer::makeImage2D(const VkImageCreateInfo& ii, VkImageAspectFlags aspect, const char* what,
                                 VkImage& img, VkDeviceMemory& mem, VkImageView* view) {
    img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    if (view) *view = VK_NULL_HANDLE;
    const auto fail = [&](const char* step, VkResult r) {
        if (mem) freeDeviceMemory(mem);
        if (img) vkDestroyImage(dev_, img, nullptr);
        img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
        static std::set<std::string> told;
        if (told.insert(what).second)
            std::fprintf(stderr, "vulkan: %s %ux%u - %s failed (%d), refused%s\n", what, ii.extent.width,
                         ii.extent.height, step, static_cast<int>(r),
                         r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? ": out of device memory" : "");
        return false;
    };
    // `OMK_VK_IMAGE_FAIL=<what>`, an instrument: every image of that category
    // refused as if the device were out of memory (`verify.py: engine: vulkan
    // images`)
    static const char* refuse = omk::envStr("OMK_VK_IMAGE_FAIL");
    if (refuse && std::strcmp(refuse, what) == 0) return fail("the image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    VkResult r = vkCreateImage(dev_, &ii, nullptr, &img);
    if (r != VK_SUCCESS) { img = VK_NULL_HANDLE; return fail("the image", r); }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // a TRANSIENT attachment (never loaded nor stored: the MSAA samples, a
    // depth nothing reads) in LAZILY ALLOCATED memory where the device has
    // it - MEMORYLESS on Apple's GPUs, the samples living in tile memory
    // alone (the audit's 4K trims: 4x MSAA at 4K is ~133 MB of colour and
    // ~166 MB of depth and stencil that were real allocations)
    bool lazy = false;
    if (ii.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) &&
                (memProps_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
                ai.memoryTypeIndex = i;
                lazy = true;
                break;
            }
    r = vkAllocateMemory(dev_, &ai, nullptr, &mem);
    if (r != VK_SUCCESS) { mem = VK_NULL_HANDLE; return fail("its memory", r); }
    if (lazy) {
        lazyBytes_ += static_cast<long long>(ai.allocationSize);
        ++lazyImages_;
    }
    OMK_GPU_ALLOC(what, omk::prof::gpuKey(omk::prof::kVkMemory, mem), ai.allocationSize);
    r = vkBindImageMemory(dev_, img, mem, 0);
    if (r != VK_SUCCESS) return fail("binding its memory", r);
    if (!view) return true;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format;
    vi.subresourceRange = {aspect, 0, ii.mipLevels, 0, 1};
    r = vkCreateImageView(dev_, &vi, nullptr, view);
    if (r != VK_SUCCESS) { *view = VK_NULL_HANDLE; return fail("its view", r); }
    return true;
}

VkCommandBuffer VulkanRenderer::oneShotBegin() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev_, &ai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

void VulkanRenderer::oneShotEnd(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(dev_, pool_, 1, &cb);
}

VkCommandBuffer VulkanRenderer::uploadCb() {
    if (batchCb_ == VK_NULL_HANDLE) batchCb_ = oneShotBegin();
    return batchCb_;
}

// `bytes` of staging, 16-aligned (a block format's texel block, and the
// copy's 4): from the ring - the batch flushed first when it is full - or,
// past the ring's size, a buffer of its own freed at the flush
bool VulkanRenderer::stage(VkDeviceSize bytes, VkBuffer& buf, VkDeviceSize& off, unsigned char*& ptr) {
    const VkDeviceSize need = (bytes + 15) & ~VkDeviceSize{15};
    if (need <= kRingBytes && !ringFailed_) {
        if (ring_ == VK_NULL_HANDLE) {
            void* p = nullptr;
            if (makeBuffer(kRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           ring_, ringMem_) &&
                vkMapMemory(dev_, ringMem_, 0, kRingBytes, 0, &p) == VK_SUCCESS) {
                ringPtr_ = static_cast<unsigned char*>(p);
            } else {
                ringFailed_ = true;   // a buffer a texture, as before
                std::fprintf(stderr, "vulkan: the upload ring refused - a staging buffer a texture\n");
            }
        }
        if (ringPtr_) {
            if (ringAt_ + need > kRingBytes) flushUploads();
            buf = ring_; off = ringAt_; ptr = ringPtr_ + ringAt_;
            ringAt_ += need;
            return true;
        }
    }
    VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
    if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, b, m))
        return false;
    void* p = nullptr;
    if (vkMapMemory(dev_, m, 0, bytes, 0, &p) != VK_SUCCESS) {
        vkDestroyBuffer(dev_, b, nullptr);
        freeDeviceMemory(m);
        return false;
    }
    batchOwn_.push_back({b, m});
    buf = b; off = 0; ptr = static_cast<unsigned char*>(p);
    return true;
}

void VulkanRenderer::flushUploads() {
    if (batchCb_ != VK_NULL_HANDLE) {
        oneShotEnd(batchCb_);
        batchCb_ = VK_NULL_HANDLE;
    }
    for (auto& [b, m] : batchOwn_) {
        vkDestroyBuffer(dev_, b, nullptr);
        freeDeviceMemory(m);
    }
    batchOwn_.clear();
    ringAt_ = 0;
}

// The instance ALONE, so the window layer can make a surface from it before a
// device exists. Present support is a property of a (device, surface) pair, so
// the surface has to come first or the queue family cannot be chosen correctly
// - which is why this is split out rather than done inside `init`.
void* VulkanRenderer::createInstanceOnly() {
    if (inst_ != VK_NULL_HANDLE) return inst_;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omk";
    // DLSS asks what to add to the instance and the device BEFORE they are made (Streamline's
    // `slGetFeatureRequirements`); a refusal says why and the upscaler falls back to FSR
    if (tupKind_ == omk::vkup::kDlss && !dlssPrepared_) dlssPrepared_ = omk::vkup::dlssPrepare(dlssNeeds_);
    // HYBRID RAY TRACING needs Vulkan 1.2 (SPIR-V 1.5, buffer device address, descriptor indexing)
    app.apiVersion = rtWant_ || tupKind_ > 0 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    if (dlssPrepared_ && dlssNeeds_.uses12 && app.apiVersion < VK_API_VERSION_1_2) app.apiVersion = VK_API_VERSION_1_2;
    if (dlssPrepared_ && dlssNeeds_.uses13) app.apiVersion = VK_API_VERSION_1_3;
    instApi_ = app.apiVersion;
    // MoltenVK is a PORTABILITY driver: without this extension and flag the
    // loader reports zero devices on macOS and the failure looks like "no GPU"
    // rather than "not asked for".
    std::vector<const char*> iexts = {"VK_KHR_portability_enumeration"};
    for (const auto& e : wantInstExts_) iexts.push_back(e.c_str());
    // HDR OUTPUT needs the surface to offer more than sRGB: the colour-space
    // extension, when the loader has it
    if (postReq_.hdr) {
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne);
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, ex.data());
        for (const auto& e : ex)
            if (std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
                iexts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }
    if (dlssPrepared_) {
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne);
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, ex.data());
        for (const std::string& want : dlssNeeds_.instanceExtensions) {
            bool have = false;
            for (const auto& e : ex) have = have || want == e.extensionName;
            bool dup = false;
            for (const auto& i : iexts) dup = dup || want == i;
            if (have && !dup) iexts.push_back(want.c_str());
        }
    }
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(iexts.size());
    ci.ppEnabledExtensionNames = iexts.data();
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    if (vkCreateInstance(&ci, nullptr, &inst_) != VK_SUCCESS) {
        // Try again without portability: on a machine with a native driver the
        // extension may simply not be there, and that is not an error.
        iexts.erase(iexts.begin());
        ci.enabledExtensionCount = static_cast<uint32_t>(iexts.size());
        ci.ppEnabledExtensionNames = iexts.empty() ? nullptr : iexts.data();
        ci.flags = 0;
        if (vkCreateInstance(&ci, nullptr, &inst_) != VK_SUCCESS) {
            std::fprintf(stderr, "vulkan: vkCreateInstance failed\n");
            inst_ = VK_NULL_HANDLE;
        }
    }
    return inst_;
}

bool VulkanRenderer::attachSurface(unsigned long long surf) {
    surface_ = reinterpret_cast<VkSurfaceKHR>(surf);
    return surface_ != VK_NULL_HANDLE;
}

bool VulkanRenderer::pickDevice() {
    if (!createInstanceOnly()) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst_, &n, nullptr);
    if (!n) { std::fprintf(stderr, "vulkan: no physical device\n"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst_, &n, devs.data());
    phys_ = devs[0];
    vkGetPhysicalDeviceProperties(phys_, &props_);
    {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R8_UNORM, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                          VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        r8Ok_ = (fp.optimalTilingFeatures & need) == need;
    }
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qs(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &qn, qs.data());
    // With a surface the family must also be able to PRESENT to it. Choosing a
    // graphics family and assuming it can present is the usual way this breaks
    // on a machine where the two differ.
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i) {
        if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (surface_ != VK_NULL_HANDLE) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &ok);
            if (!ok) continue;
        }
        qfam_ = i; presentFam_ = i; found = true; break;
    }
    if (!found) {
        std::fprintf(stderr, "vulkan: no %squeue\n",
                     surface_ ? "graphics+present " : "graphics ");
        return false;
    }

    // VK_KHR_portability_subset is REQUIRED to be enabled when the device
    // exposes it - the spec says so - and MoltenVK does.
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &en, exts.data());
    std::vector<const char*> want;
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
            want.push_back("VK_KHR_portability_subset");
    if (surface_ != VK_NULL_HANDLE) want.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // ...and the display's metadata (its peak, paper white) for an HDR swapchain
    if (surface_ != VK_NULL_HANDLE && postReq_.hdr)
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME) == 0) {
                want.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
                hdrMeta_ = true;
            }

    const float pri[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_; qi.queueCount = 1; qi.pQueuePriorities = pri;
    // Anisotropic filtering is a device FEATURE, asked for at creation and
    // only when the enhancement wants it AND the device has it; the base
    // renderer enables no feature at all.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    VkPhysicalDeviceFeatures enable{};
    // FRAME GENERATION's in-between pass stores to storage images from a fragment shader, and the
    // deferred real-frame present goes out on a second queue of the family (a window's alone)
    // (and the motion target's own blend state, which the opaque and the blended pipelines set apart)
    fgFeat_ = fgWant_ && surface_ != VK_NULL_HANDLE && have.fragmentStoresAndAtomics && have.independentBlend;
    if (fgFeat_) {
        enable.fragmentStoresAndAtomics = VK_TRUE;
        enable.independentBlend = VK_TRUE;
        if (qs[qfam_].queueCount >= 2) qi.queueCount = 2;
    }
    // THE TEMPORAL UPSCALER's motion target is written beside the colour with a blend state of its own
    // (the scene pipelines' `_mv` builds): the device feature that allows it, enabled whenever asked
    tupFeat_ = tupKind_ > 0 && have.independentBlend;
    if (tupFeat_) enable.independentBlend = VK_TRUE;
    // ...and enabled whenever the device has it, asked or not - it changes
    // nothing a sampler does not ask for, and OMK's options menu may ask later
    // (`retexture`)
    if (have.samplerAnisotropy) { enable.samplerAnisotropy = VK_TRUE; anisoAvail_ = true; }
    if (aniso_ > 1 && filter_ >= 2 && have.samplerAnisotropy) {
        anisoOn_ = true;
    } else if (aniso_ > 1) {
        std::fprintf(stderr, "vulkan: anisotropy %d ignored - %s\n", aniso_,
                     filter_ < 2 ? "it needs trilinear filtering (a mip chain)"
                                 : "the device has no samplerAnisotropy feature");
    }
    // HYBRID RAY TRACING (`todo/enhancements.md` 19): VK_KHR_ray_query and the acceleration structures
    // it traces, the descriptor-indexing features the bindless tables need, buffer device address;
    // asked for only when the device has all of them, else a line says what is missing and the frames
    // are drawn without
    VkPhysicalDeviceVulkan12Features rt12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR rtAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rtRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rtFeat_ = false;
    if (rtWant_) {
        const auto has = [&](const char* name) {
            for (const auto& e : exts)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        const char* why = nullptr;
        if (instApi_ < VK_API_VERSION_1_2 || props_.apiVersion < VK_API_VERSION_1_2)
            why = "the device does not speak Vulkan 1.2";
        else if (!has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !has(VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
                 !has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
            why = "the device has no VK_KHR_ray_query / VK_KHR_acceleration_structure";
        else if (props_.limits.maxBoundDescriptorSets < 7)
            why = "the device binds fewer than 7 descriptor sets";
        else {
            VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceAccelerationStructureFeaturesKHR hAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            VkPhysicalDeviceRayQueryFeaturesKHR hRq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &h12; h12.pNext = &hAs; hAs.pNext = &hRq;
            vkGetPhysicalDeviceFeatures2(phys_, &f2);
            VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
            VkPhysicalDeviceAccelerationStructurePropertiesKHR pAs{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &p12; p12.pNext = &pAs;
            vkGetPhysicalDeviceProperties2(phys_, &p2);
            if (!hAs.accelerationStructure || !hRq.rayQuery) why = "the device cannot trace rays in a fragment shader";
            else if (!h12.bufferDeviceAddress) why = "the device has no buffer device address";
            else if (!h12.runtimeDescriptorArray || !h12.descriptorBindingPartiallyBound ||
                     !h12.shaderSampledImageArrayNonUniformIndexing || !h12.shaderStorageBufferArrayNonUniformIndexing ||
                     !h12.descriptorBindingSampledImageUpdateAfterBind || !h12.descriptorBindingStorageBufferUpdateAfterBind)
                why = "the device lacks the descriptor indexing the bindless tables need";
            else if (p12.maxPerStageDescriptorUpdateAfterBindSampledImages < kRtTexSlots + 16u ||
                     p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers < kRtRecSlots + 16u ||
                     p12.maxDescriptorSetUpdateAfterBindSampledImages < kRtTexSlots + 16u ||
                     p12.maxDescriptorSetUpdateAfterBindStorageBuffers < kRtRecSlots + 16u)
                why = "the device's update-after-bind descriptor limits are too small";
            else
                rtScratchAlign_ = std::max<VkDeviceSize>(16, pAs.minAccelerationStructureScratchOffsetAlignment);
        }
        if (why) {
            std::printf("vulkan: ray tracing asked for, but %s - off\n", why);
        } else {
            rtFeat_ = true;
            for (const char* name : {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME})
                want.push_back(name);
            rt12.bufferDeviceAddress = rt12.runtimeDescriptorArray = rt12.descriptorBindingPartiallyBound = VK_TRUE;
            rt12.shaderSampledImageArrayNonUniformIndexing = rt12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            rt12.descriptorBindingSampledImageUpdateAfterBind = rt12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            rtAs.accelerationStructure = VK_TRUE;
            rtRq.rayQuery = VK_TRUE;
            rt12.pNext = &rtAs; rtAs.pNext = &rtRq;
            // the rays' tally is written from the fragment shader (one frame in 60)
            rtCountFeat_ = have.fragmentStoresAndAtomics != VK_FALSE;
            if (rtCountFeat_) enable.fragmentStoresAndAtomics = VK_TRUE;
        }
    }
    // THE PACK'S BLOCK FORMATS (`o3de/texpack.h`): BC7 for the colours and
    // the normal maps, BC4 for the light maps - read from the pack's KTX2
    // files when the device samples and filters both (Apple's GPUs through
    // MoltenVK do). `OMK_NO_BLOCK_FORMATS=1` keeps the PNGs, for comparison.
    if (have.textureCompressionBC && !omk::envFlag("OMK_NO_BLOCK_FORMATS")) {
        bool all = true;
        for (const VkFormat f : {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC4_UNORM_BLOCK}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            all = all && (fp.optimalTilingFeatures & need) == need;
        }
        if (all) { enable.textureCompressionBC = VK_TRUE; bcOk_ = true; }
    }
    omk::texturePack().setCodecs(bcOk_ ? omk::kCodecBc : 0u);
    // DLSS (`upscale_dlss_vk.cpp`): the device extensions, 1.2 / 1.3 features and queues Streamline asked for,
    // when this device is NVIDIA's and has every one - else DLSS says so later and FSR takes over
    VkPhysicalDeviceVulkan13Features dl13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    bool dlss12 = false, dlss13 = false;
    dlssQueueStart_ = qi.queueCount;
    if (dlssPrepared_ && props_.vendorID == 0x10DE) {
        const auto hasExt = [&](const std::string& name) {
            for (const auto& e : exts)
                if (name == e.extensionName) return true;
            return false;
        };
        const char* why = nullptr;
        for (const std::string& e : dlssNeeds_.deviceExtensions)
            if (!hasExt(e)) { why = "a device extension is missing"; break; }
        if (!why && (dlssNeeds_.uses12 && props_.apiVersion < VK_API_VERSION_1_2)) why = "the device does not speak Vulkan 1.2";
        if (!why && (dlssNeeds_.uses13 && props_.apiVersion < VK_API_VERSION_1_3)) why = "the device does not speak Vulkan 1.3";
        if (!why) {
            // every feature asked for must be one the device has
            VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan13Features h13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &h12; h12.pNext = &h13;
            vkGetPhysicalDeviceFeatures2(phys_, &f2);
            // (the structures are a header, then one VkBool32 a feature, up to the 1.2 properties' strings)
            const auto covers = [](const void* want, const void* have, std::size_t bytes) {
                const auto* w = static_cast<const VkBool32*>(want);
                const auto* h = static_cast<const VkBool32*>(have);
                for (std::size_t i = 0; i < bytes / sizeof(VkBool32); ++i)
                    if (w[i] && !h[i]) return false;
                return true;
            };
            const std::size_t o12 = offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge);
            const std::size_t o13 = offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess);
            if (dlssNeeds_.uses12 &&
                !covers(reinterpret_cast<const char*>(&dlssNeeds_.f12) + o12, reinterpret_cast<const char*>(&h12) + o12,
                        sizeof h12 - o12))
                why = "a Vulkan 1.2 feature is missing";
            if (dlssNeeds_.uses13 &&
                !covers(reinterpret_cast<const char*>(&dlssNeeds_.f13) + o13, reinterpret_cast<const char*>(&h13) + o13,
                        sizeof h13 - o13))
                why = "a Vulkan 1.3 feature is missing";
        }
        if (!why && dlssNeeds_.computeQueues + dlssNeeds_.graphicsQueues + qi.queueCount > qs[qfam_].queueCount)
            why = "the queue family has no room for Streamline's queues";
        if (why) {
            std::puts((std::string("upscaler: DLSS unsupported on this GPU (") + why + ")").c_str());
        } else {
            for (const std::string& e : dlssNeeds_.deviceExtensions) {
                bool dup = false;
                for (const char* w : want) dup = dup || e == w;
                if (!dup) want.push_back(e.c_str());
            }
            dlss12 = dlssNeeds_.uses12;
            dlss13 = dlssNeeds_.uses13;
            qi.queueCount += dlssNeeds_.computeQueues + dlssNeeds_.graphicsQueues;
            dlssFeat_ = true;
        }
    } else if (dlssPrepared_) {
        std::puts("upscaler: DLSS unsupported on this GPU (not an NVIDIA device)");
    }
    if (dlss12) {
        // (Vulkan 1.2's features once: ray tracing's struct takes the OR of the two)
        const std::size_t o12 = offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge);
        const std::size_t n12 = (sizeof(VkPhysicalDeviceVulkan12Features) - o12) / sizeof(VkBool32);
        auto* dst = reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(&rt12) + o12);
        const auto* src = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&dlssNeeds_.f12) + o12);
        for (std::size_t i = 0; i < n12; ++i) dst[i] = dst[i] || src[i];
    }
    if (dlss13) {
        dl13 = dlssNeeds_.f13;
        dl13.pNext = nullptr;
        if (rtFeat_) rtRq.pNext = &dl13; else rt12.pNext = &dl13;
    }
    // THE FIDELITYFX VULKAN BACKEND (`upscale_fsr_vk.cpp`) decides what it may use from what the DEVICE offers
    // (its extension list, its feature structures) and not from what was enabled: so what it keys on is enabled
    // here when the device has it - the memory-requirements extension it calls without asking, 16-bit
    // arithmetic and storage for its half-precision shaders, the subgroup size control its wave64 permutations
    // want - as the SDK's own samples do
    VkPhysicalDeviceVulkan11Features fsr11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT fsrSg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
    bool fsr12 = false;
    if (tupKind_ > 0) {
        const auto hasExt = [&](const char* name) {
            for (const auto& e : exts)
                if (std::strcmp(e.extensionName, name) == 0) return true;
            return false;
        };
        for (const char* e : {"VK_KHR_get_memory_requirements2", "VK_KHR_dedicated_allocation", "VK_AMD_buffer_marker"}) {
            bool dup = false;
            for (const char* w : want) dup = dup || std::strcmp(w, e) == 0;
            if (hasExt(e) && !dup) want.push_back(e);
        }
        if (have.shaderInt16) enable.shaderInt16 = VK_TRUE;
        if (have.shaderStorageImageReadWithoutFormat) enable.shaderStorageImageReadWithoutFormat = VK_TRUE;
        if (have.shaderStorageImageWriteWithoutFormat) enable.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        if (props_.apiVersion >= VK_API_VERSION_1_2 && instApi_ >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceVulkan11Features h11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
            VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceSubgroupSizeControlFeaturesEXT hsg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &h11; h11.pNext = &h12;
            const bool sgExt = hasExt("VK_EXT_subgroup_size_control");
            if (sgExt) h12.pNext = &hsg;
            vkGetPhysicalDeviceFeatures2(phys_, &f2);
            if (h12.shaderFloat16 && h11.storageBuffer16BitAccess && h11.uniformAndStorageBuffer16BitAccess) {
                rt12.shaderFloat16 = VK_TRUE;
                fsr11.storageBuffer16BitAccess = fsr11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
                fsr12 = true;
            }
            if (sgExt && hsg.subgroupSizeControl) {
                want.push_back("VK_EXT_subgroup_size_control");
                fsrSg.subgroupSizeControl = VK_TRUE;
                fsrSg.computeFullSubgroups = hsg.computeFullSubgroups;
                fsr12 = true;
            }
        }
    }
    if (fsr12) {
        // (appended to the end of whatever chain the ray tracing and DLSS built on `rt12`)
        const auto append = [&](void* st) {
            auto* b = static_cast<VkBaseOutStructure*>(st);
            b->pNext = nullptr;
            VkBaseOutStructure* t = reinterpret_cast<VkBaseOutStructure*>(&rt12);
            while (t->pNext) t = t->pNext;
            t->pNext = b;
        };
        if (fsr11.storageBuffer16BitAccess) append(&fsr11);
        if (fsrSg.subgroupSizeControl) append(&fsrSg);
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    if (rtFeat_ || dlss12 || dlss13 || fsr12) di.pNext = &rt12;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &enable;
    di.enabledExtensionCount = static_cast<uint32_t>(want.size());
    di.ppEnabledExtensionNames = want.empty() ? nullptr : want.data();
    VKCHECK(vkCreateDevice(phys_, &di, nullptr, &dev_), "vkCreateDevice");
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);
    if (qi.queueCount > 1) vkGetDeviceQueue(dev_, qfam_, 1, &presQ_);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = qfam_;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(dev_, &pi, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(dev_, &cai, &cb_), "vkAllocateCommandBuffers");
    if (omk::envFlag("OMK_GPU_TIMES")) {
        if (props_.limits.timestampComputeAndGraphics && props_.limits.timestampPeriod > 0.0f) {
            VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qpi.queryType = VK_QUERY_TYPE_TIMESTAMP; qpi.queryCount = kStamps;
            if (vkCreateQueryPool(dev_, &qpi, nullptr, &tsPool_) != VK_SUCCESS) tsPool_ = VK_NULL_HANDLE;
        }
        std::printf("vulkan: the GPU's time per pass %s\n", tsPool_ ? "- every 60 frames (OMK_GPU_TIMES)"
                                                                 : "refused: the device has no timestamps");
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(dev_, &fi, nullptr, &fence_), "vkCreateFence");
    return true;
}

bool VulkanRenderer::makeTarget() {
    // R8G8B8A8_UNORM rather than a 565 format: the readback quantises to 565
    // in software, which keeps the ONE 565 quantisation rule (`PORTING` A3 -
    // bring the other side into the framebuffer's space) in one place instead
    // of delegating it to a driver whose rounding is its own.
    // THE SAMPLE COUNT - the enhancement `setMultisample` asked for, cut to
    // what the device's colour, depth AND stencil limits all allow (the
    // mirror pass needs the stencil at the same count). 1 unless asked.
    samples_ = VK_SAMPLE_COUNT_1_BIT;
    if (wantSamples_ > 1) {
        const VkSampleCountFlags ok = props_.limits.framebufferColorSampleCounts
                                    & props_.limits.framebufferDepthSampleCounts
                                    & props_.limits.framebufferStencilSampleCounts;
        for (int n = wantSamples_; n > 1; n >>= 1)
            if (ok & static_cast<VkSampleCountFlags>(n)) {
                samples_ = static_cast<VkSampleCountFlagBits>(n); break;
            }
        if (static_cast<int>(samples_) != wantSamples_)
            std::fprintf(stderr, "vulkan: %dx MSAA is not supported here - using %dx\n",
                         wantSamples_, static_cast<int>(samples_));
    }
    const bool msaa = samples_ != VK_SAMPLE_COUNT_1_BIT;

    // (THE TEMPORAL UPSCALER: `colour_` is the DISPLAY size, the scene pass drawing into its top-left
    // `rw_ x rh_` and `upscale()` writing the finished picture over all of it)
    auto imageAt = [&](int iw, int ih, VkFormat f, VkImageUsageFlags use, VkImageAspectFlags asp,
                       VkSampleCountFlagBits samples,
                       VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f;
        ii.extent = {static_cast<uint32_t>(iw), static_cast<uint32_t>(ih), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = use; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return makeImage2D(ii, asp, "render targets", img, mem, &view);
    };
    auto image = [&](VkFormat f, VkImageUsageFlags use, VkImageAspectFlags asp, VkSampleCountFlagBits samples,
                     VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        return imageAt(rw_, rh_, f, use, asp, samples, img, mem, view);
    };
    if (!imageAt(tupOn_ ? w_ : rw_, tupOn_ ? h_ : rh_, VK_FORMAT_R8G8B8A8_UNORM,
                 // SAMPLED for the present pass, which reads the frame where it is; TRANSFER_DST for the
                 // temporal upscaler's output copied over it
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT | (tupOn_ ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0u),
                 VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
                 colour_, colourMem_, colourView_)) return false;
    // ...and its display-size output (`upscale()`)
    if (tupOn_ && !makeImage2d(VK_FORMAT_R8G8B8A8_UNORM, w_, h_,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               tupOut_, tupOutMem_, tupOutView_, "upscaler output")) return false;
    // (TRANSIENT: resolved within the pass, never stored - `makeImage2D`)
    if (msaa && !image(VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, samples_,
                       msColour_, msColourMem_, msColourView_)) return false;
    // A depth+STENCIL format, because the mirror pass confines its reflection
    // with a stencil. D32_SFLOAT_S8_UINT first, D24_UNORM_S8_UINT as the
    // fallback: which one a device supports is not a given, and MoltenVK's
    // answer differs from a desktop driver's.
    dsFmt_ = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            dsFmt_ = f; break;
        }
    }
    if (dsFmt_ == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "vulkan: no depth+stencil format\n"); return false;
    }
    ssaoOn_ = false;
    // (ray tracing runs the AO pass too, its rays in place of the hemisphere)
    if (wantSsao_ || rtOn_) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        // under MSAA the multisampled depth is sampled as it is (its first
        // sample), which the device must allow at this count
        const bool msOk = !msaa || (props_.limits.sampledImageDepthSampleCounts & samples_);
        ssaoOn_ = msOk && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!resizing_ && !(rtOn_ && ssaoOn_))
            std::printf(ssaoOn_ ? "vulkan: SSAO on - an ENHANCEMENT: ambient occlusion from the "
                                  "depth buffer, multiplied over the frame%s\n"
                                : "vulkan: SSAO asked for but not available (%s)\n",
                        ssaoOn_ ? (msaa ? " (multisampled depth)" : "")
                                : !msOk ? "the device cannot sample a multisampled depth"
                                        : "the depth format cannot be sampled");
    }
    // DEPTH OF FIELD (`todo/enhancements.md` 22) reads the depth and, for a
    // translucent body, the stencil plane's mark: sampleable at this sample
    // count, the final pass's 144-byte push block within the device's limit
    dofOn_ = false;
    if (postReq_.dof) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        const bool msOk = !msaa || ((props_.limits.sampledImageDepthSampleCounts & samples_) &&
                                    (props_.limits.sampledImageStencilSampleCounts & samples_));
        const bool pushOk = props_.limits.maxPushConstantsSize >= sizeof(omk::postfx::PostPush);
        dofOn_ = msOk && pushOk && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!dofOn_ && !resizing_)
            std::printf("vulkan: depth of field asked for but not available (%s)\n",
                        !msOk ? "the device cannot sample a multisampled depth and stencil"
                              : !pushOk ? "the push constants are too small for the final pass"
                                        : "the depth format cannot be sampled");
    }
    // SOFT PARTICLES read the depth beside the test: a single-sample depth that can be sampled
    // (what Direct3D 12 needs too), and a fourth descriptor set
    softOn_ = false;
    if (softWant_ && !softNoUbo_) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        softOn_ = !msaa && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                  props_.limits.maxBoundDescriptorSets >= 4;
        if (!softOn_ && !resizing_) {
            if (msaa)
                std::printf("vulkan: soft particles need a single-sample depth - off under %ux MSAA\n",
                            static_cast<unsigned>(samples_));
            else
                std::printf("vulkan: soft particles asked for but not available (the depth cannot be sampled)\n");
            omk::event("softparticles", {{"on", 0}, {"samples", static_cast<int>(samples_)}});
        }
    }
    // VOLUMETRIC LAMP LIGHT reads the depth too, per sample under MSAA like SSAO
    volOn_ = false;
    if (volWant_ && !volNoBuf_) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        const bool msOk = !msaa || (props_.limits.sampledImageDepthSampleCounts & samples_);
        volOn_ = msOk && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!volOn_ && !resizing_) {
            std::printf("vulkan: volumetric lamp light asked for but not available (%s)\n",
                        !msOk ? "the device cannot sample a multisampled depth" : "the depth format cannot be sampled");
        }
    }
    // FRAME GENERATION (`todo/enhancements.md` 27): a way of PRESENTING, so the window's alone; it warps along
    // single-sample motion vectors, so not under MSAA
    fgOn_ = false;
    if (fgWant_) {
        const char* why = nullptr;
        if (surface_ == VK_NULL_HANDLE)
            why = "framegen: a way of presenting - no window here (the offscreen harness), off";
        else if (wantSamples_ > 1)
            why = nullptr;   // said below with the sample count
        else if (!fgFeat_)
            why = "framegen: the device cannot store to an image from a fragment shader or blend its targets apart - off";
        else if (props_.limits.maxPushConstantsSize < sizeof(PushMv))
            why = "framegen: the device's push constants are too small for the motion block - off";
        else {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                why = "framegen: the depth cannot be sampled - off";
        }
        fgOn_ = surface_ != VK_NULL_HANDLE && wantSamples_ <= 1 && !why;
        if (!fgOn_ && !resizing_) {
            if (why) std::printf("%s\n", why);
            else if (surface_ != VK_NULL_HANDLE)
                std::printf("framegen: it warps along single-sample motion vectors - off under %dx MSAA "
                            "(aa = off lets it run)\n", wantSamples_);
            omk::event("framegen", {{"on", 0}, {"refresh", 0}});
        }
    }
    mvOn_ = fgOn_ || tupOn_;   // (the temporal upscaler reads the same vectors)
    poseRange_ = mvOn_ ? kPoseRangeMv : kPoseRange;
    // ...SAMPLED and KEPT where the ambient occlusion reads it - and only there
    aoOk_ = false;
    if (ao_ > 0.0f) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        aoOk_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
        if (!aoOk_) std::fprintf(stderr, "vulkan: the depth format cannot be sampled - no ambient occlusion\n");
    }
    const bool depthKept = ssaoOn_ || dofOn_ || softOn_ || volOn_ || fgOn_ || aoOk_ || tupOn_;
    // (TRANSIENT where nothing reads it after the pass - the AO's absence - SAMPLED where the AO or any
    // of this backend's depth readers does: the post chain, SSAO, soft particles, volumetrics, the warp)
    VkImageUsageFlags depthUse = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        (aoOk_ ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    if (depthKept) depthUse = (depthUse & ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!image(dsFmt_, depthUse,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, samples_,
               depth_, depthMem_, depthView_)) return false;
    if (mvOn_ && !makeImage2d(VK_FORMAT_R16G16_SFLOAT, rw_, rh_,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              motion_, motionMem_, motionView_, "motion vectors")) return false;
    if (depthKept) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = dsFmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &depthSampleView_), "depth sample view");
    }
    // THE TEMPORAL UPSCALER's depth image and the pass that fills it from the depth just made
    if (tupOn_ && !makeTupDepth()) return false;
    if (dofOn_) {
        // the stencil plane as the shader reads it: `post.frag` takes the mark
        // from `.g` (Direct3D's X32_TYPELESS_G8X24_UINT view), a Vulkan stencil
        // view holds it in `.r` - so every channel is swizzled to it
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = dsFmt_;
        vi.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R};
        vi.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &stencilSampleView_), "stencil sample view");
    }
    if (aoOk_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = dsFmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &depthRead_), "vkCreateImageView(depth read)");
    }

    // Attachment 0 is what the pipelines rasterise into: `colour_` itself
    // without MSAA, `msColour_` with it - in which case it is not kept
    // (DONT_CARE) and attachment 2 is the resolve into `colour_`, which then
    // ends in the same TRANSFER_SRC layout the readback and the blit expect,
    // so nothing downstream knows which way the frame was made.
    VkAttachmentDescription att[3]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = samples_;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                              : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = dsFmt_;
    att[1].samples = samples_;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = aoOk_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    if (depthKept) att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // (this backend's own depth readers too)
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // the stencil lives within the pass (the mirror's, cleared at its start):
    // nothing loads it after, so it is not stored (the audit's 4K trims)
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = depthKept ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (mvOn_) {
        // FRAME GENERATION's second target: the motion vectors (cleared to none), kept for the capture pass
        att[2] = {};
        att[2].format = VK_FORMAT_R16G16_SFLOAT;
        att[2].samples = VK_SAMPLE_COUNT_1_BIT;
        att[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const VkAttachmentReference crefs[2] = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                            {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = mvOn_ ? 2 : 1; sub.pColorAttachments = crefs;
    sub.pResolveAttachments = msaa ? &rref : nullptr;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = (msaa || mvOn_) ? 3 : 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    // (with soft particles the pass has its dependencies written out - the continuations below carry
    // the very same, so their pipelines stay compatible - the frame's writes before the next
    // segment's reads and tests, and out)
    VkSubpassDependency softDep[2]{};
    if (softOn_) {
        const VkPipelineStageFlags all = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        const VkAccessFlags wr = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        const VkAccessFlags rw = wr | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        softDep[0] = {VK_SUBPASS_EXTERNAL, 0, all, all, wr, rw, 0};
        softDep[1] = {0, VK_SUBPASS_EXTERNAL, all, all, wr, rw, 0};
        rpi.dependencyCount = 2; rpi.pDependencies = softDep;
    }
    VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &pass_), "vkCreateRenderPass");
    // ...and with the AO, the WATER REFLECTION's own: the same but for the
    // depth's store - nothing reads the reflection's depth, and storing it is
    // a full extra depth write (on a tiled GPU at 4K). Store ops do not touch
    // compatibility: its framebuffer and every pipeline stay `pass_`'s.
    if (aoOk_) {
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        if (vkCreateRenderPass(dev_, &rpi, nullptr, &reflPass_) != VK_SUCCESS) reflPass_ = VK_NULL_HANDLE;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (softOn_) {
        // the continuations: what the first segment left is loaded; the final layouts are the
        // initial ones, so a frame may be cut as often as it likes
        VkAttachmentDescription ca[3] = {att[0], att[1], att[2]};
        ca[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;                      // (the motion vectors, when asked)
        ca[2].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ca[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ca[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        ca[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        for (int ro = 0; ro < 2; ++ro) {
            VkAttachmentReference cd{1, ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription cs = sub;
            cs.pResolveAttachments = nullptr;
            cs.pDepthStencilAttachment = &cd;
            VkRenderPassCreateInfo ci = rpi;
            ci.attachmentCount = mvOn_ ? 3 : 2; ci.pAttachments = ca;
            ci.pSubpasses = &cs;
            VKCHECK(vkCreateRenderPass(dev_, &ci, nullptr, &passCont_[ro]), "soft continuation pass");
        }
    }

    VkImageView views[3] = {msaa ? msColourView_ : colourView_, depthView_, mvOn_ ? motionView_ : colourView_};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass_; fi.attachmentCount = (msaa || mvOn_) ? 3 : 2; fi.pAttachments = views;
    fi.width = static_cast<uint32_t>(rw_); fi.height = static_cast<uint32_t>(rh_);
    fi.layers = 1;
    VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &fbuf_), "vkCreateFramebuffer");

    // `readBuf_` and `fb_` are made by the first `readback`: a GPU-presented
    // run never reads a frame back, and at 4K they are 33 + 16 MB
    fb_ = omk::Surface();
    if (lazyImages_ > 0) {
        static bool said = false;
        if (!said)
            std::printf("vulkan: %d render-target attachments TRANSIENT in lazily allocated memory, %.0f MB "
                        "(memoryless on Apple's GPUs)\n", lazyImages_, static_cast<double>(lazyBytes_) / 1048576.0);
        said = true;
        omk::event("vulkan.memoryless", {{"images", lazyImages_}, {"mb", static_cast<double>(lazyBytes_) / 1048576.0}});
    }
    writeSoftSet();   // (the depth was remade: a resize points the set at the new one)
    // a RESIZE stops here: what follows is sized once and kept
    if (resizing_) return true;

    // ---- THE SHADOW MAP's own target (`todo/enhancements.md` 6) ----------
    //
    // One depth image, its own render pass, and a sampler. It is sized once
    // and reused: the light's slab is fitted to the CASTERS every frame, so
    // the resolution follows them rather than the world.
    shFmt_ = VK_FORMAT_UNDEFINED;
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            shFmt_ = f; break;
        }
    }
    if (shFmt_ != VK_FORMAT_UNDEFINED) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = shFmt_;
        ii.extent = {kShadowSide, kShadowSide, 1};
        ii.mipLevels = 1; ii.arrayLayers = kShadowLayers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (makeImage2D(ii, VK_IMAGE_ASPECT_DEPTH_BIT, "shadow map", shImg_, shMem_, nullptr)) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            // the SAMPLED view spans every layer (`sampler2DArray`)...
            vi.image = shImg_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format = shFmt_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowLayers};
            vkCreateImageView(dev_, &vi, nullptr, &shView_);
            // ...and each layer is rendered through a view of its own
            for (int L = 0; L < kShadowLayers; ++L) {
                VkImageViewCreateInfo lv = vi;
                lv.viewType = VK_IMAGE_VIEW_TYPE_2D;
                lv.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                                       static_cast<uint32_t>(L), 1};
                vkCreateImageView(dev_, &lv, nullptr, &shLayerView_[L]);
            }
            // SHADER_READ from the start: every frame's scene set binds it,
            // and a run whose shadow pass never draws (no mapped shadows)
            // read it UNDEFINED (the validation layer's VUID-vkCmdDraw-None-09600,
            // found 2026-10-24); its pass starts from UNDEFINED as before
            {
                VkCommandBuffer cb = oneShotBegin();
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = shImg_; b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, kShadowLayers};
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &b);
                oneShotEnd(cb);
            }

            VkAttachmentDescription da{};
            da.format = shFmt_; da.samples = VK_SAMPLE_COUNT_1_BIT;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            da.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            da.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkAttachmentReference dr{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sp{};
            sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sp.pDepthStencilAttachment = &dr;
            // The frame's own pass READS what this one wrote, so the
            // dependency out has to name the fragment stage.
            VkSubpassDependency dep[2]{};
            dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
            dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dep[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
            dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dep[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
            dep[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dep[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            VkRenderPassCreateInfo shadowRpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            shadowRpi.attachmentCount = 1; shadowRpi.pAttachments = &da;
            shadowRpi.subpassCount = 1; shadowRpi.pSubpasses = &sp;
            shadowRpi.dependencyCount = 2; shadowRpi.pDependencies = dep;
            vkCreateRenderPass(dev_, &shadowRpi, nullptr, &shPass_);

            VkFramebufferCreateInfo shadowFi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            shadowFi.renderPass = shPass_; shadowFi.attachmentCount = 1;
            shadowFi.width = kShadowSide; shadowFi.height = kShadowSide; shadowFi.layers = 1;
            for (int L = 0; L < kShadowLayers; ++L) {
                shadowFi.pAttachments = &shLayerView_[L];
                vkCreateFramebuffer(dev_, &shadowFi, nullptr, &shLayerFbuf_[L]);
            }
            shFbuf_ = shLayerFbuf_[0];

            // NEAREST, because the fragment shader compares depths itself and
            // a linear filter would average DEPTHS rather than the comparison.
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = si.minFilter = VK_FILTER_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW =
                VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            si.maxLod = 0.0f;
            vkCreateSampler(dev_, &si, nullptr, &shSampler_);

            makeBuffer(sizeof(ShadowUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       shUbo_, shUboMem_);
            vkMapMemory(dev_, shUboMem_, 0, sizeof(ShadowUbo), 0, &shUboPtr_);
            if (shUboPtr_) std::memset(shUboPtr_, 0, sizeof(ShadowUbo));
            makeBuffer(sizeof(LightUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       litUbo_, litUboMem_);
            vkMapMemory(dev_, litUboMem_, 0, sizeof(LightUbo), 0, &litUboPtr_);
            if (litUboPtr_) std::memset(litUboPtr_, 0, sizeof(LightUbo));
            // the tiled point lights' buffers, only for a frame that sends
            // the emitters' lights: without them no tiled pipeline is bound,
            // and the 32 MB stayed mapped for nothing (`verify.py: engine:
            // gpu readback on demand`, 2026-10-21)
            if (emitters_) {
                makeBuffer(kPointsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           pointsBuf_, pointsMem_);
                vkMapMemory(dev_, pointsMem_, 0, kPointsBytes, 0, &pointsPtr_);
                // TWO HALVES: the frame's camera's tiles, then the water
                // reflection's - binned for ITS camera (`ds1Refl_`)
                makeBuffer(kTileWordsMax * 4 * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           tilesBuf_, tilesMem_);
                vkMapMemory(dev_, tilesMem_, 0, kTileWordsMax * 4 * 2, 0, &tilesPtr_);
                if (tilesPtr_) {
                    std::memset(tilesPtr_, 0, 16);
                    std::memset(static_cast<char*>(tilesPtr_) + kTileWordsMax * 4, 0, 16);
                }
            }
        }
    }
    // volumetrics: the lamps' uniform block, written every frame
    if (volOn_) {
        if (!makeBuffer(sizeof(omk::postfx::VolUbo), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        volUbo_, volUboMem_) ||
            vkMapMemory(dev_, volUboMem_, 0, sizeof(omk::postfx::VolUbo), 0, &volUboPtr_) != VK_SUCCESS) {
            volOn_ = false; volNoBuf_ = true;
            std::fprintf(stderr, "vulkan: no lamp buffer - volumetrics off\n");
        } else {
            volTune_.fromEnv();
            std::printf("vulkan: volumetric lamp light on - an ENHANCEMENT: the lamps' light scattered in the air, "
                        "half-size, added before the post chain (density %g, core %g and glow %g of the reach)\n",
                        static_cast<double>(volTune_.density), static_cast<double>(volTune_.core),
                        static_cast<double>(volTune_.reach));
        }
    }
    // soft particles' scene shader reads the fade's constants from the lights' uniform block
    if (softOn_ && !litUboPtr_) {
        softOn_ = false; softNoUbo_ = true;
        std::printf("vulkan: soft particles asked for but not available (no per-frame uniform block)\n");
        omk::event("softparticles", {{"on", 0}, {"samples", static_cast<int>(samples_)}});
    }
    if (softWant_ && softOn_) {
        if (const char* r = omk::envStr("OMK_SOFT_RANGE"))
            if (std::atof(r) > 0.0) softRange_ = static_cast<float>(std::atof(r));
        std::printf("vulkan: soft particles on - an ENHANCEMENT: the effect sprites fade over %.0f units "
                    "where they meet the opaque depth\n", static_cast<double>(softRange_));
        omk::event("softparticles", {{"on", 1}, {"samples", static_cast<int>(samples_)}});
    }
    // NO SHADOW MAP, NO RENDERER: set 1 is made with it - the shadow map, the
    // lights' uniforms, the tiled lights' buffers - and every scene pipeline
    // reads that set, so a frame without it bound draws on nothing (MoltenVK
    // faults in its first draw: `OMK_VK_IMAGE_FAIL="shadow map"`, 2026-10-24).
    // Refused here like a render target, and the frontend falls back. (It
    // used to say the fitted blobs would stand in - they never could.)
    if (shFbuf_ == VK_NULL_HANDLE) {
        std::fprintf(stderr, "vulkan: no shadow map here - its set carries the scene's lights too, refused\n");
        return false;
    }
    return true;
}

// THE TEXTURES' SAMPLER, by `filter_` and `anisoOn_` - made at the
// pipelines', and again by `retexture`
bool VulkanRenderer::makeSampler() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // NEAREST, and REPEAT. Nearest because the software rasterizer's `sample()`
    // takes one texel with no filtering and the two are being differenced;
    // repeat because `% t.width` is what lets one atlas tile across a wall.
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    // ...unless the filtering ENHANCEMENT asked for bilinear (renderer.h).
    // The colour key survives it because `upload` puts the key in ALPHA and
    // the shader treats a filtered sample as premultiplied.
    if (filter_ >= 1) { si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR; }
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    // Trilinear: the mip chain `upload` generates, blended between levels.
    // With one level (every mode below 2) maxLod 0 samples level 0 exactly.
    if (filter_ >= 2) { si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; si.maxLod = VK_LOD_CLAMP_NONE; }
    if (anisoOn_) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(static_cast<float>(aniso_), props_.limits.maxSamplerAnisotropy);
    }
    VKCHECK(vkCreateSampler(dev_, &si, nullptr, &sampler_), "vkCreateSampler");
    return true;
}

// THE TEXTURE FILTER and the ANISOTROPY asked again on a running renderer
// (OMK's options menu, `todo/graphics-menu.md` 3d). Both are baked: the
// sampler is written into every texture's set, and trilinear's mip chain is
// made at upload. So a new sampler (the old one RETIRED, not destroyed - the
// 1x1 stand-ins' sets keep it, a 1x1 texel filtering alike either way) and
// every upload dropped, the preloaded ones unpinned too; the caller hands the
// pool over again, and it uploads as a start with the values would.
bool VulkanRenderer::retexture(int filter, int aniso) {
    filter_ = filter < 1 ? 0 : filter < 2 ? 1 : 2;
    aniso_ = aniso < 1 ? 1 : aniso > 16 ? 16 : aniso;
    if (dev_ == VK_NULL_HANDLE) return true;
    vkDeviceWaitIdle(dev_);
    anisoOn_ = aniso_ > 1 && filter_ >= 2 && anisoAvail_;
    retiredSamplers_.push_back(sampler_);
    sampler_ = VK_NULL_HANDLE;
    if (!makeSampler()) return false;
    setPreloadTextures({});
    poolTextures({}, false);
    std::printf("vulkan: retextured - %s filtering, anisotropy %d; every texture dropped, uploaded again\n",
                filter_ == 0 ? "nearest" : filter_ == 1 ? "bilinear" : "trilinear", anisoOn_ ? aniso_ : 1);
    return true;
}

bool VulkanRenderer::makePipelines() {
    // SET 0 a texture's: binding 0 the texture, 1 its LIGHT MAP (the bloom's;
    // black without one), 2 the WATER pass's reflection (black without one),
    // 3 its NORMAL MAP (the relief's; flat, alpha 0, without one)
    VkDescriptorSetLayoutBinding b2tex[4]{};
    for (uint32_t k = 0; k < 4; ++k) {
        b2tex[k].binding = k; b2tex[k].descriptorCount = 1;
        b2tex[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b2tex[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 4; dli.pBindings = b2tex;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &dsl_), "descriptorSetLayout");

    // SET 1 is per FRAME - the shadow map and the light's transform. It exists
    // whether or not a shadow is drawn, because a pipeline layout is fixed at
    // creation and `strength = 0` is how a frame says it has no shadow.
    VkDescriptorSetLayoutBinding b1[5]{};
    b1[0].binding = 0; b1[0].descriptorCount = 1;
    b1[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b1[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b1[1].binding = 1; b1[1].descriptorCount = 1;
    b1[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b1[2].binding = 2; b1[2].descriptorCount = 1;
    b1[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b1[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // ...3 the frame's point lights and 4 their TILES (`o3de/lighttiles.h`)
    for (uint32_t k = 3; k < 5; ++k) {
        b1[k].binding = k; b1[k].descriptorCount = 1;
        b1[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b1[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli1.bindingCount = 5; dli1.pBindings = b1;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli1, nullptr, &dsl1_), "descriptorSetLayout1");

    // SET 2 is the POSE BUFFER (`posesBodies`): one dynamic storage buffer,
    // offset per posed body, read by the vertex stage
    VkDescriptorSetLayoutBinding b2{};
    b2.binding = 0; b2.descriptorCount = 1;
    b2.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    b2.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo dli2{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli2.bindingCount = 1; dli2.pBindings = &b2;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli2, nullptr, &dsl2_), "descriptorSetLayout2");

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, mvOn_ ? sizeof(PushMv) : sizeof(Push)};
    // SET 3, with soft particles: the scene's depth, sampled (`scene.frag` -DOMK_SOFT)
    if (softOn_) {
        VkDescriptorSetLayoutBinding b3{};
        b3.binding = 0; b3.descriptorCount = 1;
        b3.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b3.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli3{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli3.bindingCount = 1; dli3.pBindings = &b3;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli3, nullptr, &dsl3_), "descriptorSetLayout3");
        // a pool of its own: `setTextures` resets the texture pool, and this set must outlive that
        const VkDescriptorPoolSize sp{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo spi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        spi.maxSets = 1; spi.poolSizeCount = 1; spi.pPoolSizes = &sp;
        VKCHECK(vkCreateDescriptorPool(dev_, &spi, nullptr, &softPool_), "soft descriptorPool");
        VkDescriptorSetAllocateInfo sai3{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai3.descriptorPool = softPool_; sai3.descriptorSetCount = 1; sai3.pSetLayouts = &dsl3_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &sai3, &softDs_), "soft descriptor set");
        VkSamplerCreateInfo ssi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ssi.magFilter = ssi.minFilter = VK_FILTER_NEAREST;   // (the shader only texel-fetches)
        ssi.addressModeU = ssi.addressModeV = ssi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK(vkCreateSampler(dev_, &ssi, nullptr, &softSampler_), "soft sampler");
        writeSoftSet();
    }
    // HYBRID RAY TRACING's three sets (`rt.glsl`) follow as 4, 5 and 6; with no soft particles set 3 is empty
    if (rtOn_ && !softOn_) {
        VkDescriptorSetLayoutCreateInfo de{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &de, nullptr, &dslEmpty_), "descriptorSetLayout3 (empty)");
    }
    const VkDescriptorSetLayout sets[7] = {dsl_, dsl1_, dsl2_, softOn_ ? dsl3_ : dslEmpty_, rtDslA_, rtDslB_, rtDslC_};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = rtOn_ ? 7 : softOn_ ? 4 : 3; pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &plo_), "pipelineLayout");

    auto module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes; si.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &si, nullptr, &m);
        return m;
    };
    VkShaderModule vs = mvOn_ ? module(kVertMv, sizeof(kVertMv)) : module(kVert, sizeof(kVert));
    VkShaderModule fs = rtOn_ ? (mvOn_ ? module(kFragRtMv, sizeof(kFragRtMv)) : module(kFragRt, sizeof(kFragRt)))
                      : mvOn_ ? module(kFragMv, sizeof(kFragMv)) : module(kFrag, sizeof(kFrag));
    // THE REMASTER'S modules (the tiled point lights, the cutout's late depth - built from the plain
    // scene shader: not on a ray-traced, motion-vector or soft frame, whose passes keep their own)
    const bool plainBuild = !rtOn_ && !mvOn_;
    VkShaderModule fsTiled = plainBuild ? module(kFragTiled, sizeof(kFragTiled)) : VK_NULL_HANDLE;
    VkShaderModule fsCut = plainBuild ? module(kFragCut, sizeof(kFragCut)) : VK_NULL_HANDLE;
    VkShaderModule fsTiledCut = plainBuild ? module(kFragTiledCut, sizeof(kFragTiledCut)) : VK_NULL_HANDLE;
    // ...and the scene pass that COUNTS its rays (`pipeCount_`, one frame in 60)
    VkShaderModule fsCount = !(rtOn_ && rtCountFeat_) ? VK_NULL_HANDLE
                           : mvOn_ ? module(kFragRtcMv, sizeof(kFragRtcMv)) : module(kFragRtc, sizeof(kFragRtc));
    VkShaderModule fsSoft = !softOn_ ? VK_NULL_HANDLE
                          : mvOn_ ? module(kFragSoftMv, sizeof(kFragSoftMv)) : module(kFragSoft, sizeof(kFragSoft));
    // FRAME GENERATION: the pass's second target takes the motion of OPAQUE geometry only - an additive glow
    // or a multiply decal over a wall leaves the wall's motion, which is what the warp should follow
    const auto mvAtt = [](bool write) {
        VkPipelineColorBlendAttachmentState a{};
        a.colorWriteMask = write ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT) : 0;
        return a;
    };
    if (!vs || !fs || (softOn_ && !fsSoft)) { std::fprintf(stderr, "vulkan: shader module\n"); return false; }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

    VkVertexInputBindingDescription vb{0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[6] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVert, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(GpuVert, u)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVert, r)},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVert, nx)},
        {4, 0, VK_FORMAT_R32_SFLOAT,       offsetof(GpuVert, phase)},
        {5, 0, VK_FORMAT_R32_SFLOAT,       offsetof(GpuVert, mesh)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 6; vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // DYNAMIC, because the letterbox is a viewport: camera mode draws into the
    // top-left 800x440 of a window-sized attachment and free roaming into all
    // of it, and baking either into the pipeline would need two of every one.
    VkViewport vp{0, 0, static_cast<float>(rw_), static_cast<float>(rh_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp;
    vps.scissorCount = 1; vps.pScissors = &sc;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // D3DCULL_NONE - `Raster_DrawTriangles` sets it. The engine's own cull is
    // in software, before the device: `push_.cull` and scene.frag's discard.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples_;   // 1 unless the enhancement is on

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    // the three pipelines are the three blend modes, by `blendRule`
    // (`o3de/drawcore.h`): opaque, additive (0x1000|0x2000, 211 meshes) and
    // multiply (0x1000|0x4000, 6 meshes and the mode-6 sprites - `dst * (1 -
    // src)`, darkening where the source is bright; DST_COLOR / ZERO would be
    // `dst * src`, the wrong law). Only an OPAQUE pass owns the depth.
    const auto vkFactor = [](omk::BlendFactor f) {
        return f == omk::BlendFactor::Zero ? VK_BLEND_FACTOR_ZERO
             : f == omk::BlendFactor::One  ? VK_BLEND_FACTOR_ONE
                                           : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    };
    const omk::Blend kModes[3] = {omk::Blend::Opaque, omk::Blend::Add, omk::Blend::Mul};
    const auto blendState = [&](int k) {
        const omk::BlendRule rule = omk::blendRule(kModes[k]);
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        cba.blendEnable = rule.blend ? VK_TRUE : VK_FALSE;
        if (rule.blend) {
            cba.srcColorBlendFactor = vkFactor(rule.src);
            cba.dstColorBlendFactor = vkFactor(rule.dst);
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            // the alpha's factors are this backend's own (`BlendRule`)
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = kModes[k] == omk::Blend::Add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        return cba;
    };
    for (int k = 0; k < 3; ++k) {
        const VkPipelineColorBlendAttachmentState cba = blendState(k);
        ds.depthWriteEnable = omk::blendRule(kModes[k]).depthWrite ? VK_TRUE : VK_FALSE;

        const VkPipelineColorBlendAttachmentState cbas[2] = {cba, mvAtt(k == 0)};
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = mvOn_ ? 2 : 1; cb.pAttachments = cbas;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsi; gp.layout = plo_; gp.renderPass = pass_;
        VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe_[k]),
                "vkCreateGraphicsPipelines");
        // SOFT PARTICLES' twin of the two blended pipelines: the -DOMK_SOFT shader, no depth write
        // (the pass segment it draws in holds the depth read-only), no stencil
        if (softOn_ && k > 0) {
            VkPipelineShaderStageCreateInfo ss2[2] = {stages[0], stages[1]};
            ss2[1].module = fsSoft;
            VkPipelineDepthStencilStateCreateInfo sd = ds;
            sd.depthWriteEnable = VK_FALSE;
            gp.pStages = ss2; gp.pDepthStencilState = &sd;
            VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeSoft_[k]),
                    "soft pipeline");
            gp.pStages = stages; gp.pDepthStencilState = &ds;
        }
        if (fsCount) {
            VkPipelineShaderStageCreateInfo cs[2] = {stages[0], stages[1]};
            cs[1].module = fsCount;
            gp.pStages = cs;
            VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeCount_[k]),
                    "counting pipeline");
            gp.pStages = stages;
        }
        // DEPTH OF FIELD'S TRANSLUCENT SUBJECT: a blended body (a hologram, a
        // ghost) writes no depth, so the post pass saw the wall behind it and
        // blurred it. On a frame that will take depth of field the blended
        // pipelines' twins mark such a body's pixels in the STENCIL
        // (`kSubjectStencil`) - the picture and the depth untouched - and
        // `post.frag` keeps them at the focus.
        if (dofOn_ && k > 0) {
            VkPipelineDepthStencilStateCreateInfo sd = ds;
            sd.stencilTestEnable = VK_TRUE;
            VkStencilOpState o{};
            o.failOp = VK_STENCIL_OP_KEEP; o.depthFailOp = VK_STENCIL_OP_KEEP;
            o.passOp = VK_STENCIL_OP_REPLACE; o.compareOp = VK_COMPARE_OP_ALWAYS;
            o.compareMask = 0xFF; o.writeMask = 0xFF; o.reference = omk::postfx::kSubjectStencil;
            sd.front = sd.back = o;
            gp.pDepthStencilState = &sd;
            VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeSubj_[k]),
                    "subject pipeline");
        }
        // ...and the same with the TILED fragment module
        if (fsTiled) {
            VkPipelineShaderStageCreateInfo st2[2] = {stages[0], stages[1]};
            st2[1].module = fsTiled;
            gp.pStages = st2;
            if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeTiled_[k]) != VK_SUCCESS)
                pipeTiled_[k] = VK_NULL_HANDLE;
            gp.pStages = stages;
        }
        // ...and the OPAQUE CUTOUT's, the depth the shader's (only an opaque
        // draw writes depth); without one a cutout draw keeps the plain
        if (k == 0) {
            VkPipelineShaderStageCreateInfo st3[2] = {stages[0], stages[1]};
            gp.pStages = st3;
            st3[1].module = fsCut;
            if (!fsCut || vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeCut_) != VK_SUCCESS)
                pipeCut_ = VK_NULL_HANDLE;
            st3[1].module = fsTiledCut;
            if (!fsTiled || !fsTiledCut ||
                vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeTiledCut_) != VK_SUCCESS)
                pipeTiledCut_ = VK_NULL_HANDLE;
            gp.pStages = stages;
        }
    }
    // ---- THE FLOORS' REFLECTION at one sample (`reflPass1_`), with MSAA on
    if (samples_ != VK_SAMPLE_COUNT_1_BIT) {
        VkAttachmentDescription a1[2]{};
        a1[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        a1[0].samples = VK_SAMPLE_COUNT_1_BIT;
        a1[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a1[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a1[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a1[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a1[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a1[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // as the MSAA pass's resolve leaves it
        a1[1].format = dsFmt_;
        a1[1].samples = VK_SAMPLE_COUNT_1_BIT;
        a1[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a1[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a1[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a1[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a1[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a1[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference c1{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference d1{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub1{};
        sub1.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub1.colorAttachmentCount = 1; sub1.pColorAttachments = &c1;
        sub1.pDepthStencilAttachment = &d1;
        VkRenderPassCreateInfo rp1{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp1.attachmentCount = 2; rp1.pAttachments = a1;
        rp1.subpassCount = 1; rp1.pSubpasses = &sub1;
        if (vkCreateRenderPass(dev_, &rp1, nullptr, &reflPass1_) != VK_SUCCESS) reflPass1_ = VK_NULL_HANDLE;
        VkPipelineMultisampleStateCreateInfo ms1{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms1.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        for (int k = 0; k < 3 && reflPass1_; ++k) {
            const VkPipelineColorBlendAttachmentState cba = blendState(k);
            ds.depthWriteEnable = omk::blendRule(kModes[k]).depthWrite ? VK_TRUE : VK_FALSE;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.stageCount = 2; gp.pStages = stages;
            gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms1; gp.pDepthStencilState = &ds;
            gp.pColorBlendState = &cb;
            gp.pDynamicState = &dsi; gp.layout = plo_; gp.renderPass = reflPass1_;
            if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe1_[k]) != VK_SUCCESS)
                pipe1_[k] = VK_NULL_HANDLE;
            if (fsTiled) {
                VkPipelineShaderStageCreateInfo st2[2] = {stages[0], stages[1]};
                st2[1].module = fsTiled;
                gp.pStages = st2;
                if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeTiled1_[k]) != VK_SUCCESS)
                    pipeTiled1_[k] = VK_NULL_HANDLE;
            }
        }
        // all or none: a missing one and the floors keep the MSAA pass
        for (int k = 0; k < 3; ++k)
            if (!pipe1_[k] || (fsTiled && !pipeTiled1_[k])) {
                for (auto& p : pipe1_) if (p) { vkDestroyPipeline(dev_, p, nullptr); p = VK_NULL_HANDLE; }
                for (auto& p : pipeTiled1_) if (p) { vkDestroyPipeline(dev_, p, nullptr); p = VK_NULL_HANDLE; }
                if (reflPass1_) { vkDestroyRenderPass(dev_, reflPass1_, nullptr); reflPass1_ = VK_NULL_HANDLE; }
                break;
            }
    }
    // ---- the mirror pass's pipelines.
    //
    // A stencil mirror, the classic construction, and each state below is one
    // step of it:
    //
    //   1. `pipeStencil_` draws the mirror's own faces with the colour mask
    //      OFF and depth write off, replacing the stencil with 1 wherever they
    //      pass the depth test. So the mark is the mirror as actually SEEN -
    //      occluded parts of the plane do not get marked, which is the bug the
    //      CPU version had to be taught by hand.
    //   2. `pipeDepthReset_` puts depth back to the far plane inside the mark,
    //      so the room already drawn does not occlude the reflection.
    //   3. `pipeRefl_[blend]` is the ordinary scene state with the stencil TEST
    //      set to EQUAL 1, so the reflection can only land on the mirror.
    //
    // Then the mirror's own faces draw again with their real blend, over the
    // reflection - which is what the blend mode is for (ASSETS 4c).
    auto stencilOp = [](VkStencilOp pass, VkCompareOp cmp, uint32_t wmask) {
        VkStencilOpState o{};
        o.failOp = VK_STENCIL_OP_KEEP;
        o.depthFailOp = VK_STENCIL_OP_KEEP;
        o.passOp = pass;
        o.compareOp = cmp;
        o.compareMask = 0xFF;
        o.writeMask = wmask;
        o.reference = 1;
        return o;
    };
    {
        VkPipelineColorBlendAttachmentState none{};
        none.blendEnable = VK_FALSE;
        none.colorWriteMask = 0;              // marks the stencil, draws nothing
        const VkPipelineColorBlendAttachmentState nones[2] = {none, mvAtt(false)};
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = mvOn_ ? 2 : 1; cb.pAttachments = nones;

        VkPipelineDepthStencilStateCreateInfo sd{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        sd.depthTestEnable = VK_TRUE;
        sd.depthWriteEnable = VK_FALSE;
        sd.depthCompareOp = VK_COMPARE_OP_LESS;
        sd.stencilTestEnable = VK_TRUE;
        sd.front = sd.back = stencilOp(VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xFF);

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &sd;
        gp.pColorBlendState = &cb; gp.layout = plo_; gp.renderPass = pass_;
        VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeStencil_),
                "stencil pipeline");

        // the depth reset: a full-screen triangle, no vertex input at all
        VkShaderModule qv = module(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = module(kFsqFrag, sizeof(kFsqFrag));
        VkPipelineShaderStageCreateInfo qstages[2]{};
        qstages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                      VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        qstages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                      VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo qvi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo qd{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        qd.depthTestEnable = VK_TRUE;
        qd.depthWriteEnable = VK_TRUE;
        qd.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        qd.stencilTestEnable = VK_TRUE;
        qd.front = qd.back = stencilOp(VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0);
        gp.pStages = qstages;
        gp.pVertexInputState = &qvi;
        gp.pDepthStencilState = &qd;
        VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeDepthReset_),
                "depth reset pipeline");
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
    }

    // and the reflection's three, the scene states with the stencil test on
    for (int k = 0; k < 3; ++k) {
        const VkPipelineColorBlendAttachmentState cba = blendState(k);
        VkPipelineDepthStencilStateCreateInfo rd{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        rd.depthTestEnable = VK_TRUE;
        rd.depthWriteEnable = omk::blendRule(kModes[k]).depthWrite ? VK_TRUE : VK_FALSE;
        rd.depthCompareOp = VK_COMPARE_OP_LESS;
        rd.stencilTestEnable = VK_TRUE;
        rd.front = rd.back = stencilOp(VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0);
        const VkPipelineColorBlendAttachmentState cbas[2] = {cba, mvAtt(k == 0)};
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = mvOn_ ? 2 : 1; cb.pAttachments = cbas;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &rd;
        gp.pColorBlendState = &cb; gp.layout = plo_; gp.renderPass = pass_;
        VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeRefl_[k]),
                "reflection pipeline");
    }

    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    if (fsSoft) vkDestroyShaderModule(dev_, fsSoft, nullptr);
    if (fsCount) vkDestroyShaderModule(dev_, fsCount, nullptr);
    if (fsTiled) vkDestroyShaderModule(dev_, fsTiled, nullptr);
    if (fsCut) vkDestroyShaderModule(dev_, fsCut, nullptr);
    if (fsTiledCut) vkDestroyShaderModule(dev_, fsTiledCut, nullptr);

    if (!makeSampler()) return false;

    // 64 sets: the engine's texture pool is 58 slots, plus the white fallback.
    // Plus set 1, the per-frame shadow set, and its one uniform buffer.
    // two samplers a texture's set since the light map (binding 1)
    VkDescriptorPoolSize ps[4] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 531},   // four a texture's set
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}};   // set 1's tiled lights
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    // (+1: the reflection's own set 1, `ds1Refl_`)
    dpi.maxSets = 132; dpi.poolSizeCount = 4; dpi.pPoolSizes = ps;
    // a texture's set is freed on its own when it leaves the pool (`setTextures`)
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKCHECK(vkCreateDescriptorPool(dev_, &dpi, nullptr, &dpool_), "descriptorPool");

    // ---- the POSE BUFFER and its set (set 2) ------------------------------
    // Without it every body is posed on the CPU (`posesBodies` no).
    poseAlign_ = std::max<VkDeviceSize>(16, props_.limits.minStorageBufferOffsetAlignment);
    if (makeBuffer(kPoseBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   poseBuf_, poseMem_) &&
        vkMapMemory(dev_, poseMem_, 0, kPoseBytes, 0, &posePtr_) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = dpool_; sai.descriptorSetCount = 1; sai.pSetLayouts = &dsl2_;
        if (vkAllocateDescriptorSets(dev_, &sai, &poseDs_) == VK_SUCCESS) {
            VkDescriptorBufferInfo bufi{poseBuf_, 0, poseRange_};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet = poseDs_; wr.dstBinding = 0; wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            wr.pBufferInfo = &bufi;
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        } else {
            poseDs_ = VK_NULL_HANDLE;
        }
    }
    if (poseDs_ == VK_NULL_HANDLE)
        std::fprintf(stderr, "vulkan: no pose buffer - every body posed on the CPU\n");

    // ---- the SHADOW pipeline and its one descriptor set -------------------
    if (shFbuf_ != VK_NULL_HANDLE) {
        makeShadowSet();

        // A DEPTH-ONLY pipeline: one vertex stage, no colour attachment, and
        // a depth bias, without which the ground shadows itself in stripes.
        VkShaderModule svs = module(kShadowVert, sizeof(kShadowVert));
        VkPushConstantRange spcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof(float)};
        VkPipelineLayoutCreateInfo spli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        spli.pushConstantRangeCount = 1; spli.pPushConstantRanges = &spcr;
        VKCHECK(vkCreatePipelineLayout(dev_, &spli, nullptr, &shPlo_), "shadow pipelineLayout");
        VkPipelineShaderStageCreateInfo sst{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            VK_SHADER_STAGE_VERTEX_BIT, svs, "main", nullptr};
        VkVertexInputBindingDescription svb{0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription sva{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(GpuVert, x)};
        VkPipelineVertexInputStateCreateInfo svi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        svi.vertexBindingDescriptionCount = 1; svi.pVertexBindingDescriptions = &svb;
        svi.vertexAttributeDescriptionCount = 1; svi.pVertexAttributeDescriptions = &sva;
        VkPipelineInputAssemblyStateCreateInfo sia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        sia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport svp{0, 0, static_cast<float>(kShadowSide),
                       static_cast<float>(kShadowSide), 0.0f, 1.0f};
        VkRect2D ssc{{0, 0}, {kShadowSide, kShadowSide}};
        VkPipelineViewportStateCreateInfo svs2{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        svs2.viewportCount = 1; svs2.pViewports = &svp;
        svs2.scissorCount = 1; svs2.pScissors = &ssc;
        VkPipelineRasterizationStateCreateInfo srs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        srs.polygonMode = VK_POLYGON_MODE_FILL;
        // NO culling: a character model is not a closed solid, and a shadow
        // MAP (an enhancement the engine never had) wants every face that can
        // block the light, whichever way it winds. The engine's own draw does
        // cull back faces, in software (ASSETS 4b, `geom3do.h` kTwoSided).
        srs.cullMode = VK_CULL_MODE_NONE;
        srs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        srs.lineWidth = 1.0f;
        srs.depthBiasEnable = VK_TRUE;
        srs.depthBiasConstantFactor = 1.5f;
        srs.depthBiasSlopeFactor = 2.5f;
        VkPipelineMultisampleStateCreateInfo sms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        sms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo sds{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        sds.depthTestEnable = VK_TRUE; sds.depthWriteEnable = VK_TRUE;
        sds.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo scb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        scb.attachmentCount = 0;
        VkGraphicsPipelineCreateInfo sgp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        sgp.stageCount = 1; sgp.pStages = &sst;
        sgp.pVertexInputState = &svi; sgp.pInputAssemblyState = &sia;
        sgp.pViewportState = &svs2; sgp.pRasterizationState = &srs;
        sgp.pMultisampleState = &sms; sgp.pDepthStencilState = &sds;
        sgp.pColorBlendState = &scb; sgp.layout = shPlo_; sgp.renderPass = shPass_;
        if (vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &sgp, nullptr, &shPipe_)
            != VK_SUCCESS) {
            std::fprintf(stderr, "vulkan: no shadow pipeline - mapped shadows off\n");
            shPipe_ = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(dev_, svs, nullptr);
    }
    return true;
}

// SET 1, made once by `makePipelines` and kept as long as the pool: a
// texture's set now leaves the pool on its own (`FREE_DESCRIPTOR_SET`), and
// nothing resets it. (It was a function because `setTextures` once RESET the
// pool, freeing every set - this one included - and was remade after each:
// the first version, made once, was quietly destroyed by the game's first
// `setTextures` and no shadow was ever drawn, while the synthetic probe's EMPTY
// texture list never reset and worked - the bug was in the difference between
// the two callers, not in the shadow.)
void VulkanRenderer::makeShadowSet() {
    if (shFbuf_ == VK_NULL_HANDLE || dpool_ == VK_NULL_HANDLE) return;
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    sai.descriptorPool = dpool_; sai.descriptorSetCount = 1; sai.pSetLayouts = &dsl1_;
    ds1_ = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev_, &sai, &ds1_) != VK_SUCCESS) {
        std::fprintf(stderr, "vulkan: no shadow descriptor set\n");
        ds1_ = VK_NULL_HANDLE; return;
    }
    VkDescriptorBufferInfo bufi{shUbo_, 0, sizeof(ShadowUbo)};
    VkDescriptorImageInfo imgi{shSampler_, shView_,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo lbufi{litUbo_, 0, sizeof(LightUbo)};
    VkDescriptorBufferInfo pbufi{pointsBuf_, 0, kPointsBytes};
    VkDescriptorBufferInfo tbufi{tilesBuf_, 0, kTileWordsMax * 4};
    // ...and the water REFLECTION's twin, its tiles the buffer's second half
    if (pointsBuf_ && tilesBuf_) {
        if (ds1Refl_) { vkFreeDescriptorSets(dev_, dpool_, 1, &ds1Refl_); ds1Refl_ = VK_NULL_HANDLE; }
        if (vkAllocateDescriptorSets(dev_, &sai, &ds1Refl_) != VK_SUCCESS) ds1Refl_ = VK_NULL_HANDLE;
    }
    VkWriteDescriptorSet wr[5]{};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[0].dstSet = ds1_; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &bufi;
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[1].dstSet = ds1_; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &imgi;
    wr[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[2].dstSet = ds1_; wr[2].dstBinding = 2; wr[2].descriptorCount = 1;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[2].pBufferInfo = &lbufi;
    for (uint32_t k = 3; k < 5; ++k) {
        wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[k].dstSet = ds1_; wr[k].dstBinding = k; wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr[k].pBufferInfo = k == 3 ? &pbufi : &tbufi;
    }
    vkUpdateDescriptorSets(dev_, pointsBuf_ && tilesBuf_ ? 5 : 3, wr, 0, nullptr);
    if (ds1Refl_) {
        VkDescriptorBufferInfo rbufi{tilesBuf_, kTileWordsMax * 4, kTileWordsMax * 4};
        for (auto& w : wr) w.dstSet = ds1Refl_;
        wr[4].pBufferInfo = &rbufi;
        vkUpdateDescriptorSets(dev_, 5, wr, 0, nullptr);
    }
}

// The swapchain, and the presentation path.
//
// The frame is drawn into the SAME offscreen colour attachment the offscreen
// path uses and then BLITTED into the acquired swapchain image. Two reasons,
// and neither is laziness: `readback()` must keep working (it is what the
// checks and the mirror pass use), and the offscreen target is 640x352 while
// the window is 640x480 - the game's camera mode is letterboxed, so the blit
// lands at y=64 and the bands are the clear colour, which is what the engine's
// own frames show.
//
// `vkCmdBlitImage` rather than `vkCmdCopyImage` because the swapchain's format
// is the surface's choice and is usually B8G8R8A8 while the attachment is
// R8G8B8A8; a copy would reinterpret the bytes and swap red for blue, a blit
// converts.
bool VulkanRenderer::makeSwapchain(int w, int h) {
    VkSurfaceCapabilitiesKHR caps{};
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps),
            "surfaceCapabilities");
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf ? nf : 1);
    if (nf) vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &nf, fmts.data());
    if (!nf) { std::fprintf(stderr, "vulkan: no surface format\n"); return false; }
    VkSurfaceFormatKHR pick = fmts[0];
    for (const auto& f : fmts)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            pick = f; break;
        }
    // HDR OUTPUT (`hdr = on`, todo/enhancements.md 22), decided once: when the
    // surface offers scRGB (FP16, linear, BT.709 primaries, 1.0 = 80 nits -
    // `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`, which Windows' drivers list
    // only with the display's HDR switch on) AND the window layer's own look
    // at the output does not say SDR (DXGI, where the build has it); else a
    // line and the SDR swapchain. `OMK_HDR_FORCE=1` takes the HDR passes on
    // any display, to exercise them.
    if (postReq_.hdr && !hdrDecided_) {
        hdrDecided_ = true;
        bool scRgb = false;
        for (const auto& f : fmts)
            if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
                scRgb = true;
        const bool force = omk::envFlag("OMK_HDR_FORCE");
        const bool usable = (props_.limits.maxPushConstantsSize >= sizeof(BlitPush)) && scRgb;
        if (usable && (hdrKnown_ != 0 || force)) {
            hdrOn_ = hdrSwap_ = true;
        } else if (force) {
            hdrOn_ = true;
            std::printf("hdr: OMK_HDR_FORCE on a surface that offers no scRGB swapchain (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) - "
                        "the HDR passes run, the window gets the picture clipped to SDR\n");
        } else if (scRgb) {
            std::printf("hdr: the display reports no HDR (not ST.2084) - the picture stays SDR\n");
        } else {
            std::printf("hdr: the surface offers no scRGB swapchain (VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, "
                        "VK_EXT_swapchain_colorspace) - the picture stays SDR\n");
        }
        presFmt_ = hdrOn_ ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    }
    if (hdrSwap_)
        for (const auto& f : fmts)
            if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
                pick = f; break;
            }
    swapFmt_ = pick.format;
    swapExt_ = caps.currentExtent.width != 0xFFFFFFFFu
             ? caps.currentExtent
             : VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = want;
    sc.imageFormat = swapFmt_;
    sc.imageColorSpace = pick.colorSpace;
    sc.imageExtent = swapExt_;
    sc.imageArrayLayers = 1;
    // TRANSFER_DST, not COLOR_ATTACHMENT: nothing renders into these, the
    // finished frame is blitted in - except an scRGB swapchain's, which the
    // HDR blit draws (the picture is gamma 2.2, the swapchain linear light)
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (hdrSwap_ ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : 0u);
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // FIFO is the one mode every implementation must support, and it is
    // v-synced, which is what a 30 Hz game wants anyway.
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VKCHECK(vkCreateSwapchainKHR(dev_, &sc, nullptr, &swap_), "vkCreateSwapchainKHR");

    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(dev_, swap_, &ni, nullptr);
    swapImgs_.resize(ni);
    vkGetSwapchainImagesKHR(dev_, swap_, &ni, swapImgs_.data());
    if (hdrSwap_) {
        if (!makeHdrBlit()) return false;
        for (uint32_t i = 0; i < ni; ++i) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = swapImgs_[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = swapFmt_;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v = VK_NULL_HANDLE;
            VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &v), "swapchain view");
            swapViews_.push_back(v);
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fi.renderPass = hdrPass_; fi.attachmentCount = 1; fi.pAttachments = &v;
            fi.width = swapExt_.width; fi.height = swapExt_.height; fi.layers = 1;
            VkFramebuffer fb = VK_NULL_HANDLE;
            VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &fb), "swapchain framebuffer");
            swapFbufs_.push_back(fb);
        }
        if (hdrMeta_) {
            // BT.709 primaries and D65 (scRGB's), the display's peak and the paper white
            auto set = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(dev_, "vkSetHdrMetadataEXT"));
            if (set) {
                VkHdrMetadataEXT m{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
                m.displayPrimaryRed = {0.640f, 0.330f}; m.displayPrimaryGreen = {0.300f, 0.600f};
                m.displayPrimaryBlue = {0.150f, 0.060f}; m.whitePoint = {0.3127f, 0.3290f};
                m.maxLuminance = hdrPeak_; m.minLuminance = 0.001f;
                m.maxContentLightLevel = hdrPeak_; m.maxFrameAverageLightLevel = kPaperWhite;
                set(dev_, 1, &swap_, &m);
            }
        }
    }

    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(dev_, &si, nullptr, &acquired_), "semaphore");
    VKCHECK(vkCreateSemaphore(dev_, &si, nullptr, &drawn_), "semaphore");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(dev_, &cai, &pcb_), "present cb");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKCHECK(vkCreateFence(dev_, &fi, nullptr, &pfence_), "present fence");
    std::printf("vulkan: swapchain %ux%u, %u images, format %d\n",
                swapExt_.width, swapExt_.height, ni, static_cast<int>(swapFmt_));
    if (hdrSwap_ && !hdrToldSwap_) {
        hdrToldSwap_ = true;
        std::printf("hdr: on - an ENHANCEMENT: an scRGB (FP16) swapchain, paper white %.0f nits, the "
                    "display's peak %.0f nits\n", static_cast<double>(kPaperWhite), static_cast<double>(hdrPeak_));
    }
    return true;
}

void VulkanRenderer::killSwapchain() {
    if (!dev_) return;
    for (auto f : swapFbufs_) vkDestroyFramebuffer(dev_, f, nullptr);
    for (auto v : swapViews_) vkDestroyImageView(dev_, v, nullptr);
    swapFbufs_.clear(); swapViews_.clear();
    if (pcb_) { vkFreeCommandBuffers(dev_, pool_, 1, &pcb_); pcb_ = VK_NULL_HANDLE; }
    if (pfence_) { vkDestroyFence(dev_, pfence_, nullptr); pfence_ = VK_NULL_HANDLE; }
    if (acquired_) { vkDestroySemaphore(dev_, acquired_, nullptr); acquired_ = VK_NULL_HANDLE; }
    if (drawn_) { vkDestroySemaphore(dev_, drawn_, nullptr); drawn_ = VK_NULL_HANDLE; }
    if (swap_) { vkDestroySwapchainKHR(dev_, swap_, nullptr); swap_ = VK_NULL_HANDLE; }
    swapImgs_.clear();
}

bool VulkanRenderer::remakeSwapchain() {
    if (!dev_ || surface_ == VK_NULL_HANDLE) return false;
    fgSettle();
    vkDeviceWaitIdle(dev_);
    killSwapchain();
    return makeSwapchain(w_, h_);
}

bool VulkanRenderer::resize(int w, int h) {
    if (!dev_ || w <= 0 || h <= 0) return false;
    fgSettle();
    vkDeviceWaitIdle(dev_);
    const auto img = [&](VkImageView& v, VkImage& i, VkDeviceMemory& m) {
        if (v) vkDestroyImageView(dev_, v, nullptr);
        if (i) vkDestroyImage(dev_, i, nullptr);
        if (m) freeDeviceMemory(m);
        v = VK_NULL_HANDLE; i = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
    };
    const auto buf = [&](VkBuffer& b, VkDeviceMemory& m) {
        if (b) vkDestroyBuffer(dev_, b, nullptr);
        if (m) freeDeviceMemory(m);
        b = VK_NULL_HANDLE; m = VK_NULL_HANDLE;
    };
    killFg();   // sized to the picture
    resetOwed_ = true; havePrev_ = false;
    if (fbuf_) { vkDestroyFramebuffer(dev_, fbuf_, nullptr); fbuf_ = VK_NULL_HANDLE; }
    if (pass_) { vkDestroyRenderPass(dev_, pass_, nullptr); pass_ = VK_NULL_HANDLE; }
    for (auto& cp : passCont_) if (cp) { vkDestroyRenderPass(dev_, cp, nullptr); cp = VK_NULL_HANDLE; }
    if (reflPass_) { vkDestroyRenderPass(dev_, reflPass_, nullptr); reflPass_ = VK_NULL_HANDLE; }
    img(colourView_, colour_, colourMem_);
    img(msColourView_, msColour_, msColourMem_);
    img(motionView_, motion_, motionMem_);
    img(tupOutView_, tupOut_, tupOutMem_);
    killTupDepth();
    killSsao();
    killVol();
    if (depthRead_) { vkDestroyImageView(dev_, depthRead_, nullptr); depthRead_ = VK_NULL_HANDLE; }
    if (depthSampleView_) { vkDestroyImageView(dev_, depthSampleView_, nullptr); depthSampleView_ = VK_NULL_HANDLE; }
    img(depthView_, depth_, depthMem_);
    buf(readBuf_, readMem_);
    buf(upBuf_, upMem_);
    killUpscale();   // before the present's pool and layout, which it borrows
    // the present pass reads `colourView_`, so all of it goes; its guard is
    // `presPipe_`, and it is rebuilt at the new size on its next use
    if (presPipe_) { vkDestroyPipeline(dev_, presPipe_, nullptr); presPipe_ = VK_NULL_HANDLE; }
    if (presPlo_) { vkDestroyPipelineLayout(dev_, presPlo_, nullptr); presPlo_ = VK_NULL_HANDLE; }
    if (presSampler_) { vkDestroySampler(dev_, presSampler_, nullptr); presSampler_ = VK_NULL_HANDLE; }
    if (presPool_) { vkDestroyDescriptorPool(dev_, presPool_, nullptr); presPool_ = VK_NULL_HANDLE; }
    if (presDsl_) { vkDestroyDescriptorSetLayout(dev_, presDsl_, nullptr); presDsl_ = VK_NULL_HANDLE; }
    if (presFbuf_) { vkDestroyFramebuffer(dev_, presFbuf_, nullptr); presFbuf_ = VK_NULL_HANDLE; }
    if (presPass_) { vkDestroyRenderPass(dev_, presPass_, nullptr); presPass_ = VK_NULL_HANDLE; }
    img(presView_, presImg_, presMem_);
    buf(presRead_, presReadMem_);
    killOverlay();   // sized to the frame
    killPost();
    killHdrBlit();
    if (stencilSampleView_) { vkDestroyImageView(dev_, stencilSampleView_, nullptr); stencilSampleView_ = VK_NULL_HANDLE; }
    killRmPost();   // it reads `colour_` and is sized by the target
    killReflection();
    w_ = w; h_ = h;
    // (the upscaler is made for ONE display size: made again at the new one, which gives the render size)
    tup_.reset(); tupOn_ = false; tupDiv_ = 1.0f;
    rw_ = rs(w_); rh_ = rs(h_);
    resizing_ = true;
    if (tupKind_ > 0 && !makeTemporalUpscaler())
        std::puts("upscaler: off after the resize - the native frame");
    resizing_ = false;
    srcW_ = tupOn_ ? w_ : rw_; srcH_ = tupOn_ ? h_ : rh_;
    resizing_ = true;
    const bool ok = makeTarget();
    resizing_ = false;
    if (!ok) return false;
    std::printf("vulkan: frame target remade at %dx%d\n", w_, h_);
    return surface_ == VK_NULL_HANDLE || remakeSwapchain();
}

// A RETARGET (OMK's options menu, `todo/graphics-menu.md` 3c): the
// supersample, the upscale and the ambient occlusion asked again on a running
// renderer - the target remade by `resize` at the same size, which reads them
// (`makeTarget`: the render size, the readable depth). Viewport and scissor are
// dynamic state, so the pipelines stand. NOT MSAA: the scene pipelines are
// compiled for the sample count (`makePipelines`), so a different one is
// refused and waits for the next start.
bool VulkanRenderer::retarget(int samples, int ss, float up, float ao) {
    const int want = samples < 2 ? 1 : samples < 4 ? 2 : samples < 8 ? 4 : 8;
    if (dev_ != VK_NULL_HANDLE && want != wantSamples_) return false;
    wantSamples_ = want;
    ss_ = ss < 2 ? 1 : ss < 4 ? 2 : 4;
    up_ = up > 1.0f ? std::min(up, 3.0f) : 1.0f;
    ao_ = ao < 0.0f ? 0.0f : ao > 4.0f ? 4.0f : ao;
    if (up_ > 1.0f && ss_ > 1) {
        std::printf("vulkan: the upscale takes the supersample's place (--ssaa %d ignored)\n", ss_);
        ss_ = 1;
    }
    if (dev_ == VK_NULL_HANDLE) return true;
    const bool ok = resize(w_, h_);
    std::printf("vulkan: retargeted - %dx supersampling, upscale x%.2f, ambient occlusion %.2f%s\n", ss_,
                static_cast<double>(up_), static_cast<double>(ao_), aoOk_ || ao_ <= 0.0f ? "" : " (refused here)");
    return ok;
}

bool VulkanRenderer::presentDirect() {
    return presentImage(colour_, srcW_ ? srcW_ : rw_, srcH_ ? srcH_ : rh_);
}

// The swapchain half of every present: `src`'s top-left `sw x sh`, in
// TRANSFER_SRC layout, blitted into the window's middle rows.
bool VulkanRenderer::presentImage(VkImage src, int sw, int sh, bool defer) {
    fgSettle();   // a deferred real frame is out before the swapchain is touched again
    if (swap_ == VK_NULL_HANDLE) return false;
    // THE WINDOW MAY HAVE CHANGED SIZE - fullscreen, a resize - and a
    // swapchain at the old extent would present stretched or not at all.
    {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) == VK_SUCCESS &&
            caps.currentExtent.width != 0xFFFFFFFFu) {
            if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return false;
            if (caps.currentExtent.width != swapExt_.width ||
                caps.currentExtent.height != swapExt_.height) {
                if (!remakeSwapchain()) return false;
            }
        }
    }
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev_, swap_, UINT64_MAX, acquired_,
                                       VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { remakeSwapchain(); return false; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return false;

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(pcb_, 0);
    vkBeginCommandBuffer(pcb_, &bi);

    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA,
                       VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(pcb_, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(swapImgs_[idx], VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // The letterbox bands. The engine clears to black and the picture occupies
    // the middle rows; clearing the whole image and blitting into the middle
    // reproduces that without a second attachment.
    VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(pcb_, swapImgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &all);

    // The `w_ x h_` frame fitted into the window AT ITS OWN ASPECT, centred:
    // in a window of the frame's own width that is the old rule exactly (the
    // full width, `h_` rows in the middle), and fullscreen or a wider window
    // gets bars rather than a stretched picture.
    const int sW = static_cast<int>(swapExt_.width), sH = static_cast<int>(swapExt_.height);
    int dW = sW, dH = sW * h_ / w_;
    if (dH > sH) { dH = sH; dW = sH * w_ / h_; }
    if (sW == w_ && h_ <= sH) { dW = w_; dH = h_; }
    const int x0 = (sW - dW) / 2;
    const int y0 = (sH - dH) / 2;
    if (hdrSwap_ && presentImageHdr(src, sw, sh, idx, x0, y0, dW, dH)) {
        // drawn by the HDR blit (`blit.frag -DOMK_HDR`), which left the image presentable
    } else {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[0] = {0, 0, 0};
    blit.srcOffsets[1] = {sw, sh, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x0, y0, 0};
    blit.dstOffsets[1] = {x0 + dW, y0 + dH, 1};
    vkCmdBlitImage(pcb_, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   swapImgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   // LINEAR only when there is something to average: at ss 1
                   // this must stay NEAREST or every existing frame changes.
                   1, &blit, ss_ > 1 || up_ > 1.0f ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    barrier(swapImgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(pcb_);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &pcb_;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &drawn_;
    vkResetFences(dev_, 1, &pfence_);
    vkQueueSubmit(queue_, 1, &si, pfence_);

    if (defer && fgThread_.joinable()) {
        // FRAME GENERATION's real frame: blitted (waited for - the command buffer is reused), its present
        // handed to the pacing thread, which sends it half an interval after its in-between frame
        vkWaitForFences(dev_, 1, &pfence_, VK_TRUE, UINT64_MAX);
        {
            std::lock_guard<std::mutex> lk(fgMu_);
            fgPendIdx_ = idx;
            fgDue_ = fgDeferDue_;
            fgPending_ = true;
            fgHurry_ = false;
        }
        fgCv_.notify_all();
        return true;
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &drawn_;
    pi.swapchainCount = 1; pi.pSwapchains = &swap_;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(queue_, &pi);
    vkWaitForFences(dev_, 1, &pfence_, VK_TRUE, UINT64_MAX);
    return true;
}

// Present a CPU-composited frame - the mirror pass's output.
//
// `drawWithMirror` composites on the CPU because the pass lives on the
// boundary (`PORTING` A2) and must work for every backend, so on a frame where
// it ran the finished picture is in a `Surface` and not in the colour
// attachment. Uploading it costs one conversion and one transfer, which is
// what a direct-presented frame otherwise avoids entirely - so the mirror
// frame is the slow one, and a GPU composite (a stencil pass) is the named
// next step rather than something quietly pretended away.
bool VulkanRenderer::presentSurface(const omk::Surface& s) {
    fgSettle();
    if (swap_ == VK_NULL_HANDLE) return false;
    fgCpuFrame();   // FRAME GENERATION: a frame composed on the CPU goes out as it is
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w_) * h_ * 4;
    if (upBuf_ == VK_NULL_HANDLE &&
        !makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    upBuf_, upMem_)) return false;
    void* p = nullptr;
    vkMapMemory(dev_, upMem_, 0, bytes, 0, &p);
    auto* dst = static_cast<unsigned char*>(p);
    const int n = std::min<int>(w_ * h_, static_cast<int>(s.px.size()));
    // 565 -> replicated RGBA8 through the 65536-entry table, the same bytes
    // the per-channel shifts gave (todo/optimization.md step 4).
    const unsigned char* lut = omk::expand565Rgba();
    for (int i = 0; i < n; ++i)
        std::memcpy(dst + 4 * static_cast<std::size_t>(i),
                    lut + 4 * static_cast<std::size_t>(s.px[static_cast<std::size_t>(i)]), 4);
    vkUnmapMemory(dev_, upMem_);

    // UPSCALED, `colour_` is smaller than the picture: it goes into the
    // present pass's output-size image instead, and is presented from there
    const bool up = up_ > 1.0f;
    if (up && !makePresentPass()) return false;
    VkCommandBuffer cb = oneShotBegin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = up ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = up ? presImg_ : colour_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyBufferToImage(cb, upBuf_, up ? presImg_ : colour_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    if (!up) { srcW_ = w_; srcH_ = h_; }   // only the top-left holds the uploaded picture
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    oneShotEnd(cb);
    return up ? presentImage(presImg_, w_, h_) : presentDirect();
}

// ---- THE PRESENT PASS (todo/optimization.md step 4b) --------------------------

bool VulkanRenderer::makePresentPass() {
    if (presPipe_ != VK_NULL_HANDLE) return true;
    const VkFormat fmt = presFmt_;   // FP16 under HDR: the highlights run past 1.0
    {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_DST: `presentSurface` uploads into it when the upscale
        // leaves `colour_` smaller than the output
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!makeImage2D(ii, VK_IMAGE_ASPECT_COLOR_BIT, "present image", presImg_, presMem_, &presView_))
            return false;
    }
    {
        // every pixel is written, so nothing is loaded; it ends where the
        // swapchain blit and the verification copy both read from
        VkAttachmentDescription a{};
        a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        // in: after the last frame's blit or copy read it and `presentSurface`
        // wrote it; out: before the next blit or copy reads it
        VkSubpassDependency dep[2]{};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2; rpi.pDependencies = dep;
        VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &presPass_), "present render pass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = presPass_; fi.attachmentCount = 1; fi.pAttachments = &presView_;
        fi.width = static_cast<uint32_t>(w_); fi.height = static_cast<uint32_t>(h_); fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &presFbuf_), "present framebuffer");
    }
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 1; dli.pBindings = &b;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &presDsl_), "present set layout");
        // its OWN pool: `setTextures` resets the scene's, and this set must
        // survive a texture reload
        // (a second set for the post chain's picture, `makePost`)
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &presPool_), "present pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = presPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &presDsl_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &presDs_), "present set");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK(vkCreateSampler(dev_, &si, nullptr, &presSampler_), "present sampler");
        VkDescriptorImageInfo ii{presSampler_, colourView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = presDs_; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    }
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PresentPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &presDsl_;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &presPlo_), "present pipeline layout");
        const auto module = [&](const uint32_t* code, size_t bytes) {
            VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            si.codeSize = bytes; si.pCode = code;
            VkShaderModule m = VK_NULL_HANDLE;
            vkCreateShaderModule(dev_, &si, nullptr, &m);
            return m;
        };
        VkShaderModule qv = module(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = module(kPresentFrag, sizeof(kPresentFrag));
        if (!qv || !qf) {
            // (the one that was made, not leaked)
            if (qv) vkDestroyShaderModule(dev_, qv, nullptr);
            if (qf) vkDestroyShaderModule(dev_, qf, nullptr);
            std::fprintf(stderr, "vulkan: present shader module\n");
            return false;
        }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = VK_FALSE; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = presPlo_; gp.renderPass = presPass_;
        const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &presPipe_);
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
        if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: present pipeline\n"); return false; }
    }
    return true;   // `presRead_` is `worldPicture`'s, made on its first call
}

// `colour_` (TRANSFER_SRC, as `end()` and `presentSurface` leave it) through the
// present shader into `presImg_`, and back to TRANSFER_SRC.
bool VulkanRenderer::runPresentPass(int vy, int vh, bool mid, bool raw) {
    if (!makePresentPass()) return false;
    if (mid && fgMidDs_ == VK_NULL_HANDLE) return false;
    const bool up = up_ > 1.0f;
    if (up && !makeUpscale()) return false;
    vh = std::clamp(vh, 1, h_);
    // THE POST CHAIN's display-size picture when it ran this frame (the
    // supersample folded already), else the scene's colour - under the remaster's UPSCALE the
    // chain stands down (`postLive_` is false there), and `up` takes the EASU's picture below
    const bool post = !up && postLive_ && postImg_ != VK_NULL_HANDLE && presDsPost_ != VK_NULL_HANDLE;
    // ...or, `mid`, FRAME GENERATION's in-between picture (already in the sampled layout: no transitions)
    const VkImage srcImg = post ? postImg_ : colour_;
    const VkDescriptorSet srcDs = mid ? fgMidDs_ : post ? presDsPost_ : presDs_;
    VkCommandBuffer cb = oneShotBegin();
    // Real scopes both ways (the audit's item 8): the top of the pipe and no
    // access ordered nothing, and only the host's wait on the frame's fence
    // kept it right
    const auto layout = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = srcImg;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    // its last writer: the scene's or the post's pass, or `presentSurface`'s copy
    if (!mid) layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (up) {
        // EASU: the drawn picture, `rs(w_) x rs(vh)` of `colour_`'s top-left,
        // up to the picture's `w_ x vh` in `easuImg_`'s
        VkRenderPassBeginInfo ep{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        ep.renderPass = easuPass_; ep.framebuffer = easuFb_;
        ep.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(vh)}};
        vkCmdBeginRenderPass(cb, &ep, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, easuPipe_);
        const VkViewport vp{0, 0, static_cast<float>(w_), static_cast<float>(vh), 0.0f, 1.0f};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &ep.renderArea);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, easuPlo_, 0, 1, &easuDs_, 0, nullptr);
        std::uint32_t con[16];
        omk::fsrEasuCon(con, static_cast<float>(rs(w_)), static_cast<float>(rs(vh)), static_cast<float>(rw_),
                        static_cast<float>(rh_), static_cast<float>(w_), static_cast<float>(vh));
        vkCmdPushConstants(cb, easuPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof con, con);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);   // `easuImg_` left SHADER_READ_ONLY for the pass below
    }
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = presPass_; rp.framebuffer = presFbuf_;
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, presPipe_);
    const VkDescriptorSet* presIn = up ? &presUpDs_ : &presDs_;
    if (!up && (post || mid)) presIn = &srcDs;   // the post chain's picture, or the in-between one
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, presPlo_, 0, 1, presIn,
                            0, nullptr);
    // (`depth`: 32 passes the picture through; `rcas`/`raw`: the remaster's upscale, `present.frag`)
    const PresentPush pp{vy, vh, w_, dither_ ? 1 : 0, up ? 1 : (post || mid) ? 1 : outSs(), hdrOn_ ? 32 : colourDepth_,
                         up ? 1 : 0, up ? omk::fsrRcasCon(omk::kFsrRcasStops) : 0u, raw ? 1 : 0};
    vkCmdPushConstants(cb, presPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    // after the EASU's or this pass's reads, before the readback's copy or
    // `presentDirect`'s blit
    if (!mid) layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           0, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT);
    oneShotEnd(cb);
    return true;
}

bool VulkanRenderer::presentWorld(int vy, int vh) {
    if (swap_ == VK_NULL_HANDLE) return false;
    // FRAME GENERATION: the in-between picture first, through the same present pass, then the real frame -
    // at once, or half an interval later
    const bool mid = fgMidFrame();
    if (mid && runPresentPass(vy, vh, true)) presentImage(presImg_, w_, h_);
    if (!runPresentPass(vy, vh)) return false;
    return fgPresentReal(mid);
}

bool VulkanRenderer::worldPicture(int vy, int vh, std::vector<unsigned char>& rgba) {
    if (!runPresentPass(vy, vh)) return false;
    return copyPresentOut(rgba);
}

// `presImg_` (TRANSFER_SRC, as the present pass and the overlay leave it) read back
bool VulkanRenderer::copyPresentOut(std::vector<unsigned char>& rgba) {
    if (presImg_ == VK_NULL_HANDLE) return false;
    // (made on the first call: the picture is 8 bytes a pixel on an HDR swapchain)
    if (presRead_ == VK_NULL_HANDLE &&
        !makeBuffer(static_cast<VkDeviceSize>(w_) * h_ * (hdrOn_ ? 8 : 4), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    presRead_, presReadMem_))
        return false;
    VkCommandBuffer cb = oneShotBegin();
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyImageToBuffer(cb, presImg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presRead_, 1, &cp);
    oneShotEnd(cb);
    const std::size_t n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    rgba.resize(n * 4);
    void* p = nullptr;
    if (vkMapMemory(dev_, presReadMem_, 0, n * (hdrOn_ ? 8 : 4), 0, &p) != VK_SUCCESS) return false;
    if (!hdrOn_) {
        std::memcpy(rgba.data(), p, n * 4);
    } else {
        // HDR: the FP16 picture is gamma 2.2 with 1.0 = paper white and its highlights
        // past 1.0 - clipped to 8 bits, as the Direct3D 12 back buffer's dump is
        const auto* h = static_cast<const std::uint16_t*>(p);
        const auto half = [](std::uint16_t v) {
            const int e = (v >> 10) & 31, m = v & 1023;
            float f = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                    : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m + 1024), e - 25);
            return (v & 0x8000) ? -f : f;
        };
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < 3; ++c)
                rgba[4 * i + static_cast<std::size_t>(c)] = static_cast<unsigned char>(
                    std::lround(std::min(std::max(half(h[4 * i + static_cast<std::size_t>(c)]), 0.0f), 1.0f) * 255.0f));
            rgba[4 * i + 3] = 255;
        }
    }
    vkUnmapMemory(dev_, presReadMem_);
    return true;
}

// ---- HDR OUTPUT's blit (`todo/enhancements.md` 22) --------------------------
//
// The Direct3D 12 backend's `blit_hdr`, from the same GLSL: everything this
// backend hands the window - the present pass's picture, a CPU frame, the post
// chain's picture - is gamma 2.2 with 1.0 = paper white, its highlights alone
// running past 1.0, and an scRGB swapchain is linear light with 1.0 = 80 nits.
// So the frame goes to the swapchain through a draw (the viewport the
// destination rectangle, as `vkCmdBlitImage`'s was) that converts it: to linear
// light, scaled to paper white's nits, clamped at the display's peak.

bool VulkanRenderer::makeHdrBlit() {
    if (blitPipe_ != VK_NULL_HANDLE) return true;
    if (!makePresentPass()) return false;   // `presImg_` and its view, at the present image's format
    {
        // the swapchain's image, as the clear before it left it; presentable after
        VkAttachmentDescription a{};
        a.format = swapFmt_; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 1; rpi.pDependencies = &dep;
        VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &hdrPass_), "hdr render pass");
    }
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 1; dli.pBindings = &b;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &blitDsl_), "hdr set layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &blitPool_), "hdr pool");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK(vkCreateSampler(dev_, &si, nullptr, &blitSampler_), "hdr sampler");
        const VkImageView views[2] = {colourView_, presView_};
        for (int i = 0; i < 2; ++i) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = blitPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &blitDsl_;
            VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &blitDs_[i]), "hdr set");
            const VkDescriptorImageInfo ii{blitSampler_, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet = blitDs_[i]; wr.dstBinding = 0; wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        }
    }
    {
        const VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &blitDsl_;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
        VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &blitPlo_), "hdr pipeline layout");
        VkShaderModule qv = shaderModule(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = shaderModule(kBlitHdrFrag, sizeof(kBlitHdrFrag));
        if (!qv || !qf) { std::fprintf(stderr, "vulkan: hdr shader module\n"); return false; }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi;
        gp.layout = blitPlo_; gp.renderPass = hdrPass_;
        const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &blitPipe_);
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
        if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: hdr pipeline\n"); return false; }
    }
    return true;
}

void VulkanRenderer::killHdrBlit() {
    if (!dev_) return;
    if (blitPipe_) { vkDestroyPipeline(dev_, blitPipe_, nullptr); blitPipe_ = VK_NULL_HANDLE; }
    if (blitPlo_) { vkDestroyPipelineLayout(dev_, blitPlo_, nullptr); blitPlo_ = VK_NULL_HANDLE; }
    if (blitSampler_) { vkDestroySampler(dev_, blitSampler_, nullptr); blitSampler_ = VK_NULL_HANDLE; }
    if (blitPool_) { vkDestroyDescriptorPool(dev_, blitPool_, nullptr); blitPool_ = VK_NULL_HANDLE; }
    blitDs_[0] = blitDs_[1] = VK_NULL_HANDLE;
    if (blitDsl_) { vkDestroyDescriptorSetLayout(dev_, blitDsl_, nullptr); blitDsl_ = VK_NULL_HANDLE; }
    if (hdrPass_) { vkDestroyRenderPass(dev_, hdrPass_, nullptr); hdrPass_ = VK_NULL_HANDLE; }
}

// recorded on `pcb_` in `presentImage`, the swapchain's image cleared and in
// TRANSFER_DST: `src` (TRANSFER_SRC, whole) drawn into the destination
// rectangle, the image left presentable. -> false: no HDR blit made (a plain blit follows)
bool VulkanRenderer::presentImageHdr(VkImage src, int sw, int sh, uint32_t idx, int x0, int y0, int dW, int dH) {
    if (blitPipe_ == VK_NULL_HANDLE || idx >= swapFbufs_.size()) return false;
    const bool isPres = src == presImg_;
    const VkDescriptorSet ds = isPres ? blitDs_[1] : blitDs_[0];
    const float texW = static_cast<float>(isPres || tupOn_ ? w_ : rw_), texH = static_cast<float>(isPres || tupOn_ ? h_ : rh_);
    const auto layout = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                            VkPipelineStageFlags ss, VkPipelineStageFlags ds_) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = src; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(pcb_, ss, ds_, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = hdrPass_; rp.framebuffer = swapFbufs_[idx];
    rp.renderArea = {{0, 0}, swapExt_};
    vkCmdBeginRenderPass(pcb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport vp{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(dW), static_cast<float>(dH), 0.0f, 1.0f};
    const VkRect2D sc{{x0, y0}, {static_cast<uint32_t>(dW), static_cast<uint32_t>(dH)}};
    vkCmdSetViewport(pcb_, 0, 1, &vp);
    vkCmdSetScissor(pcb_, 0, 1, &sc);
    vkCmdBindPipeline(pcb_, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipe_);
    vkCmdBindDescriptorSets(pcb_, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPlo_, 0, 1, &ds, 0, nullptr);
    const BlitPush bp{{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(dW), static_cast<float>(dH)},
                      {static_cast<float>(sw), static_cast<float>(sh), texW, texH},
                      outSs() > 1 ? 1 : 0, kPaperWhite / 80.0f, hdrPeak_ / 80.0f};
    vkCmdPushConstants(pcb_, blitPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
    vkCmdDraw(pcb_, 3, 1, 0, 0);
    vkCmdEndRenderPass(pcb_);
    layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return true;
}

// ---- THE POST-PROCESSING CHAIN (`todo/enhancements.md` 22) ----------------
//
// The Direct3D 12 backend's chain (`D3D12Renderer::runPost`) on this one, the
// same GLSL (`bloom.frag`, `post.frag`) and the same numbers (`o3de/postfx.h`),
// recorded on the frame's own command buffer after the scene and SSAO:
//
//   1. BLOOM, when asked: the bright pass into level 0 from the frame (half
//      the display size), then level by level down, then back UP the chain with
//      each tent added onto the larger level - six R11G11B10 levels;
//   2. THE FINAL PASS into `postImg_` (display size): the supersample folded,
//      depth of field on a cinematic frame, linear light with the tone curve's
//      expansion, the bloom added, the shoulder, the grade.
//
// The scene target stays RGBA8 (the headroom is made where it is used: `expand`
// in the shaders), so the pictures agree with Direct3D 12's wherever both run
// the same arithmetic.

namespace {
// the render passes' ordering, generous: a pass reads what the last wrote
// (a sampled level, the frame), and what it wrote is read by the next
const VkSubpassDependency kPostDeps[2] = {
    {VK_SUBPASS_EXTERNAL, 0,
     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
    {0, VK_SUBPASS_EXTERNAL,
     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0}};
}  // namespace

bool VulkanRenderer::makePost() {
    if (postPipe_ != VK_NULL_HANDLE) return true;
    if (!makePresentPass()) return false;   // the descriptor set layout and pool the chain's picture is presented through
    const VkFormat fmtBloom = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    const VkFormat fmtOut = presFmt_;   // the present image's format: FP16 under HDR
    const auto fail = [&](const char* what) { std::fprintf(stderr, "vulkan: post chain: %s\n", what); killPost(); return false; };
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_LINEAR; si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev_, &si, nullptr, &postLin_) != VK_SUCCESS) return fail("sampler");
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(dev_, &si, nullptr, &postNear_) != VK_SUCCESS) return fail("sampler");
    }
    // the images: the display-size picture, the bloom's levels, and - where no
    // depth of field samples - a 1x1 depth and stencil for the table's two slots
    if (!makeImage2d(fmtOut, w_, h_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT, postImg_, postMem_, postView_, "post")) return fail("image");
    if (postReq_.bloom) {
        int bw = w_, bh = h_;
        for (int i = 0; i < omk::postfx::kBloomLevels; ++i) {
            bw = std::max(1, (bw + 1) / 2);
            bh = std::max(1, (bh + 1) / 2);
            bloomW_[i] = bw; bloomH_[i] = bh;
            if (!makeImage2d(fmtBloom, bw, bh, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             bloomImg_[i], bloomMem_[i], bloomView_[i], "post")) return fail("bloom level");
        }
    }
    if (!dofOn_) {
        if (!makeImage2d(VK_FORMAT_R32_SFLOAT, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         dummyDepth_, dummyDepthMem_, dummyDepthView_, "post") ||
            !makeImage2d(VK_FORMAT_R8_UINT, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         dummyStencil_, dummyStencilMem_, dummyStencilView_, "post")) return fail("image");
        VkCommandBuffer cb = oneShotBegin();
        for (VkImage img : {dummyDepth_, dummyStencil_}) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b);
        }
        oneShotEnd(cb);
    }
    // the render passes: a bloom level down (written whole), up (blended onto), the picture
    const auto pass = [&](VkFormat f, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final_,
                          VkRenderPass& out) {
        VkAttachmentDescription a{};
        a.format = f; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = load; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = initial; a.finalLayout = final_;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        rpi.dependencyCount = 2; rpi.pDependencies = kPostDeps;
        return vkCreateRenderPass(dev_, &rpi, nullptr, &out) == VK_SUCCESS;
    };
    if (!pass(fmtOut, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, postPass_))
        return fail("render pass");
    if (postReq_.bloom &&
        (!pass(fmtBloom, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bloomDownPass_) ||
         !pass(fmtBloom, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, bloomUpPass_)))
        return fail("render pass");
    const auto fbuf = [&](VkRenderPass rp, VkImageView v, int fw, int fh, VkFramebuffer& out) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &v;
        fi.width = static_cast<uint32_t>(fw); fi.height = static_cast<uint32_t>(fh); fi.layers = 1;
        return vkCreateFramebuffer(dev_, &fi, nullptr, &out) == VK_SUCCESS;
    };
    if (!fbuf(postPass_, postView_, w_, h_, postFbuf_)) return fail("framebuffer");
    if (postReq_.bloom)
        for (int i = 0; i < omk::postfx::kBloomLevels; ++i)
            if (!fbuf(bloomDownPass_, bloomView_[i], bloomW_[i], bloomH_[i], bloomFbuf_[i])) return fail("framebuffer");
    // the descriptors: the bloom passes read one image; the final pass four
    {
        VkDescriptorSetLayoutBinding b[4]{};
        for (uint32_t i = 0; i < 4; ++i) {
            b[i].binding = i; b[i].descriptorCount = 1;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 1; dli.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &bloomDsl_) != VK_SUCCESS) return fail("set layout");
        dli.bindingCount = 4;
        if (vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &postDsl_) != VK_SUCCESS) return fail("set layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 16; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(dev_, &pi, nullptr, &postPool_) != VK_SUCCESS) return fail("pool");
        const auto alloc = [&](VkDescriptorSetLayout l, VkDescriptorSet& out) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = postPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &l;
            return vkAllocateDescriptorSets(dev_, &ai, &out) == VK_SUCCESS;
        };
        const auto write = [&](VkDescriptorSet ds, uint32_t binding, VkSampler s, VkImageView v, VkImageLayout lay) {
            const VkDescriptorImageInfo ii{s, v, lay};
            VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr.dstSet = ds; wr.dstBinding = binding; wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
            vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        };
        const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (postReq_.bloom) {
            if (!alloc(bloomDsl_, bloomSrcDs_)) return fail("set");
            write(bloomSrcDs_, 0, postLin_, colourView_, ro);
            for (int i = 0; i < omk::postfx::kBloomLevels; ++i) {
                if (!alloc(bloomDsl_, bloomLvlDs_[i])) return fail("set");
                write(bloomLvlDs_[i], 0, postLin_, bloomView_[i], ro);
            }
        }
        if (!alloc(postDsl_, postDs_)) return fail("set");
        write(postDs_, 0, postLin_, colourView_, ro);                               // the frame
        // bloom level 0 (the frame again where there is no bloom: never read)
        write(postDs_, 1, postLin_, postReq_.bloom ? bloomView_[0] : colourView_, ro);
        if (dofOn_) {
            write(postDs_, 2, postNear_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            write(postDs_, 3, postNear_, stencilSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        } else {
            write(postDs_, 2, postNear_, dummyDepthView_, ro);
            write(postDs_, 3, postNear_, dummyStencilView_, ro);
        }
        // the present pass reading the chain's picture
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = presPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &presDsl_;
        if (vkAllocateDescriptorSets(dev_, &ai, &presDsPost_) != VK_SUCCESS) return fail("set");
        write(presDsPost_, 0, presSampler_, postView_, ro);
    }
    // the pipelines: viewport and scissor are dynamic - six sizes of target
    {
        const VkPushConstantRange pb{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(omk::postfx::BloomPush)};
        const VkPushConstantRange pp{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(omk::postfx::PostPush)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &bloomDsl_;
        pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pb;
        if (vkCreatePipelineLayout(dev_, &pli, nullptr, &bloomPlo_) != VK_SUCCESS) return fail("pipeline layout");
        pli.pSetLayouts = &postDsl_; pli.pPushConstantRanges = &pp;
        if (vkCreatePipelineLayout(dev_, &pli, nullptr, &postPlo_) != VK_SUCCESS) return fail("pipeline layout");
        const bool ms = dofOn_ && samples_ != VK_SAMPLE_COUNT_1_BIT;   // the depth read per sample
        VkShaderModule qv = shaderModule(kFsqVert, sizeof(kFsqVert));
        VkShaderModule bf = postReq_.bloom ? shaderModule(kBloomFrag, sizeof(kBloomFrag)) : VK_NULL_HANDLE;
        VkShaderModule pf = ms ? shaderModule(kPostMsFrag, sizeof(kPostMsFrag)) : shaderModule(kPostFrag, sizeof(kPostFrag));
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo msi{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msi.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState none{};
        none.colorWriteMask = 0xF;
        VkPipelineColorBlendAttachmentState add = none;   // back up the chain: added onto the larger level
        add.blendEnable = VK_TRUE;
        add.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; add.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        add.colorBlendOp = VK_BLEND_OP_ADD;
        add.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; add.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        add.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, VK_NULL_HANDLE, "main", nullptr};
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &msi; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi;
        bool ok = qv && pf;
        if (ok && postReq_.bloom) {
            ok = bf != VK_NULL_HANDLE;
            st[1].module = bf; cb.pAttachments = &none;
            gp.layout = bloomPlo_; gp.renderPass = bloomDownPass_;
            ok = ok && vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &bloomDownPipe_) == VK_SUCCESS;
            cb.pAttachments = &add; gp.renderPass = bloomUpPass_;
            ok = ok && vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &bloomUpPipe_) == VK_SUCCESS;
        }
        st[1].module = pf; cb.pAttachments = &none;
        gp.layout = postPlo_; gp.renderPass = postPass_;
        ok = ok && vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &postPipe_) == VK_SUCCESS;
        if (qv) vkDestroyShaderModule(dev_, qv, nullptr);
        if (bf) vkDestroyShaderModule(dev_, bf, nullptr);
        if (pf) vkDestroyShaderModule(dev_, pf, nullptr);
        if (!ok) return fail("pipeline");
    }
    return true;
}

void VulkanRenderer::killPost() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    if (postPipe_) { vkDestroyPipeline(dev_, postPipe_, nullptr); postPipe_ = VK_NULL_HANDLE; }
    if (bloomUpPipe_) { vkDestroyPipeline(dev_, bloomUpPipe_, nullptr); bloomUpPipe_ = VK_NULL_HANDLE; }
    if (bloomDownPipe_) { vkDestroyPipeline(dev_, bloomDownPipe_, nullptr); bloomDownPipe_ = VK_NULL_HANDLE; }
    if (postPlo_) { vkDestroyPipelineLayout(dev_, postPlo_, nullptr); postPlo_ = VK_NULL_HANDLE; }
    if (bloomPlo_) { vkDestroyPipelineLayout(dev_, bloomPlo_, nullptr); bloomPlo_ = VK_NULL_HANDLE; }
    if (postPool_) { vkDestroyDescriptorPool(dev_, postPool_, nullptr); postPool_ = VK_NULL_HANDLE; }
    bloomSrcDs_ = postDs_ = VK_NULL_HANDLE;
    for (auto& d : bloomLvlDs_) d = VK_NULL_HANDLE;
    presDsPost_ = VK_NULL_HANDLE;   // freed with the present pass's pool, which `resize` destroys first
    if (postDsl_) { vkDestroyDescriptorSetLayout(dev_, postDsl_, nullptr); postDsl_ = VK_NULL_HANDLE; }
    if (bloomDsl_) { vkDestroyDescriptorSetLayout(dev_, bloomDsl_, nullptr); bloomDsl_ = VK_NULL_HANDLE; }
    if (postFbuf_) { vkDestroyFramebuffer(dev_, postFbuf_, nullptr); postFbuf_ = VK_NULL_HANDLE; }
    for (auto& f : bloomFbuf_) if (f) { vkDestroyFramebuffer(dev_, f, nullptr); f = VK_NULL_HANDLE; }
    if (postPass_) { vkDestroyRenderPass(dev_, postPass_, nullptr); postPass_ = VK_NULL_HANDLE; }
    if (bloomDownPass_) { vkDestroyRenderPass(dev_, bloomDownPass_, nullptr); bloomDownPass_ = VK_NULL_HANDLE; }
    if (bloomUpPass_) { vkDestroyRenderPass(dev_, bloomUpPass_, nullptr); bloomUpPass_ = VK_NULL_HANDLE; }
    for (int i = 0; i < omk::postfx::kBloomLevels; ++i) freeImage2d(bloomImg_[i], bloomMem_[i], bloomView_[i]);
    freeImage2d(postImg_, postMem_, postView_);
    freeImage2d(dummyDepth_, dummyDepthMem_, dummyDepthView_);
    freeImage2d(dummyStencil_, dummyStencilMem_, dummyStencilView_);
    if (postLin_) { vkDestroySampler(dev_, postLin_, nullptr); postLin_ = VK_NULL_HANDLE; }
    if (postNear_) { vkDestroySampler(dev_, postNear_, nullptr); postNear_ = VK_NULL_HANDLE; }
    postLive_ = false;
}

void VulkanRenderer::runPost() {
    postLive_ = false;
    if (!postWant_ || postFailed_ || enhOff_) return;
    // DEPTH OF FIELD on cinematic frames only; the rest of the chain always
    const bool dof = dofOn_ && foc_.dofLive(postReq_.dof);
    if (!(postReq_.bloom || postReq_.grade > 0 || hdrOn_ || dof)) return;
    if (!makePost()) {
        std::fprintf(stderr, "post: the chain could not be made - the frame without it\n");
        postFailed_ = true;
        return;
    }
    VkCommandBuffer cb = cb_;
    // the frame (TRANSFER_SRC, as the scene pass and SSAO leave it) to a
    // sampled image, and the depth and stencil the scene pass wrote
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = colour_; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 1, &b);
    }
    const float uvMax[2] = {static_cast<float>(foc_.vw) / static_cast<float>(w_),
                            static_cast<float>(foc_.vh) / static_cast<float>(h_)};
    const auto begin = [&](VkRenderPass rp, VkFramebuffer fb, int tw, int th) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {static_cast<uint32_t>(tw), static_cast<uint32_t>(th)}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport vp{0.0f, 0.0f, static_cast<float>(tw), static_cast<float>(th), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(tw), static_cast<uint32_t>(th)}};
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
    };
    if (postReq_.bloom) {
        // DOWN: the bright pass into level 0 from the frame, then level by level
        for (int i = 0; i < omk::postfx::kBloomLevels; ++i) {
            const float iw = i == 0 ? static_cast<float>(w_) : static_cast<float>(bloomW_[i - 1]);
            const float ih = i == 0 ? static_cast<float>(h_) : static_cast<float>(bloomH_[i - 1]);
            begin(bloomDownPass_, bloomFbuf_[i], bloomW_[i], bloomH_[i]);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomDownPipe_);
            const VkDescriptorSet ds = i == 0 ? bloomSrcDs_ : bloomLvlDs_[i - 1];
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPlo_, 0, 1, &ds, 0, nullptr);
            const omk::postfx::BloomPush bp = omk::postfx::bloomDown(i, iw, ih, bloomW_[i], bloomH_[i], uvMax);
            vkCmdPushConstants(cb, bloomPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRenderPass(cb);
        }
        // UP: each level's tent added onto the next larger
        for (int i = omk::postfx::kBloomLevels - 2; i >= 0; --i) {
            begin(bloomUpPass_, bloomFbuf_[i], bloomW_[i], bloomH_[i]);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomUpPipe_);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bloomPlo_, 0, 1, &bloomLvlDs_[i + 1], 0, nullptr);
            const omk::postfx::BloomPush bp = omk::postfx::bloomUp(bloomW_[i + 1], bloomH_[i + 1], bloomW_[i], bloomH_[i], uvMax);
            vkCmdPushConstants(cb, bloomPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bp, &bp);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRenderPass(cb);
        }
    }
    // THE FINAL PASS
    begin(postPass_, postFbuf_, w_, h_);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, postPlo_, 0, 1, &postDs_, 0, nullptr);
    const omk::postfx::PostPush pp = omk::postfx::buildPost(postReq_, foc_, dof, w_, h_, rw_, rh_, outSs(),
                                                            hdrOn_ ? hdrPeak_ : 0.0f);
    vkCmdPushConstants(cb, postPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    // the frame back to where the rest of the backend expects it
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = colour_; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    }
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

bool VulkanRenderer::makeImage2d(VkFormat f, int w, int h, VkImageUsageFlags use, VkImage& img,
                                 VkDeviceMemory& mem, VkImageView& view, const char* label) {
    // (through `makeImage2D`, the one checked image helper: a refusal says so once and fails cleanly)
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f;
    ii.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = use; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return makeImage2D(ii, VK_IMAGE_ASPECT_COLOR_BIT, label, img, mem, &view);
}

void VulkanRenderer::freeImage2d(VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    if (view) vkDestroyImageView(dev_, view, nullptr);
    if (img) vkDestroyImage(dev_, img, nullptr);
    if (mem) freeDeviceMemory(mem);
    view = VK_NULL_HANDLE; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
}

VkShaderModule VulkanRenderer::shaderModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = bytes; si.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev_, &si, nullptr, &m);
    return m;
}

// ---- THE OVERLAY AT COLOUR DEPTH 32 (`todo/enhancements.md` 21) -----------
//
// The Direct3D 12 backend's overlay present on this one: the present pass
// writes the world into `presImg_`, then `overlay.frag` blends the interface's
// two planes over it - C (the 565 frame, R16_UINT) and M (R8) - as
// `C + world * M` with the colour fade, (ONE, SRC_ALPHA), and the result goes
// to the window like any present. The same GLSL as D3D12 compiles to this
// SPIR-V directly.

bool VulkanRenderer::makeOverlayPass() {
    if (ovPipe_ == VK_NULL_HANDLE) {
        if (!makePresentPass()) return false;   // `presImg_`, its framebuffer
        {
            // loaded, blended over, left where the present's blit reads it
            VkAttachmentDescription a{};
            a.format = presFmt_; a.samples = VK_SAMPLE_COUNT_1_BIT;
            a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            a.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkSubpassDescription sub{};
            sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
            VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            rpi.attachmentCount = 1; rpi.pAttachments = &a;
            rpi.subpassCount = 1; rpi.pSubpasses = &sub;
            VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &ovPass_), "overlay render pass");
        }
        {
            VkDescriptorSetLayoutBinding b[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                b[i].binding = i; b[i].descriptorCount = 1;
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dli.bindingCount = 2; dli.pBindings = b;
            VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &ovDsl_), "overlay set layout");
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
            VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &ovPool_), "overlay pool");
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = ovPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &ovDsl_;
            VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &ovDs_), "overlay set");
            VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            VKCHECK(vkCreateSampler(dev_, &si, nullptr, &ovSampler_), "overlay sampler");
        }
        {
            VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(OverlayPush)};
            VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pli.setLayoutCount = 1; pli.pSetLayouts = &ovDsl_;
            pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
            VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &ovPlo_), "overlay pipeline layout");
            VkShaderModule qv = shaderModule(kFsqVert, sizeof(kFsqVert));
            VkShaderModule qf = shaderModule(kOverlayFrag, sizeof(kOverlayFrag));
            if (!qv || !qf) { std::fprintf(stderr, "vulkan: overlay shader module\n"); return false; }
            VkPipelineShaderStageCreateInfo st[2]{};
            st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
            st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport vp{0, 0, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
            VkRect2D sc{{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
            VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            // new = overlay + world x weight (ONE, SRC_ALPHA); alpha stays the world's
            VkPipelineColorBlendAttachmentState cba{};
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = 1; cb.pAttachments = &cba;
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.stageCount = 2; gp.pStages = st;
            gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
            gp.pViewportState = &vps; gp.pRasterizationState = &rs;
            gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
            gp.layout = ovPlo_; gp.renderPass = ovPass_;
            const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &ovPipe_);
            vkDestroyShaderModule(dev_, qv, nullptr);
            vkDestroyShaderModule(dev_, qf, nullptr);
            if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: overlay pipeline\n"); return false; }
        }
    }
    if (ovPic_ != VK_NULL_HANDLE) return true;
    if (!makeImage2d(VK_FORMAT_R16_UINT, w_, h_, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     ovPic_, ovPicMem_, ovPicView_, "overlay colour") ||
        !makeImage2d(VK_FORMAT_R8_UNORM, w_, h_, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     ovMask_, ovMaskMem_, ovMaskView_, "overlay mask")) return false;
    const VkDescriptorImageInfo ii[2] = {
        {ovSampler_, ovPicView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {ovSampler_, ovMaskView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = ovDs_; wr[i].dstBinding = i; wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
    ovMaskOff_ = (static_cast<VkDeviceSize>(w_) * h_ * 2 + 15) & ~static_cast<VkDeviceSize>(15);
    return makeBuffer(ovMaskOff_ + static_cast<VkDeviceSize>(w_) * h_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      ovBuf_, ovBufMem_);
}

void VulkanRenderer::killOverlay() {
    if (!dev_) return;
    if (ovPipe_) { vkDestroyPipeline(dev_, ovPipe_, nullptr); ovPipe_ = VK_NULL_HANDLE; }
    if (ovPlo_) { vkDestroyPipelineLayout(dev_, ovPlo_, nullptr); ovPlo_ = VK_NULL_HANDLE; }
    if (ovSampler_) { vkDestroySampler(dev_, ovSampler_, nullptr); ovSampler_ = VK_NULL_HANDLE; }
    if (ovPool_) { vkDestroyDescriptorPool(dev_, ovPool_, nullptr); ovPool_ = VK_NULL_HANDLE; ovDs_ = VK_NULL_HANDLE; }
    if (ovDsl_) { vkDestroyDescriptorSetLayout(dev_, ovDsl_, nullptr); ovDsl_ = VK_NULL_HANDLE; }
    if (ovPass_) { vkDestroyRenderPass(dev_, ovPass_, nullptr); ovPass_ = VK_NULL_HANDLE; }
    freeImage2d(ovPic_, ovPicMem_, ovPicView_);
    freeImage2d(ovMask_, ovMaskMem_, ovMaskView_);
    if (ovBuf_) { vkDestroyBuffer(dev_, ovBuf_, nullptr); ovBuf_ = VK_NULL_HANDLE; }
    if (ovBufMem_) { freeDeviceMemory(ovBufMem_); ovBufMem_ = VK_NULL_HANDLE; }
}

bool VulkanRenderer::presentOverlay(const omk::Surface& s, const std::uint8_t* mask, const float fade[4],
                                    int vy, int vh) {
    if (swap_ == VK_NULL_HANDLE || s.w != w_ || s.h != h_ || !mask) return false;
    if (!makeOverlayPass()) return false;
    // FRAME GENERATION: an in-between world, if one is due, under the SAME interface (the 2D is never
    // interpolated: this frame's, over both)
    const bool mid = fgMidFrame();
    // both planes up, whole: ~3 bytes a pixel against the CPU path's readback,
    // dither and RGBA upload of the entire frame
    const std::size_t n = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_);
    void* p = nullptr;
    if (vkMapMemory(dev_, ovBufMem_, 0, ovMaskOff_ + n, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, s.px.data(), n * 2);
    std::memcpy(static_cast<unsigned char*>(p) + ovMaskOff_, mask, n);
    vkUnmapMemory(dev_, ovBufMem_);
    {
        VkCommandBuffer cb = oneShotBegin();
        const auto up = [&](VkImage img, VkDeviceSize off) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy cp{};
            cp.bufferOffset = off;
            cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
            vkCmdCopyBufferToImage(cb, ovBuf_, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &b);
        };
        up(ovPic_, 0);
        up(ovMask_, ovMaskOff_);
        oneShotEnd(cb);
    }
    if (mid && runPresentPass(vy, vh, true)) {
        ovDraw(fade);
        presentImage(presImg_, w_, h_);
    }
    if (!runPresentPass(vy, vh)) return false;
    ovDraw(fade);
    return fgPresentReal(mid);
}

// The overlay's planes blended over the present pass's picture in `presImg_`.
void VulkanRenderer::ovDraw(const float fade[4]) {
    VkCommandBuffer cb = oneShotBegin();
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = ovPass_; rp.framebuffer = presFbuf_;   // render-pass COMPATIBLE with the present's
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ovPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ovPlo_, 0, 1, &ovDs_, 0, nullptr);
    const OverlayPush op{{fade[0], fade[1], fade[2], fade[3]}};
    vkCmdPushConstants(cb, ovPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof op, &op);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    oneShotEnd(cb);
}

// Load `colour_` with a `w_ x h_` RGBA8 picture - the probe's way of asking the
// pass about colours no scene is guaranteed to contain.
bool VulkanRenderer::probeUpload(const unsigned char* rgba) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w_) * h_ * 4;
    if (upBuf_ == VK_NULL_HANDLE &&
        !makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    upBuf_, upMem_)) return false;
    void* p = nullptr;
    if (vkMapMemory(dev_, upMem_, 0, bytes, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(p, rgba, static_cast<std::size_t>(bytes));
    vkUnmapMemory(dev_, upMem_);
    VkCommandBuffer cb = oneShotBegin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = colour_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyBufferToImage(cb, upBuf_, colour_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    oneShotEnd(cb);
    return true;
}

// The upscaler's depth image and the pass that fills it (`tupDepth_`): made with the target, at the render size.
bool VulkanRenderer::makeTupDepth() {
    if (!makeImage2d(VK_FORMAT_R32_SFLOAT, rw_, rh_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     tupDepth_, tupDepthMem_, tupDepthView_, "upscaler depth")) return false;
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R32_SFLOAT; a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;   // (every texel is written)
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = 0; dep.dstSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1; rpi.pAttachments = &a;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1; rpi.pDependencies = &dep;
    VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &tdPass_), "upscaler depth pass");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = tdPass_; fi.attachmentCount = 1; fi.pAttachments = &tupDepthView_;
    fi.width = static_cast<uint32_t>(rw_); fi.height = static_cast<uint32_t>(rh_); fi.layers = 1;
    VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &tdFb_), "upscaler depth framebuffer");
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorCount = 1;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1; dli.pBindings = &b;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &tdDsl_), "upscaler depth set layout");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
    VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &tdPool_), "upscaler depth pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = tdPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &tdDsl_;
    VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &tdDs_), "upscaler depth set");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK(vkCreateSampler(dev_, &si, nullptr, &tdSampler_), "upscaler depth sampler");
    // (the scene's depth, sampled where the scene pass leaves it)
    const VkDescriptorImageInfo ii{tdSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = tdDs_; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    const VkPushConstantRange* noRange = nullptr;
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &tdDsl_;
    pli.pushConstantRangeCount = 0; pli.pPushConstantRanges = noRange;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &tdPlo_), "upscaler depth pipeline layout");
    VkShaderModule qv = shaderModule(kFsqVert, sizeof(kFsqVert));
    VkShaderModule qf = shaderModule(kDepthCopyFrag, sizeof(kDepthCopyFrag));
    if (!qv || !qf) { std::puts("vulkan: upscaler depth shader module"); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, static_cast<float>(rw_), static_cast<float>(rh_), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.layout = tdPlo_; gp.renderPass = tdPass_;
    const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &tdPipe_);
    vkDestroyShaderModule(dev_, qv, nullptr);
    vkDestroyShaderModule(dev_, qf, nullptr);
    if (pr != VK_SUCCESS) { std::puts("vulkan: upscaler depth pipeline"); return false; }
    return true;
}

void VulkanRenderer::killTupDepth() {
    if (!dev_) return;
    if (tdPipe_) { vkDestroyPipeline(dev_, tdPipe_, nullptr); tdPipe_ = VK_NULL_HANDLE; }
    if (tdPlo_) { vkDestroyPipelineLayout(dev_, tdPlo_, nullptr); tdPlo_ = VK_NULL_HANDLE; }
    if (tdSampler_) { vkDestroySampler(dev_, tdSampler_, nullptr); tdSampler_ = VK_NULL_HANDLE; }
    if (tdPool_) { vkDestroyDescriptorPool(dev_, tdPool_, nullptr); tdPool_ = VK_NULL_HANDLE; tdDs_ = VK_NULL_HANDLE; }
    if (tdDsl_) { vkDestroyDescriptorSetLayout(dev_, tdDsl_, nullptr); tdDsl_ = VK_NULL_HANDLE; }
    if (tdFb_) { vkDestroyFramebuffer(dev_, tdFb_, nullptr); tdFb_ = VK_NULL_HANDLE; }
    if (tdPass_) { vkDestroyRenderPass(dev_, tdPass_, nullptr); tdPass_ = VK_NULL_HANDLE; }
    if (tupDepthView_) vkDestroyImageView(dev_, tupDepthView_, nullptr);
    if (tupDepth_) vkDestroyImage(dev_, tupDepth_, nullptr);
    if (tupDepthMem_) freeDeviceMemory(tupDepthMem_);
    tupDepthView_ = VK_NULL_HANDLE; tupDepth_ = VK_NULL_HANDLE; tupDepthMem_ = VK_NULL_HANDLE;
}

// ---- THE TEMPORAL UPSCALER (`upscale_vk.h`, `todo/enhancements.md` 18) ----------
//
// Made once the device exists, for the display size: DLSS when asked and the GPU
// runs it, else FSR 3.1, else nothing - each step said. With one running the
// RENDER size is the SDK's answer for the quality (the scene, SSAO and the
// volumetrics draw at it, `rs`), supersampling and the remaster's FSR 1 upscale
// give way (all three decide the render size; the temporal upscaler is the one
// asked for), and so does MSAA: the SDKs take a single-sampled colour and depth,
// and a temporal upscaler's own accumulation IS the anti-aliasing.
bool VulkanRenderer::makeTemporalUpscaler() {
    namespace up = omk::vkup;
    tup_.reset();
    tupOn_ = false;
    tupDiv_ = 1.0f;
    if (tupKind_ == up::kOff || !dev_) return false;
    // what the scene pass must write for it: the motion vectors (the `_mv` builds, their push block, the
    // targets' separate blend) and a depth the SDK can sample
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    const char* why = nullptr;
    if (!tupFeat_) why = "the device cannot blend its colour and motion targets apart (independentBlend)";
    else if (props_.limits.maxPushConstantsSize < sizeof(PushMv))
        why = "the device's push constants are too small for the motion block";
    else if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
             !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        why = "the 32-bit float depth cannot be sampled";
    else {
        VkFormatProperties dp{};
        vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R32_SFLOAT, &dp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if ((dp.optimalTilingFeatures & need) != need) why = "a 32-bit float colour image cannot be drawn and sampled";
    }
    if (why) {
        if (!resizing_) std::printf("upscaler: %s - the native frame\n", why);
        return false;
    }
    up::Device d;
    d.instance = inst_; d.phys = phys_; d.device = dev_; d.queue = queue_; d.family = qfam_;
    d.getInstanceProcAddr = vkGetInstanceProcAddr;
    d.getDeviceProcAddr = vkGetDeviceProcAddr;
    d.vendorId = props_.vendorID;
    d.extraQueueStart = dlssQueueStart_;
    if (tupKind_ == up::kDlss) {
        if (dlssFeat_) tup_ = up::makeDlss(d, w_, h_, tupQuality_);
        if (!tup_) std::printf("upscaler: DLSS not available - trying FSR\n");
    }
    if (!tup_) tup_ = up::makeFsr(d, w_, h_, tupQuality_);
    if (!tup_) {
        std::printf("upscaler: none available - the native frame\n");
        return false;
    }
    if (ss_ > 1) {
        std::printf("upscaler: supersampling x%d ignored - the upscaler decides the render size\n", ss_);
        ss_ = 1;
    }
    if (up_ > 1.0f) {
        std::printf("upscaler: the remaster's upscale (FSR 1, x%.2f) ignored - the upscaler decides the "
                    "render size\n", static_cast<double>(up_));
        up_ = 1.0f;
    }
    if (wantSamples_ > 1) {
        std::printf("upscaler: %dx MSAA off - the upscaler takes one sample a pixel, and its temporal "
                    "resolve is the anti-aliasing\n", wantSamples_);
        wantSamples_ = 1;
    }
    int rw = 0, rh = 0;
    tup_->renderSize(rw, rh);
    rw = std::clamp(rw, 1, w_);
    tupDiv_ = static_cast<float>(w_) / static_cast<float>(rw);
    tupOn_ = true;
    rw_ = rs(w_);
    rh_ = rs(h_);   // (the scene's own size rule, so the viewport and the target agree to the pixel)
    std::printf("upscaler: %s %s - render %dx%d -> display %dx%d (ratio %.2f, %s)\n", tup_->name(),
                tup_->version().c_str(), rw_, rh_, w_, h_, static_cast<double>(w_) / rw_,
                up::qualityName(tupQuality_));
    omk::event("upscaler", {{"sdk", tup_->name()}, {"version", tup_->version()}, {"rw", rw_}, {"rh", rh_},
                            {"dw", w_}, {"dh", h_}, {"quality", up::qualityName(tupQuality_)}, {"api", "vulkan"}});
    jitterIndex_ = 0;
    havePrev_ = false;
    resetOwed_ = true;
    return true;
}

// The upscale itself, recorded on the frame's command buffer after SSAO and the volumetrics: the colour,
// depth and motion at the render size in, the display-size image out - and that copied back over
// `colour_`, which the rest of the backend then reads as the finished display-size picture.
void VulkanRenderer::upscale() {
    VkCommandBuffer cb = cb_;
    const auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                             VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS,
                             VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = {aspect, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    const VkImageAspectFlags colourAsp = VK_IMAGE_ASPECT_COLOR_BIT;
    // what the scene pass, SSAO and the volumetrics wrote - the colour, the depth, the motion - visible to the SDK
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    // the inputs to the layout the SDK is told they are in, the output to GENERAL
    barrier(colour_, colourAsp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            0, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // the depth: drawn into the plain R32_SFLOAT image the SDK takes (see `tupDepth_`); `depth_` stays where the
    // scene pass left it, read-only, which is where the pass samples it
    {
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = tdPass_; rp.framebuffer = tdFb_;
        rp.renderArea = {{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
        vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tdPipe_);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tdPlo_, 0, 1, &tdDs_, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);   // `tupDepth_` left SHADER_READ_ONLY, visible to the SDK (the pass's dependency)
    }
    barrier(tupOut_, colourAsp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    omk::vkup::Frame f;
    f.cb = cb;
    const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    f.colour = {colour_, colourView_, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(w_), static_cast<uint32_t>(h_),
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_SAMPLED_BIT, ro};
    f.depth = {tupDepth_, tupDepthView_, VK_FORMAT_R32_SFLOAT, static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_),
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ro};
    f.motion = {motion_, motionView_, VK_FORMAT_R16G16_SFLOAT, static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_),
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, ro};
    f.output = {tupOut_, tupOutView_, VK_FORMAT_R8G8B8A8_UNORM, static_cast<uint32_t>(w_), static_cast<uint32_t>(h_),
                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_LAYOUT_GENERAL};
    f.rw = rw_; f.rh = rh_; f.dw = w_; f.dh = h_;
    f.jitterX = jitterPx_[0]; f.jitterY = jitterPx_[1];
    f.reset = resetOwed_;
    f.frameMs = 1000.0f / 30.0f;   // the game's tick
    // THE CAMERA, for the SDK that wants it (DLSS), in the Y-UP clip space the Direct3D 12 glue gives it: this
    // engine's matrices are Vulkan's Y-down ones, whose second row is negated here (`upscale_vk.h`)
    omk::vkup::Camera& c = f.cam;
    const float nearP = omk::kNearCut, farP = 200000.0f;
    c.nearP = nearP;
    c.farP = farP;
    c.fovV = 2.0f * std::atan(aoTanV_);
    c.aspect = aoTanH_ / aoTanV_;
    const float* ax[3] = {mainS_, mainU_, mainF_};
    std::memset(c.worldToView, 0, sizeof c.worldToView);
    for (int r = 0; r < 3; ++r) {
        float dd = 0.0f;
        for (int k = 0; k < 3; ++k) { c.worldToView[k * 4 + r] = ax[r][k]; dd += ax[r][k] * frameEye_[k]; }
        c.worldToView[12 + r] = -dd;
    }
    c.worldToView[15] = 1.0f;
    std::memset(c.viewToClip, 0, sizeof c.viewToClip);
    c.viewToClip[0] = 1.0f / aoTanH_;
    c.viewToClip[5] = 1.0f / aoTanV_;   // -(-1 / tanV): the flip
    c.viewToClip[10] = farP / (farP - nearP);
    c.viewToClip[14] = -farP * nearP / (farP - nearP);
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
    const bool ok = tup_->dispatch(f);
    if (!ok) {
        // the SDK refused (it said so): the render-size picture scaled up instead, so the frame is still whole
        barrier(colour_, colourAsp, ro, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(tupOut_, colourAsp, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit bl{};
        bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {rw_, rh_, 1};
        bl.dstOffsets[1] = {w_, h_, 1};
        vkCmdBlitImage(cb, colour_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tupOut_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &bl, VK_FILTER_LINEAR);
        barrier(tupOut_, colourAsp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(colour_, colourAsp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    } else {
        barrier(tupOut_, colourAsp, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(colour_, colourAsp, ro, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    VkImageCopy cp{};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyImage(cb, tupOut_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colour_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    // the picture where the rest of the backend expects it (TRANSFER_SRC), the depth where the post chain reads it
    barrier(colour_, colourAsp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    ++upFrames_;
}

// `OMK_MOTION_STATS=1`: the frame's motion target read back and summed up - how much of the picture moves
// more than half a render pixel, the largest and the mean vector - as a line and an `upscaler.motion`
// event, the Direct3D 12 backend's (`engine: upscaler`, `engine: vulkan upscaler`). Called after the
// frame is submitted and waited for.
void VulkanRenderer::motionStats() {
    if (motion_ == VK_NULL_HANDLE) return;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rw_) * rh_ * 4;   // R16G16: four bytes a pixel
    VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
    if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, mem)) return;
    VkCommandBuffer cb = oneShotBegin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = motion_; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_), 1};
    vkCmdCopyImageToBuffer(cb, motion_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    oneShotEnd(cb);
    void* p = nullptr;
    if (vkMapMemory(dev_, mem, 0, bytes, 0, &p) == VK_SUCCESS) {
        const auto half = [](std::uint16_t h) {
            const std::uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
            float v = e == 0 ? std::ldexp(static_cast<float>(m), -24)
                    : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m | 1024u), static_cast<int>(e) - 25);
            return s ? -v : v;
        };
        const auto* px = static_cast<const std::uint16_t*>(p);
        long moving = 0;
        double sum = 0.0, most = 0.0;
        const long n = static_cast<long>(rw_) * rh_;
        for (long i = 0; i < n; ++i) {
            const double mx = half(px[2 * i]) * rw_, my = half(px[2 * i + 1]) * rh_;
            const double len = std::sqrt(mx * mx + my * my);
            sum += len;
            most = std::max(most, len);
            moving += len > 0.5;
        }
        vkUnmapMemory(dev_, mem);
        const double share = n ? static_cast<double>(moving) / n : 0.0;
        std::printf("vulkan motion: frame %ld - %.2f%% of pixels move more than half a pixel, largest %.1f px, "
                    "mean %.3f px%s\n", upFrames_, 100.0 * share, most, n ? sum / n : 0.0,
                    resetOwed_ ? " (a cut: no motion)" : "");
        omk::event("upscaler.motion", {{"frame", upFrames_}, {"moving", share}, {"max", most},
                                       {"mean", n ? sum / n : 0.0}, {"reset", resetOwed_}});
    }
    vkDestroyBuffer(dev_, buf, nullptr);
    freeDeviceMemory(mem);
}

bool VulkanRenderer::init(int w, int h) {
    w_ = w; h_ = h;
    if (up_ > 1.0f && ss_ > 1) {
        std::printf("vulkan: the upscale takes the supersample's place (--ssaa %d ignored)\n", ss_);
        ss_ = 1;
    }
    rw_ = rs(w_); rh_ = rs(h_);
    if (up_ > 1.0f) {
        std::printf("vulkan: the frame drawn at %dx%d and upscaled to %dx%d by FSR 1 (x%.2f) - "
                    "a REMASTER feature\n", rw_, rh_, w_, h_, static_cast<double>(up_));
        omk::event("vulkan.upscale", {{"factor", static_cast<double>(up_)}, {"drawW", static_cast<long>(rw_)},
                                      {"drawH", static_cast<long>(rh_)}, {"outW", static_cast<long>(w_)},
                                      {"outH", static_cast<long>(h_)}});
    }
    srcW_ = rw_; srcH_ = rh_;
    if (!pickDevice()) return false;
    // THE TEMPORAL UPSCALER (`makeTemporalUpscaler`): decides the render size, so before the target
    if (tupKind_ > 0 && makeTemporalUpscaler()) srcW_ = w_, srcH_ = h_;
    // HYBRID RAY TRACING, before the pipelines it changes (`rtInit`), and its REFLECTIONS on the
    // ground (`todo/enhancements.md` 26)
    if (rtFeat_) rtInit();
    if (rtWantRefl_) {
        rtReflOn_ = rtOn_;
        if (rtOn_) {
            std::printf("vulkan: RT REFLECTIONS on - an ENHANCEMENT: the ground wet in patches, one traced "
                        "ray a pixel mirroring the set, the bodies and the neon\n");
            omk::event("rt_refl", {{"on", 1}});
        } else {
            std::printf("vulkan: rt reflections asked for, but the ray tracing is not live - off\n");
        }
    }
    if (!makeTarget()) return false;
    if (!makePipelines()) return false;
    // The window is 640x480 while the 3D target is the letterboxed 640x352.
    if (surface_ != VK_NULL_HANDLE && !makeSwapchain(w, 480)) return false;
    if (fgOn_ && !fgStart()) fgOn_ = false;
    return true;
}

void VulkanRenderer::setPreloadTextures(std::span<const omk::Texture> t) {
    const auto keyOf = [](const omk::Texture& x) {
        return std::make_tuple(x.idx.data(), x.pal.data(), x.width, x.height);
    };
    for (auto& [key, u] : uploaded_) u.pinned = false;
    preQueue_.clear();
    for (const auto& x : t) {
        if (!x.hasPixels()) continue;
        if (auto hit = uploaded_.find(keyOf(x)); hit != uploaded_.end()) hit->second.pinned = true;
        else preQueue_.push_back(x);   // shares its pixel buffers: the key stays
    }
}

int VulkanRenderer::preloadStep(int budget) {
    if (preQueue_.empty() || budget <= 0) return static_cast<int>(preQueue_.size());
    const std::size_t n = std::min(preQueue_.size(), static_cast<std::size_t>(budget));
    std::vector<omk::Texture> now(preQueue_.end() - static_cast<std::ptrdiff_t>(n), preQueue_.end());
    preQueue_.resize(preQueue_.size() - n);
    poolTextures(now, true);
    return static_cast<int>(preQueue_.size());
}

void VulkanRenderer::poolTextures(std::span<const omk::Texture> t, bool preload) {
    // One VkImage and one descriptor set per material, plus a white 1x1 for a
    // material with no texture - which `raster.cpp` handles by leaving the
    // texel at 255 and letting the vertex colour stand alone.
    // `fill` writes tw*th RGBA texels, alpha the colour key
    // `withSet` false: the image alone - a light map, a normal map, a 1x1
    // stand-in, bound into ANOTHER texture's set (each had one allocated,
    // written and freed at once)
    // `one`: ONE channel (R8), `fill` writing a byte a texel - a light map,
    // which the shader reads `.r` of; a quarter of its memory as RGBA (each
    // 1024x1024 map 4 MB -> 1 MB). Only where the device blits and filters
    // R8 (`r8Ok_`), else RGBA as before.
    // an uploaded image's view, and its descriptor set unless `withSet` false
    auto viewAndSet = [&](Tex& out, VkFormat fmt, uint32_t levels, bool withSet) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        if (vkCreateImageView(dev_, &vi, nullptr, &out.view) != VK_SUCCESS) {
            // no view, no texture: it draws white (and a map stands aside)
            out.view = VK_NULL_HANDLE;
            freeDeviceMemory(out.mem);
            vkDestroyImage(dev_, out.img, nullptr);
            out.img = VK_NULL_HANDLE; out.mem = VK_NULL_HANDLE;
            return;
        }
        if (!withSet) return;
        // HYBRID RAY TRACING reads it bindless, from a table of its own
        if (rtOn_) out.rt = rtAllocTex(out.view);

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl_;
        if (vkAllocateDescriptorSets(dev_, &dai, &out.ds) != VK_SUCCESS) {
            // said, never written into: the draw then takes the white set
            out.ds = VK_NULL_HANDLE;
            static bool told = false;
            if (!told) { told = true; std::fprintf(stderr, "vulkan: the texture descriptor pool is exhausted\n"); }
            return;
        }
        VkDescriptorImageInfo dii{sampler_, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = out.ds; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    };
    // `what`: the image's category - "textures", or the 1x1 stand-ins every
    // missing texture or map falls back to
    auto upload = [&](auto&& fill, int tw, int th, Tex& out, bool withSet = true, bool one = false,
                      const char* what = "textures") {
        const VkFormat fmt = one ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tw) * th * (one ? 1 : 4);
        VkBuffer sb; VkDeviceSize so = 0;
        unsigned char* dst = nullptr;
        if (!stage(bytes, sb, so, dst)) return;
        // Alpha carries the COLOUR KEY: 0 where the texel is black, the key
        // the cutout path (flag 0x800) discards on, 255 elsewhere. With the
        // nearest sampler the shader's test on it is the same test on the
        // same texel as before; with a linear one the (0,0,0,0) key texels
        // make the sample PREMULTIPLIED, which is what lets the edge stay
        // clean. A non-cutout batch never reads alpha, so a black texel
        // still draws black.
        fill(dst);

        // THE MIP CHAIN - the trilinear enhancement's, generated here by a
        // ladder of half-size linear blits, because the .3DT ships ONE level
        // (MIPFILTER NONE in the original). Averaging RGBA with the key
        // texels at (0,0,0,0) keeps every level premultiplied, so the
        // shader's cutout rule holds down the chain; a non-cutout batch's
        // black texels average as black, which is what they are.
        uint32_t levels = 1;
        if (filter_ >= 2)
            for (int m = std::max(tw, th); m > 1; m >>= 1) ++levels;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
        ii.mipLevels = levels; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                 | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!makeImage2D(ii, VK_IMAGE_ASPECT_COLOR_BIT, what, out.img, out.mem, nullptr))
            return;   // no image: the texture draws white (its staging left to the flush)

        VkCommandBuffer cb = uploadCb();
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = out.img;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy cp{};
        cp.bufferOffset = so;
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
        vkCmdCopyBufferToImage(cb, sb, out.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        // The ladder: each level becomes a blit SOURCE once written, and the
        // next is blitted from it at half size.
        int pw = tw, ph = th;
        for (uint32_t l = 1; l < levels; ++l) {
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 1, 0, 1};
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
            const int nw = std::max(pw / 2, 1), nh = std::max(ph / 2, 1);
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
            bl.srcOffsets[1] = {pw, ph, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            bl.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cb, out.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           out.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
            pw = nw; ph = nh;
        }
        // Level 0..levels-2 are now TRANSFER_SRC, the last TRANSFER_DST; the
        // last level is moved to SRC too so ONE barrier can hand the whole
        // image to the shader. With one level there is no ladder: level 0 goes
        // from DST to the shader - the image has no TRANSFER_SRC usage then
        // (the validation layer's VUID-VkImageMemoryBarrier-oldLayout-01212,
        // found 2026-10-24).
        if (levels > 1) {
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, 0, 1};
            bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        }
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        bar.oldLayout = levels > 1 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = levels > 1 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        // (submitted with the pool's other copies - `flushUploads`)
        viewAndSet(out, fmt, levels, withSet);
    };
    // ...and a texture of the pack's BLOCK FORMATS (`o3de/texpack.h`
    // PackBlocks): the file's levels copied as they are - no blit ladder,
    // a block image cannot be blitted, and the KTX2 carries its own chain
    // made the ladder's way (`tools/tex_encode.cpp`). Only its level 0 when
    // the sampler takes no mips, as the RGBA path makes none.
    auto uploadZ = [&](const omk::PackBlocks& z, Tex& out, bool withSet = true) {
        const VkFormat fmt = static_cast<VkFormat>(z.vkFormat);
        const uint32_t levels = filter_ >= 2 ? static_cast<uint32_t>(z.levels.size()) : 1u;
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(z.bytes.size());
        VkBuffer sb; VkDeviceSize so = 0;
        unsigned char* dst = nullptr;
        if (!stage(bytes, sb, so, dst)) return;
        std::memcpy(dst, z.bytes.data(), z.bytes.size());
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(z.w), static_cast<uint32_t>(z.h), 1};
        ii.mipLevels = levels; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!makeImage2D(ii, VK_IMAGE_ASPECT_COLOR_BIT, "textures", out.img, out.mem, nullptr))
            return;   // no image: the texture draws white (its staging left to the flush)
        VkCommandBuffer cb = uploadCb();
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = out.img;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        std::vector<VkBufferImageCopy> cps(levels);
        for (uint32_t l = 0; l < levels; ++l) {
            cps[l] = VkBufferImageCopy{};
            cps[l].bufferOffset = so + z.levels[l].first;
            cps[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            cps[l].imageExtent = {std::max(1u, static_cast<uint32_t>(z.w) >> l),
                                  std::max(1u, static_cast<uint32_t>(z.h) >> l), 1};
        }
        vkCmdCopyBufferToImage(cb, sb, out.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, cps.data());
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        viewAndSet(out, fmt, levels, withSet);
    };

    // THE POOL, UPLOADED ONLY WHERE IT IS NEW, as the GLES, 3DS and GL1
    // backends keep it. The frame hands the pool over again whenever it
    // changes - a particle's sprite, a character coming into view - and
    // re-sending would mean waiting for the device to go idle, destroying
    // EVERY image and sending them all again. A texture already on the device from an earlier pool -
    // the same pixel storage, palette and size - is kept with its image and
    // its descriptor set, and the entry holds that storage, so its address
    // cannot come back as another texture's while cached; what no slot of the
    // new pool uses is destroyed, after a wait for the device only then.
    // A set is freed on its own and only the new ones are allocated -
    // reallocating them all on a per-frame call exhausts the descriptor pool
    // and crashes.
    // binding 1 of a texture's set: its light map, or the shared black one;
    // binding 3 its normal map, or the shared flat one
    const auto bindLight = [&](VkDescriptorSet ds, VkImageView view, VkImageView normal) {
        VkDescriptorImageInfo dii[2] = {{sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                        {sampler_, normal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[2]{};
        for (int k = 0; k < 2; ++k) {
            wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[k].dstSet = ds; wr[k].dstBinding = k == 0 ? 1 : 3; wr[k].descriptorCount = 1;
            wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[k].pImageInfo = &dii[k];
        }
        vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
        bindReflection(ds);
    };
    if (normalFlat_.img == VK_NULL_HANDLE) {
        upload([](unsigned char* d) { d[0] = 128; d[1] = 128; d[2] = 255; d[3] = 0; }, 1, 1, normalFlat_, false,
               false, "texture stand-ins");
    }
    if (lightBlack_.img == VK_NULL_HANDLE) {
        upload([](unsigned char* d) { d[0] = d[1] = d[2] = d[3] = 0; }, 1, 1, lightBlack_, false, false,
               "texture stand-ins");
        // an upload's set is written with binding 0 alone: the black one's
        // view stands in for the light map and the reflection until bound
    }
    if (white_.img == VK_NULL_HANDLE) {
        upload([](unsigned char* d) { d[0] = d[1] = d[2] = d[3] = 255; }, 1, 1, white_, true, false,
               "texture stand-ins");
        // (the stand-ins refused - 4 bytes each, so a device already out of
        // memory: no set to write into, and a draw with none is skipped)
        if (white_.ds && lightBlack_.view && normalFlat_.view)
            bindLight(white_.ds, lightBlack_.view, normalFlat_.view);
    }
    const auto keyOf = [](const omk::Texture& x) {
        return std::make_tuple(x.idx.data(), x.pal.data(), x.width, x.height);
    };
    if (!preload) {
    for (auto& [key, u] : uploaded_) u.used = false;
    for (const auto& x : t)
        if (x.hasPixels())
            if (auto hit = uploaded_.find(keyOf(x)); hit != uploaded_.end()) hit->second.used = true;
    }
    bool waited = false;
    for (auto it = uploaded_.begin(); !preload && it != uploaded_.end(); ) {
        if (it->second.used || it->second.pinned) { ++it; continue; }
        if (!waited) { vkDeviceWaitIdle(dev_); waited = true; }
        Tex& old = it->second.t;
        if (old.ds)   vkFreeDescriptorSets(dev_, dpool_, 1, &old.ds);
        if (old.view) vkDestroyImageView(dev_, old.view, nullptr);
        if (old.img)  vkDestroyImage(dev_, old.img, nullptr);
        if (old.mem)  freeDeviceMemory(old.mem);
        rtFreeTex(old.rt);
        if (old.emView) vkDestroyImageView(dev_, old.emView, nullptr);
        if (old.emImg)  vkDestroyImage(dev_, old.emImg, nullptr);
        if (old.emMem)  freeDeviceMemory(old.emMem);
        if (old.nmView) vkDestroyImageView(dev_, old.nmView, nullptr);
        if (old.nmImg)  vkDestroyImage(dev_, old.nmImg, nullptr);
        if (old.nmMem)  freeDeviceMemory(old.nmMem);
        it = uploaded_.erase(it);
    }
    if (!preload) tex_.assign(t.size(), Tex{});
    // THE TEXTURE PACK (`o3de/texpack.h`), consulted only when one was opened:
    // a texture it holds is uploaded from the pack's pixels at the pack's size
    const omk::TexturePack& pack = omk::texturePack();
    std::vector<omk::PackImage> reps;
    int lit = 0;
    // what a pool change COSTS the frame it lands on - the pack's decode and
    // every upload - said when it brings anything new (the preload's measure)
    using clk = std::chrono::steady_clock;
    const auto t0 = clk::now();
    int fresh = 0, blocksUp = 0;
    long prepared = 0;
    if (pack.shown()) {
        // every texture this pool brings that the device does not hold yet,
        // decoded together on the thread pool
        std::vector<const omk::Texture*> toDecode(t.size(), nullptr);
        for (std::size_t i = 0; i < t.size(); ++i)
            if (t[i].hasPixels() && !uploaded_.count(keyOf(t[i]))) toDecode[i] = &t[i];
        const std::size_t taken0 = pack.parkedTaken();
        reps = pack.replacements(toDecode);
        prepared = static_cast<long>(pack.parkedTaken() - taken0);   // decoded ahead, on a load's thread
    }
    const auto t1 = clk::now();
    int packed = 0, normals = 0;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const omk::Texture& x = t[i];
        if (!x.hasPixels()) continue;
        auto hit = uploaded_.find(keyOf(x));
        if (hit == uploaded_.end()) {
            Uploaded u;
            u.idx = x.idx; u.pal = x.pal; u.used = !preload; u.pinned = preload;
            if (pack.shown() && reps[i].ok && !reps[i].colourZ.empty()) {
                const omk::PackImage& rep = reps[i];
                uploadZ(rep.colourZ, u.t);
                u.t.su = static_cast<float>(rep.w) / static_cast<float>(x.width);
                u.t.sv = static_cast<float>(rep.h) / static_cast<float>(x.height);
                u.t.packed = true;
                ++blocksUp;
            } else if (pack.shown() && reps[i].ok) {
                const omk::PackImage& rep = reps[i];
                const int rw = rep.w, rh = rep.h;
                upload([&](unsigned char* d) { std::memcpy(d, rep.rgba.data(), rep.rgba.size()); }, rw, rh, u.t);
                u.t.su = static_cast<float>(rw) / static_cast<float>(x.width);
                u.t.sv = static_cast<float>(rh) / static_cast<float>(x.height);
                u.t.packed = true;
            } else {
                upload([&](unsigned char* d) { x.toRgbaKeyed(d); }, x.width, x.height, u.t);
            }
            // ...and its LIGHT MAP, when the pack has one: grey in RGBA, the
            // red channel what `scene.frag` reads
            if (pack.shown() && !reps[i].emissiveZ.empty()) {
                Tex em;
                uploadZ(reps[i].emissiveZ, em, false);
                u.t.emImg = em.img; u.t.emMem = em.mem; u.t.emView = em.view;
                ++lit;
            } else if (pack.shown() && !reps[i].emissive.empty()) {
                const omk::PackImage& rep = reps[i];
                Tex em;
                if (r8Ok_)
                    upload([&](unsigned char* d) { std::memcpy(d, rep.emissive.data(), rep.emissive.size()); },
                           rep.ew, rep.eh, em, false, true);
                else
                    upload([&](unsigned char* d) {
                        for (std::size_t q = 0; q < rep.emissive.size(); ++q)
                            d[4 * q] = d[4 * q + 1] = d[4 * q + 2] = rep.emissive[q], d[4 * q + 3] = 255;
                    }, rep.ew, rep.eh, em, false);
                u.t.emImg = em.img; u.t.emMem = em.mem; u.t.emView = em.view;
                ++lit;
            }
            // ...and its NORMAL MAP (the relief's), when the pack has one
            if (pack.shown() && !reps[i].normalZ.empty()) {
                Tex nm;
                uploadZ(reps[i].normalZ, nm, false);
                u.t.nmImg = nm.img; u.t.nmMem = nm.mem; u.t.nmView = nm.view;
                ++normals;
            } else if (pack.shown() && !reps[i].normal.empty()) {
                const omk::PackImage& rep = reps[i];
                Tex nm;
                upload([&](unsigned char* d) { std::memcpy(d, rep.normal.data(), rep.normal.size()); }, rep.nw, rep.nh, nm, false);
                u.t.nmImg = nm.img; u.t.nmMem = nm.mem; u.t.nmView = nm.view;
                ++normals;
            }
            if (u.t.ds && lightBlack_.view && normalFlat_.view)
                bindLight(u.t.ds, u.t.emView ? u.t.emView : lightBlack_.view,
                          u.t.nmView ? u.t.nmView : normalFlat_.view);
            hit = uploaded_.emplace(keyOf(x), std::move(u)).first;
            ++fresh;
        } else if (preload) {
            hit->second.pinned = true;
        }
        if (preload) continue;
        tex_[i] = hit->second.t;
        packed += tex_[i].packed ? 1 : 0;
    }
    flushUploads();   // the pool's copies, submitted and waited on once
    poolLit_ = std::any_of(tex_.begin(), tex_.end(), [](const Tex& t) { return t.emView != VK_NULL_HANDLE; });
    const double decodeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double uploadMs = std::chrono::duration<double, std::milli>(clk::now() - t1).count();
    if (preload) {
        if (fresh > 0)
            std::printf("vulkan: preload - %d textures uploaded ahead (%d from block files), read and decode %.1f ms, "
                        "uploads %.1f ms; %zu still to come\n", fresh, blocksUp, decodeMs, uploadMs, preQueue_.size());
        omk::event("texpack.preload", {{"fresh", fresh}, {"blocks", blocksUp}, {"prepared", prepared}, {"decodeMs", decodeMs},
                                       {"uploadMs", uploadMs}, {"waiting", static_cast<long>(preQueue_.size())}});
        return;
    }
    if (fresh > 0)
        std::printf("vulkan: texture pool - %d new of %zu (%d from the pack's block files), the pack's read and "
                    "decode %.1f ms, the uploads %.1f ms\n", fresh, t.size(), blocksUp, decodeMs, uploadMs);
    if (pack.shown())
        omk::event("texpack.pool", {{"backend", "vulkan"}, {"textures", static_cast<long>(t.size())},
                                    {"replaced", packed}, {"lights", lit}, {"normals", normals},
                                    {"lightChannels", r8Ok_ ? 1 : 4}, {"fresh", fresh}, {"blocks", blocksUp},
                                    {"prepared", prepared},
                                    {"decodeMs", decodeMs}, {"uploadMs", uploadMs}});
}

bool VulkanRenderer::uploadGeometry(const omk::Geometry* g) {
    // Cached by pointer, but only while the CONTENT has not moved: a posed
    // character is the same object with different vertices every frame
    // (`Geometry::revision`), and returning the cached buffer for it froze the
    // character at the first pose the GPU ever saw.
    const auto seen = vboRev_.find(g);
    if (vbo_.count(g) && seen != vboRev_.end() && seen->second == g->revision)
        return true;
    bool grow = false;
    if (vbo_.count(g)) {
        // Same object, new vertices. The previous frame has already been
        // waited on (present ends with `vkWaitForFences`), so the buffer is
        // not in flight and can be refilled in place when it still fits.
        const auto it = vboN_.find(g);
        const auto capIt = vboCap_.find(g);
        const std::size_t cap = capIt != vboCap_.end() ? capIt->second : (it != vboN_.end() ? it->second : 0);
        if (it != vboN_.end() && cap >= g->corners.size()) {
            // ...and when the geometry says which corners moved since the
            // revision this buffer holds, only those are written
            // (`Geometry::dirtyCorners`, todo/optimization.md step 8): a moving
            // crate is a few hundred of Anekbah's 139245 corners. The triangles
            // the depth tie degenerated stay degenerated; `resolveTies` puts
            // back the ones that are no longer losers.
            static const bool noDirty = omk::envFlag("OMK_NO_DIRTY");
            const bool partial = !noDirty && seen != vboRev_.end() && g->dirtyTo != 0 &&
                                 g->dirtyTo == g->revision && seen->second == g->dirtyFrom &&
                                 it->second == g->corners.size();
            void* p = nullptr;
            const VkDeviceSize bytes = g->corners.size() * sizeof(GpuVert);
            if (vkMapMemory(dev_, vbo_[g].second, 0, bytes, 0, &p) == VK_SUCCESS) {
                auto* dst = static_cast<GpuVert*>(p);
                const auto* lose = bakedOf(g);
                if (partial) {
                    for (const std::uint32_t i : g->dirtyCorners) {
                        if (i >= g->corners.size()) continue;
                        dst[i] = vertOf(*g, i, lose);
                    }
                } else {
                    for (std::size_t i = 0; i < g->corners.size(); ++i) dst[i] = vertOf(*g, i, lose);
                    tie_[g].vboReplaced();
                }
                vkUnmapMemory(dev_, vbo_[g].second);
                vboRev_[g] = g->revision;
                vboN_[g] = g->corners.size();
                return true;
            }
        }
        vkDestroyBuffer(dev_, vbo_[g].first, nullptr);
        freeDeviceMemory(vbo_[g].second);
        vbo_.erase(g); vboN_.erase(g); vboRev_.erase(g);
        grow = true;                   // it has changed size once: give it room
    }
    std::vector<GpuVert> v(g->corners.size());
    const auto* lose = bakedOf(g);
    for (std::size_t i = 0; i < g->corners.size(); ++i) v[i] = vertOf(*g, i, lose);
    const VkDeviceSize bytes = v.size() * sizeof(GpuVert);
    if (!bytes) return false;
    // a first buffer is exact - a set is never re-sized - and one that has
    // outgrown its buffer gets half as much again
    const std::size_t capCorners = grow ? v.size() + v.size() / 2 : v.size();
    VkBuffer b; VkDeviceMemory m;
    if (!makeBuffer(capCorners * sizeof(GpuVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    b, m)) return false;
    void* p = nullptr;
    vkMapMemory(dev_, m, 0, bytes, 0, &p);
    std::memcpy(p, v.data(), bytes);
    vkUnmapMemory(dev_, m);
    vbo_[g] = {b, m};
    g->resident.mark();
    vboN_[g] = v.size();
    vboCap_[g] = capCorners;
    vboRev_[g] = g->revision;
    tie_[g].vboReplaced();
    return true;
}

// The GLES backend's rule, the same `DepthTie` walked once over the set's
// whole draw order (`glesrender.cpp` has the reading): its losers draw a depth
// step back rather than degenerated, so a culled winner or a door leaf moved
// off its frame leaves the loser drawn, as the engine draws it.
bool VulkanRenderer::bakeDepthTie(const omk::Geometry* g, std::span<const omk::Draw> order) {
    if (omk::tieBakeOff() || !g) return false;
    const std::vector<std::size_t> tris = omk::bakeTieLosers(*g, order);
    std::vector<std::uint8_t> lose(g->corners.size(), 0);
    for (const std::size_t tri : tris)
        for (std::size_t k = 3 * tri; k < 3 * tri + 3 && k < lose.size(); ++k) lose[k] = 1;
    baked_[g] = std::move(lose);
    // the buffer holds the old decision - degenerated faces, or none marked:
    // forget both, so the next upload is whole and marked
    tie_.erase(g);
    vboRev_.erase(g);
    static const bool tieLog = omk::envFlag("OMK_TIE_LOG");
    if (tieLog)
        std::printf("[tie] vulkan: baked %zu losers over %zu draws on geometry %p\n",
                    tris.size(), order.size(), static_cast<const void*>(g));
    return true;
}

void VulkanRenderer::resolveTies(const omk::Draw& d) {
    if (omk::tieOff()) return;              // the fight left in, for a before/after
    const omk::Geometry* g = d.geo;
    if (baked_.count(g)) return;             // settled once, drawn back (`bakeDepthTie`)
    auto& t = tie_[g];
    std::vector<std::size_t> losers, restore;
    t.resolve(*g, d.start, d.count, d.blend == omk::Blend::Opaque, losers, restore);
    if (losers.empty() && restore.empty()) return;
    // `OMK_TIE_LOG=1`: every loser, with the mesh it came from
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
    void* p = nullptr;
    const VkDeviceSize bytes = vn->second * sizeof(GpuVert);
    if (vkMapMemory(dev_, vb->second.second, 0, bytes, 0, &p) != VK_SUCCESS) return;
    auto* v = static_cast<GpuVert*>(p);
    // first the triangles degenerated earlier that are losers no more - a
    // partial upload left them as they were - then this draw's losers
    for (const std::size_t tri : restore) {
        const std::size_t c = 3 * tri;
        if (c + 2 >= vn->second || c + 2 >= g->corners.size()) continue;
        for (std::size_t k = c; k < c + 3; ++k) {
            const auto& s = g->corners[k];
            v[k] = {s.x, s.y, s.z, s.u, s.v, s.r, s.g, s.b, s.nx, s.ny, s.nz, s.phase};
        }
    }
    for (const std::size_t tri : losers) {
        const std::size_t c = 3 * tri;
        if (c + 2 >= vn->second) continue;
        v[c + 1].x = v[c + 2].x = v[c].x;
        v[c + 1].y = v[c + 2].y = v[c].y;
        v[c + 1].z = v[c + 2].z = v[c].z;
    }
    vkUnmapMemory(dev_, vb->second.second);
    t.dropped += static_cast<long>(losers.size());
    // Reported at `end()`, once per geometry - a set's batches arrive over
    // many submits, and a running total after the first read as the whole
    // (it did: 30 for Anekbah's 248), while not every triangle is ever
    // submitted, so "all walked" never comes.
    t.touched = true;
}

void VulkanRenderer::setViewport(const omk::View& view) {
    // ...in RENDER pixels, so the letterbox's strip scales with the frame.
    picH_ = view.letterboxed() ? view.vh : h_;
    int vw = rs(view.letterboxed() ? view.vw : w_);
    int vh = rs(picH_);
    // the FLOORS' reflection: drawn at HALF size into the image's top-left
    // quarter (`reflHalf_`) - a quarter of the pixels; the water's, whole
    if (reflHalfNow()) { vw = (vw + 1) / 2; vh = (vh + 1) / 2; }
    const VkViewport vp{0, 0, static_cast<float>(vw), static_cast<float>(vh),
                        0.0f, 1.0f};
    const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(vw), static_cast<uint32_t>(vh)}};
    vkCmdSetViewport(cb_, 0, 1, &vp);
    vkCmdSetScissor(cb_, 0, 1, &sc);
}

void VulkanRenderer::pushView(const omk::View& view) {
    // The view-projection, built from the SOFTWARE rasterizer's own basis so
    // the two cannot disagree about a convention.
    omk::RCamera cam = view.cam;
    flipX_ = cam.flipX;
    // The letterbox is a VIEWPORT: the picture goes into the top-left
    // `vw x vh` of an attachment that stays window-sized, because the
    // swapchain present needs that size. The vertical fov follows from `vh`.
    cam.w = view.letterboxed() ? view.vw : w_;
    cam.h = view.letterboxed() ? view.vh : h_;
    float s[3], u[3], f[3], tanH = 0, tanV = 0;
    omk::cameraBasis(cam, s, u, f, tanH, tanV);
    viewTanH_ = tanH; viewTanV_ = tanV;

    // Vulkan clip space already has Y DOWN and Z in [0,1], which happens to
    // suit a game whose own Y points down. Screen x = w/2*(1 + (vx/vz)/tanH)
    // is x_ndc = (vx/vz)/tanH; screen y = h/2*(1 - (vy/vz)/tanV) is
    // y_ndc = -(vy/vz)/tanV. So the projection rows are 1/tanH and -1/tanV,
    // with w = vz.
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const float A = farP / (farP - nearP), B = -farP * nearP / (farP - nearP);
    // view = R * (world - eye), R's rows are s, u, f. Folded into one matrix,
    // COLUMN-major as GLSL wants it.
    const float e[3] = {cam.eye[0], cam.eye[1], cam.eye[2]};
#if OMK_VR
    // A VR eye's off-axis frustum (`RCamera::lensX`/`lensY`), as GLES's: the
    // rows take `- lens * f`, and 0 leaves them bit-identical.
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
    const float r3[4] = {f[0], f[1], f[2],
                         -(f[0] * e[0] + f[1] * e[1] + f[2] * e[2])};
    const float* rows[4] = {r0, r1, r2, r3};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            push_.mvp[c * 4 + r] = rows[r][c];
    // FRAME GENERATION's previous camera for the draws that follow. The frame's MAIN view (its eye is
    // `begin`'s) moves from where the last frame's main view was; a mirror's reflected view is given no
    // camera motion - its previous matrix is its current one - which only the reflection's own pixels carry.
    if (mvOn_) {
        const bool mainView = cam.eye[0] == frameEye_[0] && cam.eye[1] == frameEye_[1] &&
                              cam.eye[2] == frameEye_[2];
        if (mainView)
            for (int k = 0; k < 3; ++k) { mainS_[k] = s[k]; mainU_[k] = u[k]; mainF_[k] = f[k]; }
        std::memcpy(pm_.prevMvp, mainView && havePrev_ ? prevMain_ : push_.mvp, sizeof pm_.prevMvp);
    }
}

// THE DEPTH PASS FROM THE LIGHT - `todo/enhancements.md` row 6.
//
// An ORTHOGRAPHIC slab fitted to the casters, not to the scene: a fragment
// outside it is lit by definition, which is what keeps a characters-only
// shadow from darkening the far end of a street. The same matrix writes the
// map and reads it, so whatever Vulkan's clip space does to the handedness
// cancels between the two.
void VulkanRenderer::shadowPass(const omk::View& v, std::span<const omk::Draw> casters) {
    if (shPipe_ == VK_NULL_HANDLE || !v.shadow.on || casters.empty()) return;
    // ONE MAP PER LIGHT: layer 0 is `v.shadow`, layers 1..2 `v.extraShadows`,
    // each an orthographic slab around the casters as before.
    // A LAMP close enough gets a PERSPECTIVE map from its own position: the
    // frustum is the cone that just holds the casters' sphere, so everything
    // behind them - however long the shadow on the ground - is in it. A lamp
    // on top of the casters (or a light without a position) falls back to
    // the orthographic slab along its direction.
    constexpr float kLampSize = 30.0f;     // world units: the penumbra's source
    constexpr float kOrthoSpread = 0.06f;  // penumbra growth per unit, the slab's
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
        // column-major P * V, Vulkan's 0..1 depth
        // a lamp among the casters keeps a 60-degree half-angle cone
        // towards the player: what lies outside it casts nothing
        const float n = std::max((D - R) * 0.9f, 8.0f), fa = D + R * 4.0f;
        const float sn = std::min(R / D, 0.866f);
        const float s = std::sqrt(1.0f - sn * sn) / sn;     // 1 / tan(half angle)
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
    // The eye sits a slab's depth behind the casters, so nothing is clipped
    // by the near plane however the light is angled.
    const float back = R * 2.0f;
    float eye[3];
    for (int k = 0; k < 3; ++k) eye[k] = sl.centre[k] - f[k] * back;
    const float far = back + R * 2.0f;
    const float sc[3] = {1.0f / R, 1.0f / R, 1.0f / far};
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
    prm[0] = 0.0f; prm[1] = far; prm[2] = kOrthoSpread / (2.0f * R); prm[3] = 0.0f;
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

    if (!cbStarted_) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb_, 0);
        vkBeginCommandBuffer(cb_, &bi);
        cbStarted_ = true;
        tsCount_ = 0;
        stamp("start");
    }
    // a posed caster is placed on the CPU: the depth-only pipeline has no set 2
    std::vector<omk::Draw> placed;
    placed.reserve(casters.size());
    for (const auto& d : casters)
        placed.push_back(d.meshPose && d.meshPoses && d.geo &&
                                 d.geo->cornerMesh.size() == d.geo->corners.size()
                             ? cpuPose(d) : d);
    for (const auto& d : placed) if (d.geo) uploadGeometry(d.geo);

    VkClearValue clear{};
    clear.depthStencil = {1.0f, 0};
    // EVERY layer is passed through each frame - an unlit one only cleared -
    // so all three sit in SHADER_READ_ONLY for the sampled array view.
    for (int L = 0; L < kShadowLayers; ++L) {
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = shPass_; rp.framebuffer = shLayerFbuf_[L];
    rp.renderArea = {{0, 0}, {kShadowSide, kShadowSide}};
    rp.clearValueCount = 1; rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (live[L]) {
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, shPipe_);
    vkCmdPushConstants(cb_, shPlo_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof mats[L], mats[L]);
    for (const auto& d : placed) {
        if (!d.geo || !d.count) continue;
        const auto it = vbo_.find(d.geo);
        if (it == vbo_.end()) continue;
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(cb_, 0, 1, &it->second.first, &off);
        vkCmdDraw(cb_, static_cast<uint32_t>(d.count), 1,
                  static_cast<uint32_t>(d.start), 0);
    }
    }
    vkCmdEndRenderPass(cb_);
    }
    stamp("shadow");

    if (shUboPtr_) {
        ShadowUbo ub{};
        for (int L = 0; L < kShadowLayers; ++L) {
            std::memcpy(ub.lightMvp[L], mats[L], sizeof mats[L]);
            ub.strength[L] = live[L] ? lights[L]->strength : 0.0f;
            std::memcpy(ub.lightPos[L], lposs[L], sizeof lposs[L]);
            std::memcpy(ub.params[L], prms[L], sizeof prms[L]);
        }
        ub.texel = 1.0f / static_cast<float>(kShadowSide);
        // In WORLD units now (the shader compares linear depths), widened by
        // the receiver's slope there.
        ub.bias = 1.5f;
        ub.shimmer = shimmerClock_;
        std::memcpy(shUboPtr_, &ub, sizeof ub);
    }
    shadowLive_ = true;
}

void VulkanRenderer::begin(const omk::View& view) {
    st_ = omk::RasterStats{};
    // the pose buffer from its start: the frame that wrote it was waited for
    poseUsed_ = 0;
    poseCache_.clear();
    // the previous frame's fence has been waited on (the present ends with it)
    for (auto as : rtDeadAs_) rtDestroyAs_(dev_, as, nullptr);   // (before the buffers they live in)
    rtDeadAs_.clear();
    for (auto& bm : deadBufs_) {
        vkDestroyBuffer(dev_, bm.first, nullptr);
        freeDeviceMemory(bm.second);
    }
    deadBufs_.clear();
    fog_ = view.fog;
    fogStart_ = view.fogStart;
    mistOn_ = view.mist > 0.0f || view.dust > 0.0f || view.veil > 0.0f;   // the dust and veils take the marked batches
    fogEnd_ = view.fogEnd;
    for (int i = 0; i < 3; ++i)
        fogColour_[i] = static_cast<float>(view.fogColour[i]) / 255.0f;
    for (int i = 0; i < 3; ++i)
        clearColour_[i] = static_cast<float>(view.clearColour[i]) / 255.0f;
    for (int i = 0; i < 3; ++i) frameEye_[i] = view.cam.eye[i];   // the MAIN view's (`pushView`)
    pushView(view);
    // the ambient occlusion's camera: the last pass begun is the frame's own
    if (aoOk_) {
        omk::RCamera c = view.cam;
        c.w = view.letterboxed() ? view.vw : w_;
        c.h = view.letterboxed() ? view.vh : h_;
        float s3[3], u3[3], f3[3], tH = 1.0f, tV = 1.0f;
        omk::cameraBasis(c, s3, u3, f3, tH, tV);
        aoView4_[0] = tH; aoView4_[1] = tV;
        aoView4_[2] = static_cast<float>(rs(c.w)); aoView4_[3] = static_cast<float>(rs(c.h));
    }
    // ---- THE LIGHTS, per frame (`todo/enhancements.md` row 7) ------------
    shimmerClock_ = view.shimmerClock;
    rain_ = view.rain;
    if (passFb_ == VK_NULL_HANDLE) enhOff_ = view.enhOff;   // (the water reflection's begin is not the frame's)
    dither_ = view.dither;
    colourDepth_ = view.colourDepth;
    foc_.begin(view, w_, h_, dofOn_);   // the post chain's view and depth of field's focus
    // VOLUMETRIC LAMP LIGHT: the air's lamps (`View::airLights`) and the main view's camera, for `runVol`
    volCount_ = 0;
    if (volOn_ && volUboPtr_) {
        omk::postfx::VolFrame vf;
        omk::postfx::buildVolFrame(view, w_, h_, volTune_, vf);
        std::memcpy(volUboPtr_, &vf.ubo, sizeof vf.ubo);
        volCount_ = vf.count;
        for (int k = 0; k < 3; ++k) { volEye_[k] = vf.eye[k]; volS_[k] = vf.s[k]; volU_[k] = vf.u[k]; volF_[k] = vf.f[k]; }
        volTanH_ = vf.tanH; volTanV_ = vf.tanV;
        omk::postfx::volLampEvents(vf, volSig_, volFrame_);
    }
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
        ub.eye[3] = view.setDim;   // the night (`View::setDim`), w of the eye's vec4
        ub.fx[0] = view.relief;
        // THE MIST (`View::mist`): its strength, its ground and its height
        ub.fx[1] = view.mist; ub.fx[2] = view.mistGround; ub.fx[3] = view.mistHeight;
        ub.dust[0] = view.dust; ub.dust[1] = view.dustDrift[0]; ub.dust[2] = view.dustDrift[1];
        ub.dust[3] = view.dustScale;
        for (int k = 0; k < 3; ++k) ub.dustColour[k] = view.dustColour[k];
        ub.dustColour[3] = 0.0f;
        ub.veil[0] = view.veil; ub.veil[1] = view.veilFall; ub.veil[2] = view.veilScale;
        ub.veil[3] = view.puddles;   // the PUDDLES' strength, in the veils' spare
        for (int k = 0; k < 3; ++k) ub.veilColour[k] = view.veilColour[k];
        ub.veilColour[3] = 0.0f;
        // THE TILED POINT LIGHTS (`o3de/lighttiles.h`): every one the frame
        // keeps, and their tiles binned for THIS camera at the render size.
        // The reflection pass begins first and the frame's pass last - the
        // last binning is the one both read, and the reflection skips them.
        ub.tiles = 0;
        tilesNow_ = false;
        // `OMK_REFL_UNLIT=1`, an instrument: the reflection's tiles left empty,
        // as before - what `verify.py: engine: floors` compares against
        static const bool reflUnlit = omk::envFlag("OMK_REFL_UNLIT");
        if (passFb_ != VK_NULL_HANDLE && reflUnlit && !view.points.empty() && tilesPtr_ && pipeTiled_[0]) {
            // the tiled pipelines, as the reflection drew with, and NO tiles
            // (0 across): the module the same, its lights none
            std::memset(static_cast<char*>(tilesPtr_) + kTileWordsMax * 4, 0, 16);
            ub.tiles = 1;
            tilesNow_ = true;
        } else if (passFb_ != VK_NULL_HANDLE && !view.points.empty() && pointsPtr_ && tilesPtr_ && pipeTiled_[0] &&
            ds1Refl_ != VK_NULL_HANDLE) {
            // THE WATER REFLECTION'S begin: its tiles binned for the REFLECTED
            // camera into the buffer's second half, which its own set 1 binds
            // (the points are the frame's - the world's lights light the world
            // wherever it is seen from; the frame's begin writes them again)
            const std::size_t np = std::min<std::size_t>(view.points.size(), static_cast<std::size_t>(omk::kMaxPoints));
            omk::RCamera bc = view.cam;
            bc.w = view.letterboxed() ? view.vw : w_;
            bc.h = view.letterboxed() ? view.vh : h_;
            const int hd = reflHalf_ ? 2 : 1;   // the floors' reflection is drawn at half size
            omk::binLights(bc, (rs(bc.w) + hd - 1) / hd, (rs(bc.h) + hd - 1) / hd,
                           std::span<const omk::View::GpuLight>(view.points.data(), np), tilesRefl_);
            if (tilesRefl_.words.size() <= kTileWordsMax) {
                std::memcpy(static_cast<char*>(tilesPtr_) + kTileWordsMax * 4, tilesRefl_.words.data(),
                            tilesRefl_.words.size() * 4);
                ub.tiles = 1;
                tilesNow_ = true;
            }
        } else if (!view.points.empty() && pointsPtr_ && tilesPtr_ && pipeTiled_[0]) {
            const std::size_t np = std::min<std::size_t>(view.points.size(), static_cast<std::size_t>(omk::kMaxPoints));
            auto* gp = static_cast<GpuLightStd140*>(pointsPtr_);
            for (std::size_t i = 0; i < np; ++i) {
                const auto& l = view.points[i];
                for (int k = 0; k < 3; ++k) { gp[i].posA[k] = l.pos[k]; gp[i].dirB[k] = l.dir[k]; gp[i].colourI[k] = l.colour[k]; }
                gp[i].posA[3] = l.radiusA; gp[i].dirB[3] = l.radiusB; gp[i].colourI[3] = l.intensity;
            }
            omk::RCamera bc = view.cam;
            bc.w = view.letterboxed() ? view.vw : w_;
            bc.h = view.letterboxed() ? view.vh : h_;
            omk::binLights(bc, rs(bc.w), rs(bc.h), std::span<const omk::View::GpuLight>(view.points.data(), np), tiles_);
            if (tiles_.words.size() <= kTileWordsMax) {
                std::memcpy(tilesPtr_, tiles_.words.data(), tiles_.words.size() * 4);
                ub.tiles = 1;
                tilesNow_ = true;
            } else if (!tilesOverTold_) {
                tilesOverTold_ = true;   // said, never silent: every point light goes with it
                std::fprintf(stderr, "vulkan: the tiled lights' lists (%zu words) past the buffer's %llu - "
                             "the point lights off this frame\n", tiles_.words.size(),
                             static_cast<unsigned long long>(kTileWordsMax));
            }
            if (!tilesTold_) {
                tilesTold_ = true;
                omk::event("remaster.tiles", {{"lights", static_cast<long>(np)}, {"listed", tiles_.listed},
                                              {"full", tiles_.full}, {"tile", tiles_.tile}});
                std::printf("vulkan: the tiled point lights - %zu lights over %dx%d tiles of %d px, %ld listed, "
                            "%d tiles full (a REMASTER feature)\n", np, tiles_.across, tiles_.down, tiles_.tile,
                            tiles_.listed, tiles_.full);
            }
        }
        // (the soft particles' fade constants - outside the tiled lights' branch above)
        if (softOn_) {
            const float nearP = omk::kNearCut, farP = 200000.0f;
            ub.soft[0] = farP / (farP - nearP);
            ub.soft[1] = -farP * nearP / (farP - nearP);
            ub.soft[2] = 1.0f / softRange_;
        }
        litCount_ = n;
        std::memcpy(litUboPtr_, &ub, sizeof ub);
    }

    // The shadow pass may already have opened it; it must be opened ONCE, and
    // the depth pass has to be recorded before this render pass begins.
    if (!cbStarted_) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cb_, 0);
        vkBeginCommandBuffer(cb_, &bi);
        cbStarted_ = true;
        tsCount_ = 0;
        stamp("start");
    }
    // A frame with no shadow pass says so through the uniform, because the
    // descriptor set is bound either way.
    if (!shadowLive_ && shUboPtr_) {
        ShadowUbo ub{};
        ub.texel = 1.0f / static_cast<float>(kShadowSide);
        ub.shimmer = shimmerClock_;   // the shimmer runs with or without a shadow
        std::memcpy(shUboPtr_, &ub, sizeof ub);
    }
    VkClearValue clear[3]{};   // [2]: the motion vectors, cleared to none (the zero)
    // the engine clears to the scene's fog colour (`View::clearColour`); with
    // the BLOOM its alpha is the light map's value, so what no surface covers
    // clears to 0 - not light (at 1 the fog past the clip distance glowed)
    clear[0].color = {{clearColour_[0], clearColour_[1], clearColour_[2], bloom_ > 0.0f ? 0.0f : 1.0f}};
    clear[1].depthStencil = {1.0f, 0};   // depth far, stencil 0
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = passFb_ && passFb_ == reflFbuf1_ ? reflPass1_ : passFb_ && reflPass_ ? reflPass_ : pass_;
    // the post's this frame: decided here, where the scene's target is
    // chosen - with it the scene lands in `postSrc_`, and `end` must run it.
    // The bloom asks for it only with a light map in the pool: without one
    // every texel's light is 0 and the glow would move no pixel
    if (!passFb_) {
        lightMapNow_ = false;
        postNow_ = ((bloom_ > 0.0f && poolLit_) || lutN_ > 1 || aoOk_) && makeRmPost();
    }
    rp.framebuffer = passFb_ ? passFb_ : postNow_ ? fbufPost_ : fbuf_;
    // the RENDER size, not the output one: a render area smaller than the
    // attachment leaves the rest of it undefined, and with supersampling on
    // the resolve then averages four blocks of uninitialised memory.
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(reflHalfNow() ? (rw_ + 1) / 2 : rw_),
                              static_cast<uint32_t>(reflHalfNow() ? (rh_ + 1) / 2 : rh_)}};
    rp.clearValueCount = mvOn_ ? 3 : 2; rp.pClearValues = clear;
    vkCmdBeginRenderPass(cb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(view);
    recording_ = true;
    depthRo_ = false;
    softDraws_ = 0;
    if (rtOn_) rtBind(view);
    aoTanH_ = viewTanH_; aoTanV_ = viewTanV_;
    aoVw_ = tupOn_ ? rs(view.letterboxed() ? view.vw : w_) : (view.letterboxed() ? view.vw : w_) * ss_;
    aoVh_ = tupOn_ ? rs(view.letterboxed() ? view.vh : h_) : (view.letterboxed() ? view.vh : h_) * ss_;
    // ---- FRAME GENERATION's per-frame half: the motion, the cuts and the bodies' previous poses ----
    if (mvOn_) {
        std::memcpy(curMain_, push_.mvp, sizeof curMain_);
        // THE CAMERA CUT (`o3de/framegen.h`): a jump of the engine's camera - a first frame, a resize or a new
        // set are cuts too
        if (havePrev_) {
            const float turn = mainF_[0] * prevFwd_[0] + mainF_[1] * prevFwd_[1] + mainF_[2] * prevFwd_[2];
            if (omk::framegen::cameraCut(frameEye_, prevEye_, turn)) resetOwed_ = true;
        } else {
            resetOwed_ = true;
        }
        // across a cut the vectors say "no motion": the history is dropped anyway
        if (resetOwed_) std::memcpy(pm_.prevMvp, push_.mvp, sizeof pm_.prevMvp);
        pm_.jitter[0] = pm_.jitter[1] = 0.0f;   // (no upscaler, no jitter)
        if (tupOn_) {
            // THE JITTER: the SDK's sequence, one step a frame, in render pixels; the clip space is the VIEW's
            // (a letterbox's strip), a pixel `2 / width` of it, Y down as the pixels are (`upscale_vk.h`)
            const int phases = tup_->jitterPhases(rw_, w_);
            tup_->jitter(jitterIndex_ % phases, phases, jitterPx_[0], jitterPx_[1]);
            ++jitterIndex_;
            pm_.jitter[0] = 2.0f * jitterPx_[0] / static_cast<float>(aoVw_ > 0 ? aoVw_ : rw_);
            pm_.jitter[1] = 2.0f * jitterPx_[1] / static_cast<float>(aoVh_ > 0 ? aoVh_ : rh_);
        }
        // the bodies' affines: last frame's become the previous ones
        prevAffines_.swap(curAffines_);
        curAffines_.clear();
    }
}

// A POSED body's block in the pose buffer (`posesBodies`): written once a
// frame for each body (its draws share the `meshPose` and the lights), at an
// offset the device's storage alignment allows. -> false when it does not fit.
bool VulkanRenderer::writePose(const omk::Draw& d, uint32_t& off) {
    const auto key = std::make_pair(d.meshPose, d.vertexLights);
    if (const auto it = poseCache_.find(key); it != poseCache_.end()) { off = it->second; return true; }
    const VkDeviceSize at = (poseUsed_ + poseAlign_ - 1) / poseAlign_ * poseAlign_;
    if (!posePtr_ || at + poseRange_ > kPoseBytes || d.meshPoses > kPoseMaxMeshes) return false;
    float* f = reinterpret_cast<float*>(static_cast<unsigned char*>(posePtr_) + at);
    const int nl = d.vertexLights ? std::min(d.vertexLightCount, kPoseLights) : 0;
    f[0] = static_cast<float>(nl);
    f[1] = d.lightsFromBlack ? 1.0f : 0.0f;
    f[2] = d.lightBase;
    f[3] = static_cast<float>(d.meshPoses);
    for (int i = 0; i < nl; ++i) std::memcpy(f + 4 + 8 * i, d.vertexLights + 8 * i, 8 * sizeof(float));
    float* a = f + 4 + 8 * kPoseLights;
    std::memcpy(a, d.meshPose, d.meshPoses * 12 * sizeof(float));   // three vec4 an affine
    float* t = a + 12 * d.meshPoses;                                 // ...and the body light's tint
    t[0] = d.lightTint[0]; t[1] = d.lightTint[1]; t[2] = d.lightTint[2]; t[3] = 1.0f;
    poseUsed_ = at + (1 + 2 * kPoseLights + 3 * d.meshPoses + 1) * 16;
    // ...and with motion vectors, the body's PREVIOUS affines right after them (`scene.vert` -DOMK_MOTION),
    // from the last frame's block of the same `meshPose` - or the current ones again for a body new this
    // frame, which then shows only the camera's motion rather than a wrong one of its own
    if (mvOn_) {
        const std::size_t n = d.meshPoses * 12;
        const auto prev = prevAffines_.find(d.meshPose);
        // (and across a cut, where nothing has a previous position)
        const float* from = !resetOwed_ && prev != prevAffines_.end() && prev->second.size() == n
                                ? prev->second.data() : d.meshPose;
        std::memcpy(a + n + 4, from, n * sizeof(float));  // (after the tint's vec4)
        curAffines_[d.meshPose].assign(d.meshPose, d.meshPose + n);
        poseUsed_ += 3 * d.meshPoses * 16;
    }
    off = static_cast<uint32_t>(at);
    poseCache_.emplace(key, off);
    return true;
}

// A posed draw's corners placed on the CPU instead - by the same affine and,
// for the colour, `applyLights`' law on the posed normal from the scene's grey
// where the body starts black (the shader's rule, `scene.vert`). For what the
// pose buffer does not reach: a body past its room, and every caster of the
// shadow map, whose depth-only pipeline has no set 2.
omk::Draw VulkanRenderer::cpuPose(const omk::Draw& d) {
    auto slot = cpuPoseSlot_.find({d.meshPose, d.geo});
    if (slot == cpuPoseSlot_.end())
        slot = cpuPoseSlot_.emplace(std::make_pair(d.meshPose, d.geo), cpuPoseCount_[d.geo]++).first;
    omk::Geometry& cp = cpuPosed_[{d.geo, slot->second}];
    omk::poseGeometry(d, cp, omk::PoseParts{true, true, kPoseLights});   // o3de/drawcore.h
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

void VulkanRenderer::submit(const omk::Draw& d) {
    if (dofOn_) foc_.note(d);
    curBody_ = (d.meshPose && d.meshPoses) || d.castsShadow;
    if (!recording_ || !d.geo || !d.count) return;
    // HYBRID RAY TRACING: the draw into the frame's acceleration structures, and a posed body
    // takes the character's traced terms even with the game's shadows off (`caster` - no shadow
    // then receives anyway)
    if (rtOn_) {
        rtNote(d);
        rtBodyDraw_ = omk::rt::isBodyDraw(d);
        rtFloorDraws_ += (d.lit & omk::kDrawFloor) != 0;
    }
    if (d.meshPose && d.meshPoses && d.geo->cornerMesh.size() == d.geo->corners.size()) {
        uint32_t off = 0;
        if (poseDs_ != VK_NULL_HANDLE && writePose(d, off)) submitOne(d, true, off);
        else submitOne(cpuPose(d), false, 0);
        return;
    }
    submitOne(d, false, 0);
}

// SOFT PARTICLES: the open pass segment ends and a continuation begins, its depth read-only or
// writable (`passCont_`; everything the frame drew so far is loaded). Pipelines, descriptor sets
// and the dynamic viewport stay bound across the cut.
void VulkanRenderer::softSegment(bool readOnly) {
    vkCmdEndRenderPass(cb_);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = passCont_[readOnly ? 1 : 0]; rp.framebuffer = fbuf_;
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
    vkCmdBeginRenderPass(cb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    depthRo_ = readOnly;
}

// the depth image, as the sprites' shader samples it (set 3)
void VulkanRenderer::writeSoftSet() {
    if (softDs_ == VK_NULL_HANDLE || depthSampleView_ == VK_NULL_HANDLE) return;
    const VkDescriptorImageInfo di{softSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = softDs_; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
}

void VulkanRenderer::submitOne(const omk::Draw& d, bool posed, uint32_t poseOff) {
    if (!uploadGeometry(d.geo)) return;
    resolveTies(d);
    st_.triangles += static_cast<long>(d.count / 3);

    const int k = d.blend == omk::Blend::Opaque ? 0
                : d.blend == omk::Blend::Add    ? 1 : 2;
    // The mirror pass swaps the state under the same submissions: the stencil
    // marker while it is marking, the stencil-tested variants while it is
    // drawing the reflection. The DRAWS are unchanged, which is the point -
    // the backend is choosing how, not what.
    // a translucent body on a frame with depth of field: its twin, which marks the stencil
    // SOFT PARTICLES: an effect sprite (`kDrawSoft`) is drawn by the soft twin in a pass segment whose
    // depth is READ-ONLY - so its shader can sample the depth it is tested against - and the next draw
    // that writes depth (or the stencil) opens a writable segment again (the engine's order puts the
    // sprites after the opaque)
    const bool soft = softOn_ && !enhOff_ && k > 0 && (d.lit & omk::kDrawSoft) && !forcePipeline_ && !reflStencil_ &&
                      pipeSoft_[k] != VK_NULL_HANDLE;
    const bool subj = foc_.subjStencil && k > 0 && curBody_ && !soft && !forcePipeline_ && !reflStencil_ &&
                      pipeSubj_[k] != VK_NULL_HANDLE;
    if (soft && !depthRo_) softSegment(true);
    else if (depthRo_ && (k == 0 || forcePipeline_ || subj)) softSegment(false);
    softDraws_ += soft;
    VkPipeline use = forcePipeline_ ? forcePipeline_
                   : reflStencil_   ? pipeRefl_[k]
                   : soft           ? pipeSoft_[k]
                   : subj           ? pipeSubj_[k]
                   : rtCounting_ && pipeCount_[k] != VK_NULL_HANDLE ? pipeCount_[k]
                   : passFb_ && passFb_ == reflFbuf1_ ? (tilesNow_ && pipeTiled1_[k] ? pipeTiled1_[k] : pipe1_[k])
                   : tilesNow_ && pipeTiled_[k] ? pipeTiled_[k]
                                    : pipe_[k];
    // an opaque CUTOUT draw: the same pipeline with the shader's depth
    if (d.cutout && use == pipe_[0] && pipeCut_) use = pipeCut_;
    else if (d.cutout && use == pipeTiled_[0] && pipeTiledCut_) use = pipeTiledCut_;
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, use);

    // The texture is the key's LOW SIX BITS and nothing else (ASSETS 4b) -
    // or the pool's full index above them, past 64 (`drawTextureSlot`).
    const std::size_t slot = omk::drawTextureSlot(d);
    const VkDescriptorSet ds =
        (slot < tex_.size() && tex_[slot].ds != VK_NULL_HANDLE) ? tex_[slot].ds
                                                                : white_.ds;
    if (ds == VK_NULL_HANDLE) return;   // not even the white stand-in: nothing to draw with
    lightMapNow_ |= slot < tex_.size() && tex_[slot].emView != VK_NULL_HANDLE;
    vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 0, 1, &ds, 0, nullptr);
    // (the water reflection's pass binds its own: the tiles of ITS camera)
    const VkDescriptorSet ds1 = passFb_ != VK_NULL_HANDLE && ds1Refl_ != VK_NULL_HANDLE ? ds1Refl_ : ds1_;
    if (ds1 != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 1, 1,
                                &ds1, 0, nullptr);
    if (soft)
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 3, 1, &softDs_, 0, nullptr);
    // set 2, the pose buffer - bound for every draw (the shader declares it),
    // at the body's block for a posed one
    if (poseDs_ != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 2, 1,
                                &poseDs_, 1, &poseOff);
    push_.posed = posed ? 1 : 0;

    push_.cutout = d.cutout ? 1 : 0;
    push_.caster = (d.castsShadow || (rtOn_ && rtBodyDraw_)) ? 1 : 0;
    push_.bloomMode = bloom_ > 0.0f ? 1 : 0;
    // ...and a REFLECTING FLOOR's run (`Draw::reflect`), the water's rule at
    // its own strength - in the frame's pass, never the reflection's
    const bool floorNow = !reflectingNow_ && d.reflect > 0.0f;
    push_.water = waterNow_ ? 1 : floorNow ? 3 : reflectingNow_ ? 2 : 0;
    // ...and a WET floor's darkening (`Draw::wet`), in the mode's high bits -
    // the block is full: 0..255 above the low four
    if (!reflectingNow_ && d.wet > 0.0f) {
        push_.water |= static_cast<int32_t>(std::lround(std::min(d.wet, 1.0f) * 255.0f)) << 4;
        // ...and the RAIN's strength for its ripples, the eight bits above
        push_.water |= static_cast<int32_t>(std::lround(std::clamp(rain_, 0.0f, 1.0f) * 255.0f)) << 12;
        // ...and bit 20: it is SNOW lying, not rain (`Draw::snow`)
        if (d.snow) push_.water |= 1 << 20;
        // ...and bit 21: a BODY in the rain (`Draw::wetBody`), not a floor
        if (d.wetBody) push_.water |= 1 << 21;
    }
    push_.waterStrength = reflectingNow_ ? waterY_ : floorNow ? waterStrength_ * d.reflect : waterStrength_;
    {
        const Tex& bound = slot < tex_.size() && tex_[slot].ds != VK_NULL_HANDLE ? tex_[slot] : white_;
        push_.texScaled = bound.packed ? 1 : 0;
        push_.texScaleU = bound.su;
        push_.texScaleV = bound.sv;
    }
    push_.lit = litCount_ > 0 ? d.lit : (d.lit & (omk::kDrawShine | omk::kDrawFloor));
    if (!rtReflOn_) push_.lit &= ~omk::kDrawFloor;
    // THE FOG's two exclusions, applied here because this is where the bucket
    // key is - the same rule `renderer.cpp` applies for the software loop, and
    // it MUST be the same rule or the two backends draw different pictures.
    // Key bits 0x2080 (the near bucket, the transparent state) are not fogged;
    // key bit 0x800 (the cutout path, and the sky through mesh flag 0x10000)
    // doubles both ends.
    push_.fogStart = 0.0f;
    push_.fogEnd = 0.0f;
    if (fog_ && !(d.bucketKey & 0x2080u)) {
        const float fogScale = (d.bucketKey & 0x800u) ? 2.0f : 1.0f;
        push_.fogStart = fogStart_ * fogScale;
        push_.fogEnd   = fogEnd_ * fogScale;
        for (int i = 0; i < 3; ++i) push_.fogColour[i] = fogColour_[i];
    } else if (mistOn_ && !(d.bucketKey & 0x2080u)) {
        // THE MIST without the game's fog (an unlimited clip has none): the
        // same batches, marked -1 / -1 - no linear fog (`fogEnd > fogStart`
        // fails), the mist's to take (`scene.frag`)
        push_.fogStart = -1.0f;
        push_.fogEnd = -1.0f;
        for (int i = 0; i < 3; ++i) push_.fogColour[i] = fogColour_[i];
    }
    const auto it = vbo_.find(d.geo);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb_, 0, 1, &it->second.first, &off);
    // THE BACK-FACE CULL, one push per run of `cornerCull` (`geom3do.h`): a
    // batch groups by material, so single- and two-sided faces can share one.
    // `kCullSign` is this pipeline's sense of "front" for the software
    // rasterizer's winding; the mirror pass's screen-X flip reverses it, as it
    // reverses the area `raster.cpp` tests.
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
            vkCmdPushConstants(cb_, plo_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushMv), &pm_);
        } else {
            vkCmdPushConstants(cb_, plo_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(Push), &push_);
        }
        vkCmdDraw(cb_, static_cast<uint32_t>(j - i), 1, static_cast<uint32_t>(i), 0);
        i = j;
    }
    st_.drawn += static_cast<long>(d.count / 3);
}

// The NATIVE mirror pass - one frame, one submit, nothing touching the CPU.
//
// The boundary hands down decisions and this turns them into API calls, which
// is A2's rule: the reflected view is `drawWithMirror`'s (`p -= 2*dist*n` and
// the screen-X flip the engine's own bucket walk performs), `sceneClipped` is
// the scene restricted to the mirror's front half-space, and `mirror` is the
// draws whose corners carry flag `0x100000`. What is chosen HERE is only the
// stencil - the answer to "where may the reflection land", which the CPU path
// answers by differencing two frames instead.
bool VulkanRenderer::drawMirrorScene(const omk::View& v, const omk::View& refl,
                                     std::span<const omk::Draw> scene,
                                     std::span<const omk::Draw> sceneClipped,
                                     std::span<const omk::Draw> mirror) {
    // THE ORIGINAL'S TWO PASSES (`sub_440D90`, 0x00440D90):
    // the scene through the camera reflected in the mirror's plane FIRST, then
    // through the real camera over it - `Render_Frame(reflected, 1)` then
    // `Render_Frame(camera, 0)`, and `Render_FlushBuckets` begins its scene
    // with ZENABLE on and clears nothing between them. The reflected pass
    // leaves the depths of a virtual room behind the plane, so the real room's
    // walls, nearer, cover it everywhere but the mirror's opening, and the
    // mirror's own faces blend over what shows through. The GLES backend does
    // the same, so the two draw a mirror alike. The draws in FRONT of the plane
    // only, as the CPU fallback takes them.
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
// Two full-screen passes after the frame's own, over its depth: `ssao.frag`
// into an R8 target, then `ssao_blur.frag` averaging it 4x4 (depth-aware) and
// MULTIPLYING it into `colour_` through the blend, so the frame is only ever
// darkened. Built on first use at the target's size; `killSsao` on a resize.
bool VulkanRenderer::makeSsao() {
    if (aoBlurPipe_ != VK_NULL_HANDLE) return true;
    if (!ssaoOn_ || depthSampleView_ == VK_NULL_HANDLE) return false;
    if (!makeImage2d(VK_FORMAT_R8_UNORM, rw_, rh_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     aoImg_, aoMem_, aoView_, "ssao image")) return false;
    const auto pass = [&](VkFormat fmt, VkAttachmentLoadOp load, VkImageLayout from,
                          VkImageLayout to, VkImageView view, VkRenderPass& rp,
                          VkFramebuffer& fb) -> bool {
        VkAttachmentDescription a{};
        a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = load; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = from; a.finalLayout = to;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &rp), "ssao render pass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &view;
        fi.width = static_cast<uint32_t>(rw_); fi.height = static_cast<uint32_t>(rh_); fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &fb), "ssao framebuffer");
        return true;
    };
    if (!pass(VK_FORMAT_R8_UNORM, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aoView_, aoPass_, aoFbuf_)) return false;
    // the frame itself, loaded and handed back where the readback and the
    // present pass expect it
    if (!pass(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              colourView_, aoApplyPass_, aoApplyFbuf_)) return false;
    {
        VkDescriptorSetLayoutBinding b[2]{};
        for (int k = 0; k < 2; ++k) {
            b[k].binding = static_cast<uint32_t>(k); b[k].descriptorCount = 1;
            b[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 2; dli.pBindings = b;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &aoDsl_), "ssao set layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &aoPool_), "ssao pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = aoPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &aoDsl_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &aoDs_), "ssao set");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK(vkCreateSampler(dev_, &si, nullptr, &aoSampler_), "ssao sampler");
        VkDescriptorImageInfo ii[2] = {
            {aoSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {aoSampler_, aoView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet wr[2]{};
        for (int k = 0; k < 2; ++k) {
            wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr[k].dstSet = aoDs_; wr[k].dstBinding = static_cast<uint32_t>(k);
            wr[k].descriptorCount = 1;
            wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(dev_, 2, wr, 0, nullptr);
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, static_cast<uint32_t>(rtOn_ ? sizeof(SsaoRtPush) : sizeof(SsaoPush))};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    // RAY-TRACED: the rays' three sets after the pass's own (`ssao.frag` -DOMK_RT_SET=1..3)
    const VkDescriptorSetLayout aoSets[4] = {aoDsl_, rtDslA_, rtDslB_, rtDslC_};
    pli.setLayoutCount = rtOn_ ? 4 : 1; pli.pSetLayouts = aoSets;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &aoPlo_), "ssao pipeline layout");
    const auto module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes; si.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &si, nullptr, &m);
        return m;
    };
    const auto pipe = [&](const uint32_t* frag, size_t bytes, VkRenderPass rp, bool multiply,
                          VkPipeline& out) -> bool {
        VkShaderModule qv = module(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = module(frag, bytes);
        if (!qv || !qf) { std::fprintf(stderr, "vulkan: ssao shader module\n"); return false; }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, static_cast<float>(rw_), static_cast<float>(rh_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        // `OMK_SSAO_VIEW=1`: the occlusion ITSELF written over the frame, grey
        // on white, to see what the pass found rather than its effect
        static const bool aoView = omk::envFlag("OMK_SSAO_VIEW");
        if (multiply && aoView) {
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT;
        } else if (multiply) {
            // dst * src: the grey darkens, white leaves the frame as it was
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                 VK_COLOR_COMPONENT_B_BIT;
        } else {
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = aoPlo_; gp.renderPass = rp;
        const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
        if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: ssao pipeline\n"); return false; }
        return true;
    };
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        return (rtOn_ ? pipe(kSsaoRtMsFrag, sizeof(kSsaoRtMsFrag), aoPass_, false, aoPipe_)
                      : pipe(kSsaoMsFrag, sizeof(kSsaoMsFrag), aoPass_, false, aoPipe_)) &&
               pipe(kSsaoBlurMsFrag, sizeof(kSsaoBlurMsFrag), aoApplyPass_, true, aoBlurPipe_);
    return (rtOn_ ? pipe(kSsaoRtFrag, sizeof(kSsaoRtFrag), aoPass_, false, aoPipe_)
                  : pipe(kSsaoFrag, sizeof(kSsaoFrag), aoPass_, false, aoPipe_)) &&
           pipe(kSsaoBlurFrag, sizeof(kSsaoBlurFrag), aoApplyPass_, true, aoBlurPipe_);
}

void VulkanRenderer::killSsao() {
    if (aoBlurPipe_) { vkDestroyPipeline(dev_, aoBlurPipe_, nullptr); aoBlurPipe_ = VK_NULL_HANDLE; }
    if (aoPipe_) { vkDestroyPipeline(dev_, aoPipe_, nullptr); aoPipe_ = VK_NULL_HANDLE; }
    if (aoPlo_) { vkDestroyPipelineLayout(dev_, aoPlo_, nullptr); aoPlo_ = VK_NULL_HANDLE; }
    if (aoSampler_) { vkDestroySampler(dev_, aoSampler_, nullptr); aoSampler_ = VK_NULL_HANDLE; }
    if (aoPool_) { vkDestroyDescriptorPool(dev_, aoPool_, nullptr); aoPool_ = VK_NULL_HANDLE; aoDs_ = VK_NULL_HANDLE; }
    if (aoDsl_) { vkDestroyDescriptorSetLayout(dev_, aoDsl_, nullptr); aoDsl_ = VK_NULL_HANDLE; }
    if (aoApplyFbuf_) { vkDestroyFramebuffer(dev_, aoApplyFbuf_, nullptr); aoApplyFbuf_ = VK_NULL_HANDLE; }
    if (aoFbuf_) { vkDestroyFramebuffer(dev_, aoFbuf_, nullptr); aoFbuf_ = VK_NULL_HANDLE; }
    if (aoApplyPass_) { vkDestroyRenderPass(dev_, aoApplyPass_, nullptr); aoApplyPass_ = VK_NULL_HANDLE; }
    if (aoPass_) { vkDestroyRenderPass(dev_, aoPass_, nullptr); aoPass_ = VK_NULL_HANDLE; }
    if (aoView_) { vkDestroyImageView(dev_, aoView_, nullptr); aoView_ = VK_NULL_HANDLE; }
    if (aoImg_) { vkDestroyImage(dev_, aoImg_, nullptr); aoImg_ = VK_NULL_HANDLE; }
    if (aoMem_) { freeDeviceMemory(aoMem_); aoMem_ = VK_NULL_HANDLE; }
}

void VulkanRenderer::runSsao() {
    if (enhOff_) return;
    if (!makeSsao() || aoVw_ <= 0 || aoVh_ <= 0) return;
    // the frame's depth and colour writes, before the passes read them
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    // the scene's projection (`pushView`), the frame's viewport, and the
    // look: a radius of about two metres (the units are inches), so the
    // architecture's creases and corners read, not only a body's contact
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const SsaoPush push{aoTanH_, aoTanV_, farP / (farP - nearP), -farP * nearP / (farP - nearP),
                        static_cast<float>(aoVw_), static_cast<float>(aoVh_), 80.0f, 1.0f};
    // RAY-TRACED: the same push block and the camera in the world after it (every instance
    // occludes: the set and the bodies), and the scene the rays see
    const float aoMask = 255.0f;
    const SsaoRtPush rp{push, {rtEye_[0], rtEye_[1], rtEye_[2], aoMask}, {rtS_[0], rtS_[1], rtS_[2], 0.0f},
                        {rtU_[0], rtU_[1], rtU_[2], 0.0f}, {rtF_[0], rtF_[1], rtF_[2], 0.0f}};
    if (rtOn_) rtAoRays_ += 4LL * aoVw_ * aoVh_;   // at most: a sky pixel casts none
    const auto run = [&](VkRenderPass rp_, VkFramebuffer fb, VkPipeline pl) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp_; bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
        vkCmdBeginRenderPass(cb_, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        const VkDescriptorSet aoSets[4] = {aoDs_, rtDsA_, rtDsB_, rtDsC_};
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, aoPlo_, 0, rtOn_ ? 4 : 1, aoSets, 0, nullptr);
        if (rtOn_) vkCmdPushConstants(cb_, aoPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof rp, &rp);
        else vkCmdPushConstants(cb_, aoPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDraw(cb_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb_);
    };
    run(aoPass_, aoFbuf_, aoPipe_);
    VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
    run(aoApplyPass_, aoApplyFbuf_, aoBlurPipe_);
}

// ---- HYBRID RAY TRACING, `todo/enhancements.md` 19, 26 ----------------------
//
// AN ENHANCEMENT, off unless asked (`raytracing = on`, `--raytracing`): the Direct3D 12 window's
// rays, on Vulkan, from one GLSL source (`rt.glsl`, the -DOMK_RT arms of `scene.frag` and
// `ssao.frag`) and one description of the scene (`o3de/rtscene.h`). VK_KHR_ray_query casts INLINE
// ray queries from the raster passes - no ray-tracing pipeline, no shader binding table - against
// VK_KHR_acceleration_structure's BLAS and TLAS, which are built here from what the frame submits:
//   * a SET geometry (no pose, not a caster) gets a BLAS once, from all of its opaque and cutout
//     batches, remade when its revision moves; it stays in the TLAS for `kSetKeep` frames after it
//     was last drawn;
//   * the BODIES share ONE BLAS rebuilt every frame from their drawn opaque runs, posed here on the
//     CPU by the affines `scene.vert` applies, refitted while the same runs make it;
//   * one TLAS a frame, identity transforms, the sets' mask 1, the bodies' 2.
// The builds go on a command buffer of their own, submitted just before the frame's in the same
// `vkQueueSubmit` (the acceleration structures are read by the frame's fragment shaders; a barrier
// ends the builds'). Every triangle has a 16-word RECORD (texel coordinates and baked colour of its
// three corners, and its texture's word) in a buffer of its BLAS; the shader reads the records and
// the textures BINDLESS - two descriptor-indexing arrays (partially bound, updated after bind: the
// records change as draws are submitted and the pool of textures as `setTextures` is called, both
// while the frame's command buffer is recording), indexed by slot.
// -> the next free slot of a table, or -1
static int rtSlot(std::vector<int>& freeList, int& next, int cap) {
    if (!freeList.empty()) { const int s = freeList.back(); freeList.pop_back(); return s; }
    return next < cap ? next++ : -1;
}

bool VulkanRenderer::rtBuffer(RtBuf& r, VkDeviceSize bytes, VkBufferUsageFlags use, bool host, const char* tag) {
    r = RtBuf{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes ? bytes : 1; bi.usage = use; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bi, nullptr, &r.b) != VK_SUCCESS) {
        std::fprintf(stderr, "vulkan: ray tracing buffer of %llu bytes (%s) failed\n",
                     static_cast<unsigned long long>(bytes), tag);
        r = RtBuf{};
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, r.b, &req);
    const auto pick = [&](VkMemoryPropertyFlags want) {
        for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & want) == want)
                return static_cast<int>(i);
        return -1;
    };
    // what the CPU writes sits in device-local memory the host can map (Resizable BAR) when the
    // card has it, else in system memory - the Direct3D 12 backend's upload heap
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags tries[2] = {host ? (hv | dl) : dl, host ? hv : 0};
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.pNext = (use & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ? &fi : nullptr;
    bool ok = false;
    for (int k = 0; k < 2 && !ok; ++k) {
        const int t = pick(tries[k]);
        if (t < 0) continue;
        ai.memoryTypeIndex = static_cast<uint32_t>(t);
        ok = vkAllocateMemory(dev_, &ai, nullptr, &r.m) == VK_SUCCESS;
    }
    if (!ok || vkBindBufferMemory(dev_, r.b, r.m, 0) != VK_SUCCESS ||
        (host && vkMapMemory(dev_, r.m, 0, VK_WHOLE_SIZE, 0, &r.p) != VK_SUCCESS)) {
        std::fprintf(stderr, "vulkan: ray tracing buffer of %llu bytes (%s) failed\n",
                     static_cast<unsigned long long>(bytes), tag);
        if (ok) freeDeviceMemory(r.m);
        vkDestroyBuffer(dev_, r.b, nullptr);
        r = RtBuf{};
        return false;
    }
    OMK_GPU_ALLOC(tag, omk::prof::gpuKey(omk::prof::kVkMemory, r.m), ai.allocationSize);
    r.cap = bytes;
    if (use & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = r.b;
        r.va = vkGetBufferDeviceAddress(dev_, &da);
    }
    return true;
}

// a buffer the GPU may still read goes with the frame's dead ones (`begin` frees them, the
// previous frame having been waited for); one nothing reads any more is freed at once
void VulkanRenderer::rtRetire(RtBuf& r) {
    if (r.b) deadBufs_.push_back({r.b, r.m});
    r = RtBuf{};
}
void VulkanRenderer::rtFree(RtBuf& r) {
    if (r.b) vkDestroyBuffer(dev_, r.b, nullptr);
    if (r.m) freeDeviceMemory(r.m);
    r = RtBuf{};
}

// a host-visible buffer of at least `bytes`, mapped for its life; a smaller one is retired
bool VulkanRenderer::rtMapped(RtBuf& r, VkDeviceSize bytes, VkBufferUsageFlags use) {
    if (r.b && r.cap >= bytes) return true;
    rtRetire(r);
    const VkDeviceSize want = (bytes + bytes / 4 + 255) & ~static_cast<VkDeviceSize>(255);
    return rtBuffer(r, want, use, true, "ray tracing");
}

void VulkanRenderer::rtWriteTex(int slot, VkImageView view) {
    const VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = rtDsC_; wr.dstBinding = 0; wr.dstArrayElement = static_cast<uint32_t>(slot);
    wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
}
void VulkanRenderer::rtWriteRec(int slot, VkBuffer b) {
    const VkDescriptorBufferInfo bi{b, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = rtDsB_; wr.dstBinding = 0; wr.dstArrayElement = static_cast<uint32_t>(slot);
    wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
}
int VulkanRenderer::rtAllocTex(VkImageView view) {
    const int s = rtSlot(rtTexFree_, rtTexNext_, kRtTexSlots);
    if (s >= 0) rtWriteTex(s, view);
    return s;
}
void VulkanRenderer::rtFreeTex(int slot) {
    if (slot < 0) return;
    rtTexFree_.push_back(slot);
    ++texGen_;   // no ray-tracing record may name the slot now
}

// a slot's texture word for a record: the table index, and the pack's scale per axis in bits 24.. (`omk::rtTexScaleBits`)
// (`rt.glsl` `rtTexel`)
std::uint32_t VulkanRenderer::rtTexWord(std::size_t slot) const {
    if (slot >= tex_.size() || tex_[slot].rt < 0) return static_cast<std::uint32_t>(std::max(white_.rt, 0));
    return static_cast<std::uint32_t>(tex_[slot].rt) | omk::rtTexScaleBits(tex_[slot].packed ? tex_[slot].su : 1.0f, tex_[slot].packed ? tex_[slot].sv : 1.0f);
}

bool VulkanRenderer::rtInit() {
    const auto fn = [&](const char* n) { return vkGetDeviceProcAddr(dev_, n); };
    rtCreateAs_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(fn("vkCreateAccelerationStructureKHR"));
    rtDestroyAs_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(fn("vkDestroyAccelerationStructureKHR"));
    rtCmdBuild_ = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(fn("vkCmdBuildAccelerationStructuresKHR"));
    rtBuildSizes_ = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(fn("vkGetAccelerationStructureBuildSizesKHR"));
    rtAsAddr_ = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(fn("vkGetAccelerationStructureDeviceAddressKHR"));
    const auto fail = [&](const char* what) {
        std::printf("vulkan: ray tracing - %s - off\n", what);
        rtKill();
        return false;
    };
    if (!rtCreateAs_ || !rtDestroyAs_ || !rtCmdBuild_ || !rtBuildSizes_ || !rtAsAddr_)
        return fail("the device's acceleration structure functions are missing");
    // the instance table's sampler for the hit's texel: NEAREST and WRAP, the original's - whatever
    // the filtering enhancement does to the raster (`rtTexel` reads level 0)
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (vkCreateSampler(dev_, &si, nullptr, &rtSampler_) != VK_SUCCESS) return fail("no sampler");
    // set A: the TLAS, the instance table, `RtFrame`, the sampler, the rays' tally
    VkDescriptorSetLayoutBinding ba[5]{};
    const VkDescriptorType ta[5] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER,
                                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    for (uint32_t k = 0; k < 5; ++k) {
        ba[k].binding = k; ba[k].descriptorCount = 1; ba[k].descriptorType = ta[k];
        ba[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    ba[3].pImmutableSamplers = &rtSampler_;
    VkDescriptorSetLayoutCreateInfo la{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    la.bindingCount = 5; la.pBindings = ba;
    // sets B and C: the bindless tables, partially bound and updated after bind
    const VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fl.bindingCount = 1; fl.pBindingFlags = &bf;
    VkDescriptorSetLayoutBinding bb{}, bc{};
    bb.binding = 0; bb.descriptorCount = kRtRecSlots; bb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bc.binding = 0; bc.descriptorCount = kRtTexSlots; bc.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lb{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lb.pNext = &fl; lb.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lb.bindingCount = 1; lb.pBindings = &bb;
    VkDescriptorSetLayoutCreateInfo lc = lb;
    lc.pBindings = &bc;
    if (vkCreateDescriptorSetLayout(dev_, &la, nullptr, &rtDslA_) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(dev_, &lb, nullptr, &rtDslB_) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(dev_, &lc, nullptr, &rtDslC_) != VK_SUCCESS)
        return fail("the descriptor set layouts could not be made");
    const VkDescriptorPoolSize ps[5] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 + kRtRecSlots},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
                                        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kRtTexSlots}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 3; pi.poolSizeCount = 5; pi.pPoolSizes = ps;
    const VkDescriptorSetLayout ls[3] = {rtDslA_, rtDslB_, rtDslC_};
    VkDescriptorSet ds[3] = {};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    if (vkCreateDescriptorPool(dev_, &pi, nullptr, &rtPool_) != VK_SUCCESS) return fail("no descriptor pool");
    ai.descriptorPool = rtPool_; ai.descriptorSetCount = 3; ai.pSetLayouts = ls;
    if (vkAllocateDescriptorSets(dev_, &ai, ds) != VK_SUCCESS) return fail("no descriptor sets");
    rtDsA_ = ds[0]; rtDsB_ = ds[1]; rtDsC_ = ds[2];
    // the TLAS, sized for the most instances it can hold; the build's scratch is made with it
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR ti{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1; ti.pGeometries = &tg;
    const uint32_t maxInst = omk::rt::kMaxInst;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    rtBuildSizes_(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &ti, &maxInst, &sz);
    rtTlasScratch_ = (sz.buildScratchSize + rtScratchAlign_ - 1) / rtScratchAlign_ * rtScratchAlign_;
    const VkBufferUsageFlags sda = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const bool ok =
        rtBuffer(rtTlasBuf_, (sz.accelerationStructureSize + 255) & ~VkDeviceSize(255),
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | sda, false, "ray tracing") &&
        rtBuffer(rtInstDesc_, sizeof(VkAccelerationStructureInstanceKHR) * maxInst,
                 VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | sda, true, "ray tracing") &&
        rtBuffer(rtInstTab_, 16 * maxInst, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "ray tracing") &&
        rtBuffer(rtFrameBuf_, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, "ray tracing") &&
        rtBuffer(rtCountBuf_, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, "ray tracing");
    if (!ok) return fail("its buffers could not be made");
    std::memset(rtCountBuf_.p, 0, 256);
    std::memset(rtFrameBuf_.p, 0, 256);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = rtTlasBuf_.b; ci.size = sz.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    if (rtCreateAs_(dev_, &ci, nullptr, &rtTlas_) != VK_SUCCESS) return fail("the TLAS could not be made");
    VkWriteDescriptorSetAccelerationStructureKHR wa{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    wa.accelerationStructureCount = 1; wa.pAccelerationStructures = &rtTlas_;
    const VkDescriptorBufferInfo bi[3] = {{rtInstTab_.b, 0, VK_WHOLE_SIZE}, {rtFrameBuf_.b, 0, VK_WHOLE_SIZE},
                                          {rtCountBuf_.b, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[4]{};
    for (auto& w : wr) { w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = rtDsA_; w.descriptorCount = 1; }
    wr[0].pNext = &wa; wr[0].dstBinding = 0; wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wr[1].dstBinding = 1; wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[1].pBufferInfo = &bi[0];
    wr[2].dstBinding = 2; wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; wr[2].pBufferInfo = &bi[1];
    wr[3].dstBinding = 4; wr[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[3].pBufferInfo = &bi[2];
    vkUpdateDescriptorSets(dev_, 4, wr, 0, nullptr);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_, &cai, &rtCb_) != VK_SUCCESS) return fail("no command buffer");
    rtOn_ = true;
    std::printf("vulkan: RAY TRACING on - an ENHANCEMENT: VK_KHR_ray_query inline ray queries over "
                "VK_KHR_acceleration_structure: characters lit by the set around them (no ray-traced "
                "shadow on them), ray-traced AO in place of SSAO\n");
    omk::event("rt_on", {{"vulkan", 1}});
    return true;
}

void VulkanRenderer::rtDrop(RtBlas& b) {
    if (b.as) rtDestroyAs_(dev_, b.as, nullptr);
    rtFree(b.asBuf);
    rtFree(b.pos);
    rtFree(b.rec);
    if (b.recSlot >= 0) rtRecFree_.push_back(b.recSlot);
    b = RtBlas{};
}

// the frame's constants, the camera for the AO pass, and the three descriptor sets of the scene
// pass (bound after the pass begins, once: every draw's pipeline layout keeps them)
void VulkanRenderer::rtBind(const omk::View& view) {
    ++rtFrame_;
    rtCounting_ = rtCountFeat_ && rtFrame_ % 60 == 2;
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
    std::memcpy(rtFrameBuf_.p, &f, sizeof f);
    const VkDescriptorSet s[3] = {rtDsA_, rtDsB_, rtDsC_};
    vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 4, 3, s, 0, nullptr);
}

// A BLAS's triangles - opaque ones first - into its two host-visible buffers: the corners'
// positions (posed by the triangle's affines when it has them) and the records. Its records'
// buffer joins the bindless table (again, when it was remade).
bool VulkanRenderer::rtFill(RtBlas& b, const std::vector<RtTri>& tri) {
    const std::size_t n = tri.size();
    const VkBuffer recWas = b.rec.b;
    if (!rtMapped(b.pos, n * 36,
                  VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ||
        !rtMapped(b.rec, n * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT))
        return false;
    if (b.recSlot < 0) b.recSlot = rtSlot(rtRecFree_, rtRecNext_, kRtRecSlots);
    if (b.recSlot < 0) return false;
    if (b.rec.b != recWas) rtWriteRec(b.recSlot, b.rec.b);
    omk::rt::fillRecords(tri, static_cast<float*>(b.pos.p), static_cast<std::uint32_t*>(b.rec.p));
    return true;
}

// A SET geometry's BLAS inputs, made on first sight and again when its corners move: every opaque
// and cutout batch. -> null when it has none.
VulkanRenderer::RtBlas* VulkanRenderer::rtSetFor(const omk::Geometry* g) {
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
    const std::uint32_t white = rtTexWord(~std::size_t(0));
    for (const std::uint32_t c : opq) tris.push_back(RtTri{g, nullptr, 0, c, white});
    if (opq.empty() || !rtFill(b, tris)) { b.nOpaque = b.nCut = 0; return nullptr; }
    b.triOf.assign(g->corners.size() / 3, -1);
    for (std::size_t t = 0; t < opq.size(); ++t) b.triOf[opq[t] / 3] = static_cast<std::int32_t>(t);
    return &b;
}

// Every opaque draw, as it is submitted: a body's run noted for this frame's BLAS (its affines
// copied while the pointer is good), a set's texture slots written into its records when they changed.
void VulkanRenderer::rtNote(const omk::Draw& d) {
    if (d.blend != omk::Blend::Opaque || d.start + d.count > d.geo->corners.size()) return;
    const std::size_t slot = omk::drawTextureSlot(d);
    const std::uint32_t tex = rtTexWord(slot);   // the table index, with a pack's k
    const bool posed = d.meshPose && d.meshPoses && d.geo->cornerMesh.size() == d.geo->corners.size();
    if (posed || d.castsShadow || (d.lit & ~(omk::kDrawSoft | omk::kDrawFloor)) != 0) {
        RtBodyNote& n = rtNotes_[{posed ? d.meshPose : nullptr, d.geo}];
        n.geo = d.geo;
        if (posed && n.aff.empty()) n.aff.assign(d.meshPose, d.meshPose + 12 * d.meshPoses);
        n.runs[d.start] = RtRun{d.count, d.cutout != 0, tex};
        return;
    }
    RtBlas* b = rtSetFor(d.geo);
    if (!b) return;
    b->seen = rtFrame_;
    auto* rec = static_cast<std::uint32_t*>(b->rec.p);
    if (b->texGen != texGen_) {   // a texture went: no record may name its slot
        b->texGen = texGen_;
        b->texAt.clear();
        const std::uint32_t white = rtTexWord(~std::size_t(0));
        for (std::uint32_t t = 0; t < b->nOpaque + b->nCut; ++t) rec[16 * t + 15] = white;
    }
    const auto at = b->texAt.find(d.start);
    if (at != b->texAt.end() && at->second.first == d.count && at->second.second == tex) return;
    b->texAt[d.start] = {d.count, tex};
    for (std::size_t c = d.start; c + 3 <= d.start + d.count; c += 3) {
        const std::int32_t t = c / 3 < b->triOf.size() ? b->triOf[c / 3] : -1;
        if (t >= 0) rec[16 * t + 15] = tex;
    }
}

// The frame's acceleration structures, recorded on their own command buffer (submitted before the
// frame's): the bodies' BLAS (rebuilt, or refitted), any set BLAS not built yet, then the TLAS over
// the sets drawn lately and this frame's bodies.
void VulkanRenderer::rtBuild() {
    struct Job {
        RtBlas* b;
        bool body, refit;
        VkAccelerationStructureGeometryKHR gd[2];
        VkAccelerationStructureBuildRangeInfoKHR rg[2];
        uint32_t prim[2], ng;
        VkAccelerationStructureBuildGeometryInfoKHR in;
        VkDeviceSize scratch;
    };
    std::vector<Job> jobs;
    rtRecorded_ = false;
    const auto tStart = std::chrono::steady_clock::now();
    const VkDeviceSize al = rtScratchAlign_;
    const auto up = [&](VkDeviceSize v) { return (v + al - 1) / al * al; };
    // 1. this frame's bodies, posed and recorded, ALL of them in ONE BLAS: a build has a fixed
    // cost, and a street's crowd is a couple of hundred bodies of a few hundred triangles
    {
        std::vector<RtTri> tri;
        int bodies = 0;
        std::uint64_t sig = 0;
        RtBlas& b = rtBodyAll_;
        omk::rt::gatherBodies(rtNotes_, rtEye_, tri, b.nOpaque, b.nCut, bodies, sig);
        b.built = false;
        rtBodyCount_ = bodies;
        const VkBuffer posWas = b.pos.b;
        if (!tri.empty() && rtFill(b, tri)) {
            b.seen = rtFrame_;
            // REFITTED in place while the same runs of the same bodies make it (only their poses
            // moved), rebuilt when that changes and every `kBodyRefitFrames` frames - a refit's tree
            // loosens as the limbs travel
            const bool refit = b.as && sig == rtBodySig_ && b.pos.b == posWas &&
                               rtFrame_ - rtBodyRebuilt_ < omk::rt::kBodyRefitFrames;
            if (!refit) { rtBodySig_ = sig; rtBodyRebuilt_ = rtFrame_; }
            jobs.push_back(Job{&b, true, refit, {}, {}, {}, 0, {}, 0});
        } else {
            b.nOpaque = b.nCut = 0;
        }
    }
    // 2. a set's, once
    for (auto& [g, b] : rtSet_)
        if (!b.built && b.nOpaque + b.nCut && rtFrame_ - b.seen <= omk::rt::kSetKeep)
            jobs.push_back(Job{&b, false, false, {}, {}, {}, 0, {}, 0});
    // their inputs and sizes, and one scratch for all of them and the TLAS
    VkDeviceSize scratch = rtTlasScratch_;
    for (Job& j : jobs) {
        RtBlas& b = *j.b;
        const auto geom = [&](VkDeviceAddress at, std::uint32_t tris, bool opaque) {
            VkAccelerationStructureGeometryKHR& g = j.gd[j.ng];
            g = VkAccelerationStructureGeometryKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            // a CUTOUT geometry is non-opaque: `rtTrace` reads its colour key
            g.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
            auto& t = g.geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexData.deviceAddress = at;
            t.vertexStride = 12;
            t.maxVertex = 3 * tris - 1;
            t.indexType = VK_INDEX_TYPE_NONE_KHR;
            j.prim[j.ng] = tris;
            j.rg[j.ng] = VkAccelerationStructureBuildRangeInfoKHR{tris, 0, 0, 0};
            ++j.ng;
        };
        j.ng = 0;
        if (b.nOpaque) geom(b.pos.va, b.nOpaque, true);
        if (b.nCut) geom(b.pos.va + 36ull * b.nOpaque, b.nCut, false);
        j.in = VkAccelerationStructureBuildGeometryInfoKHR{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        j.in.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        j.in.flags = j.body ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR |
                                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                            : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        j.in.mode = j.refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        j.in.geometryCount = j.ng;
        j.in.pGeometries = j.gd;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        rtBuildSizes_(dev_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &j.in, j.prim, &sz);
        j.scratch = up(j.refit ? sz.updateScratchSize : sz.buildScratchSize);
        scratch += j.scratch;
        const VkDeviceSize need = (sz.accelerationStructureSize + 255) & ~VkDeviceSize(255);
        if (!j.refit && (!b.as || b.asBuf.cap < need)) {
            if (b.as) rtDeadAs_.push_back(b.as);
            b.as = VK_NULL_HANDLE;
            rtRetire(b.asBuf);
            const VkDeviceSize cap = j.body ? (need + need / 4 + 255) & ~VkDeviceSize(255) : need;
            if (rtBuffer(b.asBuf, cap, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false, "ray tracing")) {
                VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                ci.buffer = b.asBuf.b; ci.size = cap;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if (rtCreateAs_(dev_, &ci, nullptr, &b.as) != VK_SUCCESS) b.as = VK_NULL_HANDLE;
                else {
                    VkAccelerationStructureDeviceAddressInfoKHR da{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                    da.accelerationStructure = b.as;
                    b.asAddr = rtAsAddr_(dev_, &da);
                }
            }
        }
    }
    if (rtScratch_.cap < scratch) {
        rtRetire(rtScratch_);
        const VkDeviceSize cap = (scratch + scratch / 4 + 255) & ~VkDeviceSize(255);
        if (!rtBuffer(rtScratch_, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      false, "ray tracing"))
            return;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(rtCb_, 0);
    vkBeginCommandBuffer(rtCb_, &bi);
    VkDeviceSize so = 0;
    for (Job& j : jobs) {
        if (!j.b->as) continue;
        VkAccelerationStructureBuildGeometryInfoKHR in = j.in;
        in.pGeometries = j.gd;
        in.dstAccelerationStructure = j.b->as;
        if (j.refit) in.srcAccelerationStructure = j.b->as;
        in.scratchData.deviceAddress = rtScratch_.va + so;
        const VkAccelerationStructureBuildRangeInfoKHR* pr = j.rg;
        rtCmdBuild_(rtCb_, 1, &in, &pr);
        so += j.scratch;
        j.b->built = true;
        ++(j.refit ? rtBodyRefits_ : j.body ? rtBodyBuilds_ : rtSetBuilds_);
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(rtCb_, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0, nullptr, 0, nullptr);
    // 3. the TLAS: an instance a BLAS, its custom index the row of the table
    auto* inst = static_cast<VkAccelerationStructureInstanceKHR*>(rtInstDesc_.p);
    auto* tab = static_cast<std::uint32_t*>(rtInstTab_.p);
    uint32_t ni = 0;
    int sets = 0, bodies = 0;
    long long setTris = 0, bodyTris = 0;
    const auto add = [&](const RtBlas& b, uint32_t mask) {
        if (ni >= omk::rt::kMaxInst || !b.built || !b.as) return false;
        VkAccelerationStructureInstanceKHR& d = inst[ni];
        std::memset(&d, 0, sizeof d);
        d.transform.matrix[0][0] = d.transform.matrix[1][1] = d.transform.matrix[2][2] = 1.0f;
        d.instanceCustomIndex = ni;
        d.mask = mask;
        d.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        d.accelerationStructureReference = b.asAddr;
        tab[4 * ni] = static_cast<std::uint32_t>(b.recSlot);
        tab[4 * ni + 1] = b.nOpaque;
        tab[4 * ni + 2] = mask;
        tab[4 * ni + 3] = 0;
        ++ni;
        return true;
    };
    for (const auto& [g, b] : rtSet_)
        if (rtFrame_ - b.seen <= omk::rt::kSetKeep && add(b, omk::rt::kMaskSet)) { ++sets; setTris += b.nOpaque + b.nCut; }
    if (rtBodyAll_.seen == rtFrame_ && add(rtBodyAll_, omk::rt::kMaskBody)) {
        bodies = rtBodyCount_;
        bodyTris = rtBodyAll_.nOpaque + rtBodyAll_.nCut;
    }
    VkAccelerationStructureGeometryKHR tg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tg.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tg.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tg.geometry.instances.data.deviceAddress = rtInstDesc_.va;
    VkAccelerationStructureBuildGeometryInfoKHR ti{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    ti.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    ti.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    ti.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    ti.geometryCount = 1; ti.pGeometries = &tg;
    ti.dstAccelerationStructure = rtTlas_;
    ti.scratchData.deviceAddress = rtScratch_.va + so;
    const VkAccelerationStructureBuildRangeInfoKHR tr{ni, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ptr = &tr;
    rtCmdBuild_(rtCb_, 1, &ti, &ptr);
    // ...and the frame's fragment shaders read it
    VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb2.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb2.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(rtCb_, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(rtCb_);
    rtRecorded_ = true;
    // the CPU's share (the bodies posed, the inputs recorded), for the log
    rtCpuMs_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStart).count();
    rtInstances_ = static_cast<int>(ni); rtSets_ = sets; rtBodies_ = bodies;
    rtSetTris_ = setTris; rtBodyTris_ = bodyTris;
}

// After the frame ran: the rays' tally read back, and the log.
void VulkanRenderer::rtAfter() {
    std::memcpy(rtCounts_, rtCountBuf_.p, sizeof rtCounts_);
    const int ni = rtInstances_, sets = rtSets_, bodies = rtBodies_;
    const long long setTris = rtSetTris_, bodyTris = rtBodyTris_;
    if ((ni != rtToldInst_ && rtTold_ < 6) || rtFrame_ % 60 == 30) {
        rtToldInst_ = ni;
        ++rtTold_;
        // the tally covers every COUNTING frame so far (`rtBind`)
        const long cf = std::max(1L, rtCounted_);
        const unsigned indirectRays = static_cast<unsigned>(rtCounts_[1] / cf);
        std::printf("vulkan: ray tracing live (frame %ld) - TLAS of %d instances: %d set BLAS (%lld triangles), "
                    "%d bodies in one BLAS (%lld triangles); ~%u indirect rays a frame; "
                    "builds %.2f ms CPU a frame, the bodies' BLAS built %ld times and refitted %ld\n", rtFrame_,
                    ni, sets, setTris, bodies, bodyTris, indirectRays, rtCpuMs_ / rtFrame_, rtBodyBuilds_,
                    rtBodyRefits_);
        omk::event("rt_tlas", {{"frame", rtFrame_}, {"instances", ni}, {"sets", sets},
                               {"bodies", bodies}, {"set_tris", static_cast<long>(setTris)},
                               {"body_tris", static_cast<long>(bodyTris)},
                               {"indirect_rays", indirectRays}});
        if (rtReflOn_) {
            const unsigned reflRays = static_cast<unsigned>(rtCounts_[2] / cf);
            std::printf("vulkan: rt reflections (frame %ld) - %ld ground draws, ~%u reflection rays a frame\n",
                        rtFrame_, rtFloorDraws_, reflRays);
            omk::event("rt_refl.frame", {{"frame", rtFrame_}, {"floor_draws", rtFloorDraws_},
                                         {"refl_rays", reflRays}});
        }
    }
    rtFloorDraws_ = 0;
}

void VulkanRenderer::rtKill() {
    if (dev_ == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(dev_);
    if (rtOn_ && rtCountBuf_.p) {
        std::memcpy(rtCounts_, rtCountBuf_.p, sizeof rtCounts_);
        std::printf("vulkan: ray tracing - %ld frames; the last TLAS %d instances (%d set BLAS, %d bodies); "
                    "%ld set BLAS builds, %ld body BLAS builds; ~%u indirect rays in %ld counted frames\n",
                    rtFrame_, rtInstances_, rtSets_, rtBodies_, rtSetBuilds_, rtBodyBuilds_, rtCounts_[1],
                    rtCounted_);
        omk::event("rt_summary", {{"frames", rtFrame_}, {"instances", rtInstances_}, {"sets", rtSets_},
                                  {"bodies", rtBodies_}, {"set_builds", rtSetBuilds_},
                                  {"body_builds", rtBodyBuilds_}, {"indirect_rays", rtCounts_[1]}});
    }
    if (rtDestroyAs_) {
        for (auto& [g, b] : rtSet_) rtDrop(b);
        rtDrop(rtBodyAll_);
        for (auto as : rtDeadAs_) rtDestroyAs_(dev_, as, nullptr);
        if (rtTlas_) rtDestroyAs_(dev_, rtTlas_, nullptr);
    }
    rtSet_.clear();
    rtNotes_.clear();
    rtDeadAs_.clear();
    rtTlas_ = VK_NULL_HANDLE;
    for (RtBuf* r : {&rtTlasBuf_, &rtInstDesc_, &rtInstTab_, &rtScratch_, &rtFrameBuf_, &rtCountBuf_}) rtFree(*r);
    if (rtCb_) vkFreeCommandBuffers(dev_, pool_, 1, &rtCb_);
    rtCb_ = VK_NULL_HANDLE;
    if (rtPool_) vkDestroyDescriptorPool(dev_, rtPool_, nullptr);
    rtPool_ = VK_NULL_HANDLE;
    for (VkDescriptorSetLayout* l : {&rtDslA_, &rtDslB_, &rtDslC_, &dslEmpty_})
        if (*l) { vkDestroyDescriptorSetLayout(dev_, *l, nullptr); *l = VK_NULL_HANDLE; }
    if (rtSampler_) vkDestroySampler(dev_, rtSampler_, nullptr);
    rtSampler_ = VK_NULL_HANDLE;
    rtOn_ = false;
}

// ---- FRAME GENERATION, `todo/enhancements.md` 27 ---------------------------
//
// AN ENHANCEMENT, off unless asked; the Direct3D 12 window's, on Vulkan. The same three passes from one
// GLSL source (`fg_cap.frag`, `fg_splat.frag`, `fg_resolve.frag`) and the same rules (`o3de/framegen.h`):
//
//   * THE SCENE writes motion vectors (`mvOn_`: `scene.vert/frag` built -DOMK_MOTION - the previous
//     view-projection in the push block, the bodies' previous poses after the current ones in the pose
//     buffer - into a second target, R16G16_SFLOAT, opaque geometry only).
//   * THE CAPTURE (`fg_cap.frag`), on the frame's command buffer after the post chain: the finished world
//     picture at display size (the supersample folded), its motion in display pixels and its view depth;
//     two frames kept.
//   * THE SPLAT (`fg_splat.frag`): frame N's pixels half-way back along their vectors, N-1's half-way
//     forward along theirs turned round, into two R32 heads by `imageAtomicMin` of depth and source - a
//     depth-tested forward warp - and the RESOLVE (`fg_resolve.frag`): the front-most surface wins,
//     blended with the other frame where it shows the same surface there, alone where it is hidden
//     there (a disocclusion), cracks filled from the farthest neighbour.
//   * THE PRESENT: the in-between picture through the SAME present pass and the SAME overlay (this
//     frame's 2D - the interface is never interpolated) and out at once; the real frame follows half the
//     measured interval later - its blit is done on the frame's thread, its vkQueuePresentKHR deferred to
//     a thread of the backend's own on a second queue of the family (`presQ_`; without one the real
//     frame goes out at once, after the in-between one). A v-synced swapchain shows one picture a
//     refresh, so the pair costs two refreshes: hence the display's ROOM rule (`fgMidFrame`).
//   * NOT INTERPOLATED: a first frame, a pair with a CAMERA CUT on either side, a present with no new
//     frame rendered since the last, a frame composed on the CPU. `--frames N` still renders and presents
//     N real frames; the in-between ones are presents only, and the window dump (`OMK_WINDUMP`) is always
//     a real frame's (it reads `presImg_`, which the real frame's present pass writes last).

bool VulkanRenderer::fgStart() {
    fgAlways_ = omk::envFlag("OMK_FRAMEGEN_ALWAYS");
    fgProbe_ = omk::envStr("OMK_FRAMEGEN_PROBE");
    if (fgProbe_ && !*fgProbe_) fgProbe_ = nullptr;
    fgSlots_ = fgProbe_ ? 3 : 2;
    if (presQ_ != VK_NULL_HANDLE) fgThread_ = std::thread([this] { fgThreadMain(); });
    std::printf("framegen: on - an ENHANCEMENT: an in-between picture warped along the motion vectors before "
                "every rendered frame (motion vectors on for it); the display refreshes at %d Hz. The real "
                "frame goes out half an interval after its in-between one%s - that half interval is the "
                "latency it adds - and it pays only when the display can show about twice the rendered rate "
                "(or the GPU's frame is long); the simulation is unchanged. An in-between frame goes out only "
                "while a frame's work spans about two refreshes (%.0f ms here) - with less, the display has "
                "no refresh to show it in and a v-synced present would only slow the real frames\n",
                fgRefresh_, presQ_ != VK_NULL_HANDLE ? "" : " (no second queue here: right after it)",
                fgRefresh_ > 0 ? 1900.0 / fgRefresh_ : 0.0);
    omk::event("framegen", {{"on", 1}, {"refresh", fgRefresh_}});
    return true;
}

void VulkanRenderer::killFg() {
    for (VkPipeline* p : {&fgCapPipe_, &fgSplatPipe_, &fgResolvePipe_})
        if (*p) { vkDestroyPipeline(dev_, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (fgPlo_) { vkDestroyPipelineLayout(dev_, fgPlo_, nullptr); fgPlo_ = VK_NULL_HANDLE; }
    if (fgPool_) { vkDestroyDescriptorPool(dev_, fgPool_, nullptr); fgPool_ = VK_NULL_HANDLE; }
    fgCapDs_ = fgIntDs_ = fgMidDs_ = VK_NULL_HANDLE;
    if (fgDsl_) { vkDestroyDescriptorSetLayout(dev_, fgDsl_, nullptr); fgDsl_ = VK_NULL_HANDLE; }
    for (VkFramebuffer& f : fgCapFbuf_) if (f) { vkDestroyFramebuffer(dev_, f, nullptr); f = VK_NULL_HANDLE; }
    for (VkFramebuffer* f : {&fgMidFbuf_, &fgSplatFbuf_})
        if (*f) { vkDestroyFramebuffer(dev_, *f, nullptr); *f = VK_NULL_HANDLE; }
    for (VkRenderPass* r : {&fgCapPass_, &fgSplatPass_, &fgResolvePass_})
        if (*r) { vkDestroyRenderPass(dev_, *r, nullptr); *r = VK_NULL_HANDLE; }
    for (int i = 0; i < kFgMaxSlots; ++i) {
        freeImage2d(fgColImg_[i], fgColMem_[i], fgColView_[i]);
        freeImage2d(fgAuxImg_[i], fgAuxMem_[i], fgAuxView_[i]);
    }
    for (int i = 0; i < 2; ++i) freeImage2d(fgHeadImg_[i], fgHeadMem_[i], fgHeadView_[i]);
    freeImage2d(fgMidImg_, fgMidMem_, fgMidView_);
    fgHeadsReady_ = false;
}

// the targets, the passes and the pipelines, made on the first capture
bool VulkanRenderer::makeFg() {
    if (fgMidView_ != VK_NULL_HANDLE) return true;
    if (!mvOn_ || motionView_ == VK_NULL_HANDLE || depthSampleView_ == VK_NULL_HANDLE) return false;
    if (!makePresentPass()) return false;   // its sampler, its read-back buffer, the present image's format
    const VkFormat pf = presFmt_;
    const VkImageUsageFlags colUse = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (int i = 0; i < fgSlots_; ++i)
        if (!makeImage2d(pf, w_, h_, colUse, fgColImg_[i], fgColMem_[i], fgColView_[i], "framegen") ||
            !makeImage2d(VK_FORMAT_R16G16B16A16_SFLOAT, w_, h_,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                         fgAuxImg_[i], fgAuxMem_[i], fgAuxView_[i], "framegen")) return false;
    for (int i = 0; i < 2; ++i)
        if (!makeImage2d(VK_FORMAT_R32_UINT, w_, h_, VK_IMAGE_USAGE_STORAGE_BIT,
                         fgHeadImg_[i], fgHeadMem_[i], fgHeadView_[i], "framegen")) return false;
    if (!makeImage2d(pf, w_, h_, colUse, fgMidImg_, fgMidMem_, fgMidView_, "framegen")) return false;
    // the passes: the capture (two targets), the splat (none: the heads are storage images), the resolve (one)
    const auto pass = [&](int n, const VkFormat* fmts, VkRenderPass& rp) -> bool {
        VkAttachmentDescription a[2]{};
        VkAttachmentReference ref[2]{};
        for (int i = 0; i < n; ++i) {
            a[i].format = fmts[i]; a[i].samples = VK_SAMPLE_COUNT_1_BIT;
            a[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;   // (every pixel is written)
            a[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ref[i] = {static_cast<uint32_t>(i), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = static_cast<uint32_t>(n); sub.pColorAttachments = n ? ref : nullptr;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = static_cast<uint32_t>(n); rpi.pAttachments = n ? a : nullptr;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &rp), "framegen render pass");
        return true;
    };
    const VkFormat capFmts[2] = {pf, VK_FORMAT_R16G16B16A16_SFLOAT};
    if (!pass(2, capFmts, fgCapPass_) || !pass(0, nullptr, fgSplatPass_) || !pass(1, &pf, fgResolvePass_)) return false;
    const auto fbuf = [&](VkRenderPass rp, const VkImageView* views, uint32_t n, VkFramebuffer& out) -> bool {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = n; fi.pAttachments = views;
        fi.width = static_cast<uint32_t>(w_); fi.height = static_cast<uint32_t>(h_); fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &out), "framegen framebuffer");
        return true;
    };
    for (int i = 0; i < fgSlots_; ++i) {
        const VkImageView v[2] = {fgColView_[i], fgAuxView_[i]};
        if (!fbuf(fgCapPass_, v, 2, fgCapFbuf_[i])) return false;
    }
    if (!fbuf(fgSplatPass_, nullptr, 0, fgSplatFbuf_) || !fbuf(fgResolvePass_, &fgMidView_, 1, fgMidFbuf_)) return false;
    // ONE set layout for the three: four pictures (0..3) and the two heads (4, 5); the shaders fetch, so no sampler
    {
        VkDescriptorSetLayoutBinding b[6]{};
        for (int k = 0; k < 6; ++k) {
            b[k].binding = static_cast<uint32_t>(k); b[k].descriptorCount = 1;
            b[k].descriptorType = k < 4 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 6; dli.pBindings = b;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &fgDsl_), "framegen set layout");
        // (two sets of the six-binding layout, whole: four sampled and two storage each)
        const VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 3; pi.poolSizeCount = 3; pi.pPoolSizes = ps;
        VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &fgPool_), "framegen pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = fgPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &fgDsl_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &fgCapDs_), "framegen set");
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &fgIntDs_), "framegen set");
        // the in-between picture as the PRESENT pass reads it (the present pass's own layout)
        ai.pSetLayouts = &presDsl_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &fgMidDs_), "framegen present set");
        const VkDescriptorImageInfo mi{presSampler_, fgMidView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = fgMidDs_; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &mi;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
        // the heads, once: always in GENERAL (the splat both stores and fetches them)
        const VkDescriptorImageInfo hi[2] = {{VK_NULL_HANDLE, fgHeadView_[0], VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, fgHeadView_[1], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet hw[2]{};
        for (int k = 0; k < 2; ++k) {
            hw[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            hw[k].dstSet = fgIntDs_; hw[k].dstBinding = 4u + static_cast<uint32_t>(k); hw[k].descriptorCount = 1;
            hw[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; hw[k].pImageInfo = &hi[k];
        }
        vkUpdateDescriptorSets(dev_, 2, hw, 0, nullptr);
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(omk::framegen::CapPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &fgDsl_;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &fgPlo_), "framegen pipeline layout");
    const auto pipe = [&](const uint32_t* frag, size_t bytes, VkRenderPass rp, uint32_t targets,
                          VkPipeline& out) -> bool {
        VkShaderModule qv = shaderModule(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = shaderModule(frag, bytes);
        if (!qv || !qf) { std::fprintf(stderr, "vulkan: framegen shader module\n"); return false; }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba[2]{};
        cba[0].colorWriteMask = cba[1].colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = targets; cb.pAttachments = targets ? cba : nullptr;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = fgPlo_; gp.renderPass = rp;
        const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
        if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: framegen pipeline\n"); return false; }
        return true;
    };
    if (!pipe(kFgCapFrag, sizeof(kFgCapFrag), fgCapPass_, 2, fgCapPipe_) ||
        !pipe(kFgSplatFrag, sizeof(kFgSplatFrag), fgSplatPass_, 0, fgSplatPipe_) ||
        !pipe(kFgResolveFrag, sizeof(kFgResolveFrag), fgResolvePass_, 1, fgResolvePipe_)) return false;
    // the heads into GENERAL, where they stay
    VkCommandBuffer cb = oneShotBegin();
    for (int i = 0; i < 2; ++i) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = fgHeadImg_[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
    }
    oneShotEnd(cb);
    fgHeadsReady_ = true;
    fgSeq_ = fgShownSeq_ = 0;   // (re)made: nothing kept to pair with
    return true;
}

// On the frame's command buffer, after the post chain: the world picture, its motion and depth, into the
// next slot.
void VulkanRenderer::fgCapture() {
    if (!makeFg()) {
        std::fprintf(stderr, "framegen: its targets could not be made - off\n");
        fgOn_ = false;
        return;
    }
    const int s = static_cast<int>(fgSeq_ % fgSlots_);
    // the display source: the post chain's picture if it ran (the supersample folded), else the scene's colour
    const bool post = postLive_ && postImg_ != VK_NULL_HANDLE;
    const VkImage srcImg = post ? postImg_ : colour_;
    const VkImageView srcView = post ? postView_ : colourView_;
    const VkDescriptorImageInfo ii[3] = {{VK_NULL_HANDLE, srcView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                         {VK_NULL_HANDLE, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
                                         {VK_NULL_HANDLE, motionView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[3]{};
    for (int k = 0; k < 3; ++k) {
        wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[k].dstSet = fgCapDs_; wr[k].dstBinding = static_cast<uint32_t>(k); wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev_, 3, wr, 0, nullptr);
    // what the frame wrote (the picture, the depth, the motion) before the capture reads it; the picture
    // from TRANSFER_SRC (where the display half expects it) to a sampled layout and back
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    const auto layout = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = srcImg; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb_, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = fgCapPass_; rp.framebuffer = fgCapFbuf_[s];
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
    vkCmdBeginRenderPass(cb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, fgCapPipe_);
    vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, fgPlo_, 0, 1, &fgCapDs_, 0, nullptr);
    const float pw = static_cast<float>(std::max(foc_.vw, 1)), ph = static_cast<float>(std::max(foc_.vh, 1));
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const omk::framegen::CapPush cp{{pw, ph, static_cast<float>(post ? 1 : outSs()), 0.0f},
                                    {static_cast<float>(aoVw_ > 0 ? aoVw_ : rw_) / pw,
                                     static_cast<float>(aoVh_ > 0 ? aoVh_ : rh_) / ph, 0.0f, 0.0f},
                                    {farP / (farP - nearP), -farP * nearP / (farP - nearP), 0.0f, 0.0f}};
    vkCmdPushConstants(cb_, fgPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof cp, &cp);
    vkCmdDraw(cb_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb_);
    layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    fgCut_[s] = resetOwed_;
    fgPicW_[s] = foc_.vw;
    fgPicH_[s] = foc_.vh;
    ++fgSeq_;
}

// the interval between presents, smoothed: what "half an interval" is
void VulkanRenderer::fgTick() {
    const double now = nowSec();
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

// At a world present: whether an in-between frame goes out first - and if so it is in `fgMidImg_`.
bool VulkanRenderer::fgMidFrame() {
    if (!fgOn_ || enhOff_) return false;
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
        // THE DISPLAY'S ROOM, the self-limiting rule (`o3de/framegen.h`)
        fgRoom_ = omk::framegen::roomFor(fgRoom_, fgWork_, fgRefresh_, fgAlways_);
        if (fgSeq_ < 2 || fgMidView_ == VK_NULL_HANDLE || !pairs(n, p)) {
            ++fgCuts_;   // a first frame, a cut on either side, a new picture size
        } else if (!fgRoom_) {
            ++fgNoRoom_;   // the display could not show it without slowing the real frames
        } else {
            // THE PROBE: frames N-2 and N with the vectors doubled, against the real N-1
            if (fgProbe_ && S == 3 && fgSeq_ >= 3) {
                const int m = static_cast<int>((fgSeq_ - 3) % S);
                if (pairs(p, m)) fgProbeRun(n, p, m);
            }
            fgInterpolate(n, p, 1.0f);
            ++fgMids_;
            mid = true;
        }
    }
    fgReport();
    return mid;
}

// A frame composed on the CPU (`presentSurface`): no world picture of its own to warp, so it goes out as it is.
void VulkanRenderer::fgCpuFrame() {
    if (!fgOn_) return;
    fgTick();
    ++fgReal_;
    ++fgCpu_;
    fgShownSeq_ = fgSeq_;
    fgPresentDone_ = nowSec() + 0.002;   // (its present follows, ~2 ms)
    static bool told = false;
    if (!told) {
        told = true;
        std::printf("framegen: a frame composed on the CPU (the 2D over the world at colour depth 16) goes out "
                    "as it is - colordepth = 32 lets the in-between frames run under the interface\n");
    }
    fgReport();
}

// The in-between picture of kept frames `n` (the later) and `p`, vectors x `k`: the heads emptied, both frames
// splatted, resolved - on a one-shot command buffer.
void VulkanRenderer::fgInterpolate(int n, int p, float k) {
    // the passes' table, in the shaders' order: the two pictures, then the two motion-and-depth images
    // (rewritten here: the frames before were waited for)
    const VkImageView in[4] = {fgColView_[n], fgColView_[p], fgAuxView_[n], fgAuxView_[p]};
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[4]{};
    for (int i = 0; i < 4; ++i) {
        ii[i] = {VK_NULL_HANDLE, in[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = fgIntDs_; wr[i].dstBinding = static_cast<uint32_t>(i); wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev_, 4, wr, 0, nullptr);
    VkCommandBuffer cb = oneShotBegin();
    const auto run = [&](VkRenderPass rp, VkFramebuffer fb, VkPipeline pl, const omk::framegen::Push& push) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fgPlo_, 0, 1, &fgIntDs_, 0, nullptr);
        vkCmdPushConstants(cb, fgPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
    };
    // the heads' stores and atomics of one draw before the next draw's fetches and atomics
    const auto fence = [&]() {
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             1, &mb, 0, nullptr, 0, nullptr);
    };
    omk::framegen::Push fp{{static_cast<float>(fgPicW_[n]), static_cast<float>(fgPicH_[n]), 0.0f, 0.0f},
                           {k, 1.0f, 0.0f, 0.0f}};
    run(fgSplatPass_, fgSplatFbuf_, fgSplatPipe_, fp);   // the heads emptied
    fence();
    fp.k[1] = 0.0f;
    run(fgSplatPass_, fgSplatFbuf_, fgSplatPipe_, fp);   // both frames splatted
    fence();
    run(fgResolvePass_, fgMidFbuf_, fgResolvePipe_, fp);  // resolved
    oneShotEnd(cb);
}

// a display-size RGBA8 image (in the sampled layout) read back (the probe's)
bool VulkanRenderer::fgRead(VkImage img, std::vector<unsigned char>& out) {
    if (hdrOn_ || !makePresentPass()) return false;
    // (the readback buffer is made on the first call, as `copyPresentOut`'s is)
    if (presRead_ == VK_NULL_HANDLE &&
        !makeBuffer(static_cast<VkDeviceSize>(w_) * h_ * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    presRead_, presReadMem_))
        return false;
    VkCommandBuffer cb = oneShotBegin();
    const auto layout = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA,
                            VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = srcA; b.dstAccessMask = dstA;
        vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presRead_, 1, &cp);
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    oneShotEnd(cb);
    const std::size_t bytes = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4;
    out.resize(bytes);
    void* p = nullptr;
    if (vkMapMemory(dev_, presReadMem_, 0, bytes, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(out.data(), p, bytes);
    vkUnmapMemory(dev_, presReadMem_);
    return true;
}

// `OMK_FRAMEGEN_PROBE=<prefix>`: THE CHECK's instrument (`o3de/framegen.h`)
void VulkanRenderer::fgProbeRun(int n, int p, int m) {
    fgInterpolate(n, m, 2.0f);
    static std::vector<unsigned char> warp, real, a, b;
    if (!fgRead(fgMidImg_, warp) || !fgRead(fgColImg_[p], real) || !fgRead(fgColImg_[n], b) ||
        !fgRead(fgColImg_[m], a)) return;
    omk::framegen::probeCompare(warp, real, a, b, fgPicW_[n], fgPicH_[n], w_, fgSeq_, fgProbe_);
}

// The real frame in `presImg_`: presented now - or, after an in-between one, half the interval later by the
// backend's thread (the blit itself is done here: `presentImage` with `defer`).
bool VulkanRenderer::fgPresentReal(bool mid) {
    if (!mid || !fgThread_.joinable()) {
        const bool ok = presentImage(presImg_, w_, h_);
        if (fgOn_) fgPresentDone_ = nowSec();
        return ok;
    }
    // half the interval after the CALL - when the frame was ready - not after the in-between frame's
    // present returned (a blocked present would push the real frame onto the next one's heels)
    const double half = std::clamp(0.5 * fgInterval_, 0.0, 0.05);
    fgDeferDue_ = std::max(fgCallAt_ + half, nowSec() + 0.001);
    ++fgDeferred_;
    fgLatSum_ += half;
    const bool ok = presentImage(presImg_, w_, h_, true);
    fgPresentDone_ = nowSec();
    return ok;
}

// The frame's thread waits for a deferred real frame to be out - early, if it is not yet due - before it
// touches the swapchain again.
void VulkanRenderer::fgSettle() {
    if (!fgThread_.joinable()) return;
    std::unique_lock<std::mutex> lk(fgMu_);
    if (!fgPending_) return;
    fgHurry_ = true;
    fgCv_.notify_all();
    fgCv_.wait(lk, [this] { return !fgPending_; });
}

void VulkanRenderer::fgThreadMain() {
    std::unique_lock<std::mutex> lk(fgMu_);
    for (;;) {
        fgCv_.wait(lk, [this] { return fgPending_ || fgQuit_; });
        if (!fgPending_) return;   // asked to stop, nothing owed
        // to the due time: a timed wait to ~1.5 ms short of it, then yields (the pacer's way)
        while (!fgHurry_) {
            const double left = fgDue_ - nowSec();
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
        const bool quit = fgQuit_;
        const uint32_t idx = fgPendIdx_;
        lk.unlock();
        if (!quit) {
            // the blit was done on the frame's thread; this is the queue present alone, on the second queue
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &drawn_;
            pi.swapchainCount = 1; pi.pSwapchains = &swap_;
            pi.pImageIndices = &idx;
            vkQueuePresentKHR(presQ_, &pi);
        }
        lk.lock();
        // how late it reached the screen's queue against when it was due (hurried ones, sent early by the
        // next frame, are not counted)
        if (!fgHurry_) {
            fgLateSum_ += nowSec() - fgDue_;
            ++fgOnTime_;
        }
        fgPending_ = false;
        fgHurry_ = false;
        fgCv_.notify_all();
    }
}

// THE COUNTERS, after every real present: an event (what `verify.py: engine: framegen` reads) and, every
// 300, a line.
void VulkanRenderer::fgReport() {
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
                "rendered, %ld composed on the CPU, %ld with no refresh to spare - %.1f ms of work a frame); a frame "
                "every %.1f ms, the real one shown %.1f ms after its in-between one (the latency added; the present "
                "done %.1f ms after it was due), %ld sent early by the next\n", fgReal_, fgMids_, fgCuts_, fgStale_,
                fgCpu_, fgNoRoom_, 1000.0 * fgWork_, 1000.0 * fgInterval_, lat, late, hurried);
}

// ---- VOLUMETRIC LAMP LIGHT, `todo/enhancements.md` 24 -----------------------
//
// Two passes after the frame's own (and SSAO), the Direct3D 12 backend's: `vol.frag`
// marches each eye ray through the glow spheres of the frame's lamps in the air at HALF the
// render size into an R16G16B16A16 target (the light, and the surface's depth for the filter),
// and `volmix.frag` ADDS it back over `colour_` through a 4x4 joint-bilateral gather (the blend:
// one + one). One GLSL source for both backends; the push blocks and the lamps' block are
// `o3de/postfx.h`'s. Built on first use at the target's size; `killVol` on a resize.
bool VulkanRenderer::makeVol() {
    if (volMixPipe_ != VK_NULL_HANDLE) return true;
    if (!volOn_ || depthSampleView_ == VK_NULL_HANDLE || volUbo_ == VK_NULL_HANDLE) return false;
    volW_ = (rw_ + 1) / 2;
    volH_ = (rh_ + 1) / 2;
    if (!makeImage2d(VK_FORMAT_R16G16B16A16_SFLOAT, volW_, volH_,
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     volImg_, volMem_, volView_, "volumetrics image")) return false;
    const auto pass = [&](VkFormat fmt, VkAttachmentLoadOp load, VkImageLayout from, VkImageLayout to,
                          VkImageView view, int fw, int fh, VkRenderPass& rp, VkFramebuffer& fb) -> bool {
        VkAttachmentDescription a{};
        a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = load; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = from; a.finalLayout = to;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
        VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &rp), "volumetrics render pass");
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp; fi.attachmentCount = 1; fi.pAttachments = &view;
        fi.width = static_cast<uint32_t>(fw); fi.height = static_cast<uint32_t>(fh); fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &fb), "volumetrics framebuffer");
        return true;
    };
    if (!pass(VK_FORMAT_R16G16B16A16_SFLOAT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, volView_, volW_, volH_, volPass_, volFbuf_)) return false;
    // the frame itself, loaded and handed back where the readback and the present pass expect it
    if (!pass(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, colourView_, rw_, rh_, volMixPass_, volMixFbuf_)) return false;
    {
        // the depth (0), the half-size light (1), the lamps (2): one set for both passes
        VkDescriptorSetLayoutBinding b[3]{};
        for (int k = 0; k < 3; ++k) {
            b[k].binding = static_cast<uint32_t>(k); b[k].descriptorCount = 1;
            b[k].descriptorType = k == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                         : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dli.bindingCount = 3; dli.pBindings = b;
        VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &volDsl_), "volumetrics set layout");
        const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VKCHECK(vkCreateDescriptorPool(dev_, &pi, nullptr, &volPool_), "volumetrics pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = volPool_; ai.descriptorSetCount = 1; ai.pSetLayouts = &volDsl_;
        VKCHECK(vkAllocateDescriptorSets(dev_, &ai, &volDs_), "volumetrics set");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;   // (the shaders only texel-fetch)
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKCHECK(vkCreateSampler(dev_, &si, nullptr, &volSampler_), "volumetrics sampler");
        const VkDescriptorImageInfo ii[2] = {
            {volSampler_, depthSampleView_, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
            {volSampler_, volView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        const VkDescriptorBufferInfo bi{volUbo_, 0, sizeof(omk::postfx::VolUbo)};
        VkWriteDescriptorSet wr[3]{};
        for (int k = 0; k < 3; ++k) {
            wr[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            wr[k].dstSet = volDs_; wr[k].dstBinding = static_cast<uint32_t>(k);
            wr[k].descriptorCount = 1;
            wr[k].descriptorType = b[k].descriptorType;
            if (k < 2) wr[k].pImageInfo = &ii[k]; else wr[k].pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(dev_, 3, wr, 0, nullptr);
    }
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(omk::postfx::VolPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &volDsl_;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &volPlo_), "volumetrics pipeline layout");
    const auto module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes; si.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &si, nullptr, &m);
        return m;
    };
    const auto pipe = [&](const uint32_t* frag, size_t bytes, VkRenderPass rp, bool mix,
                          VkPipeline& out) -> bool {
        VkShaderModule qv = module(kFsqVert, sizeof(kFsqVert));
        VkShaderModule qf = module(frag, bytes);
        if (!qv || !qf) { std::fprintf(stderr, "vulkan: volumetrics shader module\n"); return false; }
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, qv, "main", nullptr};
        st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, qf, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.scissorCount = 1;   // dynamic: the march covers the picture's rectangle
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = 0xF;
        if (mix) {
            // ADDED to the scene's colour, its alpha left alone
            cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDynamicState = &dsi;
        gp.layout = volPlo_; gp.renderPass = rp;
        const VkResult pr = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &out);
        vkDestroyShaderModule(dev_, qv, nullptr);
        vkDestroyShaderModule(dev_, qf, nullptr);
        if (pr != VK_SUCCESS) { std::fprintf(stderr, "vulkan: volumetrics pipeline\n"); return false; }
        return true;
    };
    if (samples_ != VK_SAMPLE_COUNT_1_BIT)
        return pipe(kVolMsFrag, sizeof(kVolMsFrag), volPass_, false, volPipe_) &&
               pipe(kVolMixMsFrag, sizeof(kVolMixMsFrag), volMixPass_, true, volMixPipe_);
    return pipe(kVolFrag, sizeof(kVolFrag), volPass_, false, volPipe_) &&
           pipe(kVolMixFrag, sizeof(kVolMixFrag), volMixPass_, true, volMixPipe_);
}

void VulkanRenderer::killVol() {
    if (volMixPipe_) { vkDestroyPipeline(dev_, volMixPipe_, nullptr); volMixPipe_ = VK_NULL_HANDLE; }
    if (volPipe_) { vkDestroyPipeline(dev_, volPipe_, nullptr); volPipe_ = VK_NULL_HANDLE; }
    if (volPlo_) { vkDestroyPipelineLayout(dev_, volPlo_, nullptr); volPlo_ = VK_NULL_HANDLE; }
    if (volSampler_) { vkDestroySampler(dev_, volSampler_, nullptr); volSampler_ = VK_NULL_HANDLE; }
    if (volPool_) { vkDestroyDescriptorPool(dev_, volPool_, nullptr); volPool_ = VK_NULL_HANDLE; volDs_ = VK_NULL_HANDLE; }
    if (volDsl_) { vkDestroyDescriptorSetLayout(dev_, volDsl_, nullptr); volDsl_ = VK_NULL_HANDLE; }
    if (volMixFbuf_) { vkDestroyFramebuffer(dev_, volMixFbuf_, nullptr); volMixFbuf_ = VK_NULL_HANDLE; }
    if (volFbuf_) { vkDestroyFramebuffer(dev_, volFbuf_, nullptr); volFbuf_ = VK_NULL_HANDLE; }
    if (volMixPass_) { vkDestroyRenderPass(dev_, volMixPass_, nullptr); volMixPass_ = VK_NULL_HANDLE; }
    if (volPass_) { vkDestroyRenderPass(dev_, volPass_, nullptr); volPass_ = VK_NULL_HANDLE; }
    if (volView_) { vkDestroyImageView(dev_, volView_, nullptr); volView_ = VK_NULL_HANDLE; }
    if (volImg_) { vkDestroyImage(dev_, volImg_, nullptr); volImg_ = VK_NULL_HANDLE; }
    if (volMem_) { freeDeviceMemory(volMem_); volMem_ = VK_NULL_HANDLE; }
}

void VulkanRenderer::runVol() {
    ++volFrame_;
    if (enhOff_ || volCount_ <= 0 || aoVw_ <= 0 || aoVh_ <= 0) return;
    if (!makeVol()) {
        static bool told = false;
        if (!told) std::fprintf(stderr, "volumetrics: the passes could not be made - the frame without them\n");
        told = true;
        return;
    }
    // the frame's colour and depth writes (and SSAO's multiply), before the passes read them
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 1, &mb, 0, nullptr, 0, nullptr);
    const int hw = (aoVw_ + 1) / 2, hh = (aoVh_ + 1) / 2;
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const float A = farP / (farP - nearP), B = -farP * nearP / (farP - nearP);
    const omk::postfx::VolPush vp{{volTanH_, volTanV_, A, B},
        {static_cast<float>(aoVw_), static_cast<float>(aoVh_), static_cast<float>(volFrame_ % 64), nearP},
        {volEye_[0], volEye_[1], volEye_[2], volTune_.density},
        {volS_[0], volS_[1], volS_[2], fog_ ? fogStart_ : 0.0f},
        {volU_[0], volU_[1], volU_[2], fog_ ? fogEnd_ : 0.0f},
        {volF_[0], volF_[1], volF_[2], 40000.0f}};
    const auto run = [&](VkRenderPass rp, VkFramebuffer fb, VkPipeline pl, int fw, int fh, int vw, int vh,
                         const void* push, uint32_t bytes) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {static_cast<uint32_t>(fw), static_cast<uint32_t>(fh)}};
        vkCmdBeginRenderPass(cb_, &bi, VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport v{0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh), 0.0f, 1.0f};
        const VkRect2D sc{{0, 0}, {static_cast<uint32_t>(vw), static_cast<uint32_t>(vh)}};
        vkCmdSetViewport(cb_, 0, 1, &v);
        vkCmdSetScissor(cb_, 0, 1, &sc);
        vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, volPlo_, 0, 1, &volDs_, 0, nullptr);
        vkCmdPushConstants(cb_, volPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, push);
        vkCmdDraw(cb_, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb_);
    };
    run(volPass_, volFbuf_, volPipe_, volW_, volH_, hw, hh, &vp, sizeof vp);
    VkMemoryBarrier mb2{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb2, 0, nullptr, 0, nullptr);
    const omk::postfx::VolMixPush mp{{volTanH_, volTanV_, A, B},
        {static_cast<float>(aoVw_), static_cast<float>(aoVh_), static_cast<float>(hw), static_cast<float>(hh)}};
    run(volMixPass_, volMixFbuf_, volMixPipe_, rw_, rh_, aoVw_, aoVh_, &mp, sizeof mp);
    if (!volTold_) {
        volTold_ = true;
        std::printf("volumetrics: %d lamps in the air on this frame, marched at %dx%d\n", volCount_, hw, hh);
        omk::event("vol", {{"lamps", volCount_}, {"w", hw}, {"h", hh}});
    }
}

// the frame's stamps, read after its fence: each pass's time is from the stamp
// before it to its own (the first, "start", none)
void VulkanRenderer::readStamps() {
    static const char* const kNames[kStamps] = {"shadow", "reflection", "scene", "post"};
    if (!tsPool_ || tsCount_ < 2) return;
    uint64_t t[kStamps] = {};
    if (vkGetQueryPoolResults(dev_, tsPool_, 0, tsCount_, sizeof t, t, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
    for (uint32_t i = 1; i < tsCount_; ++i)
        for (uint32_t n = 0; n < kStamps && kNames[n]; ++n)
            if (std::strcmp(tsLabel_[i], kNames[n]) == 0)
                tsSum_[n] += static_cast<double>(t[i] - t[i - 1]) * props_.limits.timestampPeriod / 1e6;
    if (++tsFrames_ < 60) return;
    std::printf("vulkan: GPU ms a frame (mean of 60) - shadow %.2f, reflection %.2f, scene %.2f, post %.2f\n",
                tsSum_[0] / 60.0, tsSum_[1] / 60.0, tsSum_[2] / 60.0, tsSum_[3] / 60.0);
    omk::event("gpu.time", {{"shadow", tsSum_[0] / 60.0}, {"reflection", tsSum_[1] / 60.0},
                            {"scene", tsSum_[2] / 60.0}, {"post", tsSum_[3] / 60.0}});
    for (auto& v : tsSum_) v = 0.0;
    tsFrames_ = 0;
}

void VulkanRenderer::end() {
    postLive_ = false;
    cpuPoseSlot_.clear();
    cpuPoseCount_.clear();
    // `OMK_TIE_STATS=1`: every 30 frames, how each large geometry's revisions
    // were answered - replayed from the dirty corners, walked, or a replay
    // abandoned part way (`engine: dirty corners`).
    static const bool tieStats = omk::envFlag("OMK_TIE_STATS");
    static long frameNo = 0;
    if (tieStats && ++frameNo % 30 == 0) {
        for (const auto& [g, t] : tie_)
            if (g->corners.size() > 30000)
                std::printf("tie stats: frame %ld, %zu triangles - replayed %ld walked %ld fallbacks %ld\n",
                            frameNo, g->corners.size() / 3, t.replays, t.walks, t.fallbacks);
        // ...and where the full walks went: every geometry, largest cost first
        std::vector<std::pair<long, std::size_t>> cost;
        long faces = 0;
        for (auto& [g, t] : tie_) {
            if (t.facesWalked > 0) cost.push_back({t.facesWalked, g->corners.size() / 3});
            faces += t.facesWalked;
            t.facesWalked = 0;
        }
        std::sort(cost.begin(), cost.end(), std::greater<>());
        std::printf("tie walk: frames %ld-%ld, %zu geometries walked %ld faces;", frameNo - 29, frameNo,
                    cost.size(), faces);
        for (std::size_t k = 0; k < cost.size() && k < 8; ++k)
            std::printf(" %ld over %zu tris%s", cost[k].first, cost[k].second, k + 1 < cost.size() && k < 7 ? "," : "");
        std::printf("\n");
    }
    // The depth-tie pass's report, once per geometry with losers.
    for (auto& [g, t] : tie_)
        if (t.touched && !t.logged && t.dropped > 0 && g->corners.size() > 3000) {
            t.logged = true;
            std::printf("vulkan: depth tie - %ld of %ld triangles coincide with an earlier face "
                        "and are degenerated so the first drawn wins, as the engine's strict test "
                        "decides\n", t.dropped, static_cast<long>(g->corners.size() / 3));
        }
    if (!recording_) return;
    vkCmdEndRenderPass(cb_);
    stamp("scene");
    if (postNow_) {
        recordPost(cb_);
        stamp("post");
    }
    if (softOn_ && softDraws_ > 0 && !softTold_) {
        softTold_ = true;
        std::printf("vulkan: soft particles - %ld sprite batches faded against the depth on this frame\n",
                    softDraws_);
        omk::event("softparticles.drawn", {{"batches", static_cast<int>(softDraws_)}});
    }
    if (ssaoOn_) runSsao();
    // VOLUMETRIC LAMP LIGHT (todo/enhancements.md 24): added over the scene, so the post chain takes it
    // with the rest
    if (volOn_) runVol();
    // THE TEMPORAL UPSCALER: the render-size frame rebuilt at the display size, in `colour_`
    if (tupOn_) upscale();
    runPost();   // bloom, depth of field, grade - when asked and due
    // FRAME GENERATION: the finished world picture, with its motion and depth, kept for the present
    if (fgOn_) fgCapture();
    // The colour attachment ends in TRANSFER_SRC_OPTIMAL (the render pass says
    // so). The READBACK COPY is not recorded here: a frame the GPU presents
    // (`presentWorld`) never reads it, so copying every frame is waste
    // (todo/cpu-vs-original.md tier A). `readback()` records it when a frame
    // is read - the image is in TRANSFER_SRC_OPTIMAL then too, the present
    // pass handing it back so.
    copyOwed_ = true;
    srcW_ = tupOn_ ? w_ : rw_; srcH_ = tupOn_ ? h_ : rh_;   // the whole render target is live
    vkEndCommandBuffer(cb_);
    // HYBRID RAY TRACING: the frame's acceleration structures, on their own command buffer
    // submitted just before the frame's (which reads them; the builds end on a barrier)
    if (rtOn_) rtBuild();

    const VkCommandBuffer cbs[2] = {rtCb_, cb_};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = rtRecorded_ ? 2 : 1; si.pCommandBuffers = rtRecorded_ ? cbs : &cb_;
    vkResetFences(dev_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
    readStamps();
    if (rtRecorded_) {
        rtRecorded_ = false;
        rtAfter();
    }
    recording_ = false;
    cbStarted_ = false;
    shadowLive_ = false;
    dirty_ = true;
    static const bool motionStatsOn = omk::envFlag("OMK_MOTION_STATS");
    if (tupOn_ && motionStatsOn) motionStats();
    if (mvOn_) {
        // this frame's camera is the next one's previous
        std::memcpy(prevMain_, curMain_, sizeof prevMain_);
        for (int k = 0; k < 3; ++k) { prevEye_[k] = frameEye_[k]; prevFwd_[k] = mainF_[k]; }
        havePrev_ = true;
        resetOwed_ = false;
    }
}

const omk::Surface& VulkanRenderer::readback() {
    // IDEMPOTENT, and that is a contract not an optimisation. The software
    // renderer returns its stored framebuffer, so a caller may read it twice
    // and may WRITE to it between the two - which is exactly what the mirror
    // pass does when it composites the reflection in. Re-reading the GPU
    // buffer on every call silently threw that composite away and the frame
    // came back nearly black on Vulkan while software was correct. So a frame
    // is converted once, when `end()` says there is a new one.
    if (fb_.w != w_ || fb_.h != h_) fb_ = omk::Surface(w_, h_, 0);
    if (!dirty_) return fb_;
    dirty_ = false;
    // UPSCALED, the picture is read at the OUTPUT size, through the same EASU
    // and RCAS as the present pass's (`raw`: undithered), and quantised below
    // as a frame drawn at that size would be
    const bool up = up_ > 1.0f;
    const int cw = up || tupOn_ ? w_ : rw_, ch = up || tupOn_ ? h_ : rh_;
    if (readBuf_ == VK_NULL_HANDLE &&
        !makeBuffer(static_cast<VkDeviceSize>(cw) * ch * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    readBuf_, readMem_))
        return fb_;
    if (copyOwed_) {                 // the copy `end()` does not record
        copyOwed_ = false;
        if (up && !runPresentPass(0, picH_ > 0 ? picH_ : h_, false, true)) return fb_;
        VkCommandBuffer cb = oneShotBegin();
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // the scene's colour; THE POST CHAIN's display-size picture when it ran (its supersample folded
        // already); or, UPSCALED (the remaster's EASU/RCAS - the chain then stands down), the present image
        const bool postRead = postLive_ && !hdrOn_ && !up;   // (HDR's is FP16, and HDR's frames go out on the GPU)
        cp.imageExtent = {static_cast<uint32_t>(postRead ? w_ : cw), static_cast<uint32_t>(postRead ? h_ : ch), 1};
        vkCmdCopyImageToBuffer(cb, postRead ? postImg_ : up ? presImg_ : colour_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readBuf_, 1, &cp);
        oneShotEnd(cb);              // submitted and waited: the bytes are there
    }
    void* p = nullptr;
    vkMapMemory(dev_, readMem_, 0, VK_WHOLE_SIZE, 0, &p);
    const auto* src = static_cast<const unsigned char*>(p);
    // 888 -> 565 HERE, in software, and not by asking for a 565 attachment:
    // `PORTING` A3 keeps the one quantisation rule in one place rather than
    // delegating it to a driver whose rounding is its own.
    //
    // ...and the SUPERSAMPLE RESOLVE happens on the 8-bit side, BEFORE that
    // one quantisation: averaging four 565 values would quantise four times
    // and then average the error, which throws away most of the point.
    // `present.frag` does the same resolve on the GPU for a frame nothing is
    // drawn over, so this loop runs only on the CPU path (and for
    // `OMK_VERIFY_GPU_PRESENT`, which compares the two).
    // ROWS IN PARALLEL (Windows port, 2026-10-16): every output row reads its
    // own source rows and writes only itself, so the chunks are disjoint and
    // the frame is bit-identical to the inline loop (`platform/threads.h`).
    // At 2x supersampling this loop walks a 3840x2160 buffer for a 1080p
    // frame - 15 ms on one core, paid by every frame with a subtitle, a fade
    // or the HUD over the world.
    const bool postRead = postLive_ && !hdrOn_;
    const int ss = postRead ? 1 : outSs();   // the chain's picture is folded already
    const int srcW = postRead || tupOn_ ? w_ : rw_;
    omk::Threads::shared().parallelFor(0, static_cast<std::size_t>(h_), 16,
                                       [&](std::size_t rowFrom, std::size_t rowTo) {
    const int y0 = static_cast<int>(rowFrom), y1 = static_cast<int>(rowTo);
    if (ss <= 1 && dither_) {
        // Row by row through the channel tables - the same bits as
        // `quantise888Dither` a pixel, without a division and a modulo for
        // every one of them (todo/optimization.md step 4).
        for (int y = y0; y < y1; ++y)
            omk::quantise888DitherRow(src + 4 * static_cast<std::size_t>(y) * static_cast<std::size_t>(w_),
                                      w_, y, fb_.px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_));
    } else if (ss <= 1) {
        for (int i = y0 * w_; i < y1 * w_; ++i)
            fb_.px[static_cast<std::size_t>(i)] =
                omk::rgb565(src[4 * i], src[4 * i + 1], src[4 * i + 2]);
    } else {
        const int n = ss * ss;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w_; ++x) {
                int acc[3] = {0, 0, 0};
                for (int sy = 0; sy < ss; ++sy)
                    for (int sx = 0; sx < ss; ++sx) {
                        const std::size_t o =
                            (static_cast<std::size_t>(y * ss + sy) *
                                 static_cast<std::size_t>(srcW) +
                             static_cast<std::size_t>(x * ss + sx)) * 4;
                        acc[0] += src[o]; acc[1] += src[o + 1]; acc[2] += src[o + 2];
                    }
                const int ar = (acc[0] + n / 2) / n, ag = (acc[1] + n / 2) / n,
                          ab = (acc[2] + n / 2) / n;
                fb_.px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + x] =
                    dither_ ? omk::quantise888Dither(ar, ag, ab, x, y)
                            : omk::rgb565(static_cast<unsigned char>(ar),
                                          static_cast<unsigned char>(ag),
                                          static_cast<unsigned char>(ab));
            }
    }
    });
    vkUnmapMemory(dev_, readMem_);
    return fb_;
}

void VulkanRenderer::geometryGone(void* self, const omk::Geometry* g) {
    auto* r = static_cast<VulkanRenderer*>(self);
    if (!r->releaseOn_) return;
    bool had = false;
    if (auto it = r->vbo_.find(g); it != r->vbo_.end()) {
        r->deadBufs_.push_back(it->second);
        r->vbo_.erase(it);
        had = true;
    }
    r->vboN_.erase(g);
    r->vboCap_.erase(g);
    r->vboRev_.erase(g);
    had |= r->tie_.erase(g) > 0;
    had |= r->baked_.erase(g) > 0;
    // a SET going (an area left) is a cut for frame generation's history
    if (had && g->corners.size() > 30000) r->resetOwed_ = true;
    if (had) ++r->released_;
}

VulkanRenderer::~VulkanRenderer() {
    omk::removeGeometryListener(&VulkanRenderer::geometryGone, this);
    if (!dev_) { omk::vkup::dlssShutdown(); return; }
    if (fgThread_.joinable()) {   // the pacing thread first: it presents on the swapchain
        {
            std::lock_guard<std::mutex> lk(fgMu_);
            fgQuit_ = true;
            fgHurry_ = true;
        }
        fgCv_.notify_all();
        fgThread_.join();
    }
    vkDeviceWaitIdle(dev_);
    tup_.reset();   // the SDK's context before the device it was made on
    omk::vkup::dlssShutdown();
    killFg();
    rtKill();
    for (auto& p : pipeCount_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    killPost();
    killRmPost();
    killLut();
    killReflection();
    for (auto& bm : deadBufs_) {
        vkDestroyBuffer(dev_, bm.first, nullptr);
        freeDeviceMemory(bm.second);
    }
    deadBufs_.clear();
    for (auto& [g, bm] : vbo_) {
        vkDestroyBuffer(dev_, bm.first, nullptr);
        freeDeviceMemory(bm.second);
    }
    auto killTex = [&](Tex& t) {
        if (t.view) vkDestroyImageView(dev_, t.view, nullptr);
        if (t.img)  vkDestroyImage(dev_, t.img, nullptr);
        if (t.mem)  freeDeviceMemory(t.mem);
        if (t.emView) vkDestroyImageView(dev_, t.emView, nullptr);
        if (t.emImg)  vkDestroyImage(dev_, t.emImg, nullptr);
        if (t.emMem)  freeDeviceMemory(t.emMem);
        if (t.nmView) vkDestroyImageView(dev_, t.nmView, nullptr);
        if (t.nmImg)  vkDestroyImage(dev_, t.nmImg, nullptr);
        if (t.nmMem)  freeDeviceMemory(t.nmMem);
    };
    for (auto& [key, u] : uploaded_) killTex(u.t);   // the slots' handles are theirs
    uploaded_.clear();
    tex_.clear();
    killTex(white_);
    killTex(lightBlack_);
    killTex(normalFlat_);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    for (VkSampler old : retiredSamplers_) vkDestroySampler(dev_, old, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    if (tsPool_) vkDestroyQueryPool(dev_, tsPool_, nullptr);
    for (auto& p : pipe_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& p : pipeTiled_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    if (pipeCut_) vkDestroyPipeline(dev_, pipeCut_, nullptr);
    if (pipeTiledCut_) vkDestroyPipeline(dev_, pipeTiledCut_, nullptr);
    for (auto& p : pipe1_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& p : pipeTiled1_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    if (reflPass1_) vkDestroyRenderPass(dev_, reflPass1_, nullptr);
    if (plo_) vkDestroyPipelineLayout(dev_, plo_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
    if (dsl2_) vkDestroyDescriptorSetLayout(dev_, dsl2_, nullptr);
    if (poseBuf_) vkDestroyBuffer(dev_, poseBuf_, nullptr);
    if (poseMem_) freeDeviceMemory(poseMem_);
    if (pointsBuf_) vkDestroyBuffer(dev_, pointsBuf_, nullptr);
    if (pointsMem_) freeDeviceMemory(pointsMem_);
    flushUploads();   // (nothing pending outside a pool; its own buffers, if any)
    if (ring_) vkDestroyBuffer(dev_, ring_, nullptr);
    if (ringMem_) freeDeviceMemory(ringMem_);
    if (tilesBuf_) vkDestroyBuffer(dev_, tilesBuf_, nullptr);
    if (tilesMem_) freeDeviceMemory(tilesMem_);
    if (readBuf_) vkDestroyBuffer(dev_, readBuf_, nullptr);
    if (readMem_) freeDeviceMemory(readMem_);
    if (fbuf_) vkDestroyFramebuffer(dev_, fbuf_, nullptr);
    if (pass_) vkDestroyRenderPass(dev_, pass_, nullptr);
    for (auto& cp : passCont_) if (cp) vkDestroyRenderPass(dev_, cp, nullptr);
    for (auto& pp : pipeSoft_) if (pp) vkDestroyPipeline(dev_, pp, nullptr);
    if (softSampler_) vkDestroySampler(dev_, softSampler_, nullptr);
    if (softPool_) vkDestroyDescriptorPool(dev_, softPool_, nullptr);
    if (dsl3_) vkDestroyDescriptorSetLayout(dev_, dsl3_, nullptr);
    if (reflPass_) vkDestroyRenderPass(dev_, reflPass_, nullptr);
    if (colourView_) vkDestroyImageView(dev_, colourView_, nullptr);
    if (colour_) vkDestroyImage(dev_, colour_, nullptr);
    if (colourMem_) freeDeviceMemory(colourMem_);
    if (msColourView_) vkDestroyImageView(dev_, msColourView_, nullptr);
    if (msColour_) vkDestroyImage(dev_, msColour_, nullptr);
    if (msColourMem_) freeDeviceMemory(msColourMem_);
    freeImage2d(motion_, motionMem_, motionView_);
    killSsao();
    killVol();
    if (volUbo_) vkDestroyBuffer(dev_, volUbo_, nullptr);
    if (volUboMem_) freeDeviceMemory(volUboMem_);
    if (depthSampleView_) vkDestroyImageView(dev_, depthSampleView_, nullptr);
    if (depthRead_) vkDestroyImageView(dev_, depthRead_, nullptr);
    if (depthView_) vkDestroyImageView(dev_, depthView_, nullptr);
    if (depth_) vkDestroyImage(dev_, depth_, nullptr);
    if (depthMem_) freeDeviceMemory(depthMem_);
    if (tupOutView_) vkDestroyImageView(dev_, tupOutView_, nullptr);
    if (tupOut_) vkDestroyImage(dev_, tupOut_, nullptr);
    if (tupOutMem_) freeDeviceMemory(tupOutMem_);
    killTupDepth();
    killUpscale();
    if (presPipe_) vkDestroyPipeline(dev_, presPipe_, nullptr);
    if (presPlo_) vkDestroyPipelineLayout(dev_, presPlo_, nullptr);
    if (presSampler_) vkDestroySampler(dev_, presSampler_, nullptr);
    if (presPool_) vkDestroyDescriptorPool(dev_, presPool_, nullptr);
    if (presDsl_) vkDestroyDescriptorSetLayout(dev_, presDsl_, nullptr);
    if (presFbuf_) vkDestroyFramebuffer(dev_, presFbuf_, nullptr);
    if (presPass_) vkDestroyRenderPass(dev_, presPass_, nullptr);
    if (presView_) vkDestroyImageView(dev_, presView_, nullptr);
    if (presImg_) vkDestroyImage(dev_, presImg_, nullptr);
    if (presMem_) freeDeviceMemory(presMem_);
    if (presRead_) vkDestroyBuffer(dev_, presRead_, nullptr);
    if (presReadMem_) freeDeviceMemory(presReadMem_);
    killOverlay();
    killPost();
    killHdrBlit();
    if (stencilSampleView_) vkDestroyImageView(dev_, stencilSampleView_, nullptr);
    for (auto& pp : pipeSubj_) if (pp) vkDestroyPipeline(dev_, pp, nullptr);
    if (upBuf_) vkDestroyBuffer(dev_, upBuf_, nullptr);
    if (upMem_) freeDeviceMemory(upMem_);
    if (pipeStencil_) vkDestroyPipeline(dev_, pipeStencil_, nullptr);
    if (pipeDepthReset_) vkDestroyPipeline(dev_, pipeDepthReset_, nullptr);
    for (auto& pp : pipeRefl_) if (pp) vkDestroyPipeline(dev_, pp, nullptr);
    // the mapped shadow and set 1's layout and buffers - older than the
    // remaster, and never destroyed until the audit's cleanliness (2026-10-26);
    // set 1 itself goes with the pool
    if (shPipe_) vkDestroyPipeline(dev_, shPipe_, nullptr);
    if (shPlo_) vkDestroyPipelineLayout(dev_, shPlo_, nullptr);
    if (shFbuf_) vkDestroyFramebuffer(dev_, shFbuf_, nullptr);
    if (shPass_) vkDestroyRenderPass(dev_, shPass_, nullptr);
    if (shSampler_) vkDestroySampler(dev_, shSampler_, nullptr);
    if (shView_) vkDestroyImageView(dev_, shView_, nullptr);
    if (shImg_) vkDestroyImage(dev_, shImg_, nullptr);
    if (shMem_) freeDeviceMemory(shMem_);
    if (dsl1_) vkDestroyDescriptorSetLayout(dev_, dsl1_, nullptr);
    if (shUbo_) vkDestroyBuffer(dev_, shUbo_, nullptr);
    if (shUboMem_) freeDeviceMemory(shUboMem_);   // unmapped by the free
    if (litUbo_) vkDestroyBuffer(dev_, litUbo_, nullptr);
    if (litUboMem_) freeDeviceMemory(litUboMem_);
    if (pfence_) vkDestroyFence(dev_, pfence_, nullptr);
    if (acquired_) vkDestroySemaphore(dev_, acquired_, nullptr);
    if (drawn_) vkDestroySemaphore(dev_, drawn_, nullptr);
    if (swap_) vkDestroySwapchainKHR(dev_, swap_, nullptr);
    if (fence_) vkDestroyFence(dev_, fence_, nullptr);
    if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
    vkDestroyDevice(dev_, nullptr);
    if (surface_ && inst_) vkDestroySurfaceKHR(inst_, surface_, nullptr);
    if (inst_) vkDestroyInstance(inst_, nullptr);
}

// THE REMASTER'S HALF (`vkrender_remaster.inl`): the upscale, the post pass,
// the water reflection. Always compiled - this backend is built only with
// OMK_REMASTER (the Makefile drops it otherwise), so no stubs.
#if !OMK_REMASTER
#error "the Vulkan backend is built with OMK_REMASTER only"
#endif
#include "vkrender_remaster.inl"

}  // namespace

namespace omk {

// The factory. `src/` never names this - A8 rule 2 - so a caller that wants the
// live renderer links the backend and calls it, and a caller that does not gets
// a build with no Vulkan in it at all.
Renderer* makeVulkanRenderer() { return new VulkanRenderer(); }

// The presentation seam. `PORTING` A8 rule 2 keeps SDL in one file and Vulkan
// in this one, so these pass opaque handles and extension NAMES rather than
// letting either include the other's headers. The order is forced by Vulkan:
// the instance needs the window system's extensions, the surface needs the
// instance, and the device's queue choice needs the surface.
void vulkanNeedExtensions(Renderer* r, const char* const* names, unsigned n) {
    if (auto* v = dynamic_cast<VulkanRenderer*>(r)) v->needExtensions(names, n);
}
void* vulkanCreateInstance(Renderer* r) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v ? v->createInstanceOnly() : nullptr;
}
bool vulkanAttachSurface(Renderer* r, unsigned long long surf) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->attachSurface(surf);
}
bool vulkanPresent(Renderer* r) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->presentDirect();
}
bool vulkanPresentSurface(Renderer* r, const Surface& s) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->presentSurface(s);
}
// The present pass (todo/optimization.md step 4b).
bool vulkanPresentWorld(Renderer* r, int vy, int vh) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->presentWorld(vy, vh);
}
// options row 2 on the Vulkan backend (`todo/options-menu.md`)
bool vulkanResize(Renderer* r, int w, int h) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->resize(w, h);
}
bool vulkanPresentOverlay(Renderer* r, const Surface& s, const std::uint8_t* mask, const float fade[4],
                          int vy, int vh) {
    return static_cast<VulkanRenderer*>(r)->presentOverlay(s, mask, fade, vy, vh);
}

void vulkanSetRefresh(Renderer* r, int hz) {
    static_cast<VulkanRenderer*>(r)->setRefresh(hz);
}

void vulkanSetHdrDisplay(Renderer* r, int known, float peakNits) {
    static_cast<VulkanRenderer*>(r)->setHdrDisplay(known, peakNits);
}

bool vulkanWindowPicture(Renderer* r, std::vector<unsigned char>& rgba) {
    return static_cast<VulkanRenderer*>(r)->windowPicture(rgba);
}

// ...and at new enhancements (OMK's options menu, `retarget`)
bool vulkanRetarget(Renderer* r, int samples, int ss, float up, float ao) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->retarget(samples, ss, up, ao);
}
bool vulkanWorldPicture(Renderer* r, int vy, int vh, std::vector<unsigned char>& rgba) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v && v->worldPicture(vy, vh, rgba);
}
bool vulkanProbeUpload(Renderer* r, const unsigned char* rgba, bool dither) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    if (!v) return false;
    v->setDitherForProbe(dither);
    return v->probeUpload(rgba);
}
int vulkanSamples(Renderer* r) {
    return static_cast<VulkanRenderer*>(r)->samples();
}
int vulkanMaxSamples(Renderer* r) {
    return static_cast<VulkanRenderer*>(r)->maxSamples();
}
int vulkanTextureFilter(Renderer* r) {
    return static_cast<VulkanRenderer*>(r)->textureFilter();
}
int vulkanAnisotropy(Renderer* r) {
    return static_cast<VulkanRenderer*>(r)->anisotropy();
}
const char* vulkanDeviceName(Renderer* r) {
    auto* v = dynamic_cast<VulkanRenderer*>(r);
    return v ? v->device() : "";
}

}  // namespace omk
