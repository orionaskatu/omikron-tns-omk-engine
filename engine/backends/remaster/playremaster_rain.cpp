// SPDX-License-Identifier: GPL-3.0-or-later
// THE RAIN IN 3D - the remaster's (`todo/remaster.md`, "The rain in 3D").
// NOT THE ORIGINAL'S DRAW: `sub_4815D0` projects its 200 drops and draws each
// as a 2D grey line over the picture (docs/ASSETS.md 4e), through walls and
// under roofs alike. Here the SAME drops - the game's cycle, its 200, its box
// ahead of the eye and its wind, all `o3de/weather.h`'s, untouched - are
// strips IN the scene: the depth test hides them behind a wall, and a drop
// with a roof over it (straight up through the set's SHOT soup, the world
// `sub_481200`'s roof ray uses) is not drawn. Each strip runs from the drop to
// its tail as the line did, ADDITIVE, from the scene's fog colour at the drop
// to 0xC0C0C0 at the tail times `[Remaster] rain3d`, as wide as the
// original's 1-pixel line is at 640x480 (scaled with the frame's height).
// The snow keeps the original's sprites. Vulkan and GLES (`kTakesWater`'s
// backends). Compiled empty without OMK_REMASTER (`playremaster_off.cpp`).
//
// DENSER AND FAINTER than the original's lines (the reader, on the comparison
// picture, 2026-10-27: half the original's box lies below the street, so the
// 3D rain showed far fewer streaks): each drop brings `kRainCopies` more at
// fixed offsets inside the same box, wrapped as the box wraps - they move with
// it, so the cycle's count and its ramp hold - and every strip is `kRainGlow`
// as bright, fading in over the first `kRainNear` units from the eye and out
// over the box's far end: rain is seen through, not drawn over.
#if OMK_REMASTER

#include "../sdl/playstate.h"
#include "o3de/collision.h"
#include "o3de/raster.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kRainRoofRay = 4000.0;   // how far up a roof stops a drop (`kWetSkyRay`'s)
constexpr float kRainBox = 1000.0f;       // the rain's box (`dword_4D0780`, `o3de/weather.cpp`)
constexpr int kRainCopies = 3;            // the drops each original one brings
constexpr float kRainGlow = 0.55f;        // a strip's brightness against the original line's
constexpr float kRainNear = 80.0f;        // ...faded in over this from the eye
// the copies' offsets in the box, fixed (any spread that is not a lattice)
constexpr float kRainOffsets[kRainCopies][3] = {
    {317.0f, -251.0f, 173.0f}, {-211.0f, 389.0f, -443.0f}, {463.0f, 127.0f, -307.0f}};
}  // namespace

bool PlayState::remasterRainTake(const std::vector<omk::WeatherDrop>& drops, unsigned viewBits) {
    if (!(viewBits & 2u) || rain3dStrength <= 0.0f || !remasterShown_ || !world_ ||
        !(world_->takes() & omk::kTakesWater))
        return false;
    rainDrops = drops;
    return true;
}

void PlayState::remasterRainDraws(std::vector<omk::Draw>& list, const omk::View& v) {
    rainGeo.corners.clear();
    rainGeo.batches.clear();
    if (rainDrops.empty()) return;
    // the roofs: the shown set's shot soup, as the weather's own ray
    const omk::TriangleSoup* shot = nullptr;
    const omk::SoupRunBoxes* boxes = nullptr;
    for (const auto& ws : worldSlots)
        if (!ws.stem.empty() && ws.stem == worldSet) { shot = &ws.shotSoup; boxes = &ws.shotRunBoxes; }
    const float s = std::clamp(rain3dStrength, 0.0f, 1.0f) * kRainGlow;
    // the box's centre as the weather places it: a third of its size ahead
    const float dv[3] = {v.cam.at[0] - v.cam.eye[0], v.cam.at[1] - v.cam.eye[1], v.cam.at[2] - v.cam.eye[2]};
    const float dl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
    if (!(dl > 0.0f)) return;
    float centre[3];
    for (int k = 0; k < 3; ++k) centre[k] = v.cam.eye[k] + dv[k] / dl * kRainBox / 3.0f;
    // the original's drops and their copies, wrapped into the box
    static std::vector<omk::WeatherDrop> all;
    all.assign(rainDrops.begin(), rainDrops.end());
    for (const auto& d : rainDrops)
        for (const auto& o : kRainOffsets) {
            omk::WeatherDrop c = d;
            for (int k = 0; k < 3; ++k) {
                float rel = d.p[k] - centre[k] + o[k];
                rel -= kRainBox * std::floor((rel + kRainBox * 0.5f) / kRainBox);
                c.p[k] = centre[k] + rel;
                c.q[k] = c.p[k] + (d.q[k] - d.p[k]);
            }
            all.push_back(c);
        }
    // the original's line is 1 pixel at 640x480: half a pixel each side,
    // scaled with the frame
    const float halfPx = 0.5f * std::max(1.0f, static_cast<float>(fb.h) / 480.0f);
    int roofed = 0;
    const auto corner = [](const float p[3], float r, float g, float b) {
        omk::Corner c{};
        c.x = p[0]; c.y = p[1]; c.z = p[2];
        c.r = r; c.g = g; c.b = b;
        c.phase = -1.0f;
        return c;
    };
    for (const auto& d : all) {
        if (shot && boxes) {
            const double p0[3] = {d.p[0], d.p[1], d.p[2]};
            const double up[3] = {0.0, -kRainRoofRay, 0.0};   // Y points down
            if (omk::sweepSphere(*shot, *boxes, p0, up, 0.0).has_value()) { ++roofed; continue; }
        }
        // the strip's side: across the streak and the eye's line to it
        const float a[3] = {d.q[0] - d.p[0], d.q[1] - d.p[1], d.q[2] - d.p[2]};
        const float e[3] = {v.cam.eye[0] - d.p[0], v.cam.eye[1] - d.p[1], v.cam.eye[2] - d.p[2]};
        float side[3] = {a[1] * e[2] - a[2] * e[1], a[2] * e[0] - a[0] * e[2], a[0] * e[1] - a[1] * e[0]};
        const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (!(sl > 0.0f)) continue;
        for (float& k : side) k /= sl;
        // ...as wide as `halfPx` there: what one unit across measures on screen
        const float off[3] = {d.p[0] + side[0], d.p[1] + side[1], d.p[2] + side[2]};
        const omk::Projected pa = omk::project(v.cam, d.p), pb = omk::project(v.cam, off);
        if (pa.z <= 0.0f || pb.z <= 0.0f) continue;   // behind the eye
        const float ppu = std::hypot(pb.x - pa.x, pb.y - pa.y);
        if (!(ppu > 0.0f)) continue;
        const float w = halfPx / ppu;
        // seen through: faint near the eye and far off
        const float fade = std::clamp(pa.z / kRainNear, 0.0f, 1.0f) *
                           std::clamp((kRainBox - pa.z) / (kRainBox * 0.4f), 0.0f, 1.0f);
        if (!(fade > 0.0f)) continue;
        const float head[3] = {v.fogColour[0] / 255.0f * s * fade, v.fogColour[1] / 255.0f * s * fade,
                               v.fogColour[2] / 255.0f * s * fade};
        const float tail = 192.0f / 255.0f * s * fade;
        const float p0[3] = {d.p[0] - side[0] * w, d.p[1] - side[1] * w, d.p[2] - side[2] * w};
        const float p1[3] = {d.p[0] + side[0] * w, d.p[1] + side[1] * w, d.p[2] + side[2] * w};
        const float q0[3] = {d.q[0] - side[0] * w, d.q[1] - side[1] * w, d.q[2] - side[2] * w};
        const float q1[3] = {d.q[0] + side[0] * w, d.q[1] + side[1] * w, d.q[2] + side[2] * w};
        const omk::Corner c0 = corner(p0, head[0], head[1], head[2]), c1 = corner(p1, head[0], head[1], head[2]);
        const omk::Corner c2 = corner(q0, tail, tail, tail), c3 = corner(q1, tail, tail, tail);
        rainGeo.corners.insert(rainGeo.corners.end(), {c0, c1, c3, c0, c3, c2});
    }
    const std::size_t drawnDrops = rainGeo.corners.size() / 6;
    if (frameNo % 30 == 0) {
        std::printf("frame %ld: the rain in the scene - %zu of %zu drops drawn (the weather's %zu and %d each "
                    "more), %d under a roof\n", frameNo, drawnDrops, all.size(), rainDrops.size(), kRainCopies,
                    roofed);
        omk::event("remaster.rain3d", {{"frame", frameNo}, {"drops", static_cast<long>(rainDrops.size())},
                                       {"drawn", static_cast<long>(drawnDrops)}, {"roofed", roofed}});
    }
    if (rainGeo.corners.empty()) return;
    omk::Batch b;
    b.material = -1;
    b.blend = omk::Blend::Add;
    b.start = 0;
    b.count = rainGeo.corners.size();
    rainGeo.batches.push_back(b);
    ++rainGeo.revision;
    // untextured: a pool slot past every texture binds the backends' white;
    // the additive state bits as the mesh path's (`keyOf`, 0x2100)
    constexpr std::uint32_t kNoTexture = 0xFFFEu;
    omk::Draw dr;
    dr.bucketKey = omk::withPoolSlot(0x2100u | (kNoTexture & 0x3Fu), kNoTexture);
    dr.geo = &rainGeo;
    dr.start = 0;
    dr.count = rainGeo.corners.size();
    dr.blend = omk::Blend::Add;
    list.push_back(dr);
}

#endif  // OMK_REMASTER
