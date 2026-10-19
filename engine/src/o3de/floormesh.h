// SPDX-License-Identifier: GPL-3.0-or-later
// WHICH SET MESHES ARE THE GROUND - for RAY-TRACED REFLECTIONS
// (`todo/enhancements.md` 26, an ENHANCEMENT): the wet street that mirrors the
// neon. The game has no material or gloss data (a mesh carries flags, a
// material is an atlas page - Anekbah's twenty are `BATITR01`..`20`, each
// mixing walls, floors and trims), so the rule is read off the geometry and
// the artists' mesh names, measured over all 220 shipped decors
// (2026-10-19):
//   * THE SHAPE: at least 60% of a mesh's drawn opaque area faces UP (a face
//     normal within 25 degrees of -Y, the game's Y pointing down), single-
//     sided. Anekbah's ground is `YX*`, `X*`, `OP*` - 90% of the set's
//     upward area, 98% of their own; elsewhere `*ROUTE*` (road), `TROT*`
//     (pavement), `*SOL*` (floor). Walls, lamps, benches, cargo and the
//     building blocks (`BAT*`, `V*`, `Y*`: 1-3% upward, their ledges) fail it.
//   * WATER fails it already - two-sided (`kTwoSided`, 93 meshes, nearly all
//     water) or blended: `EAU*`, `*WATER*`, `LAC*` - and is not wanted here.
//   * ...and THE NAMES that pass the shape and are not a wet street: canal and
//     basin bottoms under the water (`*FOND*`, `*BASSIN*`), grass (`GREEN*`,
//     `*HERBE*`, `*GAZON*`), snow (`NG_*`, `*NEIGE*`), roofs - not walking
//     level - (`*TOIT*`), and the flames (`*FLAME*`).
// The fragment shader still tests each pixel's own face (a floor mesh's kerb
// sides are not up), so this only says where a reflection MAY be.
#pragma once

#include "formats/mesh3do.h"
#include "o3de/geom3do.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace omk {

inline bool floorNameExcluded(const char* name) {
    std::string n;
    for (const char* p = name; *p; ++p) n += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    for (const char* w : {"FOND", "BASSIN", "GREEN", "HERBE", "GAZON", "NEIGE", "TOIT", "FLAME", "WATER", "WTER"})
        if (n.find(w) != std::string::npos) return true;
    for (const char* w : {"NG_", "EAU", "LAC"})
        if (n.rfind(w, 0) == 0) return true;
    return false;
}

// -> one byte a mesh (`.3DO` order, `Geometry::cornerMesh`'s index): 1 = ground.
inline std::vector<std::uint8_t> floorMeshes(const Geometry& g, std::span<const Mesh> ms) {
    std::vector<std::uint8_t> out(ms.size(), 0);
    if (g.cornerMesh.size() != g.corners.size()) return out;
    std::vector<double> area(ms.size(), 0.0), up(ms.size(), 0.0);
    const bool haveCull = g.cornerCull.size() == g.corners.size();
    for (const Batch& b : g.batches) {
        if (b.blend != Blend::Opaque) continue;
        for (std::size_t c = b.start; c + 3 <= b.start + b.count && c + 3 <= g.corners.size(); c += 3) {
            const std::int32_t m = g.cornerMesh[c];
            if (m < 0 || static_cast<std::size_t>(m) >= ms.size()) continue;
            const Corner &p = g.corners[c], &q = g.corners[c + 1], &r = g.corners[c + 2];
            const double ux = q.x - p.x, uy = q.y - p.y, uz = q.z - p.z;
            const double vx = r.x - p.x, vy = r.y - p.y, vz = r.z - p.z;
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-9) continue;
            // the face's side from its vertex normals (the winding is the
            // engine's screen test, not a world-space convention)
            const double vny = static_cast<double>(p.ny) + q.ny + r.ny;
            const double fy = vny != 0.0 ? (vny < 0.0 ? -std::fabs(ny) : std::fabs(ny)) / len : ny / len;
            const bool twoSided = haveCull && g.cornerCull[c] == 0;
            area[static_cast<std::size_t>(m)] += len * 0.5;
            if (fy < -0.9 && !twoSided) up[static_cast<std::size_t>(m)] += len * 0.5;
        }
    }
    for (std::size_t m = 0; m < ms.size(); ++m)
        out[m] = area[m] > 0.0 && up[m] >= 0.6 * area[m] && !floorNameExcluded(ms[m].name) ? 1 : 0;
    return out;
}

}  // namespace omk
