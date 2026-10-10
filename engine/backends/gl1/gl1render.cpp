// SPDX-License-Identifier: GPL-3.0-or-later
// The fixed-function OpenGL 1.x backend - see gl1render.h.
//
// WHAT IT TAKES FROM THE ORIGINAL rather than from the software reference:
// the shape of the pipeline. Direct3D's fixed function did the per-pixel work
// (texture x Gouraud colour, colour key, linear fog, the blend), the engine
// only CHANGED STATE between buckets and handed over vertex arrays. So here:
//   * state is set per draw only when it differs from the last one - blend,
//     depth write, alpha test, fog, cull, texture - the D3D render-state calls
//     the original made between buckets, and no more;
//   * textures are 16-bit (`GL_RGB5_A1`), as the original's D3D surfaces
//     were, with no mip chain (`sub_4638C0`: MIP NONE) - point-sampled by
//     default here, BILINEAR when `setTextureFilter(1)` asks, which is what
//     the original's hardware arm set (MAG/MIN LINEAR, `docs/ASSETS.md` 4);
//   * the colour key on black (`SetRenderState(27, 1)`, mesh flag 0x800) is
//     a texel of alpha 0 and the alpha test - the fixed-function equivalent.
//     Filtered, a kept edge fragment carries some of the key's black (the
//     Vulkan and GLES shaders divide it back out; fixed function cannot), so
//     a cutout's edge pixels are darkened by the key's share of their sample,
//     at most half (the alpha test keeps above 0.5) - which is also
//     what a card that turned the key into alpha before filtering drew. The
//     card's own key path is not reachable from this tree;
//   * the vertices are TRANSFORMED ON THE CPU, near-clipped and culled there,
//     and handed over already projected - the original's `D3DTLVERTEX`
//     (sx, sy, sz, rhw), here as clip-space (x w, y w, z w, w) so the
//     texturing stays perspective-correct;
//   * the fog is computed PER VERTEX on the CPU - the original's own path for
//     a card without table fog - with the bucket key's two exclusions
//     (`renderer.h`, View::fog).
// Both of the last two were forced as well as faithful: on Tiger's emulated
// Radeon 9700 the GPU's own transform let near-plane-crossing triangles
// through as huge garbage, and its fog darkened the near scene; done on the
// CPU, the frame matches the reference (2026-10-02).
//
// WHAT IT TAKES FROM THE SOFTWARE REFERENCE (`o3de/raster.cpp`), because a
// GPU frame is checked against it: the colour law (texel * colour,
// additive `src + dst`, multiply `dst * (1 - src)`, depth written by opaque
// draws only), the near cut (`kNearCut`), the single-sided cull and its
// winding, the shimmer added to the vertex colour before anything else, and
// the 4x4 ordered dither of the RGB565 framebuffer (applied at readback).
//
// THE CONTEXT. An offscreen CGL context rendering into a framebuffer object,
// so the window stays the frontend's: the frame comes back through
// `readback()` as RGB565 and is composited and presented exactly as the
// software reference's is (PlayState's world harness). CGL is OpenGL's own
// layer on Mac OS X (10.2 onwards); the framebuffer object is
// EXT_framebuffer_object (Tiger 10.4.3 onwards). EVERY entry point makes the
// context current and gives the caller's back - SDL keeps a GL context of its
// own for its window, and drawing into it is exactly the bug that guard is.
//
// ON CLASSIC MAC OS (`OMK_GL1_AGL`, the Carbon build, 2026-10-04) the context
// is AGL's and the WINDOW's: Mac OS 9's OpenGL 1.2.1 has neither pbuffers nor
// framebuffer objects, so the world is drawn into the window context's BACK
// buffer, read back from there before the frontend presents (`gl1host.h`),
// and the frontend then draws the composited frame over it and swaps. Every
// call on that path is OpenGL 1.2 or AGL, which OS 9's `OpenGLLibrary` and
// Tiger's CFM bridge both export under that one fragment name.
#if defined(OMK_GL1_AGL)
#  include <agl.h>
#  include <gl.h>
// Apple's upload extensions by value (gl.h has none); `gl1OverlayPath` says
// whether the driver offered them
#  ifndef GL_UNPACK_CLIENT_STORAGE_APPLE
#    define GL_UNPACK_CLIENT_STORAGE_APPLE 0x85B2
#  endif
#  ifndef GL_TEXTURE_RECTANGLE_EXT
#    define GL_TEXTURE_RECTANGLE_EXT 0x84F5
#  endif
#  ifndef GL_TEXTURE_STORAGE_HINT_APPLE
#    define GL_TEXTURE_STORAGE_HINT_APPLE 0x85BC
#  endif
#  ifndef GL_STORAGE_SHARED_APPLE
#    define GL_STORAGE_SHARED_APPLE 0x85BF
#  endif
#elif defined(__APPLE__)
#  define GL_SILENCE_DEPRECATION 1
#  include <OpenGL/OpenGL.h>
#  include <OpenGL/gl.h>
#  include <OpenGL/glext.h>
#else
#  error "gl1render.cpp: CGL is Mac OS X only - another platform needs its own context"
#endif

#include "gl1render.h"
#include "gl1host.h"

#include "o3de/greybank.h"
#include "o3de/shimmer.h"
#include "platform/profile.h"
#include "ui/surface.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

namespace omk {
namespace {

#if defined(OMK_GL1_AGL)
// The fog's colour sum is EXT_secondary_color on OpenGL 1.2 (core only from
// 1.4), and Mac OS 9's library exports the EXT names alone.
#  define OMK_GL_COLOR_SUM GL_COLOR_SUM_EXT
#  define OMK_GL_SECONDARY_ARRAY GL_SECONDARY_COLOR_ARRAY_EXT
#  define omkSecondaryColorPointer glSecondaryColorPointerEXT
using GlContext = AGLContext;
GlContext currentContext() { return aglGetCurrentContext(); }
void makeCurrent(GlContext c) { aglSetCurrentContext(c); }
#else
#  define OMK_GL_COLOR_SUM GL_COLOR_SUM
#  define OMK_GL_SECONDARY_ARRAY GL_SECONDARY_COLOR_ARRAY
#  define omkSecondaryColorPointer glSecondaryColorPointer
using GlContext = CGLContextObj;
GlContext currentContext() { return CGLGetCurrentContext(); }
void makeCurrent(GlContext c) { CGLSetCurrentContext(c); }
#endif

// The caller's context out, ours in - and back on scope exit.
struct Current {
    GlContext prev;
    explicit Current(GlContext ours) : prev(currentContext()) {
        if (prev != ours) makeCurrent(ours);
    }
    ~Current() {
        if (currentContext() != prev) makeCurrent(prev);
    }
};

// THE PROFILER'S GPU MEMORY (todo/debug-tools.md 5): what this backend
// specifies on the card, by object and size, and what it deletes. Nothing in
// a release build (`OMK_PROFILE=0`).
inline void gpuNote(const char* tag, omk::prof::GpuDomain d, GLuint id, long long bytes) {
    OMK_GPU_ALLOC(tag, omk::prof::gpuKey(d, id), bytes);
    (void)tag; (void)d; (void)id; (void)bytes;
}
inline void gpuForget(omk::prof::GpuDomain d, const GLuint* ids, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) OMK_GPU_FREE(omk::prof::gpuKey(d, ids[i]));
    (void)d; (void)ids; (void)n;
}

unsigned nextPow2(unsigned v) {
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

class Gl1Renderer final : public Renderer {
public:
    ~Gl1Renderer() override {
        if (!ctx_) return;
#if defined(OMK_GL1_AGL)
        // the context is the frontend's, and it may have closed first
        if (gl1AglContext() != ctx_) return;
#endif
        {
            Current c(ctx_);
            for (auto& [key, u] : uploaded_) {
                gpuForget(omk::prof::kGlTexture, &u.name, 1);
                glDeleteTextures(1, &u.name);
            }
            uploaded_.clear();
#if defined(OMK_GL1_AGL)
            if (readTex_) gpuForget(omk::prof::kGlTexture, &readTex_, 1);
            if (readTex_) glDeleteTextures(1, &readTex_);
#else
            if (fbo_) glDeleteFramebuffersEXT(1, &fbo_);
            if (colour_) gpuForget(omk::prof::kGlRenderbuffer, &colour_, 1);
            if (colour_) glDeleteRenderbuffersEXT(1, &colour_);
            if (depth_) gpuForget(omk::prof::kGlRenderbuffer, &depth_, 1);
            if (depth_) glDeleteRenderbuffersEXT(1, &depth_);
#endif
        }
#if !defined(OMK_GL1_AGL)                    // the AGL context is the frontend's
        CGLDestroyContext(ctx_);
        if (pbuf_) CGLDestroyPBuffer(pbuf_);
#endif
    }

    bool init(int w, int h) override {
        if (!ctx_ && !makeContext()) return false;
        Current c(ctx_);
        w_ = w; h_ = h;
#if defined(OMK_GL1_AGL)
        // the window's own back buffer, which is the window's size - the
        // frame the viewer asks for (the display size)
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
#else
        if (!fbo_) glGenFramebuffersEXT(1, &fbo_);
        if (!colour_) glGenRenderbuffersEXT(1, &colour_);
        if (!depth_) glGenRenderbuffersEXT(1, &depth_);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo_);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, colour_);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
        gpuNote("render targets", omk::prof::kGlRenderbuffer, colour_, 4LL * w * h);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                     GL_RENDERBUFFER_EXT, colour_);
        glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, depth_);
        glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT24, w, h);
        gpuNote("render targets", omk::prof::kGlRenderbuffer, depth_, 4LL * w * h);
        glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                     GL_RENDERBUFFER_EXT, depth_);
        if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
            // a 24-bit depth buffer is not on every card of the period
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT16, w, h);
            gpuNote("render targets", omk::prof::kGlRenderbuffer, depth_, 2LL * w * h);
            if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
                std::fprintf(stderr, "gl1: the framebuffer object is incomplete\n");
                return false;
            }
        }
#endif
        fb_ = Surface(w, h, 0);
        rgba_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4, 0);
        readDone_ = false;
        return true;
    }

    // THE TEXTURE POOL, UPLOADED ONLY WHERE IT IS NEW (2026-10-10). The frame
    // hands the pool over again whenever it changes - a particle's sprite
    // arriving or leaving, a character coming into view - and this deleted
    // EVERY texture on the card and sent them all again, set and bodies, each
    // texel through a call: on the reader's iBook G3 that is the likeliest
    // owner of `props, guns` at 12-65 ms a frame (`todo/handoff-classic-mac.md`
    // 3f). The original binds a texture into its 58-slot cache once
    // (`Tex3DT_BindMaterials`) and keeps it. As the GLES and 3DS backends do:
    // a texture already on the card from an earlier pool - the same pixel
    // storage, palette and size - is kept, and the entry holds that storage,
    // so its address cannot come back as another texture's while cached;
    // what no slot of the new pool uses leaves the card.
    //
    // A new one goes up as 16-bit ARGB 1555 words - the internal format it
    // always had (`GL_RGB5_A1`) - built through the palette (256 entries,
    // made once a texture) instead of RGBA bytes the driver converted: the
    // channels TRUNCATED to five bits, as the engine's own 16-bit conversion
    // does (`rgb565`), and the colour key (alpha 0) on a black palette entry
    // as before.
    void setTextures(std::span<const Texture> t) override {
        Current c(ctx_);
        flush();
        for (auto& [key, u] : uploaded_) u.used = false;
        tex_.assign(t.size(), 0);
        texW_.assign(t.size(), 1.0f);
        texH_.assign(t.size(), 1.0f);
        const auto keyOf = [](const Texture& x) {
            return std::make_tuple(x.idx.data(), x.pal.data(), x.width, x.height);
        };
        for (const Texture& x : t)
            if (x.hasPixels())
                if (auto hit = uploaded_.find(keyOf(x)); hit != uploaded_.end()) hit->second.used = true;
        int kept = 0, fresh = 0, dropped = 0;
        for (auto it = uploaded_.begin(); it != uploaded_.end(); ) {
            if (it->second.used) { ++it; continue; }
            gpuForget(omk::prof::kGlTexture, &it->second.name, 1);
            glDeleteTextures(1, &it->second.name);
            it = uploaded_.erase(it);
            ++dropped;
        }
        std::vector<std::uint16_t> px;
        for (std::size_t i = 0; i < t.size(); ++i) {
            const Texture& x = t[i];
            if (!x.hasPixels()) continue;
            auto hit = uploaded_.find(keyOf(x));
            if (hit == uploaded_.end()) {
                // OpenGL 1.x wants power-of-two sides; the UVs are in texels, so
                // a padded texture keeps them and only the texture matrix's scale
                // changes. Every shipped .3DT is already a power of two.
                const unsigned pw = nextPow2(static_cast<unsigned>(x.width));
                const unsigned ph = nextPow2(static_cast<unsigned>(x.height));
                std::uint16_t lut[256];
                for (int k = 0; k < 256; ++k) {
                    const std::uint8_t* q = x.pal.data() + 3 * k;
                    lut[k] = static_cast<std::uint16_t>(((q[0] | q[1] | q[2]) ? 0x8000u : 0u) |
                                                        (q[0] >> 3) << 10 | (q[1] >> 3) << 5 | (q[2] >> 3));
                }
                px.assign(static_cast<std::size_t>(pw) * ph, 0);
                const std::uint8_t* idx = x.idx.data();
                for (int y = 0; y < x.height; ++y) {
                    std::uint16_t* d = px.data() + static_cast<std::size_t>(y) * pw;
                    const std::uint8_t* sr = idx + static_cast<std::size_t>(y) * x.width;
                    for (int xx = 0; xx < x.width; ++xx) d[xx] = lut[sr[xx]];
                }
                GLuint name = 0;
                glGenTextures(1, &name);
                glBindTexture(GL_TEXTURE_2D, name);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter_ ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_ ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
                // packed 16-bit words in the host's own order: the packed type
                // is read as a host word on either byte order
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, static_cast<GLsizei>(pw),
                             static_cast<GLsizei>(ph), 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
                             px.data());
                gpuNote("textures", omk::prof::kGlTexture, name, 2LL * pw * ph);   // 16 bits a texel
                hit = uploaded_.emplace(keyOf(x), Uploaded{name, x.idx, x.pal, static_cast<float>(pw),
                                                           static_cast<float>(ph), true}).first;
                ++fresh;
            } else {
                ++kept;
            }
            tex_[i] = hit->second.name;
            texW_[i] = hit->second.w;
            texH_[i] = hit->second.h;
        }
        boundTex_ = ~0u;
        // what stayed, what went up, what left - the first ten pools, then one
        // line in fifty, since a sprite coming and going changes the pool
        static long pools = 0;
        static long keptAll = 0, freshAll = 0;
        ++pools; keptAll += kept; freshAll += fresh;
        if (pools <= 10 || pools % 50 == 0)
            std::printf("gl1: texture pool %ld, %zu slots - %d kept on the card, %d uploaded, "
                        "%d dropped (all pools so far: %ld kept, %ld uploaded)\n",
                        pools, t.size(), kept, fresh, dropped, keptAll, freshAll);
    }

    void begin(const View& v) override {
        Current c(ctx_);
        flush();
        setCamera(v);
        RCamera cam = v.cam;
        cam.w = camW_;
        cam.h = camH_;
        bindTarget();
        glViewport(0, 0, w_, h_);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(v.clearColour[0] / 255.0f, v.clearColour[1] / 255.0f,
                     v.clearColour[2] / 255.0f, 1.0f);   // the scene's fog colour
        glClearDepth(1.0);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
#if defined(OMK_GL1_AGL)
        // THE LETTERBOX BARS BLACK. On AGL the picture is drawn at its
        // letterbox place in the window's own back buffer and presented as it
        // stands, so the rows above and below it are the window's - and the
        // clear above left them the FOG colour: on the reader's iBook the bars
        // of every cutscene "blue-ish, a bit like the sky" (2026-10-09). The
        // composed frame's bars are black, as the original's are; only the
        // two bands are cleared again, scissored.
        if (v.letterboxed()) {
            const int top = std::max(0, std::min(v.vy, h_ - cam.h));
            const int below = h_ - top - cam.h;
            glEnable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            if (top > 0) { glScissor(0, h_ - top, w_, top); glClear(GL_COLOR_BUFFER_BIT); }
            if (below > 0) { glScissor(0, 0, w_, below); glClear(GL_COLOR_BUFFER_BIT); }
            glDisable(GL_SCISSOR_TEST);
        }
#endif
        // the picture in the TOP-LEFT cam.w x cam.h, as the reference draws it;
        // GL's window origin is bottom-left. On classic Mac OS (AGL) it is
        // drawn at its LETTERBOX place in the window's back buffer instead -
        // `view.vy` rows down, where the composed frame puts it - so a frame
        // nothing is drawn over is presented as it stands (`presentWorld`,
        // todo/cpu-vs-original.md tier B); `readback` takes it from there.
#if defined(OMK_GL1_AGL)
        vy_ = v.letterboxed() ? std::max(0, std::min(v.vy, h_ - cam.h)) : 0;
#endif
        glViewport(0, h_ - vy_ - cam.h, cam.w, cam.h);

        // THE CAMERA is applied on the CPU (`setCamera`, in submit). Both GL
        // matrices stay identity: what reaches GL is already in clip space.
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // the D3D device's fixed states, set once (`sub_4638C0`)
        glShadeModel(GL_SMOOTH);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);                 // strict: the first of two equal faces wins
        glEnable(GL_TEXTURE_2D);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glAlphaFunc(GL_GREATER, 0.5f);
        // THE FOG IS COMPUTED PER VERTEX, on the CPU (submit) - the
        // original's own fallback for a card without table fog, and what
        // keeps it off the device's fog unit altogether: under emulation that
        // unit darkened the near scene. colour * (1 - a) plus the fog colour
        // * a as a SECONDARY colour added after texturing is the reference's
        // lerp(texel * colour, fog, a), vertex by vertex.
        glDisable(GL_FOG);
        fogRGB_[0] = v.fogColour[0] / 255.0f;
        fogRGB_[1] = v.fogColour[1] / 255.0f;
        fogRGB_[2] = v.fogColour[2] / 255.0f;
        fogSum_ = fogRGB_[0] > 0 || fogRGB_[1] > 0 || fogRGB_[2] > 0;
        if (fogSum_ && !hasColourSum_) {
            // a renderer without EXT_secondary_color (Mac OS 9's software
            // one): the fog's colour is lost, its fade kept. The game's fog is
            // BLACK (`docs/ASSETS.md`), so the shipped scenes never get here.
            static bool told = false;
            if (!told) { told = true; std::printf("gl1: no EXT_secondary_color - a coloured fog draws black\n"); }
            fogSum_ = false;
        }
        if (fogSum_) { glEnable(OMK_GL_COLOR_SUM); glEnableClientState(OMK_GL_SECONDARY_ARRAY); }
        else if (hasColourSum_) { glDisable(OMK_GL_COLOR_SUM); glDisableClientState(OMK_GL_SECONDARY_ARRAY); }
        glDisable(GL_CULL_FACE);              // culled on the CPU, the reference's rule
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        // nothing cached survives a begin: the state is set on the first draw
        stateValid_ = false;
        boundTex_ = ~0u;
        st_ = RasterStats{};
        submits_ = batches_ = 0;
        readDone_ = false;
    }

    void submit(const Draw& d) override {
        if (!d.geo || d.count < 3) return;
        Current c(ctx_);
        const Geometry& g = *d.geo;
        if (d.start + d.count > g.corners.size()) return;

        // ONE DRAW CALL A RUN OF EQUAL STATE, as the original's: it filled a
        // bucket's vertices into one pool and made one `DrawPrimitive` a
        // non-empty bucket (`Render_FlushBuckets`, `Raster_DrawTriangles`).
        // The vertices below are already in clip space with the fog folded
        // into their colours, so only the DEVICE state - blend, alpha test,
        // texture - divides a batch; a draw that changes none of it appends
        // to the one pending. Each `glDrawArrays` costs the driver a fixed
        // set-up, which on Mac OS X 10.2's Rage 128 path is the CPU's.
        {
            const std::size_t slot = drawTextureSlot(d);
            const bool hasTex = slot < tex_.size() && tex_[slot] != 0;
            const unsigned want = hasTex ? static_cast<unsigned>(slot) : ~1u;
            if (pending() && (!stateValid_ || d.blend != blend_ || d.cutout != cutout_ ||
                                  want != boundTex_))
                flush();
        }
        setState(d);
        ++submits_;

        // Per triangle, what the original's own vertex loop did before
        // DrawPrimitive: into view space, the near cut (Sutherland-Hodgman
        // against z > kNearCut, as `raster.cpp`), the single-sided cull with
        // the reference's sign, the screen edges, then CLIP-SPACE positions -
        // (x w, y w, depth w, w) with w the view depth, which is what keeps
        // the texturing perspective-correct the way `rhw` did for D3D.
        //
        // THE TRIANGLE WHOLLY INSIDE - most of a frame's - TAKES NO CLIP
        // (2026-10-09, for the iBook G3): each corner's OUTCODE against the
        // five planes is the clip's own test (`z > kNearCut`, then each
        // edge's `dist >= 0` with the same expression), so a triangle with
        // all corners inside comes out of the clips exactly as it went in,
        // and one with all corners outside ONE plane comes out empty - both
        // are decided here without running them. And NO DIVISION: the cull
        // is the sign of the view-space determinant (the projected area's
        // sign for corners in front of the eye), and the clip-space position
        // is (x / th, y / tv, z - 2 near, z), the reference's screen mapping
        // with the w multiplied back in. The G3 divides in 17-31 cycles and
        // does not pipeline it; this was 6 divisions a triangle and 3 a
        // vertex. What is lost is rounding: the old form went through the
        // screen position and back, and a triangle edge-on to the eye (zero
        // area, no pixels) may now cull on the other side of zero.
        const std::size_t nCorner = d.count - d.count % 3;
        const bool cullOn = g.cornerCull.size() == g.corners.size() && !noCull();
        const float gx = gx_, gy = gy_;
        for (std::size_t t = 0; t < nCorner; t += 3) {
            ++st_.triangles;
            if (pending() >= kBatchMax) flush();        // a bounded pool, as the original's
            Vtx in[3];
            unsigned oc[3];
            for (int k = 0; k < 3; ++k) {
                const Corner& c = g.corners[d.start + t + static_cast<std::size_t>(k)];
                const float dx = c.x - eye_[0], dy = c.y - eye_[1], dz = c.z - eye_[2];
                const float x = dx * bs_[0] + dy * bs_[1] + dz * bs_[2];
                const float y = dx * bu_[0] + dy * bu_[1] + dz * bu_[2];
                const float z = dx * bf_[0] + dy * bf_[1] + dz * bf_[2];
                in[k].v[0] = x; in[k].v[1] = y; in[k].v[2] = z;
                unsigned o = z > kNearCut ? 0u : 1u;
                if (z * gx - x < 0.0f) o |= 2u;
                if (z * gx + x < 0.0f) o |= 4u;
                if (z * gy - y < 0.0f) o |= 8u;
                if (z * gy + y < 0.0f) o |= 16u;
                oc[k] = o;
                // the shimmer first, as the engine adds it into the bytes
                const float sh = shimmerOffset(c.phase, view_.shimmerClock);
                in[k].c[0] = c.r + sh; in[k].c[1] = c.g + sh; in[k].c[2] = c.b + sh;
                // ...then the greyscale bank (ops 150/151, `o3de/greybank.h`)
                if (view_.grey)
                    in[k].c[0] = in[k].c[1] = in[k].c[2] =
                        lumaGreyUnit(in[k].c[0], in[k].c[1], in[k].c[2]);
                in[k].u = c.u; in[k].t = c.v;
            }
            const unsigned outAll = oc[0] & oc[1] & oc[2];
            if (outAll) {
                if (outAll & 1u) ++st_.behind; else ++st_.offscreen;
                continue;
            }
            const bool cullThis = cullOn && g.cornerCull[d.start + t] != 0;
            if ((oc[0] | oc[1] | oc[2]) == 0) {
                if (cullThis && backFacing(in[0], in[1], in[2])) { ++st_.culled; continue; }
                ++st_.drawn;
                emit(in[0]); emit(in[1]); emit(in[2]);
                continue;
            }
            Vtx poly[4];
            int np = 0;
            for (int k = 0; k < 3 && np < 4; ++k) {
                const Vtx& a = in[k];
                const Vtx& b = in[(k + 1) % 3];
                const bool ina = a.v[2] > kNearCut, inb = b.v[2] > kNearCut;
                if (ina) poly[np++] = a;
                if (ina != inb && np < 4) poly[np++] = lerp(a, b, (kNearCut - a.v[2]) / (b.v[2] - a.v[2]));
            }
            if (np < 3) { ++st_.behind; continue; }
            // the single-sided cull on the near-clipped triangle - the
            // reference's own test, before anything else is cut away
            if (cullThis && backFacing(poly[0], poly[1], poly[2])) { ++st_.culled; continue; }
            // ...then THE SCREEN EDGES, on the CPU too. A wall beside the eye
            // projects thousands of pixels off-screen once near-clipped, and
            // Tiger's emulated Radeon rasterised such a triangle unclipped -
            // its texture coordinates collapsed and the wall drew flat. The
            // original clipped its transformed triangles itself as well. A
            // hair of margin keeps the edge pixels covered.
            Vtx buf[2][8];
            int nb = np;
            for (int k = 0; k < np; ++k) buf[0][k] = poly[k];
            int cur = 0;
            for (int plane = 0; plane < 4 && nb >= 3; ++plane) {
                const auto dist = [&](const Vtx& q) {
                    switch (plane) {
                    case 0:  return q.v[2] * gx - q.v[0];   // right:  x <= z tanh
                    case 1:  return q.v[2] * gx + q.v[0];   // left:   x >= -z tanh
                    case 2:  return q.v[2] * gy - q.v[1];   // top:    y <= z tanv
                    default: return q.v[2] * gy + q.v[1];   // bottom: y >= -z tanv
                    }
                };
                int no = 0;
                for (int k = 0; k < nb && no < 8; ++k) {
                    const Vtx& a = buf[cur][k];
                    const Vtx& b = buf[cur][(k + 1) % nb];
                    const float da = dist(a), db = dist(b);
                    if (da >= 0.0f) buf[cur ^ 1][no++] = a;
                    if ((da >= 0.0f) != (db >= 0.0f) && no < 8) buf[cur ^ 1][no++] = lerp(a, b, da / (da - db));
                }
                nb = no;
                cur ^= 1;
            }
            if (nb < 3) { ++st_.offscreen; continue; }
            const Vtx* out = buf[cur];
            ++st_.drawn;
            for (int k = 1; k + 1 < nb; ++k) {            // a fan: up to 6 triangles after the clips
                emit(out[0]); emit(out[k]); emit(out[k + 1]);
            }
        }
    }

    void end() override {
        Current c(ctx_);
        flush();
        glFlush();
        // what the batching saved, once: passes 61-120 summed (a street
        // frame is past its setup by then; a frame may be several passes) -
        // the draws handed over, the draw calls they became, the triangles
        sum_[0] += submits_; sum_[1] += batches_; sum_[2] += st_.triangles;
        sum_[3] += st_.drawn; sum_[4] += st_.culled;
        if (++passes_ == 60) sum_[0] = sum_[1] = sum_[2] = sum_[3] = sum_[4] = 0;
        if (passes_ == 120)
            std::printf("gl1: 60 passes drew %ld draws in %ld draw calls, %ld of %ld triangles "
                        "(%ld culled)\n", sum_[0], sum_[1], sum_[3], sum_[2], sum_[4]);
    }

    // THE MIRROR ON THE CARD, as the original draws it (`docs/ASSETS.md` 4c,
    // read 2026-09-30) and as the GLES backend does: the reflected scene
    // first, then the room and the mirror over it with NO clear between -
    // draw order and depth confine the reflection. Without this the frame
    // took `drawWithMirror`'s CPU fallback: three passes, three readbacks and
    // a composite, and the frame then went to the CPU present ("cpu mirror")
    // - on the iBook, every frame Kay'l's bathroom mirror is in view.
    // `OMK_CPU_MIRROR=1` keeps the fallback, as on GLES.
    bool drawMirrorScene(const View& v, const View& refl, std::span<const Draw> scene,
                         std::span<const Draw> sceneClipped, std::span<const Draw> mirror) override {
        static const bool cpuMirror = std::getenv("OMK_CPU_MIRROR") != nullptr;
        if (cpuMirror || mirror.empty()) return false;
        begin(v);
        setCamera(refl);                     // the camera alone: no clear, no state
        for (const auto& d : sceneClipped) submit(d);
        setCamera(v);
        for (const auto& d : scene) submit(d);
        for (const auto& d : mirror) submit(d);
        end();
        return true;
    }

    const Surface& readback() override {
        if (readDone_) return fb_;           // idempotent (renderer.h)
        Current c(ctx_);
        flush();
        bindTarget();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        // the first read over a known pattern: a read that writes nothing
        // then shows as 85 85 85, not as a black frame (the report below)
        if (!toldRead_) std::memset(rgba_.data(), 0x55, rgba_.size());
        // bytes, not a packed type: endian-free on any Mac
#if defined(OMK_GL1_AGL)
        readWindow();
#else
        glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, rgba_.data());
#endif
        // bottom-up rows into the top-down RGB565 frame, with the dither the
        // reference applies (`DITHERENABLE` 1 on both device arms)
        // (the world `vy_` rows down - 0 but on AGL - read back to the top)
        for (int y = 0; y < h_; ++y) {
            if (y + vy_ >= h_) {                 // below the picture: the clear
                for (int x = 0; x < w_; ++x) fb_.set(x, y, 0);
                continue;
            }
            const unsigned char* row = rgba_.data() + static_cast<std::size_t>(h_ - 1 - y - vy_) * w_ * 4;
            for (int x = 0; x < w_; ++x) {
                const unsigned char* p = row + static_cast<std::size_t>(x) * 4;
                fb_.set(x, y, view_.dither ? quantise888Dither(p[0], p[1], p[2], x, y)
                                           : quantise888(p[0], p[1], p[2]));
            }
        }
        readDone_ = true;
        if (!toldRead_) {
            // what the first read found, once: a black window is otherwise
            // indistinguishable from a renderer that drew nothing
            toldRead_ = true;
            const unsigned char* m = rgba_.data() +
                (static_cast<std::size_t>(h_ / 2) * w_ + static_cast<std::size_t>(w_ / 2)) * 4;
            std::printf("gl1: first readback %dx%d, %ld triangles, GL error 0x%x, centre "
                        "pixel %u %u %u\n", w_, h_, st_.triangles,
                        static_cast<unsigned>(glGetError()), m[0], m[1], m[2]);
        }
        return fb_;
    }

#if defined(OMK_GL1_AGL)
    // ---- THE WORLD PRESENTED STRAIGHT (todo/cpu-vs-original.md tier B) ----
    //
    // The world is in the window's back buffer at its letterbox place, the
    // bands cleared black: a frame nothing is drawn over needs no readback,
    // no composite and no `glDrawPixels` - it is swapped as it stands, at the
    // drawable's own depth with the DRIVER's dither (GL_DITHER, on by
    // default), as the original's 16-bit D3D device dithered (`DITHERENABLE`,
    // ASSETS 4) - the reader's choice of 2026-10-05. ~5 -> 17 fps on emulated
    // Tiger (`handoff-classic-mac.md`). Only where the window is the frame's
    // own size; anything else stays on the composite.
    bool presentWorld(int ww, int wh) {
        if (ww != w_ || wh != h_) return false;
        Current c(ctx_);
        flush();
        OMK_ZONE("gl1: swap");
        aglSwapBuffers(ctx_);
        return true;
    }
    // ...and a frame with the interface over it: the composed frame `fb`,
    // whose world rows are the KEY (0xF81F), and `mask` - how much of the
    // world shows through a pixel - drawn OVER the world as the GLES
    // overlay does: `C + world * M`, then the colour fade (`fade` rgb + its
    // weight). Fixed function: the CPU folds key and mask into one RGBA
    // texture, a quad blends it with (GL_ONE, GL_SRC_ALPHA); only the band of
    // rows the overlay touches is uploaded and drawn. THE FADE IS A SECOND
    // QUAD (2026-10-05): untextured over the whole window, its colour at its
    // weight, blended (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA) - `dst*(1-f) +
    // fade*f`, the GLES shader's `rgb*(1-f) + fade*f` done by the blender.
    // Folded into the texture it changed every row every frame of a fade, and
    // the emulated Tiger re-sent the whole frame for 26-31 ms each.
    bool presentOverlay(const Surface& fb, const std::uint8_t* mask, const float fade[4],
                        int ww, int wh) {
        if (ww != w_ || wh != h_ || fb.w != w_ || fb.h != h_) return false;
        int lo = fb.h, hi = -1;
        const bool fading = fade[3] > 0.0f;
        for (int y = 0; y < fb.h; ++y) {
            const std::uint16_t* row = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
            bool any = false;
            for (int x = 0; x < fb.w && !any; ++x) any = row[x] != 0xF81F;
            if (any) { if (lo > y) lo = y; hi = y; }
        }
        Current c(ctx_);
        flush();
        if (hi >= lo || fading) {
            // the 2D state for the quads; `begin` sets all of its own again
            glViewport(0, 0, ww, wh);
            glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, ww, 0, wh, -1, 1);
            glMatrixMode(GL_MODELVIEW); glLoadIdentity();
            glMatrixMode(GL_TEXTURE); glLoadIdentity(); glMatrixMode(GL_MODELVIEW);
            glDisable(GL_DEPTH_TEST); glDisable(GL_FOG); glDisable(GL_CULL_FACE);
            glDisable(GL_ALPHA_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_LIGHTING);
            glEnable(GL_BLEND);
            stateValid_ = false;             // the draw state the cache assumed is gone
            boundTex_ = ~0u;
        }
        if (hi >= lo) {
            // the probe's path (`gl1OverlayPath`): client storage stores ARGB
            // 8888 words, which is what it was timed with
            const Gl1UploadPath path = gl1OverlayPath();
            const GLenum target = path.rect ? GL_TEXTURE_RECTANGLE_EXT : GL_TEXTURE_2D;
            const GLsizei tw = path.rect ? fb.w : static_cast<GLsizei>(nextPow2(static_cast<unsigned>(fb.w)));
            const GLsizei th = path.rect ? fb.h : static_cast<GLsizei>(nextPow2(static_cast<unsigned>(fb.h)));
            if (!ovTex_ || ovTexW_ != tw || ovTexH_ != th) {
                if (ovTex_) glDeleteTextures(1, &ovTex_);   // client storage is bound at creation
                glGenTextures(1, &ovTex_);
                glBindTexture(target, ovTex_);
                glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                if (path.hint) glTexParameteri(target, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_SHARED_APPLE);
                if (path.client) {
                    // the texture's memory is OURS: the changed rows are
                    // converted straight into it and the card reads them there
                    ovStore_.assign(static_cast<std::size_t>(tw) * th, 0);
                    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
                    glTexImage2D(target, 0, GL_RGBA8, tw, th, 0, GL_BGRA,
                                 GL_UNSIGNED_INT_8_8_8_8_REV, ovStore_.data());
                    glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
                } else {
                    glTexImage2D(target, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
                }
                gpuNote("overlay", omk::prof::kGlTexture, ovTex_, 4LL * tw * th);
                ovTexW_ = tw; ovTexH_ = th;
                ovRowKey_.assign(static_cast<std::size_t>(th), 0);   // nothing sent yet
            }
            bool storeWaited = false;    // the card done with the client memory, once a frame
            // ONLY THE ROWS THAT CHANGED (2026-10-05, run on Tiger): a row
            // keeps its place in the texture, and is converted and sent only
            // when its source - the 565 row and its mask row - differs from
            // what was last sent there. A subtitle is the same rows for
            // hundreds of frames; re-sending the band every frame cost the
            // emulated Tiger ~45 ms an overlay frame. The key a row is
            // compared by is a hash, as the GLES overlay's (four chains).
            // ...and the changed rows go up in RUNS, one call a run rather
            // than one a row, in the format `CarbonFrontend::probeUploads`
            // timed fastest on this driver (`gl1OverlayArgb`): ARGB 8888 as
            // 32-bit words, or RGBA bytes.
            OMK_ZONE("gl1: overlay rows");      // the profiler (todo/debug-tools.md)
            glBindTexture(target, ovTex_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, path.client ? tw : 0);
            const bool argb = path.client || gl1OverlayArgb();
            const GLenum upFormat = argb ? GL_BGRA : GL_RGBA;
            const GLenum upType = argb ? GL_UNSIGNED_INT_8_8_8_8_REV : GL_UNSIGNED_BYTE;
            const std::size_t rowWords = static_cast<std::size_t>(fb.w);
            int runStart = -1;
            for (int y = lo; y <= hi + 1; ++y) {
                bool changed = false;
                const std::uint16_t* src = nullptr;
                const std::uint8_t* msk = nullptr;
                if (y <= hi) {
                    src = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
                    msk = mask ? mask + static_cast<std::size_t>(y) * fb.w : nullptr;
                    std::uint32_t h0 = 2166136261u, h1 = 0x9E3779B9u, h2 = 0x7F4A7C15u, h3 = 0x94D049BBu;
                    int x = 0;
                    for (; x + 4 <= fb.w; x += 4) {
                        h0 = (h0 ^ src[x])     * 16777619u;
                        h1 = (h1 ^ src[x + 1]) * 16777619u;
                        h2 = (h2 ^ src[x + 2]) * 16777619u;
                        h3 = (h3 ^ src[x + 3]) * 16777619u;
                    }
                    for (; x < fb.w; ++x) h0 = (h0 ^ src[x]) * 16777619u;
                    if (msk)
                        for (int m = 0; m < fb.w; ++m) h1 = (h1 ^ msk[m]) * 16777619u;
                    const std::uint32_t key = (h0 ^ (h1 * 3u) ^ (h2 * 5u) ^ (h3 * 7u)) | 1u;   // 0 = never sent
                    changed = ovRowKey_[static_cast<std::size_t>(y)] != key;
                    ovRowKey_[static_cast<std::size_t>(y)] = key;
                }
                if (changed) {
                    if (runStart < 0) runStart = y;
                    std::uint32_t* o32 = nullptr;
                    if (path.client) {
                        // the card may still be reading last frame's rows
                        if (!storeWaited) { glFinish(); storeWaited = true; }
                        o32 = ovStore_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(tw);
                    } else {
                        const std::size_t at = static_cast<std::size_t>(y - runStart) * rowWords;
                        if (ovBand_.size() < at + rowWords) ovBand_.resize(at + rowWords);
                        o32 = ovBand_.data() + at;
                    }
                    for (int xx = 0; xx < fb.w; ++xx) {
                        const std::uint16_t p = src[xx];
                        unsigned r = 0, g = 0, bl = 0, al = 0;     // the KEY: the world as it is
                        if (p != 0xF81F) {
                            const unsigned r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
                            r = r5 << 3 | r5 >> 2; g = g6 << 2 | g6 >> 4; bl = b5 << 3 | b5 >> 2;
                            al = 255u - (msk ? msk[xx] : 0u);   // alpha = 1 - M (the blend below)
                        }
                        if (argb) {
                            o32[xx] = al << 24 | r << 16 | g << 8 | bl;
                        } else {
                            auto* o = reinterpret_cast<unsigned char*>(o32 + xx);
                            o[0] = static_cast<unsigned char>(r); o[1] = static_cast<unsigned char>(g);
                            o[2] = static_cast<unsigned char>(bl); o[3] = static_cast<unsigned char>(al);
                        }
                    }
                } else if (runStart >= 0) {             // a run ends: send it
                    const void* send = path.client
                        ? static_cast<const void*>(ovStore_.data() + static_cast<std::size_t>(runStart) * static_cast<std::size_t>(tw))
                        : static_cast<const void*>(ovBand_.data());
                    glTexSubImage2D(target, 0, 0, runStart, fb.w, y - runStart, upFormat,
                                    upType, send);
                    runStart = -1;
                }
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glEnable(target);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
            // `C + world * M` as the STANDARD premultiplied "over" - alpha
            // stored as 1 - M and (GL_ONE, GL_ONE_MINUS_SRC_ALPHA) - the same
            // sum to the bit. It was (GL_ONE, GL_SRC_ALPHA), M as alpha: a
            // destination factor of SOURCE alpha, which period hardware need
            // not have, and on the reader's iBook (Rage 128 Pro, 10.2) every
            // frame with partly transparent text over the world - a subtitle,
            // the credits, an examine line - went WHITE, while a dialogue box
            // (alpha 0 or 1 only) drew right (2026-10-09). Not seen on the
            // emulated Tiger's Radeon 9700.
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(1, 1, 1, 1);
            // the band: fb rows lo..hi, top-down, onto window rows wh-lo..wh-hi-1
            // (a rectangle texture is addressed in texels)
            const float tsc = path.rect ? 1.0f : 1.0f / static_cast<float>(th);
            const float t0 = static_cast<float>(lo) * tsc, t1 = static_cast<float>(hi + 1) * tsc,
                        s1 = path.rect ? static_cast<float>(fb.w) : static_cast<float>(fb.w) / tw;
            const float yTop = static_cast<float>(wh - lo), yBot = static_cast<float>(wh - hi - 1);
            glBegin(GL_QUADS);
            glTexCoord2f(0, t0);  glVertex2f(0, yTop);
            glTexCoord2f(s1, t0); glVertex2f(static_cast<float>(ww), yTop);
            glTexCoord2f(s1, t1); glVertex2f(static_cast<float>(ww), yBot);
            glTexCoord2f(0, t1);  glVertex2f(0, yBot);
            glEnd();
            glDisable(target);
        }
        if (fading) {
            // the fade over everything, interface included, as the GLES pass
            glDisable(GL_TEXTURE_2D);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColor4f(fade[0], fade[1], fade[2], fade[3] > 1.0f ? 1.0f : fade[3]);
            glBegin(GL_QUADS);
            glVertex2f(0, static_cast<float>(wh));
            glVertex2f(static_cast<float>(ww), static_cast<float>(wh));
            glVertex2f(static_cast<float>(ww), 0);
            glVertex2f(0, 0);
            glEnd();
            glColor4f(1, 1, 1, 1);
        }
        if (hi >= lo || fading) glDisable(GL_BLEND);
        // ANY GL ERROR the overlay raised, said once each: the iBook's log is
        // all that machine gives back (a refused upload, out of memory)
        for (GLenum e = glGetError(); e != GL_NO_ERROR; e = glGetError()) {
            static unsigned told = 0;
            const unsigned bit = e >= GL_INVALID_ENUM && e <= GL_OUT_OF_MEMORY ? 1u << (e - GL_INVALID_ENUM) : 64u;
            if (told & bit) continue;
            told |= bit;
            std::printf("gl1: the overlay raised GL error 0x%04X (rows %d..%d, %s upload)\n",
                        static_cast<unsigned>(e), lo, hi, gl1OverlayArgb() ? "ARGB8888" : "RGBA bytes");
        }
        OMK_ZONE("gl1: swap");
        aglSwapBuffers(ctx_);
        return true;
    }
#endif

    RasterStats stats() const override { return st_; }   // only `triangles` is known
    const char* name() const override { return "OpenGL 1.x fixed-function"; }
    // TEXTURE FILTERING (renderer.h): 0 nearest, 1 bilinear. No mip chain -
    // the original set MIP NONE on both arms - so trilinear is drawn bilinear
    // and said. Recorded here; applied to every texture `setTextures` made
    // and every one it makes after, so the order of the two calls is free.
    bool setTextureFilter(int mode) override {
        filter_ = mode >= 1 ? 1 : 0;
        if (mode >= 2)
            std::printf("gl1: trilinear asked - no mip chain in this backend, drawn bilinear\n");
        if (!uploaded_.empty()) {
            Current c(ctx_);
            flush();
            for (auto& [key, u] : uploaded_) {
                const GLuint id = u.name;
                glBindTexture(GL_TEXTURE_2D, id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter_ ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter_ ? GL_LINEAR : GL_NEAREST);
            }
            boundTex_ = ~0u;
        }
        return mode <= 1;
    }

private:
#if defined(OMK_GL1_AGL)
    // THE WINDOW'S BACK BUFFER, READ THROUGH A TEXTURE. `glReadPixels` from a
    // window surface returns zeros on Tiger's emulated Radeon 9700 while the
    // same frame draws correctly when swapped (2026-10-04, probed: a cleared
    // red read back black, and with the composite skipped the street showed
    // in the window); the copy into a texture and `glGetTexImage` go through
    // texture memory instead. Both calls are OpenGL 1.1, so Mac OS 9 has them.
    void readWindow() {
        const GLsizei tw = static_cast<GLsizei>(nextPow2(static_cast<unsigned>(w_)));
        const GLsizei th = static_cast<GLsizei>(nextPow2(static_cast<unsigned>(h_)));
        if (!readTex_ || readTexW_ != tw || readTexH_ != th) {
            if (!readTex_) glGenTextures(1, &readTex_);
            glBindTexture(GL_TEXTURE_2D, readTex_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gpuNote("readback", omk::prof::kGlTexture, readTex_, 4LL * tw * th);
            readTexW_ = tw; readTexH_ = th;
            readBig_.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th) * 4, 0);
        }
        glBindTexture(GL_TEXTURE_2D, readTex_);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w_, h_);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, readBig_.data());
        boundTex_ = ~0u;                     // the next draw binds its own
        // the texture's rows are the back buffer's, bottom-up, at its stride
        for (int y = 0; y < h_; ++y)
            std::memcpy(rgba_.data() + static_cast<std::size_t>(y) * w_ * 4,
                        readBig_.data() + static_cast<std::size_t>(y) * tw * 4,
                        static_cast<std::size_t>(w_) * 4);
    }
    GLuint readTex_ = 0;
    GLsizei readTexW_ = 0, readTexH_ = 0;
    std::vector<unsigned char> readBig_;
    GLuint ovTex_ = 0;                   // the overlay (`presentOverlay`)
    GLsizei ovTexW_ = 0, ovTexH_ = 0;
    std::vector<std::uint32_t> ovBand_;  // a run of changed rows, converted
    std::vector<std::uint32_t> ovStore_; // the client-storage texture's own memory (`gl1OverlayPath`)
    std::vector<std::uint32_t> ovRowKey_;   // what each texture row was last sent from
#endif

    // Where the world is drawn and read from: the framebuffer object, or on
    // classic Mac OS the window's back buffer (the frontend presents after).
    void bindTarget() {
#if defined(OMK_GL1_AGL)
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
#else
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo_);
#endif
    }

#if defined(OMK_GL1_AGL)
    // The window's context, made by the frontend (`gl1host.h`); nothing to
    // attach here, and no extension needed - the back buffer is the target.
    bool makeContext() {
        ctx_ = static_cast<GlContext>(gl1AglContext());
        if (!ctx_) {
            std::fprintf(stderr, "gl1: no AGL context on the window\n");
            return false;
        }
        Current c(ctx_);
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        hasColourSum_ = ext && std::strstr(ext, "GL_EXT_secondary_color");
        GLint vp[4] = {0, 0, 0, 0};
        glGetIntegerv(GL_VIEWPORT, vp);      // the default: the drawable's size
        std::printf("gl1: the window's drawable is %dx%d\n", static_cast<int>(vp[2]),
                    static_cast<int>(vp[3]));
        std::printf("gl1: %s, OpenGL %s, on the window's back buffer (AGL)\n",
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        return true;
    }
#else
    // THE CONTEXT NEEDS A DRAWABLE ON TIGER. With none attached, Mac OS X
    // 10.4 draws nothing and `glReadPixels` writes nothing - not even a clear,
    // not even into a framebuffer object, on the Radeon and on Apple's own
    // software renderer alike (probed 2026-10-02: the read-back bytes stayed
    // as the probe left them). A current Mac renders into the FBO without
    // one. So a small PBUFFER is attached - the period's offscreen drawable,
    // 10.3 onwards - and the FBO is drawn into as before; where pbuffers are
    // gone or refused (deprecated since 10.7) the context goes on without.
    bool makeContext() {
        for (int withPBuffer = 1; withPBuffer >= 0 && !ctx_; --withPBuffer) {
            CGLPixelFormatAttribute attrs[8];
            int k = 0;
            attrs[k++] = kCGLPFAAccelerated;
            if (withPBuffer) attrs[k++] = kCGLPFAPBuffer;
            attrs[k++] = kCGLPFAColorSize; attrs[k++] = static_cast<CGLPixelFormatAttribute>(24);
            attrs[k++] = kCGLPFADepthSize; attrs[k++] = static_cast<CGLPixelFormatAttribute>(16);
            attrs[k] = static_cast<CGLPixelFormatAttribute>(0);
            CGLPixelFormatObj pf = nullptr;
            GLint n = 0;
            if (CGLChoosePixelFormat(attrs, &pf, &n) != kCGLNoError || !pf) continue;
            const CGLError e = CGLCreateContext(pf, nullptr, &ctx_);
            CGLDestroyPixelFormat(pf);
            if (e != kCGLNoError || !ctx_) { ctx_ = nullptr; continue; }
            if (withPBuffer) {
                GLint screen = 0;
                if (CGLCreatePBuffer(16, 16, GL_TEXTURE_2D, GL_RGBA, 0, &pbuf_) != kCGLNoError ||
                    CGLGetVirtualScreen(ctx_, &screen) != kCGLNoError ||
                    CGLSetPBuffer(ctx_, pbuf_, 0, 0, screen) != kCGLNoError) {
                    if (pbuf_) { CGLDestroyPBuffer(pbuf_); pbuf_ = nullptr; }
                    CGLDestroyContext(ctx_); ctx_ = nullptr;
                    continue;
                }
            }
        }
        if (!ctx_) {
            std::fprintf(stderr, "gl1: no accelerated OpenGL context\n");
            return false;
        }
        Current c(ctx_);
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (!ext || !std::strstr(ext, "GL_EXT_framebuffer_object")) {
            std::fprintf(stderr, "gl1: no EXT_framebuffer_object (%s)\n",
                         reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
            return false;
        }
        std::printf("gl1: %s, OpenGL %s%s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                    pbuf_ ? ", on a pbuffer" : "");
        return true;
    }
#endif

    struct Vtx { float v[3]; float c[3]; float u, t; };
    static Vtx lerp(const Vtx& a, const Vtx& b, float t) {
        Vtx o;
        for (int k = 0; k < 3; ++k) { o.v[k] = a.v[k] + (b.v[k] - a.v[k]) * t; o.c[k] = a.c[k] + (b.c[k] - a.c[k]) * t; }
        o.u = a.u + (b.u - a.u) * t; o.t = a.t + (b.t - a.t) * t;
        return o;
    }
    // `OMK_NO_CULL=1` draws both sides, as it does for the reference
    static bool noCull() { static const bool n = std::getenv("OMK_NO_CULL") != nullptr; return n; }

    // THE CAMERA, applied on the CPU as the original did: it transformed
    // every vertex itself and handed D3D screen positions (`D3DTLVERTEX`:
    // sx, sy, sz, rhw), on cards that mostly had no T&L. `cameraBasis` is the
    // reference's own basis (mirror, roll and flip folded in). Apart from
    // `begin` so the mirror's reflected pass can change it without a clear.
    void setCamera(const View& v) {
        view_ = v;
        RCamera cam = v.cam;
        cam.w = v.letterboxed() ? v.vw : w_;
        cam.h = v.letterboxed() ? v.vh : h_;
        flipX_ = cam.flipX;
        cameraBasis(cam, bs_, bu_, bf_, th_, tv_);
        for (int k = 0; k < 3; ++k) eye_[k] = cam.eye[k];
        camW_ = cam.w; camH_ = cam.h;
        ith_ = 1.0f / th_; itv_ = 1.0f / tv_;
        gx_ = th_ * 1.01f; gy_ = tv_ * 1.01f;   // the screen edges' hair of margin
    }

    // The reference's cull (`raster.cpp`: the screen area's sign, flipped
    // under the mirror) without its divisions. For corners in front of the
    // eye the projected area is -det[v0; v1; v2] / (z0 z1 z2) times positive
    // scales, so its sign is the determinant's, negated.
    bool backFacing(const Vtx& a, const Vtx& b, const Vtx& c) const {
        const float det = a.v[0] * (b.v[1] * c.v[2] - b.v[2] * c.v[1]) -
                          a.v[1] * (b.v[0] * c.v[2] - b.v[2] * c.v[0]) +
                          a.v[2] * (b.v[0] * c.v[1] - b.v[1] * c.v[0]);
        return (flipX_ ? det : -det) > 0.0f;
    }

    // One clip-space vertex into the pending batch: (x / th, y / tv,
    // z - 2 near, z) is NDC (the reference's screen mapping; depth
    // 1 - near / z, increasing away from the eye, the order of the
    // original's rhw tested GREATER) times w = z. The reference's fog:
    // f = (end - z) / (end - start) past the start, the colour lerped toward
    // the fog's by 1 - f.
    void emit(const Vtx& p) {
        OutV o;
        const float w = p.v[2];
        o.p[0] = p.v[0] * ith_; o.p[1] = p.v[1] * itv_;
        o.p[2] = w - 2.0f * kNearCut; o.p[3] = w;
        float a = 0.0f;
        if (fogKind_ != 0 && w > fogStart_) {
            const float f = (fogEnd_ - w) * fogInv_;
            a = 1.0f - (f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f));
        }
        const float k = 1.0f - a;
        o.c[0] = toByte(p.c[0] * k); o.c[1] = toByte(p.c[1] * k);
        o.c[2] = toByte(p.c[2] * k); o.c[3] = 255;
        o.uv[0] = p.u; o.uv[1] = p.t;
        if (fogSum_) {
            o.f[0] = fogRGB_[0] * a; o.f[1] = fogRGB_[1] * a; o.f[2] = fogRGB_[2] * a;
            out_.push_back(o);
        } else {
            // no secondary colour this pass: the 28-byte vertex, not the
            // 40-byte one - a third less written and read a corner
            outN_.push_back(OutN{{o.p[0], o.p[1], o.p[2], o.p[3]}, {o.c[0], o.c[1], o.c[2], o.c[3]},
                                 {o.uv[0], o.uv[1]}});
        }
    }
    std::size_t pending() const { return out_.size() + outN_.size(); }

    // The pending batch drawn, with the device state it was gathered under.
    void flush() {
        if (!out_.empty()) {
            const GLsizei stride = static_cast<GLsizei>(sizeof(OutV));
            glVertexPointer(4, GL_FLOAT, stride, out_[0].p);
            glColorPointer(4, GL_UNSIGNED_BYTE, stride, out_[0].c);
            glTexCoordPointer(2, GL_FLOAT, stride, out_[0].uv);
            if (fogSum_) omkSecondaryColorPointer(3, GL_FLOAT, stride, out_[0].f);
            OMK_ZONE("gl1: draw call");          // the driver's share (todo/debug-tools.md)
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(out_.size()));
            ++batches_;
            out_.clear();
        }
        if (!outN_.empty()) {
            const GLsizei stride = static_cast<GLsizei>(sizeof(OutN));
            glVertexPointer(4, GL_FLOAT, stride, outN_[0].p);
            glColorPointer(4, GL_UNSIGNED_BYTE, stride, outN_[0].c);
            glTexCoordPointer(2, GL_FLOAT, stride, outN_[0].uv);
            OMK_ZONE("gl1: draw call");
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(outN_.size()));
            ++batches_;
            outN_.clear();
        }
    }

    static unsigned char toByte(float v) {
        // the corner colour is 0..1; the reference clamps after the multiply,
        // fixed function clamps the vertex colour first - equal for 0..1.
        // Converted FIRST and clamped as an integer (2026-10-10, for the G3,
        // whose float compares and int compares are separate units): the
        // same byte, since truncation toward zero sends (-1, 0] to 0 as the
        // float clamp did and the rest clamps the same on either side
        const float x = v * 255.0f + 0.5f;
        if (!(x > -1.0f)) return 0;                    // (and a NaN, as before: 0)
        if (x >= 255.0f) return 255;
        const int i = static_cast<int>(x);
        return static_cast<unsigned char>(i < 0 ? 0 : i);
    }

    // The D3D render states the original changed between buckets, changed
    // here only when they differ from the last draw's.
    void setState(const Draw& d) {
        const std::size_t slot = drawTextureSlot(d);   // ASSETS 4b; past 64 the pool's index
        const bool hasTex = slot < tex_.size() && tex_[slot] != 0;
        if (!stateValid_ || d.blend != blend_) {
            switch (d.blend) {
            case Blend::Opaque: glDisable(GL_BLEND); glDepthMask(GL_TRUE); break;
            case Blend::Add:                                  // src + dst, saturating
                glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE); glDepthMask(GL_FALSE); break;
            case Blend::Mul:                                  // dst * (1 - src)
                glEnable(GL_BLEND); glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                glDepthMask(GL_FALSE); break;
            }
            blend_ = d.blend;
        }
        if (!stateValid_ || d.cutout != cutout_) {
            if (d.cutout) glEnable(GL_ALPHA_TEST); else glDisable(GL_ALPHA_TEST);
            cutout_ = d.cutout;
        }
        // the fog's two exclusions (View::fog): 0x2080 none, 0x800 doubled
        const int fogKind = !view_.fog || view_.fogEnd <= view_.fogStart ? 0
                          : (d.bucketKey & 0x2080) ? 0 : (d.bucketKey & 0x800) ? 2 : 1;
        fogKind_ = fogKind;
        const float fk = fogKind == 2 ? 2.0f : 1.0f;
        fogStart_ = view_.fogStart * fk;
        fogEnd_ = view_.fogEnd * fk;
        fogInv_ = fogKind != 0 ? 1.0f / (fogEnd_ - fogStart_) : 0.0f;
        const unsigned want = hasTex ? slot : ~1u;
        if (want != boundTex_) {
            if (hasTex) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, tex_[slot]);
                // texel coordinates through the texture matrix, as a D3D port
                // divided them on the CPU
                glMatrixMode(GL_TEXTURE);
                glLoadIdentity();
                glScalef(1.0f / texW_[slot], 1.0f / texH_[slot], 1.0f);
                glMatrixMode(GL_MODELVIEW);
            } else {
                glDisable(GL_TEXTURE_2D);              // no texture: the colour alone
            }
            boundTex_ = want;
        }
        stateValid_ = true;
    }

    GlContext ctx_ = nullptr;
#if !defined(OMK_GL1_AGL)
    CGLPBufferObj pbuf_ = nullptr;
#endif
    GLuint fbo_ = 0, colour_ = 0, depth_ = 0;
    int w_ = 0, h_ = 0;
    int vy_ = 0;             // the picture's first row in the target (AGL: its letterbox place)
    std::vector<GLuint> tex_;            // per pool slot: a name `uploaded_` owns
    std::vector<float> texW_, texH_;
    // the textures on the card, by their pixel storage (`setTextures`); the
    // entry holds that storage so its address is not reused while cached
    struct Uploaded { GLuint name; PixelBuffer idx, pal; float w, h; bool used; };
    std::map<std::tuple<const std::uint8_t*, const std::uint8_t*, int, int>, Uploaded> uploaded_;
    View view_;
    bool flipX_ = false;
    Surface fb_{1, 1, 0};
    std::vector<unsigned char> rgba_;
    bool readDone_ = false, toldRead_ = false;
    RasterStats st_;
    // the state cache
    bool stateValid_ = false, cutout_ = false;
    int  filter_ = 0;        // 0 nearest (the software devices), 1 bilinear (a 3D card)
    Blend blend_ = Blend::Opaque;
    int fogKind_ = -1;
    float fogStart_ = 0, fogEnd_ = 0, fogRGB_[3]{};
    bool fogSum_ = false;
    bool hasColourSum_ = true;   // EXT_secondary_color, asked on AGL (makeContext)
    unsigned boundTex_ = ~0u;
    // THE PENDING BATCH, interleaved, kept between frames for its capacity
    // (no allocation per frame); `f` is the fog's secondary colour, read only
    // when `fogSum_`. Bounded so a frame's largest run stays a few MB.
    struct OutV { float p[4]; unsigned char c[4]; float uv[2]; float f[3]; };
    struct OutN { float p[4]; unsigned char c[4]; float uv[2]; };   // a pass without the fog sum
    static constexpr std::size_t kBatchMax = 3 * 16384;
    std::vector<OutV> out_;
    std::vector<OutN> outN_;
    float fogInv_ = 0;
    long submits_ = 0, batches_ = 0, passes_ = 0, sum_[5]{};
    // the camera, applied on the CPU (setCamera)
    float bs_[3]{}, bu_[3]{}, bf_[3]{}, th_ = 1, tv_ = 1, eye_[3]{};
    float ith_ = 1, itv_ = 1, gx_ = 1.01f, gy_ = 1.01f;
    int camW_ = 1, camH_ = 1;
};

}  // namespace

Renderer* makeGl1Renderer() { return new Gl1Renderer(); }

#if defined(OMK_GL1_AGL)
// `r` is the renderer `makeGl1Renderer` made (the glue holds no other)
bool gl1PresentWorld(Renderer* r, int ww, int wh) {
    return r && static_cast<Gl1Renderer*>(r)->presentWorld(ww, wh);
}
bool gl1PresentOverlay(Renderer* r, const Surface& fb, const std::uint8_t* mask,
                       const float fade[4], int ww, int wh) {
    return r && static_cast<Gl1Renderer*>(r)->presentOverlay(fb, mask, fade, ww, wh);
}
#endif

}  // namespace omk
