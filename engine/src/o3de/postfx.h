// SPDX-License-Identifier: GPL-3.0-or-later
// THE POST-PROCESSING CHAIN's numbers and its depth-of-field FOCUS, shared by
// the Direct3D 12 and the Vulkan backends (`todo/enhancements.md` 22).
//
// The chain's passes are one GLSL source (`backends/vulkan/shaders/bloom.frag`,
// `post.frag`: Vulkan compiles them to SPIR-V, Direct3D 12 cross-compiles the
// same SPIR-V to HLSL); this header is the other half of "the same chain": the
// push blocks those shaders read, the colour grades, the bloom's constants,
// and what the two backends decide per frame - which body or speaker the depth
// of field focuses on (`Focus`), and the numbers the final pass is handed
// (`Focus::dofParams`, `fillTone`). Written once so the pictures cannot drift
// apart for lack of a copied constant.
#pragma once

#include "o3de/raster.h"
#include "o3de/renderer.h"
#include "platform/env.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace omk::postfx {

// `bloom.frag`'s and `post.frag`'s push blocks, field for field
struct BloomPush { float texel[4]; float params[4]; float target[4]; };
struct PostPush { float frame[4], depthAB[4], dof[4], tone[4], lift[4], gam[4], gain[4], tintS[4], tintH[4]; };

// the stencil value a TRANSLUCENT BODY's pixels take for depth of field
inline constexpr std::uint32_t kSubjectStencil = 2;
inline constexpr int kBloomLevels = 6;   // half the display size down to 1/64
// paper white, nits: where the SDR picture's white goes on an HDR display
inline constexpr float kPaperWhite = 200.0f;

// THE GRADES: lift (rgb) + saturation, gamma (rgb) + S-curve, gain (rgb) +
// highlight desaturation, shadow tint + weight, highlight tint + weight.
// Small numbers on purpose: a look, not a filter.
struct Grade { float lift[4], gam[4], gain[4], tintS[4], tintH[4]; };
inline constexpr Grade kGrades[5] = {
    {},
    // CYBER - Anekbah's neon night: teal into the shadows, a magenta-orange
    // into the highlights, a mild S-curve and a little more colour
    {{0.0f, 0.012f, 0.025f, 1.15f}, {1.0f, 1.0f, 1.0f, 0.22f}, {1.02f, 0.99f, 1.0f, 0.0f},
     {0.0f, 0.62f, 0.72f, 0.08f}, {1.0f, 0.42f, 0.55f, 0.09f}},
    // WARM - late sun: the gain toward amber, a soft curve
    {{0.015f, 0.008f, 0.0f, 1.05f}, {1.0f, 1.0f, 1.03f, 0.10f}, {1.06f, 1.0f, 0.90f, 0.0f},
     {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.78f, 0.5f, 0.05f}},
    // NOIR - almost no colour, deeper blacks, more contrast, a cold shadow
    {{-0.02f, -0.02f, -0.02f, 0.10f}, {1.06f, 1.06f, 1.06f, 0.40f}, {1.06f, 1.06f, 1.06f, 0.0f},
     {0.35f, 0.5f, 0.75f, 0.04f}, {0.0f, 0.0f, 0.0f, 0.0f}},
    // FILMIC - a neutral S-curve, the highlights a little desaturated
    {{0.008f, 0.008f, 0.008f, 0.97f}, {1.0f, 1.0f, 1.0f, 0.20f}, {1.0f, 1.0f, 1.0f, 0.45f},
     {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}},
};
inline constexpr const char* kGradeNames[5] = {"off", "cyber", "warm", "noir", "filmic"};
// the bloom's strength and threshold, in the expanded linear light
inline constexpr float kBloomThreshold = 1.5f;   // ~ 247 of 255: only what nears white
inline constexpr float kBloomKnee = 0.3f;
inline constexpr float kBloomIntensity = 0.25f;  // of the levels' weighted sum
// each wider level weighs this much of the one inside it: a tight glow round
// a lamp, not a wash over the whole street (equal weights fogged the walls)
inline constexpr float kBloomFalloff = 0.6f;

// DEPTH OF FIELD'S FOCUS, per frame: the picture's rectangle, whether the
// frame is CINEMATIC, the camera's frame, the focus depth of the View's named
// subjects (`View::focusAt`) and of the nearest posed body (`note`, per draw),
// and the smoothed focus the final pass is handed.
//
// DEPTH OF FIELD FOCUSES ON A CHARACTER: the posed body nearest the camera
// among those near the picture's middle, as a view depth. The camera's own
// target is often a point in the air, and the middle of the picture is often
// the wall behind two people talking.
struct Focus {
    int vw = 0, vh = 0;            // the picture's rectangle, display pixels
    bool cine = false;             // the frame is cinematic: a full-width letterbox strip
    float focus = 0.0f;            // the camera's eye-to-target distance
    bool subjStencil = false;      // this frame marks translucent bodies in the stencil
    float eye[3] = {}, fwd[3] = {}, right[3] = {}, tan = 1.0f;
    float bodyZ = 0.0f, bodyScore = 1e30f;
    float sm = 0.0f;               // the smoothed focus depth, 0 = none yet
    float subjZ = 0.0f;            // the depth of the View's named subject, 0 = none in view

    // a frame begins: `w` x `h` the display; `dof` the request
    void begin(const View& view, int w, int h, bool dof) {
        // THE POST CHAIN's view: the picture's rectangle, whether the frame is
        // CINEMATIC - a full-width strip shorter than the frame, the letterbox of
        // conversations and cutscenes (`renderer.h`, THE LETTERBOX; a free walk
        // is a strip of the frame's own height) - and the focus distance
        vw = view.letterboxed() ? std::min(view.vw, w) : w;
        vh = view.letterboxed() ? std::min(view.vh, h) : h;
        static const bool forceCine = omk::envFlag("OMK_FORCE_CINE");   // a test switch: depth of field on any frame
        cine = forceCine || (vw >= w && vh < h);
        const float dx = view.cam.at[0] - view.cam.eye[0], dy = view.cam.at[1] - view.cam.eye[1],
                    dz = view.cam.at[2] - view.cam.eye[2];
        focus = std::sqrt(dx * dx + dy * dy + dz * dz);
        subjStencil = dof && cine && focus > 1.0f;
        // the frame's camera frame for `note` (the world's Y is DOWN)
        for (int k = 0; k < 3; ++k) eye[k] = view.cam.eye[k];
        const float il = focus > 1e-3f ? 1.0f / focus : 0.0f;
        fwd[0] = dx * il; fwd[1] = dy * il; fwd[2] = dz * il;
        float rx = fwd[1] * 0.0f - fwd[2] * -1.0f, ry = fwd[2] * 0.0f - fwd[0] * 0.0f,
              rz = fwd[0] * -1.0f - fwd[1] * 0.0f;
        const float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
        right[0] = rl > 1e-4f ? rx / rl : 1.0f; right[1] = rl > 1e-4f ? ry / rl : 0.0f;
        right[2] = rl > 1e-4f ? rz / rl : 0.0f;
        tan = std::tan(0.5f * view.cam.hfovDeg * 3.14159265f / 180.0f);
        bodyZ = 0.0f; bodyScore = 1e30f;
        // THE VIEW'S NAMED SUBJECTS (a conversation's heads) as depths along the
        // camera's axis - the same planar view depth the shader reads. Those
        // inside the picture count; with both in it and close in depth the
        // focus goes between them, else the first (the speaker), else the other.
        subjZ = 0.0f;
        const float uxv = fwd[1] * right[2] - fwd[2] * right[1],
                    uyv = fwd[2] * right[0] - fwd[0] * right[2],
                    uzv = fwd[0] * right[1] - fwd[1] * right[0];
        const float tanV = tan * static_cast<float>(vh) / static_cast<float>(std::max(1, vw));
        float zs[2] = {0, 0};
        bool vis[2] = {false, false};
        for (int i = 0; i < std::min(view.focusN, 2); ++i) {
            const float cx = view.focusAt[i][0] - eye[0], cy = view.focusAt[i][1] - eye[1],
                        cz = view.focusAt[i][2] - eye[2];
            zs[i] = cx * fwd[0] + cy * fwd[1] + cz * fwd[2];
            if (zs[i] < 8.0f || zs[i] > 4000.0f) continue;
            const float x = (cx * right[0] + cy * right[1] + cz * right[2]) / (zs[i] * tan);
            const float y = (cx * uxv + cy * uyv + cz * uzv) / (zs[i] * tanV);
            vis[i] = std::fabs(x) < 0.95f && std::fabs(y) < 0.95f;
        }
        if (vis[0] && vis[1] && std::max(zs[0], zs[1]) < 1.4f * std::min(zs[0], zs[1]))
            subjZ = 0.5f * (zs[0] + zs[1]);
        else if (vis[0]) subjZ = zs[0];
        else if (vis[1]) subjZ = zs[1];
    }

    // a draw of the frame: a posed body is a candidate for the focus
    void note(const Draw& d) {
        float c[3] = {0, 0, 0};
        if (d.meshPose && d.meshPoses) {
            for (std::size_t m = 0; m < d.meshPoses; ++m)
                for (int k = 0; k < 3; ++k) c[k] += d.meshPose[12 * m + 4 * k + 3];
            for (int k = 0; k < 3; ++k) c[k] /= static_cast<float>(d.meshPoses);
        } else if (d.castsShadow && d.geo && d.count && d.start + d.count <= d.geo->corners.size()) {
            // a body posed on the CPU (the player's, when drawn that way): its corners
            const std::size_t step = std::max<std::size_t>(1, d.count / 64);
            std::size_t n = 0;
            for (std::size_t i = d.start; i < d.start + d.count; i += step, ++n) {
                const auto& v = d.geo->corners[i];
                c[0] += v.x; c[1] += v.y; c[2] += v.z;
            }
            for (int k = 0; k < 3; ++k) c[k] /= static_cast<float>(n);
        } else {
            return;
        }
        for (int k = 0; k < 3; ++k) c[k] -= eye[k];
        const float z = c[0] * fwd[0] + c[1] * fwd[1] + c[2] * fwd[2];
        if (z < 8.0f || z > 4000.0f) return;   // a close-up's face is a few tens of units from the lens
        const float x = (c[0] * right[0] + c[1] * right[1] + c[2] * right[2]) / (z * tan);
        if (std::fabs(x) > 0.85f) return;
        const float score = z * (1.0f + 2.0f * std::fabs(x));
        if (score < bodyScore) { bodyScore = score; bodyZ = z; }
    }

    // does the final pass run depth of field on this frame
    bool dofLive(bool dofRequested) const { return dofRequested && cine && focus > 1.0f; }

    // `pp.dof` for the final pass, the focus smoothed across frames
    void dofParams(PostPush& pp, int h) {
        // the largest blur ~1.2% of the frame's height (9 px at 720p, 26 at
        // 2160p), the spiral's step chosen for ~48 taps whatever the size
        const float maxR = std::max(3.0f, 0.012f * static_cast<float>(h));
        // a character in the picture's middle third: its depth, smoothed (a
        // negative `dof[0]` tells the shader this focus is EXPLICIT); else the
        // camera's own, as before
        // (the frontend's named subject first, else the nearest body)
        float fx = focus;
        const float want = subjZ > 0.0f ? subjZ : bodyZ;
        if (want > 0.0f) {
            const float r = sm > 0.0f ? want / sm : 1.0f;
            sm = (sm <= 0.0f || r > 1.6f || r < 0.6f) ? want : sm + 0.3f * (want - sm);
            fx = -sm;
        } else {
            sm = 0.0f;
        }
        // the in-focus band: a quarter of the focus depth, but never thinner than
        // ~35 cm (14 units) - a close-up's nose and ears are not a blur apart
        pp.dof[0] = fx; pp.dof[1] = maxR;
        pp.dof[2] = fx < 0.0f ? std::max(0.25f, 14.0f / std::fabs(fx)) : 0.25f;
        static const bool dofLog = omk::envFlag("OMK_DOF_LOG");
        if (dofLog)
            std::printf("dof: focus %.1f (%s), camera target %.1f, nearest centred body %.1f, named subject %.1f, max radius %.1f px\n",
                        std::fabs(fx), fx < 0.0f ? (subjZ > 0.0f ? "a speaker" : "a character") : "the picture centre",
                        focus, bodyZ, subjZ, maxR);
        pp.dof[3] = std::max(0.5f, maxR * maxR / 96.0f);
    }
};

// the final pass's whole push block: `dof` says depth of field runs this
// frame; the display is `w` x `h`, the scene target `rw` x `rh` (what the
// depth image is), `ss` the supersample the shader folds; `hdrPeak` is the HDR
// display's peak in nits, 0 for SDR
inline PostPush buildPost(const Renderer::PostProcess& req, Focus& foc, bool dof, int w, int h, int rw, int rh,
                          int ss, float hdrPeak) {
    PostPush pp{};
    pp.frame[0] = 1.0f / static_cast<float>(w); pp.frame[1] = 1.0f / static_cast<float>(h);
    pp.frame[2] = static_cast<float>(foc.vw) / static_cast<float>(w);
    pp.frame[3] = static_cast<float>(foc.vh) / static_cast<float>(h);
    const float nearP = omk::kNearCut, farP = 200000.0f;
    pp.depthAB[0] = static_cast<float>(rw); pp.depthAB[1] = static_cast<float>(rh);
    pp.depthAB[2] = farP / (farP - nearP); pp.depthAB[3] = -farP * nearP / (farP - nearP);
    if (dof) foc.dofParams(pp, h);
    pp.tone[0] = req.bloom ? kBloomIntensity : 0.0f;
    pp.tone[1] = hdrPeak > 0.0f ? std::max(1.0f, std::min(hdrPeak, 1000.0f) / kPaperWhite) : 1.0f;
    pp.tone[2] = req.grade > 0 ? 1.0f : 0.0f;
    pp.tone[3] = static_cast<float>(ss);
    const Grade& g = kGrades[req.grade];
    std::memcpy(pp.lift, g.lift, sizeof pp.lift);
    std::memcpy(pp.gam, g.gam, sizeof pp.gam);
    std::memcpy(pp.gain, g.gain, sizeof pp.gain);
    std::memcpy(pp.tintS, g.tintS, sizeof pp.tintS);
    std::memcpy(pp.tintH, g.tintH, sizeof pp.tintH);
    if (req.grade == 0) { pp.gam[0] = pp.gam[1] = pp.gam[2] = 1.0f; }
    return pp;
}

// the bloom chain's push blocks, level `i` down (`i` = 0 is the bright pass
// from the frame) or up; `inW`/`inH` the input's size as the pass is told it,
// `uvMax` the picture's rectangle
inline BloomPush bloomDown(int i, float inW, float inH, int tw, int th, const float uvMax[2]) {
    return BloomPush{{1.0f / inW, 1.0f / inH, uvMax[0], uvMax[1]},
                     {kBloomThreshold, kBloomKnee, i == 0 ? 0.0f : 1.0f, 1.0f},
                     {1.0f / static_cast<float>(tw), 1.0f / static_cast<float>(th), 0, 0}};
}
inline BloomPush bloomUp(int inW, int inH, int tw, int th, const float uvMax[2]) {
    return BloomPush{{1.0f / static_cast<float>(inW), 1.0f / static_cast<float>(inH), uvMax[0], uvMax[1]},
                     {0.0f, 0.0f, 2.0f, kBloomFalloff},
                     {1.0f / static_cast<float>(tw), 1.0f / static_cast<float>(th), 0, 0}};
}

}  // namespace omk::postfx
