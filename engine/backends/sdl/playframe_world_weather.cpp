// SPDX-License-Identifier: GPL-3.0-or-later
// THE CITY WEATHER: `sub_4815D0` ticked and its drops projected and drawn
// (`o3de/weather.h` has the reading; `todo/scene-gameplay-audit.md` 8).
#include "playframe.h"
#include "platform/env.h"
#include "platform/events.h"

#include <algorithm>
#include <cstdlib>

// `sub_4815D0`'s frame: the gate, the roof ray, the cycle, the drops through
// the frame's own camera. Run before the present's gates so a frame that
// draws weather keeps `fb` composed.
void PlayState::weatherFrame(const omk::View& v) {
    OMK_ZONE("world: weather");   // the profiler (todo/debug-tools.md 6)
    // PAUSED, with the remaster's 3D weather: the drops held where they are
    // (the reader: the rain went on moving in the pause menu). The original
    // keeps the world drawn behind screen 31 (its +112 bit 0x40000, docs/UI.md)
    // and `sub_4815D0` steps its drops per drawn frame, its clock stopped - so
    // its 2D rain moves there too, and the original mode keeps that.
    if (uiPause && remasterWeatherRoofless()) return;
    weatherLines.clear();
    weatherFlakes.clear();
    weatherRain = 0.0f;
    weatherSnowing = false;
    rainDrops.clear();   // the remaster's 3D rain takes them afresh, or has none
    auto& session = *session_;
    weatherClockMs += frameSec * 1000.0;
    // `OMK_WEATHER_SKIP_MS=N`: an INSTRUMENT - the weather's clock jumps N ms
    // on frame 1, past the 8-14 minute wait the first cycle starts with, so a
    // headless run reaches the rain (`verify.py: engine: weather`)
    static const double skipMs = omk::envNum("OMK_WEATHER_SKIP_MS", 0.0);
    if (frameNo == 1 && skipMs > 0.0) {
        weatherClockMs += skipMs;
        std::printf("frame %ld: weather - OMK_WEATHER_SKIP_MS: the clock jumps %.0f ms\n", frameNo, skipMs);
    }
    // the view's +80 bits, from the SHOWN slot's AREA +176 (`sub_41D3F0`)
    const auto& chunk = session.residentSlot(session.activeSlot() & 1).areaChunk;
    int type = 0;
    if (chunk.size() >= 178)
        type = static_cast<int>(static_cast<std::uint8_t>(chunk[176])) |
               static_cast<int>(static_cast<std::uint8_t>(chunk[177])) << 8;
    const int areaNow = session.residentSlot(session.activeSlot() & 1).area;
    // THE RESTORED WEATHER (`[Restored] weather`, `todo/restore-weather.md`):
    // Qalisar (101) and Lahoreh (64) subscribe to the shower's messages 23 and
    // 24 with tracks 120/56 and 121/27 and ship type 0 - the rain, the type
    // their shower tracks are (the reader heard each as its city's music under
    // the rain sound); the cycle, the roof ray and the messages stay the
    // original's own code and data. OMK's reconstruction, not a reading.
    if (restoredWeather && type == 0 && (areaNow == 101 || areaNow == 64)) {
        type = 1;
        if (areaNow != weatherAreaTold)
            omk::event("weather.restored", {{"frame", frameNo}, {"area", areaNow}, {"type", type}});
    }
    const unsigned viewBits = omk::weatherBitsOfAreaType(type);
    // ...and whether the player has just come into this area (for the 3D
    // weather's music, below)
    const bool areaEntered = areaNow != weatherAreaTold;
    weatherAreaTold = areaNow;
    // `cmp byte ptr [0x90E727], 2` - option row 7 (the `--detail` flag holds it)
    const bool gate = viewBits != 0 && shadowDetail == 2 && !worldSet.empty();
    if (viewBits != weatherBitsTold) {
        weatherBitsTold = viewBits;
        std::printf("frame %ld: weather - AREA +176 type %d, view bits 0x%x, detail %d: %s\n",
                    frameNo, type, viewBits, shadowDetail,
                    !viewBits ? "none here" : gate ? "the cycle runs (sub_4815D0)"
                          : "row 7 is not 2, so sub_4815D0 does nothing");
    }
    if (!gate) return;
    weatherSnowing = (viewBits & 4u) != 0;   // the snow's cycle runs: it never stops (ASSETS 4e)
    // `sub_481200`: the eye to 196.85 above it through `sub_444810` - the
    // shown set's shot soup, as the bolts' world ray
    bool covered = false;
    for (const auto& ws : worldSlots)
        if (!ws.stem.empty() && ws.stem == worldSet) {
            const double p0[3] = {v.cam.eye[0], v.cam.eye[1], v.cam.eye[2]};
            const double d[3] = {0.0, -196.85039, 0.0};
            covered = omk::sweepSphere(ws.shotSoup, ws.shotRunBoxes, p0, d, 0.0).has_value();
        }
    // `!g_PlayerActorRec || +404 == 14`: no player ACTOR, or in the water.
    // The viewer builds its controller a frame after the session places him,
    // so "no actor" is the session's answer, not the controller's - read off
    // the controller it parked the cycle in state 0 on frame 0, where a roof
    // then holds it
    const bool noPlayer = player ? static_cast<int>(player->state()) == 14
                                 : !session.playerPlaced();
    auto crt = [this]() {               // the engine's ONE `rand()`, shared
        gun.randSeed = gun.randSeed * 214013u + 2531011u;
        return static_cast<int>((gun.randSeed >> 16) & 0x7FFFu);
    };
    const long now = static_cast<long>(weatherClockMs);
    // THE REMASTER's 3D weather (`backends/remaster/playremaster_rain.cpp`)
    // stops at a roof drop by drop, so its cycle does not end at one: in the
    // original the roof ray sends a running shower to its 200 ms fade and
    // posts no message 24 - the shower gone at once, its music playing on
    // (the reader's two reports, 2026-10-27). The original's rule otherwise.
    const bool roofEnds = covered && !remasterWeatherRoofless();
    weather.tick(viewBits, now, roofEnds, noPlayer, crt, [&](int m) {
        const bool ran = session.postMessage(m, -1);    // event 43; the sender word is unset
        std::printf("frame %ld: weather - message %d posted (Game_RaiseEvent 43) %s\n",
                    frameNo, m, ran ? "to its handler" : "- no handler subscribes");
    });
    // the RAIN's strength for the remaster's wet floors (`o3de/wetfloor.h`):
    // its drops over their 200, the snow and the undrawn third none
    if (viewBits & 2u)
        weatherRain = static_cast<float>(std::clamp(weather.count(), 0, omk::kWeatherDrops)) /
                      static_cast<float>(omk::kWeatherDrops);
    // COMING BACK INTO A SHOWER, with the remaster's 3D weather: the area's
    // own entry plays its track over the rain's (event 9 - Anekbah's 2), and
    // the shower's message 23, which played track 119, came when it began; so
    // a shower still running here is announced again - its handler plays the
    // rain's track (the reader: it rained on, its sound gone, after a visit
    // indoors). The original leaves the area's track playing.
    if (areaEntered && (viewBits & 2u) && (weather.state() == 1 || weather.state() == 2) &&
        remasterWeatherRoofless()) {
        const bool ran = session.postMessage(23, -1);
        std::printf("frame %ld: weather - back into the shower: message 23 posted again %s\n", frameNo,
                    ran ? "to its handler" : "- no handler subscribes");
        omk::event("remaster.rain3d.reenter", {{"frame", frameNo}, {"area", areaNow}, {"ran", ran}});
    }
    if (weather.state() != weatherStateTold) {
        weatherStateTold = weather.state();
        std::printf("frame %ld: weather - state %d, %d drops%s, next change at %ld ms (now %ld)\n",
                    frameNo, weather.state(), weather.count(), covered ? ", a roof overhead" : "",
                    weather.waitEndsAt(), now);
    }
    const auto drops = weather.step(viewBits, v.cam.eye, v.cam.at);
    if (drops.empty()) {
        if (frameNo % 30 == 0)
            std::printf("frame %ld: weather - state %d, %d drops, nothing stepped%s\n", frameNo,
                        weather.state(), weather.count(), covered ? " (a roof overhead)" : "");
        return;
    }
    // THE RAIN IN 3D (the remaster's, `backends/remaster/playremaster_rain.cpp`):
    // these same drops drawn in the scene by the next frame's list, not here
    if (remasterRainTake(drops, viewBits)) {
        if (frameNo % 30 == 0)
            std::printf("frame %ld: weather - %zu drops stepped, drawn in the scene by the remaster\n",
                        frameNo, drops.size());
        return;
    }
    // the projection the world was drawn with; the near clip `sub_4751A0` at
    // the scene's +324 (this port's shared cut, `kNearCut`) - the end behind
    // it moved onto it along the segment, which view depth is affine in
    const auto clipProject = [&](const float a[3], const float b[3],
                                 omk::Projected& pa, omk::Projected& pb) {
        pa = omk::project(v.cam, a);
        pb = omk::project(v.cam, b);
        if (pa.z < 0.0f || pb.z < 0.0f) return false;        // both `>= 0.0` first
        const float zn = omk::kNearCut;
        if (pa.z <= zn && pb.z <= zn) return false;
        if (pa.z <= zn || pb.z <= zn) {
            const float t = (zn - pa.z) / (pb.z - pa.z);
            float m[3];
            for (int k = 0; k < 3; ++k) m[k] = a[k] + (b[k] - a[k]) * t;
            (pa.z <= zn ? pa : pb) = omk::project(v.cam, m);
        }
        return true;
    };
    const int W = fb.w, H = fb.h;
    const auto onScreen = [&](int x, int y) { return x >= 0 && y >= 0 && x <= W && y <= H; };
    const std::uint32_t fog = (std::uint32_t(v.fogColour[0]) << 16) |
                              (std::uint32_t(v.fogColour[1]) << 8) | v.fogColour[2];
    for (const auto& d : drops) {
        omk::Projected pa, pb;
        if (!clipProject(d.p, d.q, pa, pb)) continue;
        const int x0 = static_cast<int>(pa.x) + v.vx, y0 = static_cast<int>(pa.y) + v.vy;
        const int x1 = static_cast<int>(pb.x) + v.vx, y1 = static_cast<int>(pb.y) + v.vy;
        if (!onScreen(x0, y0) || !onScreen(x1, y1)) continue;
        if (viewBits & 2u) {
            // `I2D_DrawLine` flags 0x18: the scene's fog colour (+336) at the
            // drop, 0xC0C0C0 at its tail
            weatherLines.push_back({x0, y0, x1, y1, fog, 0xC0C0C0u});
        } else {
            // the flake: half the projected size, kept wholly on the screen
            const int r = std::abs(x1 - x0) / 2;
            if (r <= x0 && r <= y0 && r <= W - x0 && r <= H - y0)
                weatherFlakes.push_back({x0 - r, y0 - r, x0 + r, y0 + r});
        }
    }
    // once a second, from what the projection kept - the draw's own input
    if (frameNo % 30 == 0)
        std::printf("frame %ld: weather - %zu drops stepped, %zu streaks and %zu flakes drawn\n",
                    frameNo, drops.size(), weatherLines.size(), weatherFlakes.size());
}

// The drops into `fb`, under every interface layer - the 3D view's own
// display-list draw is where `sub_4815D0` runs.
void PlayState::weatherDraw() {
    if (!fb.valid()) return;
    for (const auto& l : weatherLines)
        omk::drawLineGouraud(fb, l.x0, l.y0, l.x1, l.y1, l.c0, l.c1);
    if (weatherFlakes.empty()) return;
    if (!weatherSnowTried) {
        weatherSnowTried = true;
        weatherSnow = omk::surfaceFromBmp(fs_->read("IMAGES/snow.bmp"));
    }
    if (!weatherSnow.valid()) return;
    // `sub_4810D0`: `Blt` with DDBLT_KEYSRC, the loader's key 0
    for (const auto& f : weatherFlakes)
        omk::blt(fb, omk::Rect{f.x0, f.y0, f.x1, f.y1}, weatherSnow,
                 omk::Rect{0, 0, weatherSnow.w, weatherSnow.h}, 0x8000u, 0);
}
