// SPDX-License-Identifier: GPL-3.0-or-later
// THE WET FLOORS - the remaster's rain on the ground (`todo/remaster.md`,
// "The rain's wet floors"). NOT IN THE ORIGINAL: its rain is 200 grey lines
// over the picture (`o3de/weather.h`, docs/ASSETS.md 4e) and wets nothing.
//
// Two halves:
//  * WHICH floors - a set's opaque triangles FACING UP within ~20 degrees of
//    level (`kWetFlat`; a street's slopes are a third of it), under the OPEN
//    SKY - the level ones of them the planes the reflection takes. "Open"
//    is the original's own roof test turned to the floor: `sub_481200` casts
//    the eye 196.85 up through the bolts' world (the set's SHOT soup) and
//    calls a hit a roof; here each floor triangle casts up `kWetSkyRay` from
//    four points of it through the same soup, and is open when most of them
//    meet nothing. Per triangle (Anekbah's street: ~1 m2 each), on the CPU,
//    so both GPU backends take it with no shader of their own.
//  * HOW wet - `Wetness`: as wet as the rain is strong (the weather's drops
//    over its 200), drying linearly over `kWetDryMs` once it eases. Only in
//    an area whose weather is the RAIN (AREA +176 type 1) and only while the
//    cycle runs (option row 7 at 2): no shower, no wet floor.
// The wet triangles reflect through the reflecting floors' pass
// (`o3de/floorreflect.h`, `drawWithWater`) - one plane a frame, the level
// below the eye with the most of them near.
#pragma once

#include "o3de/collision.h"
#include "o3de/floorreflect.h"
#include "o3de/geom3do.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace omk {

inline constexpr double kWetSkyRay = 4000.0;     // how far up a roof counts (~100 m, inches)
inline constexpr double kWetFlat = 0.94;         // a wet floor's normal at least this upright (~20 degrees)
inline constexpr double kWetDryMs = 120000.0;    // a soaked floor dry again, after the rain
inline constexpr float kWetReflect = 0.5f;       // a soaked floor's reflection, times `[Remaster] wet`
inline constexpr float kWetDarken = 0.35f;       // ...and how much darker its film makes it (`Draw::wet`)

struct WetFloors {
    // per TRIANGLE of the set's geometry (corner index / 3): 1 an opaque floor
    // under the open sky, level or gently sloped
    std::vector<std::uint8_t> open;
    FloorPlanes planes;              // those triangles as levels (`chooseFloor`); no strengths
    std::size_t openTris = 0, coveredTris = 0;
    double openArea = 0.0, coveredArea = 0.0;   // square game units
    bool any() const { return planes.any(); }
};
WetFloors wetFloors(const Geometry& g, const TriangleSoup& shot, const SoupRunBoxes& boxes);

class Wetness {
public:
    // `rain` the shower now, 0..1; `dtMs` the frame's -> the floors' wetness, 0..1
    float step(float rain, double dtMs);
    float value() const { return w_; }
private:
    float w_ = 0.0f;
};

}  // namespace omk
