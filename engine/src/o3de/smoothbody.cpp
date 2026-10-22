// SPDX-License-Identifier: GPL-3.0-or-later
// The rounded bodies - see smoothbody.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "o3de/smoothbody.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace omk {

namespace {

// a POSITION within a mesh: the mesh and the three floats' bits (corners of one
// vertex, or of two vertices at one place, are the same bits)
struct PosKey {
    std::int32_t mesh;
    std::uint32_t x, y, z;
    bool operator==(const PosKey& o) const { return mesh == o.mesh && x == o.x && y == o.y && z == o.z; }
};
struct PosHash {
    std::size_t operator()(const PosKey& k) const {
        std::uint64_t h = static_cast<std::uint32_t>(k.mesh) * 0x9E3779B97F4A7C15ull;
        h ^= k.x + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= k.y + 0x9E3779B9u + (h << 6) + (h >> 2);
        h ^= k.z + 0x9E3779B9u + (h << 6) + (h >> 2);
        return static_cast<std::size_t>(h);
    }
};

std::uint32_t bits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

struct Pos {
    double n[3] = {0, 0, 0};   // the corners' normals, summed
    bool pinned = false;
    float un[3] = {0, 0, 0};   // the unit average
};

}  // namespace

SmoothStats smoothBody(Geometry& g, std::size_t faceFrom, std::size_t faceTo, int level, float alpha) {
    SmoothStats st;
    const std::size_t nc = g.corners.size();
    st.triangles = nc / 3;
    if (level < 2 || nc < 3 || g.cornerMesh.size() != nc) return st;
    const bool hasVertex = g.cornerVertex.size() == nc;
    const auto at = [](const std::vector<std::uint8_t>& v, std::size_t i) -> std::uint8_t {
        return i < v.size() ? v[i] : 0;
    };

    // which triangles split: one mesh, not the face, not degenerate
    std::vector<std::uint8_t> split(nc / 3, 0);
    for (std::size_t t = 0; t + 2 < nc; t += 3) {
        const std::int32_t m = g.cornerMesh[t];
        if (g.cornerMesh[t + 1] != m || g.cornerMesh[t + 2] != m) { ++st.keptSeam; continue; }
        bool face = false;
        for (int k = 0; k < 3 && hasVertex; ++k) {
            const std::int32_t v = g.cornerVertex[t + k];
            face |= v >= 0 && static_cast<std::size_t>(v) >= faceFrom && static_cast<std::size_t>(v) < faceTo;
        }
        if (face) { ++st.keptFace; continue; }
        const Corner& a = g.corners[t];
        const Corner& b = g.corners[t + 1];
        const Corner& c = g.corners[t + 2];
        const double ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
        const double fx = c.x - a.x, fy = c.y - a.y, fz = c.z - a.z;
        const double cx = ey * fz - ez * fy, cy = ez * fx - ex * fz, cz = ex * fy - ey * fx;
        if (cx * cx + cy * cy + cz * cz < 1e-8) continue;
        split[t / 3] = 1;
        ++st.split;
    }

    // the positions, their normals, and what pins them
    std::unordered_map<PosKey, std::size_t, PosHash> idOf;
    std::vector<Pos> pos;
    std::vector<std::size_t> pid(nc);
    for (std::size_t i = 0; i < nc; ++i) {
        const Corner& c = g.corners[i];
        const PosKey k{g.cornerMesh[i], bits(c.x), bits(c.y), bits(c.z)};
        auto [it, fresh] = idOf.emplace(k, pos.size());
        if (fresh) pos.emplace_back();
        pid[i] = it->second;
        Pos& p = pos[it->second];
        p.n[0] += c.nx; p.n[1] += c.ny; p.n[2] += c.nz;
        if (!split[i / 3]) p.pinned = true;   // on a triangle kept whole
    }
    for (auto& p : pos) {
        const double l = std::sqrt(p.n[0] * p.n[0] + p.n[1] * p.n[1] + p.n[2] * p.n[2]);
        if (l < 1e-6) { p.pinned = true; continue; }
        for (int k = 0; k < 3; ++k) p.un[k] = static_cast<float>(p.n[k] / l);
    }
    // a CREASE: a corner's normal more than ~22.5 degrees off its position's
    // average - two of them part by more than ~45
    for (std::size_t i = 0; i < nc; ++i) {
        Pos& p = pos[pid[i]];
        if (p.pinned) continue;
        const Corner& c = g.corners[i];
        const double l = std::sqrt(double(c.nx) * c.nx + double(c.ny) * c.ny + double(c.nz) * c.nz);
        if (l < 1e-6 || (c.nx * p.un[0] + c.ny * p.un[1] + c.nz * p.un[2]) / l < 0.924) p.pinned = true;
    }
    // an OPEN BOUNDARY: an edge only one triangle of its mesh has
    {
        std::unordered_map<std::uint64_t, int> edges;
        const auto key = [](std::size_t a, std::size_t b) {
            if (a > b) std::swap(a, b);
            return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
        };
        for (std::size_t t = 0; t + 2 < nc; t += 3)
            for (int k = 0; k < 3; ++k) ++edges[key(pid[t + k], pid[t + (k + 1) % 3])];
        for (const auto& [e, n] : edges)
            if (n == 1) {
                pos[static_cast<std::size_t>(e >> 32)].pinned = true;
                pos[static_cast<std::size_t>(e & 0xFFFFFFFFu)].pinned = true;
            }
    }
    for (const auto& p : pos) st.pinned += p.pinned ? 1 : 0;
    st.positions = pos.size();

    // THE PULL, kept ONE PER EDGE, keyed by its two positions, so both
    // triangles either side of it use the same and the edge stays shut - and
    // so the guard below can take an edge's to 0 for both. On a thin limb of
    // few sides the normals part by 60-90 degrees and the full pull FOLDED
    // small triangles over: culled from behind, the arm "empty" as it turned
    // (the reader, 2026-10-22). Damping every edge by its ends' angle cured it
    // and flattened nearly every body back (most part by more than 45); the
    // guard alone flattens 211 of 44354 interiors, and cures it too.
    const auto edgeKey = [](std::size_t a, std::size_t b) {
        if (a > b) std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) | static_cast<std::uint64_t>(b);
    };
    std::unordered_map<std::uint64_t, float> pull;
    const auto pullOf = [&](std::size_t a, std::size_t b) {
        return pull.emplace(edgeKey(a, b), alpha).first->second;
    };
    const float N = static_cast<float>(level);
    const auto idx = [&](int i, int j) {   // row j starts after rows 0..j-1
        return static_cast<std::size_t>(j * (level + 1) - j * (j - 1) / 2 + i);
    };
    std::vector<Corner> grid;
    std::vector<std::int32_t> gridVertex;
    // triangle `t`'s grid (i along c0->c1, j along c0->c2, row by row), its
    // edges' pulls `ae` (opposite corner k), its interior's `aIn`
    const auto build = [&](std::size_t t, const float ae[3], float aIn) {
        const Corner* c[3] = {&g.corners[t], &g.corners[t + 1], &g.corners[t + 2]};
        const Pos* p[3] = {&pos[pid[t]], &pos[pid[t + 1]], &pos[pid[t + 2]]};
        grid.clear();
        gridVertex.clear();
        for (int j = 0; j <= level; ++j)
            for (int i = 0; i + j <= level; ++i) {
                // the weights as INTEGERS over `level`, and the corners summed
                // in one order (by position) - so the two triangles either side
                // of an edge get its points bit for bit, the only way a shared
                // edge cannot open by an ulp
                const int n[3] = {level - i - j, i, j};
                int ord[3] = {0, 1, 2};
                std::sort(ord, ord + 3, [&](int x, int y) { return pid[t + x] < pid[t + y]; });
                Corner q{};
                float px = 0, py = 0, pz = 0;
                for (int o = 0; o < 3; ++o) {
                    const int k = ord[o];
                    if (!n[k]) continue;
                    const float wk = n[k] / N;
                    px += wk * c[k]->x; py += wk * c[k]->y; pz += wk * c[k]->z;
                    q.u += wk * c[k]->u; q.v += wk * c[k]->v;
                    q.r += wk * c[k]->r; q.g += wk * c[k]->g; q.b += wk * c[k]->b;
                    q.nx += wk * c[k]->nx; q.ny += wk * c[k]->ny; q.nz += wk * c[k]->nz;
                }
                // Phong: the point projected on each corner's tangent plane (a
                // pinned corner's plane is the point itself), blended
                float sx = 0, sy = 0, sz = 0;
                for (int o = 0; o < 3; ++o) {
                    const int k = ord[o];
                    if (!n[k]) continue;
                    const float wk = n[k] / N;
                    float qx = px, qy = py, qz = pz;
                    if (!p[k]->pinned) {
                        const float* nn = p[k]->un;
                        const float d = (px - c[k]->x) * nn[0] + (py - c[k]->y) * nn[1] + (pz - c[k]->z) * nn[2];
                        qx -= d * nn[0]; qy -= d * nn[1]; qz -= d * nn[2];
                    }
                    sx += wk * qx; sy += wk * qy; sz += wk * qz;
                }
                // on an edge (one weight 0) the edge's pull, inside the interior's
                const float a = !n[0] ? ae[0] : !n[1] ? ae[1] : !n[2] ? ae[2] : aIn;
                q.x = (1.0f - a) * px + a * sx;
                q.y = (1.0f - a) * py + a * sy;
                q.z = (1.0f - a) * pz + a * sz;
                const float nl = std::sqrt(q.nx * q.nx + q.ny * q.ny + q.nz * q.nz);
                if (nl > 1e-6f) { q.nx /= nl; q.ny /= nl; q.nz /= nl; }
                // the shimmer's phase only where all three corners carry one
                q.phase = c[0]->phase < 0 || c[1]->phase < 0 || c[2]->phase < 0
                              ? -1.0f
                              : (n[0] * c[0]->phase + n[1] * c[1]->phase + n[2] * c[2]->phase) / N;
                // the three original corners are themselves, bit for bit
                const int corner = (i == 0 && j == 0) ? 0 : (i == level ? 1 : (j == level ? 2 : -1));
                if (corner >= 0) q = *c[corner];
                grid.push_back(q);
                gridVertex.push_back(corner >= 0 && hasVertex ? g.cornerVertex[t + static_cast<std::size_t>(corner)] : -1);
            }
    };
    // whether the grid built last has a sub-triangle facing against its source
    const auto folded = [&](std::size_t t) {
        const Corner& c0 = g.corners[t];
        const Corner& c1 = g.corners[t + 1];
        const Corner& c2 = g.corners[t + 2];
        const double fn[3] = {double(c1.y - c0.y) * (c2.z - c0.z) - double(c1.z - c0.z) * (c2.y - c0.y),
                              double(c1.z - c0.z) * (c2.x - c0.x) - double(c1.x - c0.x) * (c2.z - c0.z),
                              double(c1.x - c0.x) * (c2.y - c0.y) - double(c1.y - c0.y) * (c2.x - c0.x)};
        const auto faces = [&](std::size_t a0, std::size_t a1, std::size_t a2) {
            const Corner& x = grid[a0]; const Corner& y = grid[a1]; const Corner& z = grid[a2];
            const double ex = y.x - x.x, ey = y.y - x.y, ez = y.z - x.z;
            const double gx = z.x - x.x, gy = z.y - x.y, gz = z.z - x.z;
            return (ey * gz - ez * gy) * fn[0] + (ez * gx - ex * gz) * fn[1] + (ex * gy - ey * gx) * fn[2] > 0.0;
        };
        for (int j = 0; j < level; ++j)
            for (int i = 0; i + j < level; ++i)
                if (!faces(idx(i, j), idx(i + 1, j), idx(i, j + 1)) ||
                    (i + j + 1 < level && !faces(idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1))))
                    return true;
        return false;
    };
    const auto edges = [&](std::size_t t, float ae[3]) {
        ae[0] = pullOf(pid[t + 1], pid[t + 2]);
        ae[1] = pullOf(pid[t], pid[t + 2]);
        ae[2] = pullOf(pid[t], pid[t + 1]);
    };
    // THE GUARD, before anything is emitted: a triangle that still folds with
    // its interior unpulled has its EDGES' pulls taken to 0 in the shared
    // table, for its neighbours too - until nothing folds (a few rounds)
    std::vector<std::uint8_t> flat(nc / 3, 0);   // the interior unpulled
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (std::size_t t = 0; t + 2 < nc; t += 3) {
            if (!split[t / 3]) continue;
            float ae[3];
            edges(t, ae);
            build(t, ae, flat[t / 3] ? 0.0f : std::min({ae[0], ae[1], ae[2]}));
            if (!folded(t)) continue;
            if (!flat[t / 3]) { flat[t / 3] = 1; changed = true; continue; }
            if (ae[0] > 0 || ae[1] > 0 || ae[2] > 0) {
                pull[edgeKey(pid[t + 1], pid[t + 2])] = 0.0f;
                pull[edgeKey(pid[t], pid[t + 2])] = 0.0f;
                pull[edgeKey(pid[t], pid[t + 1])] = 0.0f;
                st.flatEdges += 3;
                changed = true;
            }
        }
        if (!changed) break;
    }

    // the new corners, batch by batch, the winding kept
    Geometry out;
    out.batches = g.batches;
    const std::size_t per = static_cast<std::size_t>(level) * static_cast<std::size_t>(level) * 3;
    out.corners.reserve(nc + st.split * (per - 3));
    const bool mirror = !g.cornerMirror.empty(), cull = !g.cornerCull.empty();
    const bool declared = g.cornerDeclared.size() == nc;
    const auto emit = [&](const Corner& c, std::size_t from, std::int32_t vertex) {
        out.corners.push_back(c);
        if (mirror) out.cornerMirror.push_back(at(g.cornerMirror, from));
        if (cull) out.cornerCull.push_back(at(g.cornerCull, from));
        out.cornerMesh.push_back(g.cornerMesh[from]);
        if (hasVertex) out.cornerVertex.push_back(vertex);
        if (declared) out.cornerDeclared.push_back(g.cornerDeclared[from]);
    };
    for (auto& b : out.batches) {
        const std::size_t from = b.start, to = b.start + b.count;
        b.start = out.corners.size();
        for (std::size_t t = from; t + 2 < to; t += 3) {
            if (!split[t / 3]) {
                for (int k = 0; k < 3; ++k)
                    emit(g.corners[t + k], t + k, hasVertex ? g.cornerVertex[t + k] : -1);
                continue;
            }
            float ae[3];
            edges(t, ae);
            build(t, ae, flat[t / 3] ? 0.0f : std::min({ae[0], ae[1], ae[2]}));
            st.unpulled += flat[t / 3];
            st.folded += folded(t) ? 1 : 0;
            const auto tri = [&](std::size_t a, std::size_t b2, std::size_t c2) {
                for (std::size_t k : {a, b2, c2}) emit(grid[k], t, gridVertex[k]);
            };
            for (int j = 0; j < level; ++j)
                for (int i = 0; i + j < level; ++i) {
                    tri(idx(i, j), idx(i + 1, j), idx(i, j + 1));
                    if (i + j + 1 < level) tri(idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1));
                }
        }
        b.count = out.corners.size() - b.start;
    }
    g.corners = std::move(out.corners);
    g.batches = std::move(out.batches);
    g.cornerMirror = std::move(out.cornerMirror);
    g.cornerCull = std::move(out.cornerCull);
    g.cornerMesh = std::move(out.cornerMesh);
    g.cornerVertex = std::move(out.cornerVertex);
    g.cornerDeclared = std::move(out.cornerDeclared);
    g.tieClass.clear();
    g.dirtyCorners.clear();
    g.dirtyFrom = g.dirtyTo = 0;
    return st;
}

}  // namespace omk

#endif  // OMK_REMASTER
