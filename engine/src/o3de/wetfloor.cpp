// SPDX-License-Identifier: GPL-3.0-or-later
// The wet floors - see wetfloor.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "o3de/wetfloor.h"

#include "o3de/renderer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace omk {

WetFloors wetFloors(const Geometry& g, const TriangleSoup& shot, const SoupRunBoxes& boxes, bool walls, bool grid) {
    WetFloors out;
    out.open.assign(g.corners.size() / 3, 0);
    out.rained.assign(g.corners.size() / 3, 0);
    out.cornerOpen.assign(g.corners.size(), kWetNotFloor);
    if (shot.empty()) return out;
    // EVERY RAY HERE IS STRAIGHT UP, so a grid over the soup in X and Z
    // hands each the triangles of one cell - where the meshes' boxes
    // (`boxes`, the original's broad phase) leave every mesh whose box spans
    // the point, a street's whole facade. The same triangles through the same
    // test in ascending order (`collision.h`, `SoupGrid`): the same answers,
    // bit for bit (`verify.py: engine: wet floors` part 16; `grid` false,
    // the boxes - an instrument's). The weather list's 12, 2026-10-28: the
    // drips' walk out took Anekbah's pass 223 -> ~590 ms on the M3
    SplitSoupGrid shotGrid;
    if (grid) {
        shotGrid.fixed = buildSoupGrid(shot, kWetGridCell);
        shotGrid.moving = buildSoupGrid(shot, kWetGridCell, std::span<const std::uint32_t>{});
    }
    // straight UP (the game's Y points down)
    const double up[3] = {0.0, -kWetSkyRay, 0.0};
    const auto shotUp = [&](const double p0[3]) {
        return grid ? sweepSphere(shot, shotGrid, p0, up, 0.0) : sweepSphere(shot, boxes, p0, up, 0.0);
    };
    // ...from just above the floor
    const auto ray = [&](double x, double y, double z) {
        const double p0[3] = {x, y - 2.0, z};
        return shotUp(p0).has_value();
    };
    // ...and a ROOF is wider than a cable: the soup carries the streets'
    // overhead wires, thin lines across Anekbah's street that made whole
    // triangles dry under them (a map of the test around the street start,
    // 2026-10-27). Covered when three of five rays meet one - the point and
    // four `kWetRoofSpan` around it
    const auto sky = [&](double x, double y, double z) {
        const double r = kWetRoofSpan;
        const int hit = ray(x, y, z) + ray(x + r, y, z) + ray(x - r, y, z) + ray(x, y, z + r) + ray(x, y, z - r);
        return hit < 3;
    };
    // ...and at a CORNER, once per place: the corners of neighbouring
    // triangles at one point cast one ray, so they agree, and the change from
    // wet to dry runs smoothly through the triangles a roof's edge crosses
    std::unordered_map<std::uint64_t, std::uint8_t> atCorner;
    const auto skyAt = [&](const Corner& c) -> std::uint8_t {
        const auto q = [](float v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                         static_cast<std::int32_t>(std::lround(v * 4.0f))) & 0x1FFFFFu); };
        const std::uint64_t key = q(c.x) | q(c.y) << 21 | q(c.z) << 42;
        if (const auto it = atCorner.find(key); it != atCorner.end()) return it->second;
        const std::uint8_t v = sky(c.x, c.y, c.z) ? 1 : 0;
        atCorner.emplace(key, v);
        return v;
    };
    // ...and a WALL's corner: the sky seen from `kWetWallOut` in front of it,
    // once per such point (the coplanar triangles of one face agree) - ONE
    // ray, not the floors' five: a set has twice as many wall triangles, and
    // five took Anekbah's load from 196 to 500 ms on an M1. From
    // `kWetWallBelow` BELOW the corner, never below the triangle's foot: a
    // wall's top corner sits AT the ceiling it meets, and a ray from just
    // above it starts past that ceiling - Qalisar's hall (15303, 0, -10414,
    // the reader's "rain under the hall's roof", 2026-10-28): its far wall
    // `Sol07` tops out at -136.7, the ceiling `PlafSSol02`'s height, and ran
    // with water
    std::unordered_map<std::uint64_t, std::uint8_t> atWall;
    const auto skyFront = [&](const Corner& c, double ox, double oz, double foot) -> std::uint8_t {
        const double x = c.x + ox, z = c.z + oz;
        const double y = c.y + std::clamp(foot - c.y, 0.0, kWetWallBelow);   // Y points down
        const auto q = [](double v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                         static_cast<std::int32_t>(std::lround(v * 4.0))) & 0x1FFFFFu); };
        const std::uint64_t key = q(x) | q(y) << 21 | q(z) << 42;
        if (const auto it = atWall.find(key); it != atWall.end()) return it->second;
        const std::uint8_t v = ray(x, y, z) ? 0 : 1;   // one ray: a wire over a wall's foot matters little
        atWall.emplace(key, v);
        return v;
    };
    std::map<int, FloorPlanes::Level> byHeight;
    // THE DRIPS: every floor edge, by its two corners' places - whether an
    // open triangle has it, a covered one, its corners disagreeing in an open
    // one, and a point toward the covered side (where the roof is)
    struct DripEdge {
        const Corner* p = nullptr;
        const Corner* q = nullptr;
        bool open = false, covered = false, mixed = false;
        bool sky = false;   // one of its corners under the open sky: an edge wholly under a ceiling never drips
        float toward[2] = {0.0f, 0.0f};
    };
    std::unordered_map<std::uint64_t, DripEdge> edges;
    const auto placeKey = [](const Corner& c) {
        const auto q = [](float v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                         static_cast<std::int32_t>(std::lround(v * 4.0f))) & 0x1FFFFFu); };
        return q(c.x) | q(c.y) << 21 | q(c.z) << 42;
    };
    const auto edgeKey = [&](const Corner& a2, const Corner& b2) {
        std::uint64_t ka = placeKey(a2), kb = placeKey(b2);
        if (ka > kb) std::swap(ka, kb);
        return ka * 0x9E3779B97F4A7C15ull ^ (kb + 0x632BE59BD9B4E019ull + (ka << 6) + (ka >> 2));
    };
    for (const auto& bt : g.batches) {
        if (bt.blend != Blend::Opaque) continue;
        const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(bt.start) + bt.count, g.corners.size());
        for (std::size_t t = bt.start; t + 2 < end; t += 3) {
            const Corner& a = g.corners[t];
            const Corner& c1 = g.corners[t + 1];
            const Corner& c2 = g.corners[t + 2];
            // FACING UP and within ~20 degrees of level: a street's gentle
            // slopes wet as its flat; a ceiling (facing down - the set's floors
            // wind with the normal's y NEGATIVE, Y down: Anekbah's street 4576
            // to 97) and a wall do not
            const double ux = c1.x - a.x, uy = c1.y - a.y, uz = c1.z - a.z;
            const double vx = c2.x - a.x, vy = c2.y - a.y, vz = c2.z - a.z;
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const double n = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (!(n > 0.0)) continue;
            if (std::fabs(ny) <= kWetWallSteep * n) {
                if (!walls) continue;
                // A WALL: its outward normal level, out `kWetWallOut`
                const double h = std::hypot(nx, nz);
                if (!(h > 0.0)) continue;
                const double ox = nx / h * kWetWallOut, oz = nz / h * kWetWallOut;
                const double foot = std::max({static_cast<double>(a.y), static_cast<double>(c1.y), static_cast<double>(c2.y)});
                const std::uint8_t w0 = skyFront(a, ox, oz, foot), w1 = skyFront(c1, ox, oz, foot),
                                   w2 = skyFront(c2, ox, oz, foot);
                if (w0 + w1 + w2 == 0) continue;
                out.rained[t / 3] = 2;
                out.cornerOpen[t] = static_cast<std::uint8_t>(kWetWallCorner + w0);
                out.cornerOpen[t + 1] = static_cast<std::uint8_t>(kWetWallCorner + w1);
                out.cornerOpen[t + 2] = static_cast<std::uint8_t>(kWetWallCorner + w2);
                ++out.wallTris;
                out.wallArea += n * 0.5;
                continue;
            }
            if (ny >= 0.0 || -ny < kWetFlat * n) continue;
            const double area = std::fabs(ny) * 0.5;   // the horizontal area
            const double cx = (a.x + c1.x + c2.x) / 3.0, cy = (a.y + c1.y + c2.y) / 3.0,
                         cz = (a.z + c1.z + c2.z) / 3.0;
            // the three corners and the centre: wet when any of them sees the
            // sky - how much at each point, the corners' answers blended
            const std::uint8_t k0 = skyAt(a), k1 = skyAt(c1), k2 = skyAt(c2);
            const int seen = k0 + k1 + k2 + (sky(cx, cy, cz) ? 1 : 0);
            if (seen == 0) {
                ++out.coveredTris;
                out.coveredArea += area;
                for (int e = 0; e < 3; ++e) {   // (THE DRIPS: an edge it shares with an open one)
                    auto& E = edges[edgeKey(g.corners[t + e], g.corners[t + (e + 1) % 3])];
                    E.covered = true;
                    E.toward[0] = static_cast<float>(cx); E.toward[1] = static_cast<float>(cz);
                }
                continue;
            }
            out.open[t / 3] = 1;
            out.rained[t / 3] = 1;
            out.cornerOpen[t] = k0;
            out.cornerOpen[t + 1] = k1;
            out.cornerOpen[t + 2] = k2;
            ++out.openTris;
            if (k0 != k1 || k0 != k2) ++out.blendedTris;
            // (THE DRIPS: its edges, the covered side's corner where they disagree)
            for (int e = 0; e < 3; ++e) {
                const Corner& p = g.corners[t + e];
                const Corner& q = g.corners[t + (e + 1) % 3];
                const std::uint8_t kp = e == 0 ? k0 : e == 1 ? k1 : k2;
                const std::uint8_t kq = e == 0 ? k1 : e == 1 ? k2 : k0;
                auto& E = edges[edgeKey(p, q)];
                E.p = &p; E.q = &q; E.open = true;
                E.sky = E.sky || kp || kq;   // a corner of it under the open sky
                if (kp != kq) { E.mixed = true; E.toward[0] = kp ? q.x : p.x; E.toward[1] = kp ? q.z : p.z; }
            }
            out.openArea += area;
            // ...and the LEVEL ones are the planes the reflection may take
            // (`forLevelTriangles`' 2 units)
            if (std::fabs(a.y - c1.y) > 2.0f || std::fabs(a.y - c2.y) > 2.0f) continue;
            auto& L = byHeight[static_cast<int>(std::lround(a.y / 4.0f))];
            L.area += area;
            L.tris.insert(L.tris.end(), {static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(area)});
        }
    }
    // THE DRIPS (`DripPoint`): along each edge where a roof's edge crosses
    // the wet floor - shared by an open triangle and a covered one, or its
    // corners disagreeing in an open one (a roof's edge across a triangle, or
    // along its side: both are in the sets) - a point every `kDripSpacing`,
    // one a cell, its roof found up from a little toward the covered side
    std::unordered_set<std::uint64_t> dripCells;
    // ...and the sky they fall from clear in the DRAWN set too: a hall's
    // ceiling may be missing from the shot soup (no bolt reaches it), and its
    // floor then reads open - drips fell inside under it (the reader, 2026-10-28)
    TriangleSoup drawn;
    std::vector<std::uint32_t> drawnRuns;   // (the boxes' way: `kDripDrawnRun` triangles a box)
    if (!edges.empty())
        for (const auto& bt : g.batches) {
            if (bt.blend != Blend::Opaque) continue;
            const std::size_t end = std::min<std::size_t>(static_cast<std::size_t>(bt.start) + bt.count, g.corners.size());
            for (std::size_t t = bt.start; t + 2 < end; t += 3) {
                if (!grid && (drawn.size() / 9) % kDripDrawnRun == 0)
                    drawnRuns.push_back(static_cast<std::uint32_t>(drawn.size() / 9));
                for (int k = 0; k < 3; ++k) {
                    drawn.push_back(g.corners[t + k].x);
                    drawn.push_back(g.corners[t + k].y);
                    drawn.push_back(g.corners[t + k].z);
                }
            }
        }
    // ...its rays straight up too: a grid, as the shot soup's
    SplitSoupGrid drawnGrid;
    SoupRunBoxes drawnBoxes;
    if (grid) {
        drawnGrid.fixed = buildSoupGrid(drawn, kWetGridCell);
        drawnGrid.moving = buildSoupGrid(drawn, kWetGridCell, std::span<const std::uint32_t>{});
    } else {
        drawnBoxes = soupRunBoxes(drawn, drawnRuns);
    }
    const auto drawnUp = [&](const double p0[3]) {
        return grid ? sweepSphere(drawn, drawnGrid, p0, up, 0.0) : sweepSphere(drawn, drawnBoxes, p0, up, 0.0);
    };
    for (const auto& [key, E] : edges) {
        if (!E.open || !E.sky || !(E.covered || E.mixed) || !E.p) continue;
        const double ex = E.q->x - E.p->x, ez = E.q->z - E.p->z;
        const int n = std::max(1, static_cast<int>(std::hypot(ex, ez) / kDripSpacing));
        for (int k = 0; k < n; ++k) {
            const double f = (k + 0.5) / n;
            const double mx = E.p->x + ex * f, mz = E.p->z + ez * f, my = E.p->y + (E.q->y - E.p->y) * f;
            const auto cell = [](double v) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                                  static_cast<std::int32_t>(std::floor(v / kDripSpacing)))); };
            if (!dripCells.insert(cell(mx) | cell(mz) << 32).second) continue;
            // the roof: up from a little toward the covered side...
            double cx0 = mx + kDripTowardRoof * (E.toward[0] - mx), cz0 = mz + kDripTowardRoof * (E.toward[1] - mz);
            const double p0[3] = {cx0, my - 2.0, cz0};
            auto hit = shotUp(p0);
            if (!hit) continue;
            // ...then OUT toward the open side, `kDripStep` at a time, to where
            // the sky is first clear: the drop falls past the eaves - a roof
            // juts out over the floor's edge, and one dropped at the floor's
            // edge fell through it (the reader's "drips through the ceiling",
            // 2026-10-28: 542 of 1760 with drawn geometry over them)
            double ox = mx - E.toward[0], oz = mz - E.toward[1];
            const double ol = std::hypot(ox, oz);
            if (!(ol > 0.0)) continue;
            ox /= ol; oz /= ol;
            double top = my - 2.0 - kWetSkyRay * hit->t;
            bool found = false;
            double dx = cx0, dz = cz0;
            for (double s2 = kDripStep; s2 <= kDripSearch; s2 += kDripStep) {
                const double qx = cx0 + ox * s2, qz = cz0 + oz * s2;
                const double q0[3] = {qx, my - 2.0, qz};
                const auto h2 = shotUp(q0);
                if (!h2) {
                    if (drawnUp(q0).has_value()) break;   // a drawn ceiling: no drip
                    found = true; dx = qx; dz = qz;
                    break;
                }
                top = my - 2.0 - kWetSkyRay * h2->t;   // still under it: its edge's height
            }
            if (!found) continue;
            const double rise = my - top;
            if (rise < kDripRoofMin || rise > kDripRoofMax) continue;
            out.drips.push_back({static_cast<float>(dx), static_cast<float>(dz),
                                 static_cast<float>(top), static_cast<float>(my)});
        }
    }
    // ...and how many fall UNDER anything - a self-check: past the eaves the
    // sky over a drop is clear, in the shot soup and the drawn set alike
    // (`verify.py: engine: rain 3d` part 16; dropped from under the roof's
    // overhang, they came "through the ceiling", the reader)
    for (const auto& d : out.drips) {
        const double p0[3] = {d.x, d.floor - 2.0, d.z};
        if (shotUp(p0).has_value() || drawnUp(p0).has_value())
            ++out.dripsCovered;
    }
    // levels within `kFloorMerge` of each other are one, at the highest -
    // `floorPlanes`' rule, for the same reason
    for (auto& [k, L] : byHeight) {
        const float y = static_cast<float>(k) * 4.0f;
        if (!out.planes.levels.empty() && y - out.planes.levels.back().y <= kFloorMerge) {
            auto& m = out.planes.levels.back();
            m.area += L.area;
            m.tris.insert(m.tris.end(), L.tris.begin(), L.tris.end());
        } else {
            L.y = y;
            out.planes.levels.push_back(std::move(L));
        }
    }
    return out;
}

std::uint32_t wetChecksum(const WetFloors& w) {
    std::uint32_t h = 2166136261u;   // FNV-1a
    const auto mix = [&](const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
    };
    mix(w.cornerOpen.data(), w.cornerOpen.size());
    mix(w.rained.data(), w.rained.size());
    for (const auto& d : w.drips) {
        const float f[4] = {d.x, d.z, d.top, d.floor};
        mix(f, sizeof f);
    }
    return h;
}

void encodeOpenSky(Geometry& g, const WetFloors& w) {
    if (w.cornerOpen.size() != g.corners.size()) return;
    for (std::size_t i = 0; i < g.corners.size(); ++i)
        if (const std::uint8_t k = w.cornerOpen[i]; k != kWetNotFloor && g.corners[i].phase == -1.0f)
            g.corners[i].phase = k >= kWetWallCorner ? kOpenSkyWall - static_cast<float>(k - kWetWallCorner)
                                                     : kOpenSkyFloor - static_cast<float>(k);
}

std::size_t coverRuns(std::vector<Draw>& draws, const Geometry* setGeo, const std::vector<std::uint8_t>& open,
                      float amount, bool snow) {
    if (!setGeo || amount <= 0.0f || open.empty()) return 0;
    static std::vector<Draw> out;
    out.clear();
    std::size_t covered = 0;
    for (const auto& d : draws) {
        if (d.geo != setGeo || d.blend != Blend::Opaque || d.meshPose) { out.push_back(d); continue; }
        std::size_t runStart = d.start;
        int runKind = -1;
        const auto flush = [&](std::size_t end) {
            if (runKind < 0 || end <= runStart) return;
            Draw o = d; o.start = runStart; o.count = end - runStart;
            if (runKind) { o.wet = amount; o.snow = snow; }
            out.push_back(o);
        };
        for (std::size_t t = d.start; t + 2 < d.start + d.count; t += 3) {
            const int kind = t / 3 < open.size() && open[t / 3] ? 1 : 0;
            covered += static_cast<std::size_t>(kind);
            if (kind != runKind) { flush(t); runStart = t; runKind = kind; }
        }
        flush(d.start + d.count);
    }
    draws.swap(out);
    return covered;
}

float Wetness::step(float rain, double dtMs) {
    const float r = std::clamp(rain, 0.0f, 1.0f);
    const float dried = w_ - static_cast<float>(std::max(0.0, dtMs) / kWetDryMs);
    w_ = std::max(r, std::max(0.0f, dried));
    return w_;
}

float stepBodyWet(float w, float floors, bool open, double dtMs) {
    const float f = std::clamp(floors, 0.0f, 1.0f);
    const double dt = std::max(0.0, dtMs);
    if (open && w < f) return std::min(f, w + static_cast<float>(dt / kWetBodySoakMs));
    return std::min(f, std::max(0.0f, w - static_cast<float>(dt / kWetBodyDryMs)));
}

}  // namespace omk

#endif  // OMK_REMASTER
