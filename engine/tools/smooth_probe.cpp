// SPDX-License-Identifier: GPL-3.0-or-later
// THE ROUNDED BODIES over every shipped character (`o3de/smoothbody.h`, the
// remaster's D1) - what `verify.py: engine: smooth bodies` reads.
//
//     smooth_probe <gamedata> [level alpha]
//
// For each `MESHES/PERSOS/*.3DO`, its rest geometry before and after
// `smoothBody`, and four things the shipped models could break:
//   * KEPT: every original corner (`cornerVertex` >= 0) comes out bit for bit,
//     as a multiset;
//   * WATERTIGHT: the output's OPEN edges (one triangle, keyed by mesh and
//     the two ends' bits) never MORE than the input predicts - an input edge
//     with `s` split and `k` kept users leaves `level` open sub-edges when s
//     is 1 and one when k is 1, nothing else. A crack (two triangles moving
//     a shared edge's points differently, even by an ulp) opens 2 x level
//     more; fewer is a sub-edge landing on another edge exactly (`closed`);
//   * the FACE: no new corner (`cornerVertex` -1) on the face's mesh;
//   * BOUNDED: no new point further from its flat triangle than half the
//     triangle's longest edge;
//   * NOT FOLDED (`flipped`): no new triangle facing against the one it came
//     from - culled from behind, it is a hole (the reader's "the arm becomes
//     empty", 2026-10-22: 453 of them at 0.75 before the damping and guard).
// The split rule (one mesh, not the face, not degenerate) is re-derived here.
#include "actor/pose.h"
#include "formats/mesh3do.h"
#include "o3de/geom3do.h"
#include "o3de/smoothbody.h"
#include "platform/datafs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

std::uint32_t bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
using P = std::tuple<std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
P posOf(const omk::Geometry& g, std::size_t i) {
    return {g.cornerMesh[i], bits(g.corners[i].x), bits(g.corners[i].y), bits(g.corners[i].z)};
}
using E = std::pair<P, P>;
E edgeOf(const omk::Geometry& g, std::size_t a, std::size_t b) {
    P pa = posOf(g, a), pb = posOf(g, b);
    return pa < pb ? E{pa, pb} : E{pb, pa};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: smooth_probe <gamedata> [level alpha]\n"); return 2; }
    const int level = argc > 2 ? std::atoi(argv[2]) : 3;
    const float alpha = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 0.75f;
    omk::DataFs fs(argv[1]);
    long models = 0, keptBad = 0, openBad = 0, faceBad = 0, farBad = 0;
    long tris = 0, split = 0, seam = 0, face = 0, openOut = 0, openWant = 0, closed = 0, flipped = 0, unpulled = 0;
    for (const auto& path : fs.list("MESHES/PERSOS", ".3DO")) {
        const auto md = omk::DataFs::readPath(path);
        omk::Geometry in = omk::buildGeometry(md, omk::DrawFilter::Engine);
        if (in.corners.empty() || in.cornerMesh.size() != in.corners.size()) continue;
        omk::FaceMesh fm;
        if (const auto mh = omk::readHeader(md)) fm = omk::faceMeshOf(omk::readMeshes(md, *mh));
        const std::size_t f0 = fm.valid() ? fm.base : 0, f1 = fm.valid() ? fm.base + static_cast<std::size_t>(fm.count) : 0;
        omk::Geometry out = in;
        const auto st = omk::smoothBody(out, f0, f1, level, alpha);
        ++models;
        unpulled += static_cast<long>(st.unpulled);
        tris += static_cast<long>(st.triangles); split += static_cast<long>(st.split);
        seam += static_cast<long>(st.keptSeam); face += static_cast<long>(st.keptFace);

        // KEPT
        using K = std::tuple<std::int32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
        std::vector<K> a, b;
        const auto collect = [](const omk::Geometry& g, std::vector<K>& v) {
            for (std::size_t i = 0; i < g.corners.size(); ++i)
                if (g.cornerVertex[i] >= 0)
                    v.emplace_back(g.cornerVertex[i], bits(g.corners[i].x), bits(g.corners[i].y),
                                   bits(g.corners[i].z), bits(g.corners[i].u), bits(g.corners[i].v));
            std::sort(v.begin(), v.end());
        };
        collect(in, a);
        collect(out, b);
        if (a != b) ++keptBad;

        // WATERTIGHT: the input's edges, by how many split and kept triangles use them
        std::map<E, std::pair<int, int>> uses;
        for (std::size_t t = 0; t + 2 < in.corners.size(); t += 3) {
            const std::int32_t m = in.cornerMesh[t];
            bool sp = in.cornerMesh[t + 1] == m && in.cornerMesh[t + 2] == m;
            for (int k = 0; k < 3 && sp; ++k) {
                const std::int32_t v = in.cornerVertex[t + k];
                if (v >= 0 && static_cast<std::size_t>(v) >= f0 && static_cast<std::size_t>(v) < f1) sp = false;
            }
            if (sp) {
                const auto& c0 = in.corners[t]; const auto& c1 = in.corners[t + 1]; const auto& c2 = in.corners[t + 2];
                const double ex = c1.x - c0.x, ey = c1.y - c0.y, ez = c1.z - c0.z;
                const double fx = c2.x - c0.x, fy = c2.y - c0.y, fz = c2.z - c0.z;
                const double cx = ey * fz - ez * fy, cy = ez * fx - ex * fz, cz = ex * fy - ey * fx;
                sp = cx * cx + cy * cy + cz * cz >= 1e-8;
            }
            for (int k = 0; k < 3; ++k) {
                auto& u = uses[edgeOf(in, t + k, t + (k + 1) % 3)];
                (sp ? u.first : u.second) += 1;
            }
        }
        long want = 0;
        for (const auto& [e, u] : uses) want += (u.first == 1 ? level : 0) + (u.second == 1 ? 1 : 0);
        std::map<E, int> outUses;
        for (std::size_t t = 0; t + 2 < out.corners.size(); t += 3)
            for (int k = 0; k < 3; ++k) ++outUses[edgeOf(out, t + k, t + (k + 1) % 3)];
        long open = 0;
        for (const auto& [e, n] : outUses) open += n == 1 ? 1 : 0;
        openOut += open; openWant += want;
        // a crack can only ADD open edges; fewer is a boundary sub-edge landing
        // on another edge exactly (4 models, 10 edges, 2026-10-22) - counted
        closed += want > open ? want - open : 0;
        if (open > want) {
            ++openBad;
            if (std::getenv("SMOOTH_PROBE_SAY"))
                std::printf("open %s: %ld, predicted %ld\n", path.c_str(), open, want);
        }

        // FLIPPED: a new triangle facing against the one it came from - a fold,
        // culled from behind (the reader's "the arm becomes empty", 2026-10-22).
        // Output triangles come in input order, `level`^2 for a split one.
        {
            std::size_t o = 0;
            const auto nrm = [](const omk::Corner& a, const omk::Corner& b, const omk::Corner& c, double* n) {
                const double ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
                const double fx = c.x - a.x, fy = c.y - a.y, fz = c.z - a.z;
                n[0] = ey * fz - ez * fy; n[1] = ez * fx - ex * fz; n[2] = ex * fy - ey * fx;
            };
            for (const auto& bt : in.batches) (void)bt;
            for (std::size_t bi = 0; bi < in.batches.size(); ++bi) {
                const auto& ib = in.batches[bi];
                o = out.batches[bi].start;
                for (std::size_t t = ib.start; t + 2 < ib.start + ib.count; t += 3) {
                    double n0[3];
                    nrm(in.corners[t], in.corners[t + 1], in.corners[t + 2], n0);
                    // split iff the output's next triangle is not this one, bit for bit
                    const bool same = std::memcmp(&in.corners[t], &out.corners[o], sizeof(omk::Corner)) == 0 &&
                                      std::memcmp(&in.corners[t + 1], &out.corners[o + 1], sizeof(omk::Corner)) == 0 &&
                                      std::memcmp(&in.corners[t + 2], &out.corners[o + 2], sizeof(omk::Corner)) == 0;
                    const std::size_t n = same ? 1 : static_cast<std::size_t>(level * level);
                    for (std::size_t k = 0; k < n; ++k, o += 3) {
                        double n1[3];
                        nrm(out.corners[o], out.corners[o + 1], out.corners[o + 2], n1);
                        if (n0[0] * n1[0] + n0[1] * n1[1] + n0[2] * n1[2] < 0) ++flipped;
                    }
                }
            }
        }
        // the FACE: no NEW corner on its mesh - the morph replaces its
        // vertices by `cornerVertex`, which a new corner does not have
        if (fm.valid())
            for (std::size_t i = 0; i < out.corners.size(); ++i)
                if (out.cornerVertex[i] < 0 && out.cornerMesh[i] == fm.mesh) { ++faceBad; break; }

        // BOUNDED: each new corner within half its source triangle's longest
        // edge of that triangle's plane - checked against every input
        // triangle of its mesh would be quadratic, so against the bounding
        // box of its mesh's corners grown by that margin
        std::map<std::int32_t, std::array<float, 7>> box;   // min xyz, max xyz, longest edge
        for (std::size_t t = 0; t + 2 < in.corners.size(); t += 3) {
            auto& bx = box.try_emplace(in.cornerMesh[t], std::array<float, 7>{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f, 0}).first->second;
            for (int k = 0; k < 3; ++k) {
                const auto& c = in.corners[t + k];
                const auto& d = in.corners[t + (k + 1) % 3];
                bx[0] = std::min(bx[0], c.x); bx[1] = std::min(bx[1], c.y); bx[2] = std::min(bx[2], c.z);
                bx[3] = std::max(bx[3], c.x); bx[4] = std::max(bx[4], c.y); bx[5] = std::max(bx[5], c.z);
                bx[6] = std::max(bx[6], std::sqrt((c.x - d.x) * (c.x - d.x) + (c.y - d.y) * (c.y - d.y) + (c.z - d.z) * (c.z - d.z)));
            }
        }
        for (std::size_t i = 0; i < out.corners.size(); ++i) {
            if (out.cornerVertex[i] >= 0) continue;
            const auto it = box.find(out.cornerMesh[i]);
            if (it == box.end()) { ++farBad; break; }
            const auto& bx = it->second;
            const float m = bx[6] * 0.5f;
            const auto& c = out.corners[i];
            if (c.x < bx[0] - m || c.y < bx[1] - m || c.z < bx[2] - m || c.x > bx[3] + m || c.y > bx[4] + m ||
                c.z > bx[5] + m || !std::isfinite(c.x)) { ++farBad; break; }
        }
    }
    std::printf("@smooth models %ld triangles %ld split %ld seam %ld face %ld open %ld want %ld "
                "closed %ld flipped %ld unpulled %ld kept_bad %ld open_bad %ld face_bad %ld far_bad %ld\n",
                models, tris, split, seam, face, openOut, openWant, closed, flipped, unpulled, keptBad, openBad, faceBad, farBad);
    return 0;
}
