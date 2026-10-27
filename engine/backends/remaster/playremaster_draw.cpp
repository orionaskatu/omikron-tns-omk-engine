// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER'S DRAW (`todo/remaster.md`) - the mist, the emitters' point
// lights (A5b/A5c) with the night and the relief, and the water and floor
// reflections (A3); cut out of `playframe_world_draw.cpp`'s phases so a build
// without OMK_REMASTER carries none of it (`backends/sdl/playremaster_off.cpp`).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "o3de/colourgrade.h"
#include "o3de/emitterlight.h"
#include "o3de/floorreflect.h"
#include "o3de/wetfloor.h"
#include "o3de/lighttiles.h"
#include "platform/env.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// THE MIST (`View::mist`): on the shown set's ground
void PlayState::remasterViewMist(omk::View& v) {
    // ...thickened by the SHOWER while the 3D rain shows (the shower's sky,
    // `remasterGrade`): the city's fog colour closing in as it rains
    v.mist = std::min(1.0f, mistStrength + (rain3dStrength > 0.0f && remasterShown_
                                                ? 0.3f * std::clamp(weatherRain * rain3dStrength, 0.0f, 1.0f) : 0.0f));
    v.mistGround = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)].ground;
    // ...and the RAIN's ripples on the wet floors (`o3de/wetfloor.h`): the
    // shower's strength, which is 0 outside a rain area
    v.rain = wetStrength > 0.0f ? weatherRain : 0.0f;
}

// THE EMITTERS' LIGHTS under per-pixel lighting: the night, the relief, the
// reach - `fxPix` whether they light at all, `kFxView` how far they are taken;
// -> the slots they take among the frame's lights (0 when TILED: they go to
// `View::points` instead)
int PlayState::remasterViewLights(omk::View& v, bool& fxPix, float& kFxView) {
    // they light the SET too (mode 3, `remasterLitSet`), so they are chosen by
    // the CAMERA - the nearest slots within the reach, faded in over its last
    // quarter so none pops - and the set's own lights, nearest the player as
    // before, take the slots left
    fxPix = fxLight > 0.0f && world_ && world_->drawsEmitterLights();
    // ...and THE NIGHT (`CityGrades::nightOf`): how much of the set's painted
    // light stays under them
    v.setDim = fxPix && omk::cityGrades().active() ? omk::cityGrades().nightOf(worldSet) : 1.0f;
    if (v.setDim != nightWas) {
        nightWas = v.setDim;
        std::printf("frame %ld: the night %.2f for %s - the set's painted light kept under the emitters' "
                    "lights (`night.txt`)\n", frameNo, static_cast<double>(v.setDim), worldSet.c_str());
        omk::event("remaster.night", {{"set", worldSet}, {"dim", static_cast<double>(v.setDim)}});
    }
    // ...and on a renderer that TILES them (`o3de/lighttiles.h`), every one
    // within the game's CLIP DISTANCE (the reader, 2026-10-20: "set it
    // depending of the game distance culling setting"; 200 m at most, the
    // unlimited clip's), faded over its last fifth - no slots, no cut
    // (`OMK_NO_TILES=1`, an instrument: the nearest-slots way on a renderer
    // that tiles - to set Vulkan beside GLES, which does not yet)
    static const bool noTiles = omk::envFlag("OMK_NO_TILES");
    fxTiled = fxPix && world_->tilesLights() && !noTiles;
    v.relief = fxTiled ? relief : 0.0f;   // the relief rides the tiled module
    kFxView = fxTiled ? std::min(20000.0f, static_cast<float>(clipInches)) : 3000.0f;
    v.points.clear();
    // the point lights' slots: what the renderer has past four of the set's
    // own (60 of 64 on Vulkan and the GLES variant - 12 of 16 left most of a
    // neon street's signs unlit, "as if turned off", 2026-10-20)
    return fxTiled ? 0 : std::max(0, world_->maxPixelLights() - 4);
}

// ...and the ones picked, nearest the camera first: TILED all of them into
// `View::points` (faded over the reach's last fifth); else the nearest
// `kFxSlots`, `fxCut` the first left out
void PlayState::remasterPointLights(omk::View& v, std::vector<std::pair<float, const omk::Light3do*>>& fx,
                                    float kFxView, int kFxSlots, float& fxCut) {
    if (fxTiled) {
        for (const auto& [e2, l] : fx) {
            if (v.points.size() >= static_cast<std::size_t>(omk::kMaxPoints)) break;
            omk::View::GpuLight g{};
            for (int k = 0; k < 3; ++k) { g.pos[k] = l->pos[k]; g.dir[k] = l->dir[k]; }
            g.radiusA = l->radiusA; g.radiusB = l->radiusB;
            g.colour[0] = static_cast<float>((l->colour >> 16) & 0xFF) / 255.0f;
            g.colour[1] = static_cast<float>((l->colour >> 8) & 0xFF) / 255.0f;
            g.colour[2] = static_cast<float>(l->colour & 0xFF) / 255.0f;
            g.intensity = l->f32 * std::clamp((kFxView - std::sqrt(e2)) / (0.2f * kFxView), 0.0f, 1.0f);
            if (g.intensity > 0.0f) v.points.push_back(g);
        }
        fx.clear();
    }
    // THE CUT, without a pop: a light fades by its distance against the FIRST
    // ONE LEFT OUT - that distance moves continuously with the camera, so a
    // light leaving the nearest `kFxSlots` has faded to nothing by then
    // (ranked by distance alone, they appeared and vanished as he walked -
    // the reader, 2026-10-20)
    if (fx.size() > static_cast<std::size_t>(kFxSlots)) {
        fxCut = std::min(fxCut, std::sqrt(fx[static_cast<std::size_t>(kFxSlots)].first));
        fx.resize(static_cast<std::size_t>(kFxSlots));
    }
}

// ...and the first frame an EMITTER's light is among them (the remaster's,
// `o3de/emitterlight.h` - a point light)
void PlayState::remasterLightsTold(const omk::View& v, const std::vector<std::pair<float, const omk::Light3do*>>& near,
                                   const float at[3]) {
    if (!fxLightsTold) {
        int points = 0;
        for (const auto& [d2, l] : near) points += omk::pointLight(*l) ? 1 : 0;
        // ...or, TILED, those of `View::points` whose reach takes in the player
        for (const auto& g : v.points) {
            const float dx = at[0] - g.pos[0], dy = at[1] - g.pos[1], dz = at[2] - g.pos[2];
            points += dx * dx + dy * dy + dz * dz <= g.radiusA * g.radiusA ? 1 : 0;
        }
        if (points > 0) {
            fxLightsTold = true;
            std::printf("frame %ld: per-pixel lighting - %d of the %zu are emitters' lights\n",
                        frameNo, points, v.lights.size());
            omk::event("remaster.fxlight", {{"frame", frameNo}, {"points", points}});
        }
    }
}

// THE EMITTERS' LIGHTS ON THE SET (`o3de/emitterlight.h`): under per-pixel
// lighting the set's OPAQUE draws take the frame's point lights on top of
// their baked colour - mode 3, which skips the set's own lights (they are in
// the bake already)
void PlayState::remasterLitSet(std::vector<omk::Draw>& list) {
    if (lighting > 0 && fxLight > 0.0f && world_ && world_->drawsEmitterLights()) {
        for (auto& d : list)
            if (d.lit == 0 && d.blend == omk::Blend::Opaque && !d.meshPose &&
                (d.geo == &worldSlots[0].geo || d.geo == &worldSlots[1].geo))
                d.lit = 3;
    }
}

// THE WATER REFLECTION (`todo/remaster.md` A3), when asked for and the camera
// is above the shown slot's water; where it does not, the REFLECTING FLOORS
// (`o3de/floorreflect.h`) through the same pass - one reflection a frame.
// -> whether either drew the frame (the set's own mirror then does not)
bool PlayState::remasterDrawReflected(omk::Renderer& world, std::span<const omk::Draw> list, const omk::View& drawn) {
    auto& session = *session_;
    const WorldSlot& shownSlot = worldSlots[static_cast<std::size_t>(session.shownSlot() & 1)];
    const bool watered = waterStrength > 0.0f &&
        omk::drawWithWater(world, list, drawn, &shownSlot.geo, shownSlot.water, waterStrength);
    if (watered != waterLive) {
        waterLive = watered;
        std::printf("frame %ld: the water %s\n", frameNo,
                    watered ? "REFLECTS - the scene through the camera mirrored in its plane"
                            : "does not reflect here (no water, the camera below it, or no pass)");
        omk::event("remaster.water", {{"frame", frameNo}, {"live", watered},
                                      {"y", static_cast<double>(shownSlot.water.y)}});
    }
    // ...and where it does not, the REFLECTING FLOORS (`o3de/floorreflect.h`):
    // the listed floor's level below the eye with the most of it near, through
    // the same pass - one reflection a frame. And the WET ones
    // (`o3de/wetfloor.h`): in a RAIN area, the floors under the open sky, as
    // wet as the game's own shower is strong and drying after it
    const float wet = wetStrength > 0.0f ? wetness.step(weatherRain, frameSec * 1000.0) : 0.0f;
    const bool rainArea = weatherBitsTold != 0xFFFFu && (weatherBitsTold & 2u) != 0;
    const float wetReflect = rainArea ? wetStrength * wet * omk::kWetReflect : 0.0f;
    const float wetDarken = rainArea ? wetStrength * wet * omk::kWetDarken : 0.0f;
    const bool wetOn = wetReflect > 0.0f && shownSlot.wet.any();
    const bool listOn = floorStrength > 0.0f && shownSlot.floors.any();
    omk::WaterPlane floorPlane;
    if (!watered && listOn)
        floorPlane = omk::chooseFloor(shownSlot.floors, drawn.cam.eye);
    if (!watered && wetOn) {
        const omk::WaterPlane wp = omk::chooseFloor(shownSlot.wet.planes, drawn.cam.eye);
        if (wp.found && (!floorPlane.found || wp.area > floorPlane.area)) floorPlane = wp;
    }
    // with the rain, the pass at 1 and each run its own: the listed floors'
    // strength times `floors`, the wet ones' `wetReflect`
    static std::vector<float> listed;
    const std::vector<float>* listStrength = listOn ? &shownSlot.floors.strength : nullptr;
    float passStrength = floorStrength;
    if (wetOn) {
        passStrength = 1.0f;
        if (listOn) {
            listed.assign(shownSlot.floors.strength.begin(), shownSlot.floors.strength.end());
            for (auto& s : listed) s *= floorStrength;
            listStrength = &listed;
        }
    }
    const bool floored = floorPlane.found &&
        omk::drawWithWater(world, list, drawn, &shownSlot.geo, floorPlane, passStrength, listStrength,
                           wetOn ? &shownSlot.wet.open : nullptr, wetReflect, wetDarken);
    if (wetStrength > 0.0f && (wetOn != wetTold || frameNo % 30 == 0)) {
        if (wetOn != wetTold)
            std::printf("frame %ld: the wet floors %s (wetness %.2f, reflection %.2f)\n", frameNo,
                        wetOn ? "REFLECT - the rain has wet the floors under the open sky"
                              : "are dry (no rain here, or it has dried)",
                        static_cast<double>(wet), static_cast<double>(wetReflect));
        wetTold = wetOn;
        omk::event("remaster.wet", {{"frame", frameNo}, {"rain", weatherRain}, {"wetness", wet},
                                    {"reflect", wetReflect}, {"live", wetOn && floored},
                                    {"y", static_cast<double>(floorPlane.y)}});
    }
    if (floored != floorLive || (floored && floorPlane.y != floorLiveY)) {
        floorLive = floored;
        floorLiveY = floorPlane.y;
        std::printf("frame %ld: the floors %s (y %.0f)\n", frameNo,
                    floored ? "REFLECT - the scene mirrored in the level below the eye"
                            : "do not reflect here (none listed near, the water reflecting, or no pass)",
                    static_cast<double>(floorPlane.y));
        omk::event("remaster.floors", {{"frame", frameNo}, {"live", floored},
                                       {"y", static_cast<double>(floorPlane.y)}});
    }
    return watered || floored;
}

#endif  // OMK_REMASTER
