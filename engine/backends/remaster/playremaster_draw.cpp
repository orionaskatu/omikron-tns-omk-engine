// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER'S DRAW (`todo/remaster.md`) - the mist, and the water and floor
// reflections (A3); cut out of `playframe_world_draw.cpp`'s phases so a build
// without OMK_REMASTER carries none of it (`backends/sdl/playremaster_off.cpp`).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "o3de/colourgrade.h"
#include "o3de/floorreflect.h"
#include "o3de/wetfloor.h"
#include "platform/env.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float kShowerMist = 0.1f;        // the mist the full shower adds (0.3 too dark in Anekbah's own fog)
// the veils' and the dust's colour lit as the set's ambient: this much at
// none, `kAirAmbientGain` more per unit of its grey
constexpr float kAirAmbientMin = 0.25f, kAirAmbientGain = 1.6f;
constexpr float kVeilGrey[3] = {0.62f, 0.66f, 0.72f};   // the rain's veils, a light cold grey
constexpr double kVeilFallPerMs = 1.5;     // ...falling 1500 units a second
constexpr double kVeilFallWrap = 1.4e6;    // ...wrapped here, so the float keeps its precision
constexpr float kVeilScale = 2500.0f;      // ...63% thick at full shower this far off
constexpr float kDustOchre[3] = {0.80f, 0.64f, 0.42f};  // the sand's dust
constexpr double kDustDriftPerMs = 0.70;   // ...drifting 700 units a second with the wind (300 read as still)
constexpr double kDustRisePerMs = 0.04;    // ...and slowly across
constexpr double kDustDriftWrap = 4e5;     // ...wrapped here
constexpr float kDustScale = 1100.0f;      // ...63% at full storm this far (1400 before it came nearer)
}  // namespace

// THE MIST (`View::mist`): on the shown set's ground
void PlayState::remasterViewMist(omk::View& v) {
    // ...thickened by the SHOWER while the 3D rain shows (the shower's sky,
    // `remasterGrade`): the city's fog colour closing in as it rains
    v.mist = std::min(1.0f, mistStrength + (rain3dStrength > 0.0f && remasterShown_
                                                ? kShowerMist * std::clamp(weatherRain * rain3dStrength, 0.0f, 1.0f) : 0.0f));
    v.mistGround = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)].ground;
    // ...and the RAIN's ripples on the wet floors (`o3de/wetfloor.h`): the
    // shower's strength, which is 0 outside a rain area
    v.rain = wetStrength > 0.0f ? weatherRain : 0.0f;
    // ...and THE SAND'S DUST (the restored weather's, with the 3D weather -
    // the reader's "a moving fog effect and small particles", 2026-10-27):
    // as thick as the storm is strong, its clouds drifting with the wind
    // (-x, the arm's) on the weather's own clock - held while paused - in an
    // ochre lit as the set's ambient
    // ...and THE RAIN'S VEILS (with the 3D weather - the reader's "more
    // realistic rain", 2026-10-28): far rain as falling sheets, as thick as
    // the shower, falling 1500 units a second on the weather's clock (held
    // while paused), a light grey lit as the set's ambient
    // THE PUDDLES on the wet floors - `OMK_NO_PUDDLES=1`, an INSTRUMENT, an
    // even film instead (`verify.py: engine: wet floors` part 9)
    static const bool noPuddles = omk::envFlag("OMK_NO_PUDDLES");
    v.puddles = noPuddles ? 0.0f : 1.0f;
    v.veil = 0.0f;
    // `OMK_NO_VEIL=1`, an INSTRUMENT: the veils alone left out
    // (`verify.py: engine: rain 3d` part 13)
    static const bool noVeil = omk::envFlag("OMK_NO_VEIL");
    if (!noVeil && rain3dStrength > 0.0f && remasterShown_ && weatherRain > 0.0f) {
        const WorldSlot& ws = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)];
        const float amb = std::clamp(kAirAmbientMin + kAirAmbientGain * static_cast<float>(ws.ambientGrey) / 255.0f,
                                     kAirAmbientMin, 1.0f);
        v.veil = std::clamp(weatherRain * rain3dStrength, 0.0f, 1.0f);
        v.veilFall = static_cast<float>(std::fmod(weatherClockMs * kVeilFallPerMs, kVeilFallWrap));
        v.veilScale = kVeilScale;
        for (int k = 0; k < 3; ++k) v.veilColour[k] = kVeilGrey[k] * amb;
    }
    v.dust = 0.0f;
    // `OMK_NO_DUST=1`, an INSTRUMENT: the dust fog alone left out, the grains
    // and the grade kept (`verify.py: engine: rain 3d` part 11)
    static const bool noDust = omk::envFlag("OMK_NO_DUST");
    if (!noDust && restoredWeather && rain3dStrength > 0.0f && remasterShown_ && weatherSandLevel > 0.0f) {
        const WorldSlot& ws = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)];
        const float amb = std::clamp(kAirAmbientMin + kAirAmbientGain * static_cast<float>(ws.ambientGrey) / 255.0f,
                                     kAirAmbientMin, 1.0f);
        v.dust = std::clamp(weatherSandLevel * rain3dStrength, 0.0f, 1.0f);
        v.dustDrift[0] = static_cast<float>(std::fmod(weatherClockMs * kDustDriftPerMs, kDustDriftWrap));
        v.dustDrift[1] = static_cast<float>(std::fmod(weatherClockMs * kDustRisePerMs, kDustDriftWrap));
        v.dustScale = kDustScale;
        for (int k = 0; k < 3; ++k) v.dustColour[k] = kDustOchre[k] * amb;
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
        // the wet level nearest the height the camera looks at - the street
        // the player stands in - takes the reflection (its puddles mirror)
        const omk::WaterPlane wp = omk::chooseFloorNear(shownSlot.wet.planes, drawn.cam.eye, drawn.cam.at[1]);
        if (wp.found) floorPlane = wp;
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
                           wetOn ? &shownSlot.wet.rained : nullptr, wetReflect, wetDarken);
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

// THE SNOW LYING (`o3de/wetfloor.h` coverRuns, `[Remaster] wet` - the weather
// on the ground): where the game's SNOW falls - Mahaleel; its cycle never stops
// (docs/ASSETS.md 4e) and needs option row 7 at 2 - the set's floors under the
// open sky turn white under their own light, the corners' open sky blending
// the edge. No reflection pass: the frame's own draws, split into runs.
void PlayState::remasterSnowCover(std::vector<omk::Draw>& list) {
    if (wetStrength <= 0.0f || !weatherSnowing || !remasterShown_ || !world_ || !(world_->takes() & omk::kTakesWater))
        return;
    const WorldSlot& shownSlot = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)];
    if (shownSlot.wet.open.empty()) return;
    const std::size_t covered = omk::coverRuns(list, &shownSlot.geo, shownSlot.wet.open,
                                               std::clamp(wetStrength, 0.0f, 1.0f) * omk::kSnowCover);
    if (frameNo % 30 == 0) {
        std::printf("frame %ld: the snow lies on %zu floor triangles under the open sky\n", frameNo, covered);
        omk::event("remaster.snow", {{"frame", frameNo}, {"covered", static_cast<long>(covered)}});
    }
}

// THE WET BODIES (the reader's "wet walls and characters", 2026-10-28): in a
// rain area while the floors are wet, every body the renderer poses that
// stands under the open sky - the roof ray from `kWetBodyHead` above its root
// through the shot soup, the wet floors' own - is drawn wet (`Draw::wetBody`),
// as wet as the floors. `OMK_NO_WET_BODIES=1`, an INSTRUMENT, leaves them dry
// (`verify.py: engine: wet floors` part 11).
void PlayState::remasterWetBodies(std::vector<omk::Draw>& list) {
    static const bool noBodies = omk::envFlag("OMK_NO_WET_BODIES");
    const bool rainArea = weatherBitsTold != 0xFFFFu && (weatherBitsTold & 2u) != 0;
    const float amount = wetStrength * wetness.value();
    if (noBodies || !rainArea || !(amount > 0.0f) || !remasterShown_ || !world_ ||
        !(world_->takes() & omk::kTakesWater))
        return;
    const WorldSlot& shownSlot = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)];
    if (shownSlot.shotSoup.empty()) return;
    // the bodies POSED ON THE CPU draw their own posed copy, with no pose to
    // place them by (the player with a smoothed body on GLES, any body on a
    // renderer that poses none): known by that copy
    std::vector<const omk::Geometry*> cpuPosed{&playerBody.posed};
    for (const auto& up : staged) cpuPosed.push_back(&up->posed);
    for (const auto& up : crowd.staged) cpuPosed.push_back(&up->posed);
    for (const auto& up : traffic.staged) cpuPosed.push_back(&up->posed);
    // one ray a body: its draws share the pose, or the posed copy
    const void* lastBody = nullptr;
    bool lastOpen = false;
    int bodies = 0, wetBodies = 0;
    for (auto& d : list) {
        const bool gpu = d.meshPose && d.meshPoses != 0;
        if (!gpu && std::find(cpuPosed.begin(), cpuPosed.end(), d.geo) == cpuPosed.end()) continue;
        const void* body = gpu ? static_cast<const void*>(d.meshPose) : static_cast<const void*>(d.geo);
        if (body != lastBody) {
            lastBody = body;
            // its root, or the posed copy's lowest corner (Y is down) below its middle
            double at[3] = {0.0, 0.0, 0.0};
            if (gpu) {
                at[0] = d.meshPose[3]; at[1] = d.meshPose[7]; at[2] = d.meshPose[11];
            } else if (!d.geo->corners.empty()) {
                float lo[3] = {1e30f, -1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (const auto& c : d.geo->corners) {
                    lo[0] = std::min(lo[0], c.x); hi[0] = std::max(hi[0], c.x);
                    lo[2] = std::min(lo[2], c.z); hi[2] = std::max(hi[2], c.z);
                    hi[1] = std::max(hi[1], c.y);
                }
                at[0] = 0.5 * (lo[0] + hi[0]); at[1] = hi[1]; at[2] = 0.5 * (lo[2] + hi[2]);
            } else {
                lastOpen = false;
                continue;
            }
            const double up[3] = {0.0, -omk::kWetSkyRay, 0.0};   // Y points down
            const double p0[3] = {at[0], at[1] - omk::kWetBodyHead, at[2]};
            lastOpen = !omk::sweepSphere(shownSlot.shotSoup, shownSlot.shotRunBoxes, p0, up, 0.0).has_value();
            ++bodies;
            wetBodies += lastOpen ? 1 : 0;
        }
        if (!lastOpen) continue;
        d.wet = std::min(amount, 1.0f);
        d.wetBody = true;
    }
    if (frameNo % 30 == 0) {
        std::printf("frame %ld: %d of %d bodies wet in the rain\n", frameNo, wetBodies, bodies);
        omk::event("remaster.wetbodies", {{"frame", frameNo}, {"bodies", bodies}, {"wet", wetBodies}});
    }
}

#endif  // OMK_REMASTER
