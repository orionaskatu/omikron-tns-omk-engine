// SPDX-License-Identifier: GPL-3.0-or-later
// THE REFLECTING FLOORS - the remaster's A3 beyond the water (`todo/remaster.md`).
// NOT IN THE ORIGINAL: its floors mirror nothing.
//
// Which textures of which set are WET or POLISHED, and how much: the grades'
// `floors.txt`, curated by eye as `lights.txt` is -
//
//     SET TEXTURE = STRENGTH       # 0..1, times `[Remaster] floors`
//
// A set's floors are then its LEVEL opaque triangles of those textures,
// grouped by height (levels within `kFloorMerge` of each other are one, at
// the highest - a kerb's 4 units); each frame mirrors in ONE of them, the
// level below the eye with the most listed floor within `kFloorNear` of it
// (`chooseFloor`), through the water's own pass (`drawWithWater`) - the water
// first when its plane reflects.
#pragma once

#include "formats/tex3dt.h"
#include "o3de/geom3do.h"
#include "o3de/renderer.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace omk {

inline constexpr float kFloorMerge = 8.0f;      // levels this close are one plane (game units)
inline constexpr float kFloorNear = 4000.0f;    // the floor that counts for the choice: within ~100 m (inches)

class FloorList {
public:
    bool open(const std::string& path);   // false (and inactive) when absent
    bool active() const { return opened_; }
    std::size_t size() const { return count_; }
    // the strength of `texture` in `set` (a `*` line for any set), 0 when unlisted
    float strength(const std::string& set, const std::string& texture) const;
private:
    bool opened_ = false;
    std::size_t count_ = 0;
    std::unordered_map<std::string, float> byKey_;   // "SET TEXTURE", upper case
};
// the session's list (`floors.txt` beside the city grades)
FloorList& floorList();

struct FloorPlanes {
    // per MATERIAL of the set's geometry (`Batch::material`): its strength, 0
    // for a texture the list does not name
    std::vector<float> strength;
    struct Level {
        float y = 0.0f;              // the plane (the game's Y points DOWN)
        double area = 0.0;
        std::vector<float> tris;     // x, z, area of each listed triangle on it
    };
    std::vector<Level> levels;
    bool any() const { return !levels.empty(); }
};
FloorPlanes floorPlanes(const Geometry& g, std::span<const Texture> tex, const FloorList& list,
                        const std::string& set);

// The plane to mirror in for an eye at `eye`: the level BELOW it (a larger y)
// with the most listed floor within `kFloorNear` of it on the ground; not
// `found` when none has any.
WaterPlane chooseFloor(const FloorPlanes& f, const float eye[3]);
// ...or, for the WET floors (`o3de/wetfloor.h`), the level below the eye
// NEAREST `refY` - the height the camera looks at, the player's - with at
// least `kWetPlaneMin` of it near: the street he stands in, where the level
// with the most floor may be another (Qalisar's lamp street at -260 against
// its lower level at 0)
inline constexpr double kWetPlaneMin = 1.5e5;   // square units near (~100 m2)
// ...and a WET floor's triangle reflects in that plane with its corners within
// this of it, where a listed floor's must be within 4: a street is not level -
// its kerbs and slopes put corners 5 to 16 units off, and nothing lies between
// 16 and 32 (Qalisar's lamp street and start, Anekbah's street, 2026-10-28:
// 3402 triangles near within 4, 119724 more within 16, none to 32). The
// mirror is out by that much there, which the ripples and puddles hide
inline constexpr float kWetPlaneBand = 16.0f;
WaterPlane chooseFloorNear(const FloorPlanes& f, const float eye[3], float refY);

}  // namespace omk
