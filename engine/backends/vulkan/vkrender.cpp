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
#include "platform/env.h"

#include <algorithm>
#include <array>
#include <set>
#include <cstdio>
#include <cstdlib>
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
const uint32_t kFsqVert[] =
#include "fsq.vert.inc"
;
const uint32_t kFsqFrag[] =
#include "fsq.frag.inc"
;
// The present pass: the adventure frame's dither and 565 expansion on the GPU
// (todo/optimization.md step 4b).
const uint32_t kPresentFrag[] =
#include "present.frag.inc"
;
struct PresentPush { int32_t vy, vh, w, dither, ss; };
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
struct SsaoPush { float tanH, tanV, A, B, vw, vh, radius, strength; };

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
    GpuLightStd140 l[8];
    int32_t count;
    float   base;      // View::litBase, in the std140 padding that was here
    int32_t grey;      // View::grey, the greyscale bank (ops 150/151) - padding too
    int32_t pad;
};

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
};
static_assert(sizeof(Push) == 104, "the push block is 104 bytes");
// Which `gl_FrontFacing` is the software rasterizer's FRONT (a negative area in
// `raster.cpp`): this pipeline's `frontFace` and Vulkan's Y-down viewport.
// Measured, not derived - the wrong sign culls every front face and the room
// turns inside out, which `engine: back-face cull` and the renderer checks see.
constexpr int32_t kCullSign = 1;

class VulkanRenderer : public omk::Renderer {
public:
    bool init(int w, int h) override;
    void setTextures(std::span<const omk::Texture> t) override;
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
    bool setAmbientOcclusion(bool on) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        wantSsao_ = on;
        return true;
    }
    bool setMultisample(int samples) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        wantSamples_ = samples < 2 ? 1 : samples < 4 ? 2 : samples < 8 ? 4 : 8;
        return true;
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
    bool setSupersample(int n) override {
        if (dev_ != VK_NULL_HANDLE) return false;   // too late: the target exists
        ss_ = n < 2 ? 1 : n < 4 ? 2 : 4;
        return true;
    }
    int supersample() const { return ss_; }
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
    VkCommandBuffer oneShotBegin();
    void oneShotEnd(VkCommandBuffer cb);
    bool uploadGeometry(const omk::Geometry* g);

    VkInstance        inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice  phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
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
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkImage        colour_ = VK_NULL_HANDLE;  VkDeviceMemory colourMem_ = VK_NULL_HANDLE;
    VkImageView    colourView_ = VK_NULL_HANDLE;
    VkImage        msColour_ = VK_NULL_HANDLE; VkDeviceMemory msColourMem_ = VK_NULL_HANDLE;
    VkImageView    msColourView_ = VK_NULL_HANDLE;
    VkImage        depth_ = VK_NULL_HANDLE;   VkDeviceMemory depthMem_ = VK_NULL_HANDLE;
    VkFormat       dsFmt_ = VK_FORMAT_UNDEFINED;
    VkImageView    depthView_ = VK_NULL_HANDLE;
    VkRenderPass   pass_ = VK_NULL_HANDLE;
    VkFramebuffer  fbuf_ = VK_NULL_HANDLE;
    VkBuffer       readBuf_ = VK_NULL_HANDLE; VkDeviceMemory readMem_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool      dpool_ = VK_NULL_HANDLE;
    VkPipelineLayout      plo_ = VK_NULL_HANDLE;
    VkPipeline            pipe_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
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
    VkBuffer        shUbo_ = VK_NULL_HANDLE;   VkDeviceMemory shUboMem_ = VK_NULL_HANDLE;
    void*           shUboPtr_ = nullptr;
    VkBuffer        litUbo_ = VK_NULL_HANDLE;  VkDeviceMemory litUboMem_ = VK_NULL_HANDLE;
    void*           litUboPtr_ = nullptr;
    // The command buffer is opened by whichever of `shadowPass` and `begin`
    // runs first, and only once - the depth pass has to be recorded BEFORE the
    // frame's own render pass begins.
    bool            cbStarted_ = false;
    bool            shadowLive_ = false;   // a depth pass was recorded this frame
    int             litCount_ = 0;         // lights uploaded for this frame
    float           shimmerClock_ = 0.0f;  // the View's, held for the uniform
    bool            dither_ = true;        // the engine's DITHERENABLE, see surface.h

    struct Tex {
        VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE; VkDescriptorSet ds = VK_NULL_HANDLE;
    };
    std::vector<Tex> tex_;                // per pool slot: handles `uploaded_` owns
    Tex              white_;              // a material with no texture
    // the textures on the device, by their pixel storage (`setTextures`); the
    // entry holds that storage so its address is not reused while cached
    struct Uploaded { Tex t; omk::PixelBuffer idx, pal; bool used = false; };
    std::map<std::tuple<const std::uint8_t*, const std::uint8_t*, int, int>, Uploaded> uploaded_;

    std::map<const omk::Geometry*, std::pair<VkBuffer, VkDeviceMemory>> vbo_;
    // THE POSE BUFFER (set 2): host-visible, written from the start at every
    // `begin` (the frame before it was waited for), one block a posed body -
    // a header (lights, from-black, the base grey, the affines' count), the
    // lights (two vec4 each, `kPoseLights` places), then three vec4 an affine
    static constexpr int kPoseLights = 16, kPoseMaxMeshes = 256;
    static constexpr VkDeviceSize kPoseBytes = 8u << 20;
    static constexpr VkDeviceSize kPoseRange = (1 + 2 * kPoseLights + 3 * kPoseMaxMeshes) * 16;
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
    bool runPresentPass(int vy, int vh);
    bool presentImage(VkImage src, int sw, int sh);

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

// The instance ALONE, so the window layer can make a surface from it before a
// device exists. Present support is a property of a (device, surface) pair, so
// the surface has to come first or the queue family cannot be chosen correctly
// - which is why this is split out rather than done inside `init`.
void* VulkanRenderer::createInstanceOnly() {
    if (inst_ != VK_NULL_HANDLE) return inst_;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "omk";
    app.apiVersion = VK_API_VERSION_1_1;
    // MoltenVK is a PORTABILITY driver: without this extension and flag the
    // loader reports zero devices on macOS and the failure looks like "no GPU"
    // rather than "not asked for".
    std::vector<const char*> iexts = {"VK_KHR_portability_enumeration"};
    for (const auto& e : wantInstExts_) iexts.push_back(e.c_str());
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

    const float pri = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = qfam_; qi.queueCount = 1; qi.pQueuePriorities = &pri;
    // Anisotropic filtering is a device FEATURE, asked for at creation and
    // only when the enhancement wants it AND the device has it; the base
    // renderer enables no feature at all.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(phys_, &have);
    VkPhysicalDeviceFeatures enable{};
    if (aniso_ > 1 && filter_ >= 2 && have.samplerAnisotropy) {
        enable.samplerAnisotropy = VK_TRUE; anisoOn_ = true;
    } else if (aniso_ > 1) {
        std::fprintf(stderr, "vulkan: anisotropy %d ignored - %s\n", aniso_,
                     filter_ < 2 ? "it needs trilinear filtering (a mip chain)"
                                 : "the device has no samplerAnisotropy feature");
    }
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &enable;
    di.enabledExtensionCount = static_cast<uint32_t>(want.size());
    di.ppEnabledExtensionNames = want.empty() ? nullptr : want.data();
    VKCHECK(vkCreateDevice(phys_, &di, nullptr, &dev_), "vkCreateDevice");
    vkGetDeviceQueue(dev_, qfam_, 0, &queue_);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = qfam_;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VKCHECK(vkCreateCommandPool(dev_, &pi, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(dev_, &cai, &cb_), "vkAllocateCommandBuffers");
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

    auto image = [&](VkFormat f, VkImageUsageFlags use, VkImageAspectFlags asp,
                     VkSampleCountFlagBits samples,
                     VkImage& img, VkDeviceMemory& mem, VkImageView& view) -> bool {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f;
        ii.extent = {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = samples; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = use; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKCHECK(vkCreateImage(dev_, &ii, nullptr, &img), "vkCreateImage");
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VKCHECK(vkAllocateMemory(dev_, &ai, nullptr, &mem), "vkAllocateMemory(image)");
        OMK_GPU_ALLOC("render targets", omk::prof::gpuKey(omk::prof::kVkMemory, mem), ai.allocationSize);
        VKCHECK(vkBindImageMemory(dev_, img, mem, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = {asp, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &view), "vkCreateImageView");
        return true;
    };
    if (!image(VK_FORMAT_R8G8B8A8_UNORM,
               // SAMPLED for the present pass, which reads the frame where it is
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_1_BIT,
               colour_, colourMem_, colourView_)) return false;
    if (msaa && !image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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
    if (wantSsao_) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(phys_, dsFmt_, &fp);
        ssaoOn_ = !msaa && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!resizing_)
            std::printf(ssaoOn_ ? "vulkan: SSAO on - an ENHANCEMENT: ambient occlusion from the "
                                  "depth buffer, multiplied over the frame\n"
                                : "vulkan: SSAO asked for but not available (%s)\n",
                        msaa ? "multisampling is on" : "the depth format cannot be sampled");
    }
    if (!image(dsFmt_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   (ssaoOn_ ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_SAMPLED_BIT) : 0u),
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, samples_,
               depth_, depthMem_, depthView_)) return false;
    if (ssaoOn_) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = dsFmt_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &depthSampleView_), "depth sample view");
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
    att[1].storeOp = ssaoOn_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = ssaoOn_ ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[2].samples = VK_SAMPLE_COUNT_1_BIT;
    att[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[2].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference rref{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pResolveAttachments = msaa ? &rref : nullptr;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = msaa ? 3 : 2; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    VKCHECK(vkCreateRenderPass(dev_, &rpi, nullptr, &pass_), "vkCreateRenderPass");

    VkImageView views[3] = {msaa ? msColourView_ : colourView_, depthView_, colourView_};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass_; fi.attachmentCount = msaa ? 3 : 2; fi.pAttachments = views;
    fi.width = static_cast<uint32_t>(rw_); fi.height = static_cast<uint32_t>(rh_);
    fi.layers = 1;
    VKCHECK(vkCreateFramebuffer(dev_, &fi, nullptr, &fbuf_), "vkCreateFramebuffer");

    if (!makeBuffer(static_cast<VkDeviceSize>(rw_) * rh_ * 4,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    readBuf_, readMem_)) return false;
    fb_ = omk::Surface(w_, h_, 0);
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
        if (vkCreateImage(dev_, &ii, nullptr, &shImg_) == VK_SUCCESS) {
            VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, shImg_, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(dev_, &ai, nullptr, &shMem_);
            OMK_GPU_ALLOC("shadow map", omk::prof::gpuKey(omk::prof::kVkMemory, shMem_), ai.allocationSize);
            vkBindImageMemory(dev_, shImg_, shMem_, 0);
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
        }
    }
    if (shFbuf_ == VK_NULL_HANDLE)
        std::fprintf(stderr, "vulkan: no shadow map here - `--shadow-quality mapped` "
                             "will draw the fitted blobs instead\n");
    return true;
}

bool VulkanRenderer::makePipelines() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorCount = 1;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1; dli.pBindings = &b;
    VKCHECK(vkCreateDescriptorSetLayout(dev_, &dli, nullptr, &dsl_), "descriptorSetLayout");

    // SET 1 is per FRAME - the shadow map and the light's transform. It exists
    // whether or not a shadow is drawn, because a pipeline layout is fixed at
    // creation and `strength = 0` is how a frame says it has no shadow.
    VkDescriptorSetLayoutBinding b1[3]{};
    b1[0].binding = 0; b1[0].descriptorCount = 1;
    b1[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b1[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b1[1].binding = 1; b1[1].descriptorCount = 1;
    b1[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b1[2].binding = 2; b1[2].descriptorCount = 1;
    b1[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b1[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli1.bindingCount = 3; dli1.pBindings = b1;
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
                            0, sizeof(Push)};
    const VkDescriptorSetLayout sets[3] = {dsl_, dsl1_, dsl2_};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 3; pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKCHECK(vkCreatePipelineLayout(dev_, &pli, nullptr, &plo_), "pipelineLayout");

    auto module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes; si.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev_, &si, nullptr, &m);
        return m;
    };
    VkShaderModule vs = module(kVert, sizeof(kVert));
    VkShaderModule fs = module(kFrag, sizeof(kFrag));
    if (!vs || !fs) { std::fprintf(stderr, "vulkan: shader module\n"); return false; }

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

        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = stages;
        gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dsi; gp.layout = plo_; gp.renderPass = pass_;
        VKCHECK(vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe_[k]),
                "vkCreateGraphicsPipelines");
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
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &none;

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
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
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

    // 64 sets: the engine's texture pool is 58 slots, plus the white fallback.
    // Plus set 1, the per-frame shadow set, and its one uniform buffer.
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 130},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 131; dpi.poolSizeCount = 3; dpi.pPoolSizes = ps;
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
            VkDescriptorBufferInfo bufi{poseBuf_, 0, kPoseRange};
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

// SET 1, rebuilt. It has to be a function because `setTextures` RESETS the
// descriptor pool, and a reset frees every set allocated from it - this one
// included. The first version allocated it once in `makePipelines` and the
// game's first `setTextures` quietly destroyed it, so the fragment shader read
// a strength of 0 and no shadow was ever drawn. The synthetic probe passed an
// EMPTY texture list, took the early-out and never reset, which is why it
// worked while the game did not: the bug was in the difference between the two
// callers, not in the shadow.
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
    VkWriteDescriptorSet wr[3]{};
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
    vkUpdateDescriptorSets(dev_, 3, wr, 0, nullptr);
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
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
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
    // finished frame is blitted in.
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    return true;
}

void VulkanRenderer::killSwapchain() {
    if (!dev_) return;
    if (pcb_) { vkFreeCommandBuffers(dev_, pool_, 1, &pcb_); pcb_ = VK_NULL_HANDLE; }
    if (pfence_) { vkDestroyFence(dev_, pfence_, nullptr); pfence_ = VK_NULL_HANDLE; }
    if (acquired_) { vkDestroySemaphore(dev_, acquired_, nullptr); acquired_ = VK_NULL_HANDLE; }
    if (drawn_) { vkDestroySemaphore(dev_, drawn_, nullptr); drawn_ = VK_NULL_HANDLE; }
    if (swap_) { vkDestroySwapchainKHR(dev_, swap_, nullptr); swap_ = VK_NULL_HANDLE; }
    swapImgs_.clear();
}

bool VulkanRenderer::remakeSwapchain() {
    if (!dev_ || surface_ == VK_NULL_HANDLE) return false;
    vkDeviceWaitIdle(dev_);
    killSwapchain();
    return makeSwapchain(w_, h_);
}

bool VulkanRenderer::resize(int w, int h) {
    if (!dev_ || w <= 0 || h <= 0) return false;
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
    if (fbuf_) { vkDestroyFramebuffer(dev_, fbuf_, nullptr); fbuf_ = VK_NULL_HANDLE; }
    if (pass_) { vkDestroyRenderPass(dev_, pass_, nullptr); pass_ = VK_NULL_HANDLE; }
    img(colourView_, colour_, colourMem_);
    img(msColourView_, msColour_, msColourMem_);
    killSsao();
    if (depthSampleView_) { vkDestroyImageView(dev_, depthSampleView_, nullptr); depthSampleView_ = VK_NULL_HANDLE; }
    img(depthView_, depth_, depthMem_);
    buf(readBuf_, readMem_);
    buf(upBuf_, upMem_);
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
    w_ = w; h_ = h;
    rw_ = w_ * ss_; rh_ = h_ * ss_;
    srcW_ = rw_; srcH_ = rh_;
    resizing_ = true;
    const bool ok = makeTarget();
    resizing_ = false;
    if (!ok) return false;
    std::printf("vulkan: frame target remade at %dx%d\n", w_, h_);
    return surface_ == VK_NULL_HANDLE || remakeSwapchain();
}

bool VulkanRenderer::presentDirect() {
    return presentImage(colour_, srcW_ ? srcW_ : rw_, srcH_ ? srcH_ : rh_);
}

// The swapchain half of every present: `src`'s top-left `sw x sh`, in
// TRANSFER_SRC layout, blitted into the window's middle rows.
bool VulkanRenderer::presentImage(VkImage src, int sw, int sh) {
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
                   1, &blit, ss_ > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

    barrier(swapImgs_[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(pcb_);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &pcb_;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &drawn_;
    vkResetFences(dev_, 1, &pfence_);
    vkQueueSubmit(queue_, 1, &si, pfence_);

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
    if (swap_ == VK_NULL_HANDLE) return false;
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

    VkCommandBuffer cb = oneShotBegin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = colour_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyBufferToImage(cb, upBuf_, colour_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    srcW_ = w_; srcH_ = h_;   // only the top-left holds the uploaded picture
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    oneShotEnd(cb);
    return presentDirect();
}

// ---- THE PRESENT PASS (todo/optimization.md step 4b) --------------------------

bool VulkanRenderer::makePresentPass() {
    if (presPipe_ != VK_NULL_HANDLE) return true;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKCHECK(vkCreateImage(dev_, &ii, nullptr, &presImg_), "present image");
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, presImg_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VKCHECK(vkAllocateMemory(dev_, &ai, nullptr, &presMem_), "present image memory");
        OMK_GPU_ALLOC("present image", omk::prof::gpuKey(omk::prof::kVkMemory, presMem_), ai.allocationSize);
        VKCHECK(vkBindImageMemory(dev_, presImg_, presMem_, 0), "present image bind");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = presImg_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &presView_), "present image view");
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
        VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpi.attachmentCount = 1; rpi.pAttachments = &a;
        rpi.subpassCount = 1; rpi.pSubpasses = &sub;
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
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
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
        if (!qv || !qf) { std::fprintf(stderr, "vulkan: present shader module\n"); return false; }
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
    return makeBuffer(static_cast<VkDeviceSize>(w_) * h_ * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      presRead_, presReadMem_);
}

// `colour_` (TRANSFER_SRC, as `end()` and `presentSurface` leave it) through the
// present shader into `presImg_`, and back to TRANSFER_SRC.
bool VulkanRenderer::runPresentPass(int vy, int vh) {
    if (!makePresentPass()) return false;
    VkCommandBuffer cb = oneShotBegin();
    const auto layout = [&](VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = colour_;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = 0; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    layout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = presPass_; rp.framebuffer = presFbuf_;
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_)}};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, presPipe_);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, presPlo_, 0, 1, &presDs_, 0, nullptr);
    const PresentPush pp{vy, vh, w_, dither_ ? 1 : 0, ss_};
    vkCmdPushConstants(cb, presPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pp, &pp);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    oneShotEnd(cb);
    return true;
}

bool VulkanRenderer::presentWorld(int vy, int vh) {
    if (swap_ == VK_NULL_HANDLE) return false;
    if (!runPresentPass(vy, vh)) return false;
    return presentImage(presImg_, w_, h_);
}

bool VulkanRenderer::worldPicture(int vy, int vh, std::vector<unsigned char>& rgba) {
    if (!runPresentPass(vy, vh)) return false;
    VkCommandBuffer cb = oneShotBegin();
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {static_cast<uint32_t>(w_), static_cast<uint32_t>(h_), 1};
    vkCmdCopyImageToBuffer(cb, presImg_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presRead_, 1, &cp);
    oneShotEnd(cb);
    const std::size_t bytes = static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4;
    rgba.resize(bytes);
    void* p = nullptr;
    if (vkMapMemory(dev_, presReadMem_, 0, bytes, 0, &p) != VK_SUCCESS) return false;
    std::memcpy(rgba.data(), p, bytes);
    vkUnmapMemory(dev_, presReadMem_);
    return true;
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

bool VulkanRenderer::init(int w, int h) {
    w_ = w; h_ = h;
    rw_ = w_ * ss_; rh_ = h_ * ss_;
    srcW_ = rw_; srcH_ = rh_;
    if (!pickDevice()) return false;
    if (!makeTarget()) return false;
    if (!makePipelines()) return false;
    // The window is 640x480 while the 3D target is the letterboxed 640x352.
    if (surface_ != VK_NULL_HANDLE && !makeSwapchain(w, 480)) return false;
    return true;
}

void VulkanRenderer::setTextures(std::span<const omk::Texture> t) {
    // One VkImage and one descriptor set per material, plus a white 1x1 for a
    // material with no texture - which `raster.cpp` handles by leaving the
    // texel at 255 and letting the vertex colour stand alone.
    // `fill` writes tw*th RGBA texels, alpha the colour key
    auto upload = [&](auto&& fill, int tw, int th, Tex& out) {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tw) * th * 4;
        VkBuffer sb; VkDeviceMemory sm;
        if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        sb, sm)) return;
        void* p = nullptr;
        vkMapMemory(dev_, sm, 0, bytes, 0, &p);
        auto* dst = static_cast<unsigned char*>(p);
        // Alpha carries the COLOUR KEY: 0 where the texel is black, the key
        // the cutout path (flag 0x800) discards on, 255 elsewhere. With the
        // nearest sampler the shader's test on it is the same test on the
        // same texel as before; with a linear one the (0,0,0,0) key texels
        // make the sample PREMULTIPLIED, which is what lets the edge stay
        // clean. A non-cutout batch never reads alpha, so a black texel
        // still draws black.
        fill(dst);
        vkUnmapMemory(dev_, sm);

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
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {static_cast<uint32_t>(tw), static_cast<uint32_t>(th), 1};
        ii.mipLevels = levels; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                 | (levels > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(dev_, &ii, nullptr, &out.img);
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, out.img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(dev_, &ai, nullptr, &out.mem);
        OMK_GPU_ALLOC("textures", omk::prof::gpuKey(omk::prof::kVkMemory, out.mem), ai.allocationSize);
        vkBindImageMemory(dev_, out.img, out.mem, 0);

        VkCommandBuffer cb = oneShotBegin();
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
        // image to the shader. With one level that is level 0 (DST -> SRC ->
        // shader), a layout change and nothing else.
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, 0, 1};
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        oneShotEnd(cb);
        vkDestroyBuffer(dev_, sb, nullptr);
        freeDeviceMemory(sm);

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        vkCreateImageView(dev_, &vi, nullptr, &out.view);

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl_;
        vkAllocateDescriptorSets(dev_, &dai, &out.ds);
        VkDescriptorImageInfo dii{sampler_, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = out.ds; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
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
    if (white_.img == VK_NULL_HANDLE)
        upload([](unsigned char* d) { d[0] = d[1] = d[2] = d[3] = 255; }, 1, 1, white_);
    const auto keyOf = [](const omk::Texture& x) {
        return std::make_tuple(x.idx.data(), x.pal.data(), x.width, x.height);
    };
    for (auto& [key, u] : uploaded_) u.used = false;
    for (const auto& x : t)
        if (x.hasPixels())
            if (auto hit = uploaded_.find(keyOf(x)); hit != uploaded_.end()) hit->second.used = true;
    bool waited = false;
    for (auto it = uploaded_.begin(); it != uploaded_.end(); ) {
        if (it->second.used) { ++it; continue; }
        if (!waited) { vkDeviceWaitIdle(dev_); waited = true; }
        Tex& old = it->second.t;
        if (old.ds)   vkFreeDescriptorSets(dev_, dpool_, 1, &old.ds);
        if (old.view) vkDestroyImageView(dev_, old.view, nullptr);
        if (old.img)  vkDestroyImage(dev_, old.img, nullptr);
        if (old.mem)  freeDeviceMemory(old.mem);
        it = uploaded_.erase(it);
    }
    tex_.assign(t.size(), Tex{});
    for (std::size_t i = 0; i < t.size(); ++i) {
        const omk::Texture& x = t[i];
        if (!x.hasPixels()) continue;
        auto hit = uploaded_.find(keyOf(x));
        if (hit == uploaded_.end()) {
            Uploaded u;
            u.idx = x.idx; u.pal = x.pal; u.used = true;
            upload([&](unsigned char* d) { x.toRgbaKeyed(d); }, x.width, x.height, u.t);
            hit = uploaded_.emplace(keyOf(x), std::move(u)).first;
        }
        tex_[i] = hit->second.t;
    }
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
    const int vw = (view.letterboxed() ? view.vw : w_) * ss_;
    const int vh = (view.letterboxed() ? view.vh : h_) * ss_;
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
    for (auto& bm : deadBufs_) {
        vkDestroyBuffer(dev_, bm.first, nullptr);
        freeDeviceMemory(bm.second);
    }
    deadBufs_.clear();
    fog_ = view.fog;
    fogStart_ = view.fogStart;
    fogEnd_ = view.fogEnd;
    for (int i = 0; i < 3; ++i)
        fogColour_[i] = static_cast<float>(view.fogColour[i]) / 255.0f;
    for (int i = 0; i < 3; ++i)
        clearColour_[i] = static_cast<float>(view.clearColour[i]) / 255.0f;
    pushView(view);
    // ---- THE LIGHTS, per frame (`todo/enhancements.md` row 7) ------------
    shimmerClock_ = view.shimmerClock;
    dither_ = view.dither;
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
    }
    // A frame with no shadow pass says so through the uniform, because the
    // descriptor set is bound either way.
    if (!shadowLive_ && shUboPtr_) {
        ShadowUbo ub{};
        ub.texel = 1.0f / static_cast<float>(kShadowSide);
        ub.shimmer = shimmerClock_;   // the shimmer runs with or without a shadow
        std::memcpy(shUboPtr_, &ub, sizeof ub);
    }
    VkClearValue clear[2]{};
    // the engine clears to the scene's fog colour (`View::clearColour`)
    clear[0].color = {{clearColour_[0], clearColour_[1], clearColour_[2], 1.0f}};
    clear[1].depthStencil = {1.0f, 0};   // depth far, stencil 0
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass_; rp.framebuffer = fbuf_;
    // the RENDER size, not the output one: a render area smaller than the
    // attachment leaves the rest of it undefined, and with supersampling on
    // the resolve then averages four blocks of uninitialised memory.
    rp.renderArea = {{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
    rp.clearValueCount = 2; rp.pClearValues = clear;
    vkCmdBeginRenderPass(cb_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    setViewport(view);
    recording_ = true;
    aoTanH_ = viewTanH_; aoTanV_ = viewTanV_;
    aoVw_ = (view.letterboxed() ? view.vw : w_) * ss_;
    aoVh_ = (view.letterboxed() ? view.vh : h_) * ss_;
}

// A POSED body's block in the pose buffer (`posesBodies`): written once a
// frame for each body (its draws share the `meshPose` and the lights), at an
// offset the device's storage alignment allows. -> false when it does not fit.
bool VulkanRenderer::writePose(const omk::Draw& d, uint32_t& off) {
    const auto key = std::make_pair(d.meshPose, d.vertexLights);
    if (const auto it = poseCache_.find(key); it != poseCache_.end()) { off = it->second; return true; }
    const VkDeviceSize at = (poseUsed_ + poseAlign_ - 1) / poseAlign_ * poseAlign_;
    if (!posePtr_ || at + kPoseRange > kPoseBytes || d.meshPoses > kPoseMaxMeshes) return false;
    float* f = reinterpret_cast<float*>(static_cast<unsigned char*>(posePtr_) + at);
    const int nl = d.vertexLights ? std::min(d.vertexLightCount, kPoseLights) : 0;
    f[0] = static_cast<float>(nl);
    f[1] = d.lightsFromBlack ? 1.0f : 0.0f;
    f[2] = d.lightBase;
    f[3] = static_cast<float>(d.meshPoses);
    for (int i = 0; i < nl; ++i) std::memcpy(f + 4 + 8 * i, d.vertexLights + 8 * i, 8 * sizeof(float));
    float* a = f + 4 + 8 * kPoseLights;
    std::memcpy(a, d.meshPose, d.meshPoses * 12 * sizeof(float));   // three vec4 an affine
    poseUsed_ = at + (1 + 2 * kPoseLights + 3 * d.meshPoses) * 16;
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
    if (!recording_ || !d.geo || !d.count) return;
    if (d.meshPose && d.meshPoses && d.geo->cornerMesh.size() == d.geo->corners.size()) {
        uint32_t off = 0;
        if (poseDs_ != VK_NULL_HANDLE && writePose(d, off)) submitOne(d, true, off);
        else submitOne(cpuPose(d), false, 0);
        return;
    }
    submitOne(d, false, 0);
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
    VkPipeline use = forcePipeline_ ? forcePipeline_
                   : reflStencil_   ? pipeRefl_[k]
                                    : pipe_[k];
    vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, use);

    // The texture is the key's LOW SIX BITS and nothing else (ASSETS 4b) -
    // or the pool's full index above them, past 64 (`drawTextureSlot`).
    const std::size_t slot = omk::drawTextureSlot(d);
    const VkDescriptorSet ds =
        (slot < tex_.size() && tex_[slot].ds != VK_NULL_HANDLE) ? tex_[slot].ds
                                                                : white_.ds;
    vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 0, 1, &ds, 0, nullptr);
    if (ds1_ != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 1, 1,
                                &ds1_, 0, nullptr);
    // set 2, the pose buffer - bound for every draw (the shader declares it),
    // at the body's block for a posed one
    if (poseDs_ != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, plo_, 2, 1,
                                &poseDs_, 1, &poseOff);
    push_.posed = posed ? 1 : 0;

    push_.cutout = d.cutout ? 1 : 0;
    push_.caster = d.castsShadow ? 1 : 0;
    push_.lit = litCount_ > 0 ? d.lit : 0;
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
        vkCmdPushConstants(cb_, plo_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(Push), &push_);
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
    {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8_UNORM;
        ii.extent = {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_), 1};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKCHECK(vkCreateImage(dev_, &ii, nullptr, &aoImg_), "ssao image");
        VkMemoryRequirements req; vkGetImageMemoryRequirements(dev_, aoImg_, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VKCHECK(vkAllocateMemory(dev_, &ai, nullptr, &aoMem_), "ssao memory");
        OMK_GPU_ALLOC("ssao", omk::prof::gpuKey(omk::prof::kVkMemory, aoMem_), ai.allocationSize);
        VKCHECK(vkBindImageMemory(dev_, aoImg_, aoMem_, 0), "ssao bind");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = aoImg_; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(dev_, &vi, nullptr, &aoView_), "ssao view");
    }
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
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsaoPush)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &aoDsl_;
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
        if (multiply) {
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
    return pipe(kSsaoFrag, sizeof(kSsaoFrag), aoPass_, false, aoPipe_) &&
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
    // look: a radius of about a forearm, three quarters dark at most
    const float nearP = omk::kNearCut, farP = 200000.0f;
    const SsaoPush push{aoTanH_, aoTanV_, farP / (farP - nearP), -farP * nearP / (farP - nearP),
                        static_cast<float>(aoVw_), static_cast<float>(aoVh_), 45.0f, 0.75f};
    const auto run = [&](VkRenderPass rp, VkFramebuffer fb, VkPipeline pl) {
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_)}};
        vkCmdBeginRenderPass(cb_, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, aoPlo_, 0, 1, &aoDs_, 0, nullptr);
        vkCmdPushConstants(cb_, aoPlo_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
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

void VulkanRenderer::end() {
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
    if (ssaoOn_) runSsao();
    // The colour attachment ends in TRANSFER_SRC_OPTIMAL (the render pass says
    // so). The READBACK COPY is not recorded here: a frame the GPU presents
    // (`presentWorld`) never reads it, so copying every frame is waste
    // (todo/cpu-vs-original.md tier A). `readback()` records it when a frame
    // is read - the image is in TRANSFER_SRC_OPTIMAL then too, the present
    // pass handing it back so.
    copyOwed_ = true;
    srcW_ = rw_; srcH_ = rh_;   // the whole render target is live
    vkEndCommandBuffer(cb_);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb_;
    vkResetFences(dev_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &si, fence_);
    vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
    recording_ = false;
    cbStarted_ = false;
    shadowLive_ = false;
    dirty_ = true;
}

const omk::Surface& VulkanRenderer::readback() {
    // IDEMPOTENT, and that is a contract not an optimisation. The software
    // renderer returns its stored framebuffer, so a caller may read it twice
    // and may WRITE to it between the two - which is exactly what the mirror
    // pass does when it composites the reflection in. Re-reading the GPU
    // buffer on every call silently threw that composite away and the frame
    // came back nearly black on Vulkan while software was correct. So a frame
    // is converted once, when `end()` says there is a new one.
    if (!dirty_) return fb_;
    dirty_ = false;
    if (copyOwed_) {                 // the copy `end()` does not record
        copyOwed_ = false;
        VkCommandBuffer cb = oneShotBegin();
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {static_cast<uint32_t>(rw_), static_cast<uint32_t>(rh_), 1};
        vkCmdCopyImageToBuffer(cb, colour_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
    omk::Threads::shared().parallelFor(0, static_cast<std::size_t>(h_), 16,
                                       [&](std::size_t rowFrom, std::size_t rowTo) {
    const int y0 = static_cast<int>(rowFrom), y1 = static_cast<int>(rowTo);
    if (ss_ <= 1 && dither_) {
        // Row by row through the channel tables - the same bits as
        // `quantise888Dither` a pixel, without a division and a modulo for
        // every one of them (todo/optimization.md step 4).
        for (int y = y0; y < y1; ++y)
            omk::quantise888DitherRow(src + 4 * static_cast<std::size_t>(y) * static_cast<std::size_t>(w_),
                                      w_, y, fb_.px.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w_));
    } else if (ss_ <= 1) {
        for (int i = y0 * w_; i < y1 * w_; ++i)
            fb_.px[static_cast<std::size_t>(i)] =
                omk::rgb565(src[4 * i], src[4 * i + 1], src[4 * i + 2]);
    } else {
        const int n = ss_ * ss_;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w_; ++x) {
                int acc[3] = {0, 0, 0};
                for (int sy = 0; sy < ss_; ++sy)
                    for (int sx = 0; sx < ss_; ++sx) {
                        const std::size_t o =
                            (static_cast<std::size_t>(y * ss_ + sy) *
                                 static_cast<std::size_t>(rw_) +
                             static_cast<std::size_t>(x * ss_ + sx)) * 4;
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
    if (had) ++r->released_;
}

VulkanRenderer::~VulkanRenderer() {
    omk::removeGeometryListener(&VulkanRenderer::geometryGone, this);
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
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
    };
    for (auto& [key, u] : uploaded_) killTex(u.t);   // the slots' handles are theirs
    uploaded_.clear();
    tex_.clear();
    killTex(white_);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
    if (dpool_) vkDestroyDescriptorPool(dev_, dpool_, nullptr);
    for (auto& p : pipe_) if (p) vkDestroyPipeline(dev_, p, nullptr);
    if (plo_) vkDestroyPipelineLayout(dev_, plo_, nullptr);
    if (dsl_) vkDestroyDescriptorSetLayout(dev_, dsl_, nullptr);
    if (dsl2_) vkDestroyDescriptorSetLayout(dev_, dsl2_, nullptr);
    if (poseBuf_) vkDestroyBuffer(dev_, poseBuf_, nullptr);
    if (poseMem_) freeDeviceMemory(poseMem_);
    if (readBuf_) vkDestroyBuffer(dev_, readBuf_, nullptr);
    if (readMem_) freeDeviceMemory(readMem_);
    if (fbuf_) vkDestroyFramebuffer(dev_, fbuf_, nullptr);
    if (pass_) vkDestroyRenderPass(dev_, pass_, nullptr);
    if (colourView_) vkDestroyImageView(dev_, colourView_, nullptr);
    if (colour_) vkDestroyImage(dev_, colour_, nullptr);
    if (colourMem_) freeDeviceMemory(colourMem_);
    if (msColourView_) vkDestroyImageView(dev_, msColourView_, nullptr);
    if (msColour_) vkDestroyImage(dev_, msColour_, nullptr);
    if (msColourMem_) freeDeviceMemory(msColourMem_);
    killSsao();
    if (depthSampleView_) vkDestroyImageView(dev_, depthSampleView_, nullptr);
    if (depthView_) vkDestroyImageView(dev_, depthView_, nullptr);
    if (depth_) vkDestroyImage(dev_, depth_, nullptr);
    if (depthMem_) freeDeviceMemory(depthMem_);
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
    if (upBuf_) vkDestroyBuffer(dev_, upBuf_, nullptr);
    if (upMem_) freeDeviceMemory(upMem_);
    if (pipeStencil_) vkDestroyPipeline(dev_, pipeStencil_, nullptr);
    if (pipeDepthReset_) vkDestroyPipeline(dev_, pipeDepthReset_, nullptr);
    for (auto& pp : pipeRefl_) if (pp) vkDestroyPipeline(dev_, pp, nullptr);
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
