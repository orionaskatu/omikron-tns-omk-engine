// SPDX-License-Identifier: GPL-3.0-or-later
// HYBRID RAY TRACING's scene, shared by the Direct3D 12 and the Vulkan backends
// (`todo/enhancements.md` 19, 26).
//
// The rays are one GLSL source (`backends/vulkan/shaders/rt.glsl`, and the
// -DOMK_RT arms of `scene.frag` and `ssao.frag`); this is the other half of "the
// same rays": what the frame's acceleration structures are made of and the
// constants the shaders are handed. Written once so the two pictures cannot
// drift apart for lack of a copied number. Each backend keeps only the part that
// is its API's - the buffers, the BLAS and TLAS builds, the bindless tables.
//
//   * the SET's triangles (`setCorners`): every opaque and cutout batch of a
//     static geometry, opaque first;
//   * the BODIES' (`gatherBodies`): the drawn opaque runs of the posed bodies
//     within reach, all of them for ONE BLAS, with the signature that says when
//     the same runs of the same bodies were taken again (a refit, not a rebuild);
//   * the 16-word RECORDS and the corner positions (`fillRecords`), posed by the
//     very affines `scene.vert` applies;
//   * `Frame` (`fillFrame`): the constants of `scene.frag`'s `RtFrame` block.
#pragma once

#include "o3de/renderer.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace omk::rt {

inline constexpr unsigned kMaxInst = 2048;        // instances in the TLAS
inline constexpr float kBodyReach = 4500.0f;      // a body further than this (115 m) from the eye is left out
inline constexpr std::uint32_t kMaskSet = 1, kMaskBody = 2;
inline constexpr long kSetKeep = 120;             // frames a set stays in the TLAS after it was last drawn
inline constexpr long kBodyRefitFrames = 30;      // a refit's tree loosens as the limbs travel: rebuilt this often

// `scene.frag`'s `RtFrame` uniform block (-DOMK_RT), field for field
// (`refl`, `refl2`, `skyUp`: RAY-TRACED REFLECTIONS', `todo/enhancements.md` 26 -
// after the three, so nothing above moves)
struct Frame { float sky[4]; float eye[4]; float params[4]; float refl[4]; float refl2[4]; float skyUp[4]; };

// The frame's constants. `sky`: what a ray leaving the set sees (the fog
// colour, else the clear colour); `skyMean`: `View::skyMean`, -1 when none;
// `reflections`: the ground's ray-traced reflections are on.
// `OMK_RT_NO_INDIRECT`, `OMK_RT_TUNE`, `OMK_RT_REFL_TUNE` act here, for both backends.
void fillFrame(const float sky[3], const float eye[3], const float skyMean[3], bool reflections, Frame& f);

// a triangle going into a BLAS: its geometry, first corner, the affines that
// pose it (none for a set) and its texture's word (the backend's table slot,
// and for a TEXTURE PACK's replacement k - 1 in bits 24.., `rt.glsl` `rtTexel`)
struct Tri { const Geometry* g; const float* aff; std::uint32_t poses, first, tex; };
struct Run { std::size_t count; bool cutout; std::uint32_t tex; };
struct BodyNote {
    const Geometry* geo = nullptr;
    std::vector<float> aff;                     // its affines, copied at submit
    std::map<std::size_t, Run> runs;            // its opaque draws, by start
};
using BodyNotes = std::map<std::pair<const float*, const Geometry*>, BodyNote>;

// -> true when the draw is a BODY's (posed, a caster, or lit per pixel as a body
// is): its run is noted for the frame's body BLAS instead of a set's
bool isBodyDraw(const Draw& d);

// A SET geometry's corners for its BLAS: every opaque and cutout batch (a
// blended one is a glow or a decal, not a wall), the opaque ones first - `all`
// holds each triangle's first corner, `nOpaque` of them opaque, then `nCut`
void setCorners(const Geometry& g, std::vector<std::uint32_t>& all, std::uint32_t& nOpaque, std::uint32_t& nCut);

// The frame's bodies, from the runs noted: opaque triangles first, then the
// cutout ones, in `tri`; how many bodies took part; and an FNV-1a signature
// over the runs taken. Bodies beyond `kBodyReach` of `eye` are left out.
void gatherBodies(const BodyNotes& notes, const float eye[3], std::vector<Tri>& tri,
                  std::uint32_t& nOpaque, std::uint32_t& nCut, int& bodies, std::uint64_t& sig);

// The triangles' corner positions (float3 each, 9 floats a triangle) and
// records (16 words: three corners of u, v, r, g, b - the texel coordinates and
// the BAKED vertex colour - and the texture word), on the shared threads: a
// frame's bodies are tens of thousands of triangles.
void fillRecords(const std::vector<Tri>& tri, float* pos, std::uint32_t* rec);

}  // namespace omk::rt
