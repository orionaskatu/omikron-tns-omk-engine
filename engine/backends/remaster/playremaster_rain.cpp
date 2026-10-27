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
//
// ...and THE SNOW too (the reader's "ok, continue", 2026-10-27): Mahaleel's
// 200 flakes (`sub_481D90`'s bit-4 arm - the box of 400, the drift, all the
// original's) as quads facing the eye, the original's 8 units across, with
// its own `Images\snow.bmp` - a 64x64 8-bit picture keyed on BLACK, which an
// additive draw adds nothing of, so the key holds by itself - lit and faded
// as the rain, three copies a flake. A flake under a roof is not drawn: the
// ice cave's flakes, which the original re-scatters every frame there once it
// has snowed (docs/ASSETS.md 4e - a static), are gone in the remaster.
//
// ...and THE RESTORED SAND (`[Restored] weather`, todo/restore-weather.md):
// type 3's drops, one grain of `Images\sand.bmp` each - cut from the field,
// as a short soft streak along the wind in that grain's own colour (OMK's
// design - grains drawn as dots read as snow), additive, lit by the set's
// lights, faded in as the snow, three copies a drop; and the shower's DUSTY
// sky (`remasterGrade`).
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
constexpr float kSnowBox = 400.0f;        // the snow's box (`sub_481D90`'s bit-4 arm)
constexpr float kSandBox = 300.0f;        // ...and the sand's (its else branch)
constexpr float kSandStreak = 15.0f;      // ...a grain's streak: half a frame of the arm's 31 units of wind
constexpr float kSandGlow = 0.8f;         // ...its brightness, additive
constexpr float kFlakeHalf = 4.0f;        // ...a flake's half size, the original's 8 units across
constexpr float kSnowNearFrom = 20.0f, kSnowNear = 80.0f;   // a flake's fade in from the eye

// `Images\snow.bmp`, the original's flake: an 8-bit BMP, bottom-up, its
// palette as stored - an empty Texture on anything else
omk::Texture snowTexture(std::span<const std::byte> d) {
    omk::Texture t;
    const auto u8 = [&](std::size_t o) { return o < d.size() ? static_cast<std::uint8_t>(d[o]) : std::uint8_t{0}; };
    const auto u32 = [&](std::size_t o) { return u8(o) | u8(o + 1) << 8 | u8(o + 2) << 16 | static_cast<std::uint32_t>(u8(o + 3)) << 24; };
    if (d.size() < 54 || u8(0) != 'B' || u8(1) != 'M' || (u8(28) | u8(29) << 8) != 8) return t;
    const std::uint32_t off = u32(10), hdr = u32(14);
    const int w = static_cast<int>(u32(18)), h = static_cast<int>(u32(22));
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return t;
    const std::size_t stride = (static_cast<std::size_t>(w) + 3) & ~std::size_t{3};
    if (off + stride * h > d.size()) return t;
    t.name = "IMAGES/SNOW.BMP";
    t.width = w; t.height = h; t.bpp = 8; t.exact = true;
    t.pal.assign(768, 0);
    std::uint8_t* pal = t.pal.mutableData();
    for (int i = 0; i < 256; ++i) {
        const std::size_t e = 14 + hdr + 4u * static_cast<std::size_t>(i);   // B, G, R, 0
        if (e + 3 > off) break;
        pal[3 * i] = u8(e + 2); pal[3 * i + 1] = u8(e + 1); pal[3 * i + 2] = u8(e);
    }
    t.idx.assign(static_cast<std::size_t>(w) * h, 0);
    std::uint8_t* px = t.idx.mutableData();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            px[static_cast<std::size_t>(y) * w + x] = u8(off + stride * static_cast<std::size_t>(h - 1 - y) + x);
    return t;
}
}  // namespace

// the streak's texture into the frame's pool, after the sprites, while the
// rain in 3D was armed at the start
void PlayState::remasterPoolRain(std::vector<omk::Texture>& slots) {
    rainTexSlot = -1;
    if (remasterArmed("rain3d") <= 0.0f) return;
    static const omk::Texture streak = rainStreakTexture();
    rainTexSlot = static_cast<int>(slots.size());
    slots.push_back(streak);
    // ...and the snow's flake, the original's own picture
    static const omk::Texture flake = snowTexture(fs_->read("IMAGES/snow.bmp"));
    snowTexSlot = -1;
    if (flake.hasPixels()) {
        snowTexSlot = static_cast<int>(slots.size());
        slots.push_back(flake);
    }
    // ...and the restored sand's field, cut into its grains (a texel is solid
    // where its palette colour is not black - the key)
    sandTexSlot = -1;
    if (restoredWeather) {
        static const omk::Texture field = snowTexture(fs_->read("IMAGES/sand.bmp"));
        if (field.hasPixels()) {
            sandTexSlot = static_cast<int>(slots.size());
            slots.push_back(field);
            if (sandGrains3d.empty()) {
                sandGrains3d = omk::grainsOf(field.width, field.height, [&](int x, int y) {
                    const std::uint8_t* c = field.pal.data() + 3u * field.idx.data()[y * field.width + x];
                    return c[0] != 0 || c[1] != 0 || c[2] != 0;
                });
                // ...and each grain's own colour: its solid texels' mean
                sandGrainColour3d.clear();
                for (const auto& g : sandGrains3d) {
                    float sum[3] = {0, 0, 0};
                    int n = 0;
                    for (int y = g.y; y < g.y + g.h; ++y)
                        for (int x = g.x; x < g.x + g.w; ++x) {
                            const std::uint8_t* c = field.pal.data() + 3u * field.idx.data()[y * field.width + x];
                            if (!c[0] && !c[1] && !c[2]) continue;
                            for (int k = 0; k < 3; ++k) sum[k] += c[k] / 255.0f;
                            ++n;
                        }
                    sandGrainColour3d.push_back({n ? sum[0] / n : 0.8f, n ? sum[1] / n : 0.7f, n ? sum[2] / n : 0.5f});
                }
            }
        }
    }
}

// whether the cycle keeps a shower going under a roof: the 3D weather shows
// (it stops the drops at the roof itself)
bool PlayState::remasterWeatherRoofless() const {
    return rain3dStrength > 0.0f && remasterShown_ && world_ && (world_->takes() & omk::kTakesWater);
}

bool PlayState::remasterRainTake(const std::vector<omk::WeatherDrop>& drops, unsigned viewBits) {
    // the rain (bit 2) or the snow (bit 4); the third type, the sand (bit 8),
    // with the restored weather alone
    const bool sandToo = (viewBits & 8u) && restoredWeather;
    if (!((viewBits & 6u) || sandToo) || rain3dStrength <= 0.0f || !remasterShown_ || !world_ ||
        !(world_->takes() & omk::kTakesWater))
        return false;
    rainDrops = drops;
    rainBits = viewBits;
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
    const bool snow = (rainBits & 2u) == 0 && (rainBits & 4u) != 0;   // the snow's arm: bit 4 alone
    const bool sand = (rainBits & 6u) == 0;                            // the restored sand: bit 8
    if (sand && (sandTexSlot < 0 || sandGrains3d.empty())) return;
    const float box = snow ? kSnowBox : sand ? kSandBox : kRainBox;
    const float s = std::clamp(rain3dStrength, 0.0f, 1.0f) * (snow ? 1.0f : kRainGlow);
    // the box's centre as the weather places it: a third of its size ahead
    const float dv[3] = {v.cam.at[0] - v.cam.eye[0], v.cam.at[1] - v.cam.eye[1], v.cam.at[2] - v.cam.eye[2]};
    const float dl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
    if (!(dl > 0.0f)) return;
    float centre[3];
    for (int k = 0; k < 3; ++k) centre[k] = v.cam.eye[k] + dv[k] / dl * box / 3.0f;
    // the original's drops and their copies, wrapped into the box
    static std::vector<omk::WeatherDrop> all;
    all.assign(rainDrops.begin(), rainDrops.end());
    for (const auto& d : rainDrops)
        for (const auto& o : kRainOffsets) {
            omk::WeatherDrop c = d;
            for (int k = 0; k < 3; ++k) {
                float rel = d.p[k] - centre[k] + o[k] * box / kRainBox;
                rel -= box * std::floor((rel + box * 0.5f) / box);
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
        if (sand) {
            // THE GRAIN, BLOWN: a short soft streak along the wind - the arm's
            // own course, x back 0.866 and y down 0.3 a frame (Y down) - half a
            // frame of it long, the rain's streak texture tinted with this
            // grain's own colour in the field (the reader: dots drifting read as
            // "yellow/brown snow", 2026-10-27)
            const std::size_t gi = static_cast<std::size_t>(&d - all.data()) % sandGrains3d.size();
            const float* gc = sandGrainColour3d[gi].data();
            const float a[3] = {-0.9446f, 0.3272f, 0.0f};   // (-0.866, 0.3, 0), unit
            const float lead[3] = {d.p[0], d.p[1], d.p[2]};
            const float trail[3] = {lead[0] - a[0] * kSandStreak, lead[1] - a[1] * kSandStreak, lead[2]};
            const float e[3] = {v.cam.eye[0] - lead[0], v.cam.eye[1] - lead[1], v.cam.eye[2] - lead[2]};
            float side[3] = {a[1] * e[2] - a[2] * e[1], a[2] * e[0] - a[0] * e[2], a[0] * e[1] - a[1] * e[0]};
            const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            if (!(sl > 0.0f)) continue;
            for (float& k : side) k /= sl;
            const float off[3] = {lead[0] + side[0], lead[1] + side[1], lead[2] + side[2]};
            const omk::Projected pa = omk::project(v.cam, lead), pb = omk::project(v.cam, off);
            if (pa.z <= 0.0f || pb.z <= 0.0f) continue;
            const float ppu = std::hypot(pb.x - pa.x, pb.y - pa.y);
            if (!(ppu > 0.0f)) continue;
            const float w = halfPx / ppu;
            const float nearT = std::clamp((pa.z - kSnowNearFrom) / (kSnowNear - kSnowNearFrom), 0.0f, 1.0f);
            const float fade = nearT * nearT * (3.0f - 2.0f * nearT) *
                               std::clamp((box - pa.z) / (box * 0.4f), 0.0f, 1.0f);
            if (!(fade > 0.0f)) continue;
            // the grain's colour, under the lights that reach it (the rain's rule)
            float rgb[3] = {gc[0] * kRainUnlit * 1.6f, gc[1] * kRainUnlit * 1.6f, gc[2] * kRainUnlit * 1.6f};
            reach.clear();
            if (!lights.empty() && omk::lightReach(lead, lights, reach) > 0) {
                ++litDrops;
                for (std::size_t i = 0; i + 7 < reach.size(); i += 8) {
                    const float k = std::sqrt(reach[i] * reach[i] + reach[i + 1] * reach[i + 1] + reach[i + 2] * reach[i + 2]);
                    const int t = static_cast<int>(std::min(255.0f, k));
                    for (int c = 0; c < 3; ++c)
                        rgb[c] += kRainLit * gc[c] * omk::lightRamp(static_cast<std::uint8_t>(reach[i + 4 + c]), t) / 255.0f;
                }
            }
            const float top = std::max({rgb[0], rgb[1], rgb[2], 1.0f});
            for (float& c : rgb) c = c / top * std::clamp(rain3dStrength, 0.0f, 1.0f) * kSandGlow * fade;
            const float W = static_cast<float>(kStreakW), H = static_cast<float>(kStreakH);
            const float l0[3] = {lead[0] - side[0] * w, lead[1] - side[1] * w, lead[2] - side[2] * w};
            const float l1[3] = {lead[0] + side[0] * w, lead[1] + side[1] * w, lead[2] + side[2] * w};
            const float t0[3] = {trail[0] - side[0] * w, trail[1] - side[1] * w, trail[2] - side[2] * w};
            const float t1[3] = {trail[0] + side[0] * w, trail[1] + side[1] * w, trail[2] + side[2] * w};
            const omk::Corner c0 = corner(l0, rgb, 0.0f, 0.0f), c1 = corner(l1, rgb, W, 0.0f);
            const omk::Corner c2 = corner(t0, rgb, 0.0f, H), c3 = corner(t1, rgb, W, H);
            rainGeo.corners.insert(rainGeo.corners.end(), {c0, c1, c3, c0, c3, c2});
            continue;
        }
        if (snow) {
            // THE FLAKE: a quad facing the eye, the original's 8 units across
            const float fwd[3] = {dv[0] / dl, dv[1] / dl, dv[2] / dl};
            const float upW[3] = {0.0f, -1.0f, 0.0f};   // Y points down
            float r[3] = {fwd[1] * upW[2] - fwd[2] * upW[1], fwd[2] * upW[0] - fwd[0] * upW[2],
                          fwd[0] * upW[1] - fwd[1] * upW[0]};
            const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (!(rl > 0.0f)) continue;
            for (float& k : r) k /= rl;
            const float u[3] = {r[1] * fwd[2] - r[2] * fwd[1], r[2] * fwd[0] - r[0] * fwd[2],
                                r[0] * fwd[1] - r[1] * fwd[0]};
            const omk::Projected pf = omk::project(v.cam, d.p);
            if (pf.z <= 0.0f) continue;
            const float nearT = std::clamp((pf.z - kSnowNearFrom) / (kSnowNear - kSnowNearFrom), 0.0f, 1.0f);
            const float fade = nearT * nearT * (3.0f - 2.0f * nearT) *
                               std::clamp((box - pf.z) / (box * 0.4f), 0.0f, 1.0f);
            if (!(fade > 0.0f)) continue;
            float rgb[3] = {kRainUnlit, kRainUnlit, kRainUnlit};
            reach.clear();
            if (!lights.empty() && omk::lightReach(d.p, lights, reach) > 0) {
                ++litDrops;
                for (std::size_t i = 0; i + 7 < reach.size(); i += 8) {
                    const float k = std::sqrt(reach[i] * reach[i] + reach[i + 1] * reach[i + 1] + reach[i + 2] * reach[i + 2]);
                    const int t = static_cast<int>(std::min(255.0f, k));
                    for (int c = 0; c < 3; ++c)
                        rgb[c] += kRainLit * omk::lightRamp(static_cast<std::uint8_t>(reach[i + 4 + c]), t) / 255.0f;
                }
            }
            const float top = std::max({rgb[0], rgb[1], rgb[2], 1.0f});
            for (float& c : rgb) c = c / top * s * fade;
            const float h = kFlakeHalf, T = 64.0f;
            float q[4][3];
            for (int k = 0; k < 3; ++k) {
                q[0][k] = d.p[k] - r[k] * h - u[k] * h;
                q[1][k] = d.p[k] + r[k] * h - u[k] * h;
                q[2][k] = d.p[k] + r[k] * h + u[k] * h;
                q[3][k] = d.p[k] - r[k] * h + u[k] * h;
            }
            const omk::Corner c0 = corner(q[0], rgb, 0.0f, T), c1 = corner(q[1], rgb, T, T);
            const omk::Corner c2 = corner(q[2], rgb, T, 0.0f), c3 = corner(q[3], rgb, 0.0f, 0.0f);
            rainGeo.corners.insert(rainGeo.corners.end(), {c0, c1, c2, c0, c2, c3});
            continue;
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
                           std::clamp((box - pa.z) / (box * 0.4f), 0.0f, 1.0f);
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
        std::printf("frame %ld: the %s in the scene - %zu of %zu %s drawn (the weather's %zu and %d each "
                    "more), %d under a roof, %d lit by the set's lights\n", frameNo,
                    snow ? "snow" : sand ? "sand" : "rain", drawnDrops, all.size(),
                    snow ? "flakes" : sand ? "grains" : "drops", rainDrops.size(), kRainCopies, roofed,
                    litDrops);
        omk::event("remaster.rain3d", {{"frame", frameNo}, {"drops", static_cast<long>(rainDrops.size())},
                                       {"drawn", static_cast<long>(drawnDrops)}, {"roofed", roofed},
                                       {"streak", (snow ? snowTexSlot : rainTexSlot) >= 0}, {"lit", litDrops},
                                       {"snow", snow}, {"sand", sand}});
    }
    if (rainGeo.corners.empty()) return;
    omk::Batch b;
    b.material = -1;
    b.blend = omk::Blend::Add;   // the rain, the snow and the sand's streaks additive
    b.start = 0;
    b.count = rainGeo.corners.size();
    rainGeo.batches.push_back(b);
    ++rainGeo.revision;
    // the streak's pool slot (`remasterPoolRain`) - or, before the pool has
    // it, a slot past every texture, which binds the backends' white; the
    // additive state bits as the mesh path's (`keyOf`, 0x2100)
    const int texSlot = snow ? snowTexSlot : rainTexSlot;   // the sand: the rain's streak, tinted
    const std::uint32_t slot = texSlot >= 0 ? static_cast<std::uint32_t>(texSlot) : 0xFFFEu;
    omk::Draw dr;
    dr.bucketKey = omk::withPoolSlot(0x2100u | (slot & 0x3Fu), slot);
    dr.geo = &rainGeo;
    dr.start = 0;
    dr.count = rainGeo.corners.size();
    dr.blend = omk::Blend::Add;
    list.push_back(dr);
}

#endif  // OMK_REMASTER
