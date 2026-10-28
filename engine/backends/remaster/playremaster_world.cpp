// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER'S WORLD (`todo/remaster.md`) - what the remaster adds to a set
// as it is prepared (on a load's job or the preload's) and to the bodies
// drawn in it; moved out of the shared frontend so a build without
// OMK_REMASTER carries none of it (`backends/sdl/playremaster_off.cpp`).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "o3de/colourgrade.h"
#include "o3de/emitterlight.h"
#include "o3de/floorreflect.h"
#include "o3de/wetfloor.h"
#include "o3de/texpack.h"
#include "platform/env.h"
#include "platform/events.h"
#include "platform/filmpack.h"
#include "platform/movie.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

// a set's water and ground, its floors, probes and glowing surfaces - on the
// thread `prepareSet` runs on, after the set's own lights are read
void PlayState::remasterPrepareSet(SetLoad& L) {
    WorldSlot& w = L.out;
    // THE PACK'S DECODE, here on the load's thread rather than on the frame
    // the set's pool reaches the renderer (`TexturePack::prepare`; the audit's
    // performance item 1, 2026-10-23) - a preloaded set's and an arriving one's
    if (world_ && world_->takesPack() && omk::texturePack().shown()) {
        std::vector<const omk::Texture*> all;
        for (const auto& wt : w.tex) all.push_back(&wt);
        omk::texturePack().prepare(all);
    }
    w.water = omk::waterPlane(w.geo);
    w.ground = omk::groundLevel(w.geo);
    // what the START armed, not what runs: a set loaded in the original mode
    // (part B's switch) has its floors, probes and glows for the remaster's
    w.floors = remasterArmed("floors") > 0.0f ? omk::floorPlanes(w.geo, w.tex, omk::floorList(), L.stem)
                                              : omk::FloorPlanes{};
    // ...and its floors under the OPEN SKY, for the rain to wet
    // (`o3de/wetfloor.h`): the shot soup's roofs, the original's own
    if (remasterArmed("wet") > 0.0f) {
        const auto t0 = std::chrono::steady_clock::now();
        // `OMK_NO_WET_WALLS=1`, an INSTRUMENT: the floors alone, no wall wet
        // (`verify.py: engine: wet floors`)
        static const bool noWalls = omk::envFlag("OMK_NO_WET_WALLS");
        w.wet = omk::wetFloors(w.geo, w.shotSoup, w.shotRunBoxes, !noWalls);
        // the open sky per corner, for the shaders to blend - `OMK_WET_NO_BLEND=1`,
        // an INSTRUMENT, leaves it out (`verify.py: engine: wet floors`)
        static const bool noBlend = omk::envFlag("OMK_WET_NO_BLEND");
        if (!noBlend) omk::encodeOpenSky(w.geo, w.wet);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("wet: set %s - %zu floor triangles under the open sky (%.0f m2), %zu covered (%.0f m2), "
                    "%zu levels; %zu wall triangles the rain reaches (%.0f m2); %zu drips off roofs' edges; %.0f ms\n",
                    L.stem.c_str(), w.wet.openTris, w.wet.openArea / 1550.0, w.wet.coveredTris,
                    w.wet.coveredArea / 1550.0, w.wet.planes.levels.size(), w.wet.wallTris, w.wet.wallArea / 1550.0,
                    w.wet.drips.size(), ms);
        omk::event("world.wet", {{"set", L.stem}, {"open", static_cast<long>(w.wet.openTris)},
                                 {"covered", static_cast<long>(w.wet.coveredTris)},
                                 {"blended", static_cast<long>(w.wet.blendedTris)},
                                 {"levels", static_cast<long>(w.wet.planes.levels.size())},
                                 {"walls", static_cast<long>(w.wet.wallTris)},
                                 {"drips", static_cast<long>(w.wet.drips.size())},
                                 {"dripsCovered", static_cast<long>(w.wet.dripsCovered)}});
    } else {
        w.wet = omk::WetFloors{};
    }
    if (remasterArmed("bodylight") > 0.0f) {
        w.probes.build(w.geo);
        omk::event("world.probes", {{"set", L.stem}, {"mean", static_cast<double>(w.probes.meanLuma())}});
    } else {
        w.probes = omk::LightProbes{};
    }
    // THE GLOWING SURFACES' LIGHT (`o3de/emitterlight.h` surfaceLights,
    // the remaster's): the pack's light maps over the set's triangles - its
    // neon letters, its lit windows - after the set's own lights
    w.fxSurface.clear();
    w.fxIn = false;
    const float fxArmed = remasterArmed("fxlight");
    if (fxArmed > 0.0f && world_ && world_->drawsEmitterLights() && omk::texturePack().active() &&
        omk::texturePack().lightMaps() > 0) {
        std::vector<const omk::Texture*> ptrs;
        for (const auto& wt : w.tex) ptrs.push_back(&wt);
        // the LIGHT MAPS alone: the colours are the shipped palette's
        const auto packs = omk::texturePack().replacements(ptrs, false);
        int cells = 0;
        w.fxSurface = omk::surfaceLights(w.geo, w.tex, packs, fxArmed, &cells, L.stem, &omk::surfaceCuration());
        // in the table while the remaster shows (`remasterSlotLights` keeps it so)
        if (fxLight > 0.0f) {
            w.lights.insert(w.lights.end(), w.fxSurface.begin(), w.fxSurface.end());
            w.fxIn = true;
        }
        std::printf("fx light: %s - %d glowing surfaces light the bodies and the set (%s)\n", L.stem.c_str(), cells,
                    omk::surfaceCuration().covers(L.stem) ? "the curated list" : "the automatic rules");
        omk::event("world.fxsurfaces", {{"set", L.stem}, {"lights", cells},
                                        {"curated", omk::surfaceCuration().covers(L.stem)}});
    }
}

// THE EMITTERS' LIGHT (`o3de/emitterlight.h`): the set's table rebuilt as its
// own lights, its glowing surfaces and a point light a glowing emitter, so
// every path that lights a body takes them - when the scene binds its emitters
// (`remasterSlotLights`, `playremaster_mode.cpp`: the switch rebuilds it too)
void PlayState::remasterEmitterLights(WorldSlot& ws) {
    if (fxLight > 0.0f && world_ && world_->drawsEmitterLights()) remasterSlotLights(ws, true);
}

// THE CITY GRADE (`o3de/colourgrade.h`): chosen for the set under the
// player's feet, and off while the greyscale bank is on - a grade would tint
// a black-and-white cutscene. A hard switch: a change of set comes with the
// game's own fade.
//
// ...and THE SHOWER'S SKY (the remaster's, with the 3D rain - `[Remaster]
// rain3d`): while the game's rain falls the grade darkens, greys and cools
// with it - `overcastLut`, over the city's own or, where it has none, over
// a plain one made for the while - rebuilt in `kOvercastSteps` steps as the
// shower ramps and fades (20 s and 10 s, `o3de/weather.h`).
namespace {
constexpr int kOvercastSteps = 16;
constexpr int kOvercastSide = 17;          // a plain grade's side, where the city has none
constexpr float kOvercastDark = 0.08f;     // the darkening at full rain (0.20 made the street too dark - the reader)
constexpr float kOvercastGrey = 0.30f;     // ...the saturation it takes away
constexpr float kOvercastCool = 0.03f;     // ...and the blue it adds over the red

// ...and THE SAND's DUSTY SKY (the restored weather's): toward an ochre of the
// pixel's own luma, a little flattened and lifted - the air full of sand
constexpr float kDustTint = 0.20f;         // how far toward the ochre at the storm's height (0.40 before the dust fog)
constexpr float kDustFlat = 0.12f;         // ...the contrast it takes, the floor it lifts

// `base` (n^3 triples, red fastest; null: the identity) under an overcast of
// `o` (0..1) into `out` - or, `dust`, the sand's ochre haze
void overcastLut(const omk::ColourLut* base, int n, float o, std::vector<float>& out, bool dust = false) {
    out.resize(static_cast<std::size_t>(n) * n * n * 3);
    for (int b = 0, i = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r, ++i) {
                float c[3];
                if (base) for (int k = 0; k < 3; ++k) c[k] = base->rgb[static_cast<std::size_t>(3 * i + k)];
                else c[0] = r / float(n - 1), c[1] = g / float(n - 1), c[2] = b / float(n - 1);
                const float l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                if (dust) {
                    const float ochre[3] = {1.12f, 0.96f, 0.72f};
                    for (int k = 0; k < 3; ++k) {
                        c[k] += (l * ochre[k] - c[k]) * kDustTint * o;
                        c[k] = c[k] * (1.0f - kDustFlat * o) + kDustFlat * o * 0.45f * ochre[k];
                    }
                    for (int k = 0; k < 3; ++k) out[static_cast<std::size_t>(3 * i + k)] = std::clamp(c[k], 0.0f, 1.0f);
                    continue;
                }
                for (int k = 0; k < 3; ++k) c[k] = (l + (c[k] - l) * (1.0f - kOvercastGrey * o)) * (1.0f - kOvercastDark * o);
                c[0] -= kOvercastCool * o * l;
                c[2] += kOvercastCool * o * l;
                for (int k = 0; k < 3; ++k) out[static_cast<std::size_t>(3 * i + k)] = std::clamp(c[k], 0.0f, 1.0f);
            }
}
}  // namespace

void PlayState::remasterGrade() {
    auto& session = *session_;
    omk::Renderer& world = *world_;
    // the shower's step: the rain's drops over 200, while the 3D rain shows
    const bool shown = !session.renderGrey() && remasterShown_;
    // ...the rain's, or the restored sand's (its dusty sky - a negative step)
    const bool dust = weatherSandLevel > 0.0f;
    const int mag = shown && rain3dStrength > 0.0f
        ? static_cast<int>(std::lround(std::clamp((dust ? weatherSandLevel : weatherRain) * rain3dStrength,
                                                  0.0f, 1.0f) * kOvercastSteps)) : 0;
    const int step = dust ? -mag : mag;
    if (!omk::cityGrades().active() && step == 0 && gradeOvercast == 0) return;
    // none in the original mode (part B's switch)
    const std::string want = shown ? worldSet : std::string();
    if (want == gradeFor && step == gradeOvercast) return;
    const omk::ColourLut* lut = want.empty() || !omk::cityGrades().active() ? nullptr : omk::cityGrades().forSet(want);
    bool took;
    if (step != 0) {
        static std::vector<float> rgb;
        const int n = lut ? lut->n : kOvercastSide;
        overcastLut(lut, n, static_cast<float>(std::abs(step)) / kOvercastSteps, rgb, step < 0);
        took = world.setGrade(rgb.data(), n);
    } else {
        took = world.setGrade(lut ? lut->rgb.data() : nullptr, lut ? lut->n : 0);
    }
    if (want != gradeFor || (step != 0) != (gradeOvercast != 0)) {
        const std::string src = lut ? omk::cityGrades().sourceFor(want) : "";
        std::printf("frame %ld: grade %s for %s%s%s\n", frameNo, src.empty() ? "none" : src.c_str(),
                    want.empty() ? "the greyscale bank" : want.c_str(),
                    step > 0 ? " - under the shower's sky" : step < 0 ? " - in the sand's dusty air" : "",
                    took ? "" : " - REFUSED, this renderer has no grade");
    }
    omk::event("remaster.grade", {{"set", want}, {"lut", lut ? omk::cityGrades().sourceFor(want) : ""},
                                  {"taken", took}, {"overcast", step}});
    gradeFor = want;
    gradeOvercast = step;
}

// the GREYSCALE BANK through the texture pack: told where a grey texture came
// from, the pack greys the colour replacement (`TexturePack::aliasGrey`)
void PlayState::remasterAliasGrey(const omk::Texture& colour, const omk::Texture& grey) {
    if (omk::texturePack().active()) omk::texturePack().aliasGrey(colour, grey);
}

// the scene viewer's TEXTURE PACK: its flag alone
void PlayState::remasterSceneViewer() {
    if (!texturePackFlag.empty()) omk::openTexturePack(texturePackFlag);
}

// a GPU renderer's remaster requests (GLES; Vulkan, desktop only, asks its own)
void PlayState::remasterConfigure(omk::Renderer& r) {
    // the pack's NORMAL MAPS only for a renderer that draws the relief: GLES
    // has none, and a set's load decoded and parked them for nothing - the
    // ports' memory (`remasterSetup` asked them for the relief at start)
    if (relief > 0.0f && !r.drawsRelief()) {
        omk::texturePack().setNormals(false);
        std::printf("relief: not drawn by this renderer - the pack's normal maps are not decoded\n");
    }
    if (relief > 0.0f)
        omk::event("texpack.normals", {{"on", omk::texturePack().normals()}, {"relief", r.drawsRelief()}});
    if (upscale > 1.0f) r.setUpscale(upscale);   // in the supersample's place
    if (bloomStrength > 0.0f) r.setBloom(bloomStrength);
    // the floors, listed or wet, ride on the water's pass - and the MIST, the
    // shower's haze with it, on GLES takes the same variant of the programs
    const float mistWant = mistStrength > 0.0f || rain3dStrength > 0.0f ? 1.0f : 0.0f;
    if (std::max({waterStrength, floorStrength, wetStrength, mistWant}) > 0.0f)
        r.setWater(std::max({waterStrength, floorStrength, wetStrength, mistWant}));
    if (bodyLight > 0.0f) r.setBodyTint(true);
    if (fxLight > 0.0f) r.setEmitterLights(true);
}

// THE FILM PACK (`platform/filmpack.h`): the same film upscaled, the same
// sound - played by the same decoder, smoothed to the display
bool PlayState::remasterOpenFilm(const std::string& real, omk::Movie& mov, const char* name) {
    if (!remasterShown_) return false;   // the original mode: the shipped film (part B's switch)
    const std::string hd = omk::filmPack().find(real);
    if (hd.empty() || !mov.open(hd)) return false;
    mov.setSmooth(true);
    std::printf("  %s: from the film pack, %s - a REMASTER feature\n", name, hd.c_str());
    omk::event("film.pack", {{"film", name}, {"w", mov.info().width}, {"h", mov.info().height}});
    return true;
}

// A STRENGTH MOVED BY OMK'S OPTIONS MENU (`todo/graphics-menu.md` 3a), on the
// running game. The rule: a strength that was ON AT START moves at once, 0
// included - its resources were loaded (the light maps, the normal maps, the
// floors' list, the probes) and its pass exists; one that was 0 waits for the
// next start, which loads them. The mist needs nothing loaded and moves
// always, and the preload radius too (its phase loads what it needs). The emitters' light waits too: its strength is baked into each
// set's surface lights when the set loads (`remasterPrepareSet`).
bool PlayState::remasterApplyLive(const std::string& key, float v) {
    if (remasterAtStart_.empty()) return false;   // the remaster did not run
    // THE MODE (part B, `playremaster_mode.cpp`): the switch itself
    if (key == "mode") return remasterShow(v > 0.5f, "OMK's options");
    const auto at = remasterAtStart_.find(key);
    const bool armed = at != remasterAtStart_.end() && at->second > 0.0f;
    // ...and while the ORIGINAL shows, a strength moved is held for the
    // remaster's return - one it can take (armed, or the mist)
    if (!remasterShown_ && remasterStrength(key)) {
        if (!armed && key != "mist") return false;
        remasterHeld_[key] = v;
        return true;
    }
    if (key == "mist") { mistStrength = v; return true; }
    // the PRELOAD RADIUS is read by its phase each frame, which also loads
    // what it needs: any value moves - to 0, what it prepared is released
    if (key == "preload") {
        if (v <= 0.0f && preloadMetres > 0.0f) preloadReleaseAll();
        preloadMetres = v;
        return true;
    }
    if (!armed) return false;
    if (key == "bloom") {
        if (!world_ || !world_->setBloom(v)) return false;
        bloomStrength = v;
        return true;
    }
    if (key == "water")     { waterStrength = v; return true; }
    if (key == "floors")    { floorStrength = v; return true; }
    if (key == "wet")       { wetStrength = v; return true; }
    if (key == "rain3d")    { rain3dStrength = v; return true; }
    if (key == "relief")    { relief = v; return true; }
    if (key == "bodylight") { bodyLight = v; return true; }
    if (key == "ao") {
        if (!world_ || !world_->setAmbientOcclusion(v)) return false;
        aoStrength = v;
        return true;
    }
    return false;
}

#endif  // OMK_REMASTER
