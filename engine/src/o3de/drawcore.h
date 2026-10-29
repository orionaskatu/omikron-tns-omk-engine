// SPDX-License-Identifier: GPL-3.0-or-later
// WHAT EVERY GPU BACKEND COMPUTES THE SAME WAY (`todo/code-readability-audit.md`
// §6).
//
// Four renderers stand behind `renderer.h` beside the software reference:
// Vulkan, GLES (the Vita's), the fixed-function GL 1.x (the classic Mac's) and
// citro3d (the 3DS's). Each carried its own copy of rules that are the ENGINE's
// and not the API's, and the copies had already drifted in detail - which
// switch turned the depth tie off, whether a light's colour was read as a byte
// or as a float:
//
//   * how a posed body's corner is placed and turned on the CPU - one affine a
//     mesh, `Draw::meshPose`, as `meshAffines` makes them;
//   * how the lights that reach it colour it - `applyLights`' law, the ramp
//     and the saturating add, over `Draw::vertexLights`;
//   * which of two coincident faces loses the DEPTH TIE when it is settled
//     once (`renderer.h` `bakeDepthTie`), and the two switches that turn it off;
//   * what each blend mode does to the target's colour, and whether it writes
//     depth.
//
// Here they are once. A backend keeps only how it hands the answer to its API:
// a buffer layout, a pipeline, a `glBlendFunc`. `verify.py: engine: draw core`
// holds the pose and the light against `applyPose` and `applyLights`, and the
// four backend files against carrying a copy again.
#pragma once

#include "o3de/geom3do.h"
#include "o3de/renderer.h"
#include "o3de/vertexlight.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omk {

// ---- the pose: one affine a mesh, 12 floats, row r = R[r][0..2], t[r]

inline void posePoint(const float* a, float x, float y, float z, float out[3]) {
    out[0] = a[0] * x + a[1] * y + a[2] * z + a[3];
    out[1] = a[4] * x + a[5] * y + a[6] * z + a[7];
    out[2] = a[8] * x + a[9] * y + a[10] * z + a[11];
}

// A normal turns with the mesh and does not move with it.
inline void poseNormal(const float* a, float x, float y, float z, float out[3]) {
    out[0] = a[0] * x + a[1] * y + a[2] * z;
    out[1] = a[4] * x + a[5] * y + a[6] * z;
    out[2] = a[8] * x + a[9] * y + a[10] * z;
}

// The affine corner `i` of `d.geo` is posed by, or null when its mesh has none
// (a mesh the pose does not cover stays at rest, as `applyPose` leaves it).
inline const float* cornerAffine(const Draw& d, std::size_t i) {
    const std::int32_t m = d.geo->cornerMesh[i];
    if (m < 0 || static_cast<std::size_t>(m) >= d.meshPoses) return nullptr;
    return d.meshPose + 12 * static_cast<std::size_t>(m);
}

// ---- the vertex lights

// One light of `Draw::vertexLights` (`lightReach`'s 8 floats: the direction
// scaled by the strength, then the colour's three bytes), read once a draw.
struct CornerLight {
    float lx, ly, lz;
    std::uint8_t r, g, b;
};

// `d`'s lights into `out`, at most `most` of them (the backend's own cap: the
// lights its GPU path can take, which its CPU path keeps to so the two agree).
// -> how many.
inline int prepareLights(const Draw& d, int most, CornerLight* out) {
    const int n = d.vertexLights ? (d.vertexLightCount < most ? d.vertexLightCount : most) : 0;
    for (int i = 0; i < n; ++i) {
        const float* v = d.vertexLights + 8 * i;
        out[i] = {v[0], v[1], v[2], static_cast<std::uint8_t>(v[4]),
                  static_cast<std::uint8_t>(v[5]), static_cast<std::uint8_t>(v[6])};
    }
    return n;
}

// Whether a draw's corners are coloured here at all: it has lights, or its
// corners start from the scene's grey rather than their baked colour.
inline bool relit(const Draw& d, int lights) { return lights > 0 || d.lightsFromBlack; }

// The colour a corner's lighting starts from: the scene's grey for a body that
// starts black (`Draw::lightBase`), else the corner's own.
inline void lightStart(const Draw& d, const Corner& c, float rgb[3]) {
    rgb[0] = d.lightsFromBlack ? d.lightBase : c.r;
    rgb[1] = d.lightsFromBlack ? d.lightBase : c.g;
    rgb[2] = d.lightsFromBlack ? d.lightBase : c.b;
}

// `applyLights`' per-vertex half (`sub_493E40`): for each light in turn,
// `t = -(N . L)` truncated and clamped to the ramp's 0..255, the ramp
// `(t * c) >> 8` of each colour byte added and clamped to 1. `n` is the POSED
// normal.
inline void lightCorner(const float n[3], const CornerLight* L, int count, float rgb[3]) {
    float cr = rgb[0], cg = rgb[1], cb = rgb[2];
    for (int k = 0; k < count; ++k) {
        int ti = static_cast<int>(-(n[0] * L[k].lx + n[1] * L[k].ly + n[2] * L[k].lz));
        ti = ti < 0 ? 0 : (ti > 255 ? 255 : ti);
        const float ar = static_cast<float>(lightRamp(L[k].r, ti)) / 255.0f;
        const float ag = static_cast<float>(lightRamp(L[k].g, ti)) / 255.0f;
        const float ab = static_cast<float>(lightRamp(L[k].b, ti)) / 255.0f;
        cr = cr + ar > 1.0f ? 1.0f : cr + ar;
        cg = cg + ag > 1.0f ? 1.0f : cg + ag;
        cb = cb + ab > 1.0f ? 1.0f : cb + ab;
    }
    rgb[0] = cr; rgb[1] = cg; rgb[2] = cb;
}

// ---- a whole body posed on the CPU, into a geometry the backend then draws
// unposed: the fallback of a renderer whose GPU path cannot take this body

struct PoseParts {
    bool rest = true;      // copy every field of the rest corner first (else only
                           // what is posed is written: the rest was copied once)
    bool normals = true;   // turn the normals (a renderer lighting per pixel, or here)
    int  lights = 0;       // the most lights to apply here; 0 leaves the colour
};

// `out` is `d.geo` posed by `d.meshPose` - copied whole first when its size
// differs - and lit by `d.vertexLights` when `parts.lights` allows.
void poseGeometry(const Draw& d, Geometry& out, const PoseParts& parts);

// ---- the depth tie, settled once

// `OMK_NO_TIE=1`: the coincident faces left to fight, for a before/after.
bool tieOff();
// ...or `OMK_NO_TIE_BAKE=1`: settled per draw, never baked once.
bool tieBakeOff();

// The triangles of `g` that lose a depth tie across the draws of `order` that
// use it, in that order (`DepthTie::resolve`, `o3de/depthtie.h`): each once,
// ascending.
std::vector<std::size_t> bakeTieLosers(const Geometry& g, std::span<const Draw> order);

// ---- the blend modes (`Draw::blend`, ASSETS 4)

enum class BlendFactor { Zero, One, OneMinusSrcColor };

// What a mode does to the target's COLOUR - the engine's two transparent modes
// and opaque - and whether it writes depth. The alpha channel's factors are
// each backend's own: they differ (GL applies this pair to it, Vulkan and the
// PICA keep their own) and nothing on screen is made of the target's alpha.
struct BlendRule {
    bool blend;              // false: the target is overwritten
    BlendFactor src, dst;
    bool depthWrite;
};

constexpr BlendRule blendRule(Blend b) {
    switch (b) {
    case Blend::Add: return {true, BlendFactor::One, BlendFactor::One, false};               // src + dst, saturating
    case Blend::Mul: return {true, BlendFactor::Zero, BlendFactor::OneMinusSrcColor, false};  // dst * (1 - src)
    case Blend::Opaque: break;
    }
    return {false, BlendFactor::One, BlendFactor::Zero, true};
}

}  // namespace omk
