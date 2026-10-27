// SPDX-License-Identifier: GPL-3.0-or-later
// THE ORIGINAL/REMASTER SWITCH (`todo/graphics-menu.md` B) - one key in play
// (F10), the Mode row of OMK's Remaster page, and `[Remaster] mode` at start:
// the remaster's look put away and brought back on the running game.
//
// Everything the remaster needs is ARMED at start whatever the mode
// (`remasterSetup`), so the switch moves only what a frame reads: the
// strengths to 0 and back (held in `remasterHeld_`), the three packs hidden
// (`setShown`) and the world's textures uploaded again, the emitters' lights
// taken out of the sets' tables, the grade, the films. Outside it: the
// upscale and the preload radius (a frame rate's choices, not a look) and,
// for now, the rounded bodies (cut from the rest at load: an original start
// leaves them flat, a switch keeps what the start made).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "o3de/colourgrade.h"
#include "o3de/emitterlight.h"
#include "o3de/texpack.h"
#include "platform/events.h"
#include "platform/options.h"
#include "ui/fontpack.h"
#include "ui/uipack.h"

#include <cstdio>

namespace {
// the strengths the switch holds - each read by the frame
constexpr const char* kHeld[] = {"bloom", "water", "floors", "mist", "relief", "ao", "bodylight", "fxlight"};
}  // namespace

float* PlayState::remasterStrength(const std::string& key) {
    if (key == "bloom") return &bloomStrength;
    if (key == "water") return &waterStrength;
    if (key == "floors") return &floorStrength;
    if (key == "mist") return &mistStrength;
    if (key == "relief") return &relief;
    if (key == "ao") return &aoStrength;
    if (key == "bodylight") return &bodyLight;
    if (key == "fxlight") return &fxLight;
    return nullptr;
}

// what the start armed: a strength > 0 had its resources loaded
float PlayState::remasterArmed(const char* key) const {
    const auto at = remasterAtStart_.find(key);
    return at == remasterAtStart_.end() ? 0.0f : at->second;
}

// what the remaster mode runs with - the held strength while the original shows
float PlayState::remasterRunning(const char* key, float v) const {
    if (remasterShown_) return v;
    const auto at = remasterHeld_.find(key);
    return at == remasterHeld_.end() ? v : at->second;
}

// anything to switch: a strength armed, or a pack open
bool PlayState::remasterSwitchable() const {
    for (const auto& [k, v] : remasterAtStart_)
        if (v > 0.0f) return true;
    return omk::texturePack().active() || omk::uiPack().active() || omk::fontPack().active() ||
           omk::cityGrades().active();
}

// a set's lights as the mode wants them: its own, then - the remaster's - its
// glowing surfaces and, in the set the scene's emitters are bound into, their
// point lights (`remasterEmitterLights`)
void PlayState::remasterSlotLights(WorldSlot& ws, bool emitters) {
    const bool want = fxLight > 0.0f && world_ && world_->drawsEmitterLights();
    ws.lights.resize(ws.setLights);
    ws.fxIn = want;
    if (!want) return;
    ws.lights.insert(ws.lights.end(), ws.fxSurface.begin(), ws.fxSurface.end());
    if (!emitters || !session_) return;
    omk::EmitterLightCount n;
    const auto fl = omk::emitterLights(
        session_->scene().effects(), [&](int id) { return sprites.tab.texOf(id); }, fxLight, &n);
    ws.lights.insert(ws.lights.end(), fl.begin(), fl.end());
    std::printf("fx light: %s - %d glows and %d flames of %d emitters light the bodies "
                "(%zu set lights)\n", ws.stem.c_str(), n.glows, n.flames,
                n.glows + n.flames + n.dark, ws.setLights);
    omk::event("world.fxlights", {{"set", ws.stem}, {"glows", n.glows}, {"flames", n.flames},
                                  {"dark", n.dark}});
}

// THE SWITCH. False, said once, when nothing was armed to switch.
bool PlayState::remasterShow(bool on, const char* why) {
    if (on == remasterShown_) return true;
    if (!remasterSwitchable()) {
        if (!remasterNothingTold_)
            std::printf("remaster: %s - nothing to switch, no remaster configured (`[Remaster] dir`, its keys "
                        "or a flag)\n", why);
        remasterNothingTold_ = true;
        omk::event("remaster.mode", {{"mode", std::string(on ? "remaster" : "original")}, {"why", std::string(why)},
                                     {"taken", false}});
        return false;
    }
    if (!on) {
        for (const char* k : kHeld) remasterHeld_[k] = *remasterStrength(k);
        if (world_ && bloomStrength > 0.0f) world_->setBloom(0.0f);
        if (world_ && aoStrength > 0.0f) world_->setAmbientOcclusion(0.0f);
        for (const char* k : kHeld) *remasterStrength(k) = 0.0f;
    } else {
        for (const char* k : kHeld) {
            const float v = remasterHeld_[k];
            if (world_ && v > 0.0f && std::string(k) == "bloom") world_->setBloom(v);
            if (world_ && v > 0.0f && std::string(k) == "ao") world_->setAmbientOcclusion(v);
            *remasterStrength(k) = v;
        }
    }
    remasterShown_ = on;
    settings.remasterOn = on;
    omk::uiPack().setShown(on);
    omk::fontPack().setShown(on);
    // the world's textures uploaded again, from the pack or as shipped - the
    // filter's way (3d): dropped, and the pool handed over again this frame
    // (an original START hid the pack before any pool: nothing to send again)
    bool retextured = false;
    if (omk::texturePack().active() && omk::texturePack().shown() != on) {
        omk::texturePack().setShown(on);
        if (world_ && world_->takesPack() && world_->retexture(texFilter, texAniso)) {
            ++texPool.composition;
            retextured = true;
        }
    }
    remasterSlotsFrame(true);
    gradeFor = "\x01";   // `remasterGrade` chooses again
    std::printf("remaster: %s - the %s mode%s\n", why, on ? "remaster" : "original",
                retextured ? ", the world's textures uploaded again" : "");
    omk::event("remaster.mode", {{"mode", std::string(on ? "remaster" : "original")}, {"why", std::string(why)},
                                 {"taken", true}, {"retextured", retextured}});
    return true;
}

// each set's lights brought to the mode - all of them on a switch, else the
// ones a load in flight across it prepared the other way
void PlayState::remasterSlotsFrame(bool all) {
    if (!session_) return;
    const bool want = fxLight > 0.0f && world_ && world_->drawsEmitterLights();
    bool emitters = session_->scene().loaded() && session_->scene().setEmittersBound();
    for (WorldSlot& ws : worldSlots) {
        if (ws.stem.empty()) continue;
        const bool bound = emitters && ws.area == session_->sceneArea();
        if (bound) emitters = false;   // the first of its area, as the binding takes it
        if (all || ws.fxIn != want) remasterSlotLights(ws, bound);
    }
}

// EACH FRAME, beside ESC's read (`inputPause`, F10 = DIK 0x44 in the devices
// the scripted streams joined, so `--hold k68` presses it): the start's mode
// once the renderer exists, the key's edge, the sets' lights
void PlayState::remasterModeFrame(bool down) {
    if (remasterStartHidden_) {
        remasterStartHidden_ = false;
        remasterShow(false, "[Remaster] mode = original at start");
    }
    if (down && !remasterKeyDown_ && remasterShow(!remasterShown_, "F10")) {
        // ...and the next start resumes in it
        const bool ok = !configFile.empty() &&
            omk::writeOptionsFile(configFile, {{"Remaster", "mode", remasterShown_ ? "remaster" : "original"}});
        std::printf("remaster: [Remaster] mode = %s %s %s\n", remasterShown_ ? "remaster" : "original",
                    ok ? "written to" : "NOT written to", configFile.c_str());
        if (omkOpened_) omkOptionsOpen();   // the Mode row, should the menu be up
    }
    remasterKeyDown_ = down;
    remasterSlotsFrame(false);
}

#endif  // OMK_REMASTER
