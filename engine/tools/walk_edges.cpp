// SPDX-License-Identifier: GPL-3.0-or-later
// THE WALKER AT TWO EDGES, on sets built here so nothing else is measured.
//
//     walk_edges
//
// A reader's report (2026-10-20): the player falls into the void and loops in
// the fall clip. Two faults of `Walker`, each with its own little set (y grows
// DOWN; every face a quad of two triangles):
//
//   VOID   a platform at y 0 for x < 100, a ledge 30 lower from x 100 to 115,
//          and nothing past it. Walked +x at 4 a frame from x 90: the 30-unit
//          drop is a SHORT fall, which keeps walking, and the walk carries him
//          past x 115 over nothing. `Actor_ApplyMotion` (0x004672D0) puts a
//          body whose probe finds nothing back where it was; without that he
//          falls for ever. Asserted: he ends on the ledge, y 30, grounded.
//   RAMP   a platform at y 0 for x < 0, a 45-degree ramp down to y 50 at x 50,
//          a floor at y 50 beyond, and a deep floor at y 200 UNDER the ramp.
//          Walked +x from x -10: the engine's probe meets the nearest face, the
//          ramp, and slides him down it; asking the steep faces only where no
//          walkable floor lies below dropped him THROUGH it onto the deep one.
//          Asserted: he ends on the floor at y 50.
//
// One line, `@walk_edges void_y=.. void_air=.. ramp_y=.. ramp_air=..`, read
// by `verify.py: engine: walk edges`.
#include "actor/walk.h"

#include <cstdio>

namespace {

void quad(omk::TriangleSoup& s, double x0, double y0, double x1, double y1, double z0, double z1) {
    // x0..x1 along x, the height y0 at x0 and y1 at x1, z0..z1 across
    const double v[4][3] = {{x0, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y0, z1}};
    for (const int i : {0, 1, 2, 0, 2, 3})
        for (int k = 0; k < 3; ++k) s.push_back(static_cast<float>(v[i][k]));
}

// walk +x for `frames`, then let the vertical settle for ten seconds
const double* walk(omk::Walker& w, int frames) {
    for (int f = 0; f < frames; ++f) w.step(4.0, 0.0, 1.0);
    for (int f = 0; f < 300 && (w.airborne() || w.sliding()); ++f) w.tick(1.0);
    return w.pos();
}

}  // namespace

int main() {
    // ---- VOID
    omk::TriangleSoup voidWalk, voidSteep;
    quad(voidWalk, -100.0, 0.0, 100.0, 0.0, -50.0, 50.0);
    quad(voidWalk, 100.0, 30.0, 115.0, 30.0, -50.0, 50.0);
    omk::Walker v(voidWalk, 90.0, 0.0, 0.0);
    v.setSteep(&voidSteep);
    const double* vp = walk(v, 12);

    // ---- RAMP
    omk::TriangleSoup rampWalk, rampSteep;
    quad(rampWalk, -50.0, 0.0, 0.0, 0.0, -50.0, 50.0);
    quad(rampWalk, 50.0, 50.0, 150.0, 50.0, -50.0, 50.0);
    quad(rampWalk, 0.0, 200.0, 50.0, 200.0, -50.0, 50.0);
    quad(rampSteep, 0.0, 0.0, 50.0, 50.0, -50.0, 50.0);
    omk::Walker r(rampWalk, -10.0, 0.0, 0.0);
    r.setSteep(&rampSteep);
    const double* rp = walk(r, 8);

    std::printf("void: from x 90 on y 0 -> %.1f %.1f %.1f, %s\n", vp[0], vp[1], vp[2],
                v.airborne() ? "STILL FALLING" : "standing");
    std::printf("ramp: from x -10 on y 0 -> %.1f %.1f %.1f, %s\n", rp[0], rp[1], rp[2],
                r.airborne() ? "STILL FALLING" : r.sliding() ? "STILL SLIDING" : "standing");
    std::printf("@walk_edges void_y=%.1f void_air=%d ramp_y=%.1f ramp_air=%d\n", vp[1],
                v.airborne() ? 1 : 0, rp[1], (r.airborne() || r.sliding()) ? 1 : 0);
    return 0;
}
