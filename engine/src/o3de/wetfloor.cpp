// SPDX-License-Identifier: GPL-3.0-or-later
// The wet floors - see wetfloor.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "o3de/wetfloor.h"

#include "o3de/renderer.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace omk {

WetFloors wetFloors(const Geometry& g, const TriangleSoup& shot, const SoupRunBoxes& boxes) {
    WetFloors out;
    out.open.assign(g.corners.size() / 3, 0);
    if (shot.empty()) return out;
    // straight UP (the game's Y points down), from just above the floor
    const double up[3] = {0.0, -kWetSkyRay, 0.0};
    const auto sky = [&](double x, double y, double z) {
        const double p0[3] = {x, y - 2.0, z};
        return !sweepSphere(shot, boxes, p0, up, 0.0).has_value();
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
            // the centre and three points halfway to the corners: open when at
            // least three of the four see the sky
            int seen = sky(cx, cy, cz);
            for (const Corner* c : {&a, &c1, &c2})
                seen += sky((cx + c->x) * 0.5, (cy + c->y) * 0.5, (cz + c->z) * 0.5);
            if (seen < 3) {
                ++out.coveredTris;
                out.coveredArea += area;
                continue;
            }
            out.open[t / 3] = 1;
            ++out.openTris;
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

float Wetness::step(float rain, double dtMs) {
    const float r = std::clamp(rain, 0.0f, 1.0f);
    const float dried = w_ - static_cast<float>(std::max(0.0, dtMs) / kWetDryMs);
    w_ = std::max(r, std::max(0.0f, dried));
    return w_;
}

}  // namespace omk

#endif  // OMK_REMASTER
