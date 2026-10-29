// SPDX-License-Identifier: GPL-3.0-or-later
// HYBRID RAY TRACING's scene, shared by the two live backends (`rtscene.h`).
#include "o3de/rtscene.h"

#include "o3de/drawcore.h"
#include "platform/env.h"
#include "platform/threads.h"

#include <cstdio>
#include <cstring>

namespace omk::rt {

void fillFrame(const float sky[3], const float eye[3], const float skyMean[3], bool reflections, Frame& f) {
    // `OMK_RT_NO_INDIRECT=1`: the indirect term off - the control `verify.py:
    // engine: raytracing` and `engine: character shadows` show their checks with
    static const bool noIndirect = omk::envFlag("OMK_RT_NO_INDIRECT");
    f = Frame{};
    for (int k = 0; k < 3; ++k) { f.sky[k] = sky[k]; f.eye[k] = eye[k]; }
    // the indirect term's strength: 1, a Lambertian's - the texel times the
    // cosine-weighted mean of the radiance arriving, which is what the baked
    // colour times its texel IS where the ray lands
    f.sky[3] = noIndirect ? 0.0f : 1.0f;
    f.eye[3] = 0.0f;                         // (unused: a character takes no near occlusion)
    f.params[0] = 1500.0f;                   // indirect reach: 38 m, the street fogs beyond
    f.params[1] = 400.0f;                    // occlusion reach: 10 m - an arch overhead
    f.params[2] = 0.0f;                      // (unused: a character takes no lamp shadow ray)
    f.params[3] = -1.0f;
    // `OMK_RT_TUNE="indirect,occlusion,reach,occlusion reach,lamp radius,margin"`
    // replaces the six numbers above, for judging them by eye
    static float tune[6] = {-1, -1, -1, -1, -1, -1};
    static const bool tuned = [] {
        const char* t = omk::envStr("OMK_RT_TUNE");
        return t && std::sscanf(t, "%f,%f,%f,%f,%f,%f", &tune[0], &tune[1], &tune[2], &tune[3], &tune[4],
                                &tune[5]) == 6;
    }();
    if (tuned) {
        f.sky[3] = tune[0]; f.eye[3] = tune[1];
        for (int k = 0; k < 4; ++k) f.params[k] = tune[2 + k];
    }
    // RAY-TRACED REFLECTIONS (`todo/enhancements.md` 26): x the strength (0
    // off), y F0 (Schlick), z the ray's reach, w a puddle's roughness (the
    // cone's half-width); refl2 x the share of the ground wet, y the boost a
    // bright (neon, lamp) texel takes, z a damp patch's roughness, w the
    // strength a damp (not puddled) patch keeps
    if (reflections) {
        // the sky's mean colour (`View::skyMean`), w 1 when there is one
        if (skyMean[0] >= 0.0f) {
            for (int k = 0; k < 3; ++k) f.skyUp[k] = skyMean[k];
            f.skyUp[3] = 1.0f;
        }
        f.refl[0] = 1.0f; f.refl[1] = 0.06f; f.refl[2] = 2400.0f; f.refl[3] = 0.015f;
        f.refl2[0] = 0.45f; f.refl2[1] = 1.6f; f.refl2[2] = 0.08f; f.refl2[3] = 0.3f;
        // `OMK_RT_REFL_TUNE="strength,F0,reach,roughness,wet share,boost"`, by eye
        static float rt[6] = {-1, -1, -1, -1, -1, -1};
        static const bool rtuned = [] {
            const char* t = omk::envStr("OMK_RT_REFL_TUNE");
            return t && std::sscanf(t, "%f,%f,%f,%f,%f,%f", &rt[0], &rt[1], &rt[2], &rt[3], &rt[4], &rt[5]) == 6;
        }();
        if (rtuned) {
            for (int k = 0; k < 4; ++k) f.refl[k] = rt[k];
            f.refl2[0] = rt[4]; f.refl2[1] = rt[5];
        }
    }
}

bool isBodyDraw(const Draw& d) {
    return d.meshPose != nullptr || (d.lit & ~(kDrawSoft | kDrawFloor | kDrawTintMask)) != 0;
}

void setCorners(const Geometry& g, std::vector<std::uint32_t>& all, std::uint32_t& nOpaque, std::uint32_t& nCut) {
    std::vector<std::uint32_t> opq, cut;
    for (const auto& bt : g.batches) {
        if (bt.blend != Blend::Opaque) continue;
        for (std::size_t c = bt.start; c + 3 <= bt.start + bt.count && c + 3 <= g.corners.size(); c += 3)
            (bt.cutout ? cut : opq).push_back(static_cast<std::uint32_t>(c));
    }
    nOpaque = static_cast<std::uint32_t>(opq.size());
    nCut = static_cast<std::uint32_t>(cut.size());
    opq.insert(opq.end(), cut.begin(), cut.end());
    all = std::move(opq);
}

void gatherBodies(const BodyNotes& notes, const float eye[3], std::vector<Tri>& tri, std::uint32_t& nOpaque,
                  std::uint32_t& nCut, int& bodies, std::uint64_t& sig) {
    std::vector<Tri> opq, cut;
    bodies = 0;
    sig = 1469598103934665603ull;   // FNV-1a over the runs taken
    const auto mix = [&](std::uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    for (const auto& [key, n] : notes) {
        // a body further than `kBodyReach` from the eye is left out: its AO has
        // faded (`ssao.frag`, 3500..8000 of view depth) and a character's rays
        // see the set alone
        const Corner& c0 = n.geo->corners[n.runs.begin()->first];
        const float at[3] = {n.aff.empty() ? c0.x : n.aff[3], n.aff.empty() ? c0.y : n.aff[7],
                             n.aff.empty() ? c0.z : n.aff[11]};
        const float dx = at[0] - eye[0], dy = at[1] - eye[1], dz = at[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz > kBodyReach * kBodyReach) continue;
        const std::size_t was = opq.size() + cut.size();
        const std::uint32_t poses = static_cast<std::uint32_t>(n.aff.size() / 12);
        for (const auto& [start, r] : n.runs) {
            mix(reinterpret_cast<std::uintptr_t>(n.geo));
            mix((static_cast<std::uint64_t>(start) << 33) ^ (static_cast<std::uint64_t>(r.count) << 1) ^ r.cutout);
            for (std::size_t c = start; c + 3 <= start + r.count && c + 3 <= n.geo->corners.size(); c += 3)
                (r.cutout ? cut : opq).push_back(Tri{n.geo, n.aff.empty() ? nullptr : n.aff.data(), poses,
                                                     static_cast<std::uint32_t>(c), r.tex});
        }
        bodies += opq.size() + cut.size() > was;
    }
    nOpaque = static_cast<std::uint32_t>(opq.size());
    nCut = static_cast<std::uint32_t>(cut.size());
    tri = std::move(opq);
    tri.insert(tri.end(), cut.begin(), cut.end());
}

void fillRecords(const std::vector<Tri>& tri, float* P, std::uint32_t* R) {
    const std::size_t n = tri.size();
    omk::Threads::shared().parallelFor(0, n, 2048, [&](std::size_t from, std::size_t to) {
        for (std::size_t t = from; t < to; ++t) {
            const Tri& q = tri[t];
            const Geometry& g = *q.g;
            const bool skin = q.aff && g.cornerMesh.size() == g.corners.size();
            for (std::size_t k = 0; k < 3; ++k) {
                const std::size_t c = q.first + k;
                const Corner& v = g.corners[c];
                float x = v.x, y = v.y, z = v.z;
                if (skin) {
                    const std::int32_t m = g.cornerMesh[c];
                    if (m >= 0 && static_cast<std::uint32_t>(m) < q.poses) {
                        const float* a = q.aff + 12 * m;
                        const float px = x, py = y, pz = z;
                        x = a[0] * px + a[1] * py + a[2] * pz + a[3];
                        y = a[4] * px + a[5] * py + a[6] * pz + a[7];
                        z = a[8] * px + a[9] * py + a[10] * pz + a[11];
                    }
                }
                P[9 * t + 3 * k] = x; P[9 * t + 3 * k + 1] = y; P[9 * t + 3 * k + 2] = z;
                const float rec[5] = {v.u, v.v, v.r, v.g, v.b};
                std::memcpy(R + 16 * t + 5 * k, rec, sizeof rec);
            }
            R[16 * t + 15] = q.tex;
        }
    });
}

}  // namespace omk::rt
