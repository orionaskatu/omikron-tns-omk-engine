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

namespace omk {

WetFloors wetFloors(const Geometry& g, const TriangleSoup& shot, const SoupRunBoxes& boxes) {
    WetFloors out;
    out.open.assign(g.corners.size() / 3, 0);
    out.cornerOpen.assign(g.corners.size(), kWetNotFloor);
    if (shot.empty()) return out;
    // straight UP (the game's Y points down), from just above the floor
    const double up[3] = {0.0, -kWetSkyRay, 0.0};
    const auto ray = [&](double x, double y, double z) {
        const double p0[3] = {x, y - 2.0, z};
        return sweepSphere(shot, boxes, p0, up, 0.0).has_value();
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
    std::map<int, FloorPlanes::Level> byHeight;
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
            if (!(n > 0.0) || ny >= 0.0 || -ny < kWetFlat * n) continue;
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
                continue;
            }
            out.open[t / 3] = 1;
            out.cornerOpen[t] = k0;
            out.cornerOpen[t + 1] = k1;
            out.cornerOpen[t + 2] = k2;
            ++out.openTris;
            if (k0 != k1 || k0 != k2) ++out.blendedTris;
            out.openArea += area;
            // ...and the LEVEL ones are the planes the reflection may take
            // (`forLevelTriangles`' 2 units)
            if (std::fabs(a.y - c1.y) > 2.0f || std::fabs(a.y - c2.y) > 2.0f) continue;
            auto& L = byHeight[static_cast<int>(std::lround(a.y / 4.0f))];
            L.area += area;
            L.tris.insert(L.tris.end(), {static_cast<float>(cx), static_cast<float>(cz), static_cast<float>(area)});
        }
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

void encodeOpenSky(Geometry& g, const WetFloors& w) {
    if (w.cornerOpen.size() != g.corners.size()) return;
    for (std::size_t i = 0; i < g.corners.size(); ++i)
        if (w.cornerOpen[i] != kWetNotFloor && g.corners[i].phase == -1.0f)
            g.corners[i].phase = -2.0f - static_cast<float>(w.cornerOpen[i]);
}

std::size_t coverRuns(std::vector<Draw>& draws, const Geometry* setGeo, const std::vector<std::uint8_t>& open,
                      float amount) {
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
            if (runKind) { o.wet = amount; o.snow = true; }
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

}  // namespace omk

#endif  // OMK_REMASTER
