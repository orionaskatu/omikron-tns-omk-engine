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
#include "o3de/texpack.h"
#include "platform/events.h"
#include "platform/filmpack.h"
#include "platform/movie.h"

#include <algorithm>
#include <cstdio>
#include <vector>

// a set's water and ground, its floors, probes and glowing surfaces - on the
// thread `prepareSet` runs on, after the set's own lights are read
void PlayState::remasterPrepareSet(SetLoad& L) {
    WorldSlot& w = L.out;
    // THE PACK'S DECODE, here on the load's thread rather than on the frame
    // the set's pool reaches the renderer (`TexturePack::prepare`; the audit's
    // performance item 1, 2026-10-23) - a preloaded set's and an arriving one's
    if (world_ && world_->takesPack() && omk::texturePack().active()) {
        std::vector<const omk::Texture*> all;
        for (const auto& wt : w.tex) all.push_back(&wt);
        omk::texturePack().prepare(all);
    }
    w.water = omk::waterPlane(w.geo);
    w.ground = omk::groundLevel(w.geo);
    w.floors = floorStrength > 0.0f ? omk::floorPlanes(w.geo, w.tex, omk::floorList(), L.stem) : omk::FloorPlanes{};
    if (bodyLight > 0.0f) {
        w.probes.build(w.geo);
        omk::event("world.probes", {{"set", L.stem}, {"mean", static_cast<double>(w.probes.meanLuma())}});
    } else {
        w.probes = omk::LightProbes{};
    }
    // THE GLOWING SURFACES' LIGHT (`o3de/emitterlight.h` surfaceLights,
    // the remaster's): the pack's light maps over the set's triangles - its
    // neon letters, its lit windows - after the set's own lights
    w.fxSurface.clear();
    if (fxLight > 0.0f && world_ && world_->drawsEmitterLights() && omk::texturePack().active() &&
        omk::texturePack().lightMaps() > 0) {
        std::vector<const omk::Texture*> ptrs;
        for (const auto& wt : w.tex) ptrs.push_back(&wt);
        // the LIGHT MAPS alone: the colours are the shipped palette's
        const auto packs = omk::texturePack().replacements(ptrs, false);
        int cells = 0;
        w.fxSurface = omk::surfaceLights(w.geo, w.tex, packs, fxLight, &cells, L.stem, &omk::surfaceCuration());
        w.lights.insert(w.lights.end(), w.fxSurface.begin(), w.fxSurface.end());
        std::printf("fx light: %s - %d glowing surfaces light the bodies and the set (%s)\n", L.stem.c_str(), cells,
                    omk::surfaceCuration().covers(L.stem) ? "the curated list" : "the automatic rules");
        omk::event("world.fxsurfaces", {{"set", L.stem}, {"lights", cells},
                                        {"curated", omk::surfaceCuration().covers(L.stem)}});
    }
}

// THE EMITTERS' LIGHT (`o3de/emitterlight.h`): the set's table rebuilt as its
// own lights, its glowing surfaces and a point light a glowing emitter, so
// every path that lights a body takes them - when the scene binds its emitters
void PlayState::remasterEmitterLights(WorldSlot& ws) {
    auto& session = *session_;
    if (fxLight > 0.0f && world_ && world_->drawsEmitterLights()) {
        omk::EmitterLightCount n;
        const auto fl = omk::emitterLights(
            session.scene().effects(), [&](int id) { return sprites.tab.texOf(id); }, fxLight, &n);
        ws.lights.resize(ws.setLights);
        ws.lights.insert(ws.lights.end(), ws.fxSurface.begin(), ws.fxSurface.end());
        ws.lights.insert(ws.lights.end(), fl.begin(), fl.end());
        std::printf("fx light: %s - %d glows and %d flames of %d emitters light the bodies "
                    "(%zu set lights)\n", ws.stem.c_str(), n.glows, n.flames,
                    n.glows + n.flames + n.dark, ws.setLights);
        omk::event("world.fxlights", {{"set", ws.stem}, {"glows", n.glows}, {"flames", n.flames},
                                      {"dark", n.dark}});
    }
}

// THE CITY GRADE (`o3de/colourgrade.h`): chosen for the set under the
// player's feet, and off while the greyscale bank is on - a grade would tint
// a black-and-white cutscene. A hard switch: a change of set comes with the
// game's own fade.
void PlayState::remasterGrade() {
    auto& session = *session_;
    omk::Renderer& world = *world_;
    if (omk::cityGrades().active()) {
        const std::string want = session.renderGrey() ? std::string() : worldSet;
        if (want != gradeFor) {
            const omk::ColourLut* lut = want.empty() ? nullptr : omk::cityGrades().forSet(want);
            const bool took = world.setGrade(lut ? lut->rgb.data() : nullptr, lut ? lut->n : 0);
            const std::string src = want.empty() ? "" : omk::cityGrades().sourceFor(want);
            std::printf("frame %ld: grade %s for %s%s\n", frameNo, src.empty() ? "none" : src.c_str(),
                        want.empty() ? "the greyscale bank" : want.c_str(),
                        took ? "" : " - REFUSED, this renderer has no grade");
            omk::event("remaster.grade", {{"set", want}, {"lut", src}, {"taken", took}});
            gradeFor = want;
        }
    }
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
    if (std::max(waterStrength, floorStrength) > 0.0f)   // the floors ride on the water's pass
        r.setWater(std::max(waterStrength, floorStrength));
    if (bodyLight > 0.0f) r.setBodyTint(true);
    if (fxLight > 0.0f) r.setEmitterLights(true);
}

// THE FILM PACK (`platform/filmpack.h`): the same film upscaled, the same
// sound - played by the same decoder, smoothed to the display
bool PlayState::remasterOpenFilm(const std::string& real, omk::Movie& mov, const char* name) {
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
// always. The emitters' light waits too: its strength is baked into each
// set's surface lights when the set loads (`remasterPrepareSet`).
bool PlayState::remasterApplyLive(const std::string& key, float v) {
    if (remasterAtStart_.empty()) return false;   // the remaster did not run
    const auto at = remasterAtStart_.find(key);
    const bool armed = at != remasterAtStart_.end() && at->second > 0.0f;
    if (key == "mist") { mistStrength = v; return true; }
    if (!armed) return false;
    if (key == "bloom") {
        if (!world_ || !world_->setBloom(v)) return false;
        bloomStrength = v;
        return true;
    }
    if (key == "water")     { waterStrength = v; return true; }
    if (key == "floors")    { floorStrength = v; return true; }
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
