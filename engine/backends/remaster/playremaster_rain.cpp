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
//
// ...and SOFT (the reader's "do 1 and 2", 2026-10-27, after the hard 1-pixel
// lines read as scratches near the eye): each strip carries a generated
// STREAK (`rainStreakTexture`: soft across, a soft tip at the leading end,
// fading out behind it) three times the line's width, so its edges are the
// texture's, not the raster's; and it is `kRainLength` long in the WORLD - a
// camera's shutter open half a frame on a drop falling 64 units a frame - so
// perspective makes a near drop's streak long and a far one's short, where
// the original's 12 units drew every drop alike.
//
// ...and LIT (the reader's "option 3", 2026-10-27): each drop takes the light
// of the set's own lights that reach it - and the emitters' (the flames, the
// glows) when the remaster's emitters' light put them in the slot's table -
// on the engine's own rule (`o3de/vertexlight.h` `lightReach`: the squared
// reach, the linear falloff, `intensity * 256`), as a surface facing each
// light would (a streak has none), its colour through the engine's ramp
// `(t * c) >> 8`: rain glints orange by the burner and in a lamp's pool, and
// is dimmer where nothing lights it.
#if OMK_REMASTER

#include "../sdl/playstate.h"
#include "o3de/collision.h"
#include "o3de/raster.h"
#include "o3de/vertexlight.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kRainRoofRay = 4000.0;   // how far up a roof stops a drop (`kWetSkyRay`'s)
constexpr float kRainBox = 1000.0f;       // the rain's box (`dword_4D0780`, `o3de/weather.cpp`)
constexpr int kRainCopies = 3;            // the drops each original one brings
constexpr float kRainGlow = 0.55f;        // a strip's brightness against the original line's
// ...faded in from `kRainNearFrom` to `kRainNear` from the eye, smoothly: a drop
// a step away is a long bright streak across the frame (the reader's "fade the
// near streaks", 2026-10-27 - it was 0..80, linear)
constexpr float kRainNearFrom = 40.0f;
constexpr float kRainNear = 260.0f;
constexpr float kRainLength = 32.0f;      // a streak's length in the world: half a frame's fall
constexpr float kRainHalfPx = 1.5f;       // ...its half width in pixels at 480 lines, the texture's soft edge in it
constexpr int kStreakW = 8, kStreakH = 32;   // the streak texture, across and along
constexpr float kRainUnlit = 0.35f;       // a drop no light reaches, against the line's grey
constexpr float kRainLit = 0.5f;          // ...and the lights' colour added on it - Anekbah's
                                          // street has two fills reaching everywhere, ~0.8 together

// THE STREAK, generated: a grey texel per palette index (index = brightness)
// - across, a soft bell; along it, from the leading end (v 0), a short soft
// tip, the brightest a fifth of the way back, fading out to the trailing end
omk::Texture rainStreakTexture() {
    omk::Texture t;
    t.name = "OMK_RAIN_STREAK";
    t.width = kStreakW;
    t.height = kStreakH;
    t.bpp = 8;
    t.exact = true;
    t.pal.assign(768, 0);
    std::uint8_t* pal = t.pal.mutableData();
    for (int i = 0; i < 256; ++i) pal[3 * i] = pal[3 * i + 1] = pal[3 * i + 2] = static_cast<std::uint8_t>(i);
    t.idx.assign(static_cast<std::size_t>(kStreakW) * kStreakH, 0);
    std::uint8_t* px = t.idx.mutableData();
    for (int y = 0; y < kStreakH; ++y) {
        const float along = (static_cast<float>(y) + 0.5f) / kStreakH;   // 0 the leading end
        const float tip = std::clamp(along / 0.2f, 0.0f, 1.0f);
        const float tail = std::pow(std::clamp((1.0f - along) / 0.8f, 0.0f, 1.0f), 1.5f);
        const float l = tip * tip * (3.0f - 2.0f * tip) * tail;
        for (int x = 0; x < kStreakW; ++x) {
            const float a = (static_cast<float>(x) + 0.5f) / kStreakW * 2.0f - 1.0f;   // -1..1 across
            const float across = std::pow(std::max(0.0f, 1.0f - a * a), 2.0f);
            px[y * kStreakW + x] = static_cast<std::uint8_t>(std::lround(255.0f * l * across));
        }
    }
    return t;
}
// the copies' offsets in the box, fixed (any spread that is not a lattice)
constexpr float kRainOffsets[kRainCopies][3] = {
    {317.0f, -251.0f, 173.0f}, {-211.0f, 389.0f, -443.0f}, {463.0f, 127.0f, -307.0f}};
}  // namespace

// the streak's texture into the frame's pool, after the sprites, while the
// rain in 3D was armed at the start
void PlayState::remasterPoolRain(std::vector<omk::Texture>& slots) {
    rainTexSlot = -1;
    if (remasterArmed("rain3d") <= 0.0f) return;
    static const omk::Texture streak = rainStreakTexture();
    rainTexSlot = static_cast<int>(slots.size());
    slots.push_back(streak);
}

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
    std::span<const omk::Light3do> lights;
    for (const auto& ws : worldSlots)
        if (!ws.stem.empty() && ws.stem == worldSet) {
            shot = &ws.shotSoup;
            boxes = &ws.shotRunBoxes;
            lights = ws.lights;
        }
    static std::vector<float> reach;
    int litDrops = 0;
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
    // the streak three times the original's 1-pixel line at 640x480, scaled
    // with the frame - its texture's soft edge inside that
    const float halfPx = kRainHalfPx * std::max(1.0f, static_cast<float>(fb.h) / 480.0f);
    int roofed = 0;
    const auto corner = [](const float p[3], const float rgb[3], float u, float tv) {
        omk::Corner c{};
        c.x = p[0]; c.y = p[1]; c.z = p[2];
        c.u = u; c.v = tv;
        c.r = rgb[0]; c.g = rgb[1]; c.b = rgb[2];
        c.phase = -1.0f;
        return c;
    };
    for (const auto& d : all) {
        if (shot && boxes) {
            const double p0[3] = {d.p[0], d.p[1], d.p[2]};
            const double up[3] = {0.0, -kRainRoofRay, 0.0};   // Y points down
            if (omk::sweepSphere(*shot, *boxes, p0, up, 0.0).has_value()) { ++roofed; continue; }
        }
        // the streak: from the drop's leading end - the original line's bright
        // one, its tail ahead along the wind - back `kRainLength` against it
        float a[3] = {d.q[0] - d.p[0], d.q[1] - d.p[1], d.q[2] - d.p[2]};
        const float al = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (!(al > 0.0f)) continue;
        for (float& k : a) k /= al;
        const float lead[3] = {d.q[0], d.q[1], d.q[2]};
        const float trail[3] = {lead[0] - a[0] * kRainLength, lead[1] - a[1] * kRainLength,
                                lead[2] - a[2] * kRainLength};
        // the strip's side: across the streak and the eye's line to it
        const float e[3] = {v.cam.eye[0] - lead[0], v.cam.eye[1] - lead[1], v.cam.eye[2] - lead[2]};
        float side[3] = {a[1] * e[2] - a[2] * e[1], a[2] * e[0] - a[0] * e[2], a[0] * e[1] - a[1] * e[0]};
        const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
        if (!(sl > 0.0f)) continue;
        for (float& k : side) k /= sl;
        // ...as wide as `halfPx` there: what one unit across measures on screen
        const float off[3] = {lead[0] + side[0], lead[1] + side[1], lead[2] + side[2]};
        const omk::Projected pa = omk::project(v.cam, lead), pb = omk::project(v.cam, off);
        if (pa.z <= 0.0f || pb.z <= 0.0f) continue;   // behind the eye
        const float ppu = std::hypot(pb.x - pa.x, pb.y - pa.y);
        if (!(ppu > 0.0f)) continue;
        const float w = halfPx / ppu;
        // seen through: faint near the eye and far off
        const float nearT = std::clamp((pa.z - kRainNearFrom) / (kRainNear - kRainNearFrom), 0.0f, 1.0f);
        const float fade = nearT * nearT * (3.0f - 2.0f * nearT) *
                           std::clamp((kRainBox - pa.z) / (kRainBox * 0.4f), 0.0f, 1.0f);
        if (!(fade > 0.0f)) continue;
        // the original line's grey at its bright end (0xC0), dimmed where
        // nothing lights it, and the lights that reach the drop added - each
        // as `ramp[c][t]` with `t` its whole strength; the streak's texture
        // shaping it all
        float rgb[3] = {192.0f / 255.0f * kRainUnlit, 192.0f / 255.0f * kRainUnlit, 192.0f / 255.0f * kRainUnlit};
        reach.clear();
        if (!lights.empty() && omk::lightReach(lead, lights, reach) > 0) {
            ++litDrops;
            for (std::size_t i = 0; i + 7 < reach.size(); i += 8) {
                const float k = std::sqrt(reach[i] * reach[i] + reach[i + 1] * reach[i + 1] + reach[i + 2] * reach[i + 2]);
                const int t = static_cast<int>(std::min(255.0f, k));
                for (int c = 0; c < 3; ++c)
                    rgb[c] += kRainLit * omk::lightRamp(static_cast<std::uint8_t>(reach[i + 4 + c]), t) / 255.0f;
            }
        }
        // past white, scaled down whole rather than clipped per channel: the
        // burner's drops stay orange instead of going white
        const float top = std::max({rgb[0], rgb[1], rgb[2], 1.0f});
        for (float& c : rgb) c = c / top * s * fade;
        const float W = static_cast<float>(kStreakW), H = static_cast<float>(kStreakH);
        const float l0[3] = {lead[0] - side[0] * w, lead[1] - side[1] * w, lead[2] - side[2] * w};
        const float l1[3] = {lead[0] + side[0] * w, lead[1] + side[1] * w, lead[2] + side[2] * w};
        const float t0[3] = {trail[0] - side[0] * w, trail[1] - side[1] * w, trail[2] - side[2] * w};
        const float t1[3] = {trail[0] + side[0] * w, trail[1] + side[1] * w, trail[2] + side[2] * w};
        const omk::Corner c0 = corner(l0, rgb, 0.0f, 0.0f), c1 = corner(l1, rgb, W, 0.0f);
        const omk::Corner c2 = corner(t0, rgb, 0.0f, H), c3 = corner(t1, rgb, W, H);
        rainGeo.corners.insert(rainGeo.corners.end(), {c0, c1, c3, c0, c3, c2});
    }
    const std::size_t drawnDrops = rainGeo.corners.size() / 6;
    if (frameNo % 30 == 0) {
        std::printf("frame %ld: the rain in the scene - %zu of %zu drops drawn (the weather's %zu and %d each "
                    "more), %d under a roof, %d lit by the set's lights\n", frameNo, drawnDrops, all.size(),
                    rainDrops.size(), kRainCopies, roofed, litDrops);
        omk::event("remaster.rain3d", {{"frame", frameNo}, {"drops", static_cast<long>(rainDrops.size())},
                                       {"drawn", static_cast<long>(drawnDrops)}, {"roofed", roofed},
                                       {"streak", rainTexSlot >= 0}, {"lit", litDrops}});
    }
    if (rainGeo.corners.empty()) return;
    omk::Batch b;
    b.material = -1;
    b.blend = omk::Blend::Add;
    b.start = 0;
    b.count = rainGeo.corners.size();
    rainGeo.batches.push_back(b);
    ++rainGeo.revision;
    // the streak's pool slot (`remasterPoolRain`) - or, before the pool has
    // it, a slot past every texture, which binds the backends' white; the
    // additive state bits as the mesh path's (`keyOf`, 0x2100)
    const std::uint32_t slot = rainTexSlot >= 0 ? static_cast<std::uint32_t>(rainTexSlot) : 0xFFFEu;
    omk::Draw dr;
    dr.bucketKey = omk::withPoolSlot(0x2100u | (slot & 0x3Fu), slot);
    dr.geo = &rainGeo;
    dr.start = 0;
    dr.count = rainGeo.corners.size();
    dr.blend = omk::Blend::Add;
    list.push_back(dr);
}

#endif  // OMK_REMASTER
