// SPDX-License-Identifier: GPL-3.0-or-later
// OMK'S OWN OPTIONS PAGES, in the viewer (`todo/graphics-menu.md` A2-A4): the
// rows given the values this run resolved, their changes written to the
// config and the live ones applied. The pages themselves are
// `ui/omkoptions.h`; screen 35 hosts them (`OptionsMenu::attachOmk`).
#include "playstate.h"

#include "platform/events.h"
#include "platform/options.h"
#include "platform/settings.h"
#include "actor/pose.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
// the reader's copy of the strings, beside the config (`ui/omktext.h`)
std::string textOverride(const std::string& configFile) {
    const auto cut = configFile.find_last_of("/\\");
    return (cut == std::string::npos ? std::string() : configFile.substr(0, cut + 1)) + "omk-menu.ini";
}
}  // namespace

// Once, beside screen 35 (`setupOptions`): the texts in `[OMK] language`, and
// the pages - the Remaster page only in a build that has the remaster.
void PlayState::omkOptionsSetup() {
    const bool over = omkText_.load(settings.language, textOverride(configFile));
    auto& o = omkOpt_.emplace(remasterBuilt());
    o.setText(&omkText_);
    optMenu_->attachOmk(&o);
    std::printf("options: OMK's pages - language %s%s, %s\n", omkText_.language().c_str(),
                over ? " (the texts beside the config read over the compiled ones)" : "",
                remasterBuilt() ? "the Enhancements and Remaster pages" : "the Enhancements page (a build without the remaster)");
}

// Each time screen 35 opens: what the renderer can draw, and each row's
// value as this run resolved it, its default, and the flag that holds it.
void PlayState::omkOptionsOpen() {
    if (!omkOpt_) return;
    auto& o = *omkOpt_;
    omkOpened_ = true;
    o.setTakes(world_ ? world_->takes() : 0u);
    const omk::Settings d;   // the reader's defaults - what a reset key gives
    // THE FILE, as the start read it and as it is now: a row whose file has
    // not moved shows what RUNS (a device may have reduced it - 8x MSAA run
    // at 4x); one whose file has moved - a change waiting for the next start,
    // the menu reopened - shows the file's, with its `*`
    const omk::Settings f0 = omk::resolveSettings(ini, std::nullopt);
    const omk::Settings f1 = omk::resolveSettings(omk::loadOptionsFile(configFile), std::nullopt);
    const auto row = [&](const std::string& k, double running, double conf0, double conf1) {
        if (conf1 == conf0) o.setValue(k, running);
        else                o.setValue(k, running, conf1);
    };
    // ---- [Enhancements]: a flag holds its own row, and `--enhance-all` every
    // row it raises that has no flag of its own (`enh` in playstate.cpp)
    const auto enh = [&](const char* key, double running, double def, int flag, const char* flagName,
                         double v0, double v1, double top, bool underAll = true) {
        const std::string k = std::string("enhancements.") + key;
        const auto conf = [&](double fileV) {
            return flag >= 0 ? flag : (underAll && enhanceAll) ? top : fileV;
        };
        row(k, running, conf(v0), conf(v1));
        o.setDefault(k, def);
        o.setHeld(k, flag >= 0 ? flagName : (underAll && enhanceAll) ? "--enhance-all" : "");
    };
    enh("all", (enhanceAll || settings.enhanceAll) ? 1 : 0, 0, enhanceAll ? 1 : -1, "--enhance-all",
        f0.enhanceAll, f1.enhanceAll, 1, false);
    enh("antialiasing", aaSamples, d.antiAliasing, aaFlag, "--aa", f0.antiAliasing, f1.antiAliasing,
        omk::kMaxAntiAliasing);
    enh("supersampling", ssaa, d.supersample, ssaaFlag, "--ssaa", f0.supersample, f1.supersample, 1, false);
    enh("texturefiltering", texFilter, d.textureFilter, filterFlag, "--filter", f0.textureFilter,
        f1.textureFilter, omk::kMaxTextureFilter);
    enh("anisotropy", texAniso, d.anisotropy, anisoFlag, "--anisotropy", f0.anisotropy, f1.anisotropy,
        omk::kMaxAnisotropy);
    enh("shadowquality", shadowQuality, d.shadowQuality, shadowQFlag, "--shadow-quality", f0.shadowQuality,
        f1.shadowQuality, omk::kMaxShadowQuality);
    enh("lighting", lighting, d.lighting, lightingFlag, "--lighting", f0.lighting, f1.lighting,
        omk::kMaxLighting);
    enh("uiscaling", uiScaling, d.uiScaling, uiScaleFlag, "--ui-scaling", f0.uiScaling, f1.uiScaling,
        omk::kMaxUiScaling);
    enh("textscaling", textScaling, d.textScaling, textScaleFlag, "--text-scaling", f0.textScaling,
        f1.textScaling, omk::kMaxTextScaling);
    enh("radar", radarAlways ? 1 : 0, d.radarAlways ? 1 : 0, radarFlag, "--radar", f0.radarAlways ? 1 : 0,
        f1.radarAlways ? 1 : 0, omk::kMaxRadar);
    enh("framerate", frameRate, d.frameRate, frameRateFlag, "--framerate", f0.frameRate, f1.frameRate,
        omk::kMaxFrameRate);
    enh("animation", smoothAnim ? 1 : 0, d.smoothAnimation ? 1 : 0, smoothAnimFlag, "--smooth-anim",
        f0.smoothAnimation ? 1 : 0, f1.smoothAnimation ? 1 : 0, omk::kMaxAnimation);
    enh("clipdistance", unlimitedClip ? 1 : 0, 0, clipFlag ? (clipArg <= 0 ? 1 : 0) : -1, "--clip",
        f0.unlimitedDrawDistance ? 1 : 0, f1.unlimitedDrawDistance ? 1 : 0, 1);
    // ---- [Remaster]: what the remaster mode runs with - the flag, the key,
    // the preset's - from the file, since `mode = original` left this run's
    // settings without them; what runs is this run's strength
    if (remasterBuilt()) {
        const bool preset0 = !remasterFlag.empty() || !f0.remasterDir.empty();
        const bool preset1 = !remasterFlag.empty() || !f1.remasterDir.empty();
        const auto rem = [&](const char* key, float running, float flag, const char* flagName,
                             float omk::Settings::*field, omk::Settings::Source omk::Settings::*src,
                             float off = 0.0f) {
            const std::string k = std::string("remaster.") + key;
            const bool presetKey = std::string(key) != "upscale";
            const auto conf = [&](const omk::Settings& f, bool preset) {
                const float def = preset && presetKey ? omk::remasterPresetValue(key) : off;
                return flag >= 0.0f ? flag : f.*src == omk::Settings::Source::Ini ? f.*field : def;
            };
            // a start in ORIGINAL ran none of it: the rows show what the remaster
            // mode would run, not the zeros
            row(k, remasterAtStart_.empty() ? conf(f1, preset1) : running, conf(f0, preset0), conf(f1, preset1));
            o.setDefault(k, preset1 && presetKey ? omk::remasterPresetValue(key) : off);
            o.setHeld(k, flag >= 0.0f ? flagName : "");
        };
        using S = omk::Settings;
        row("remaster.mode", settings.remasterOn ? 1 : 0, f0.remasterOn ? 1 : 0, f1.remasterOn ? 1 : 0);
        o.setDefault("remaster.mode", 1);
        rem("bloom", bloomStrength, bloomFlag, "--bloom", &S::bloom, &S::bloomSource);
        rem("water", waterStrength, waterFlag, "--water", &S::water, &S::waterSource);
        rem("floors", floorStrength, floorsFlag, "--floors", &S::floors, &S::floorsSource);
        rem("mist", mistStrength, mistFlag, "--mist", &S::mist, &S::mistSource);
        rem("relief", relief, reliefFlag, "--relief", &S::relief, &S::reliefSource);
        rem("ao", aoStrength, aoFlag, "--ao", &S::ao, &S::aoSource);
        rem("bodylight", bodyLight, bodyLightFlag, "--body-light", &S::bodyLight, &S::bodyLightSource);
        rem("fxlight", fxLight, fxLightFlag, "--fx-light", &S::fxLight, &S::fxLightSource);
        rem("smooth", smoothBodies, smoothFlag, "--smooth", &S::smooth, &S::smoothSource);
        rem("upscale", upscale > 1.0f ? upscale : 1.0f, upscaleFlag, "--upscale", &S::upscale, &S::upscaleSource,
            1.0f);
        rem("preload", preloadMetres, preloadFlag, "--preload", &S::preload, &S::preloadSource);
    }
    row("omk.language", settings.language == "en" ? 1 : 0, f0.language == "en" ? 1 : 0,
        f1.language == "en" ? 1 : 0);
    o.setDefault("omk.language", 0);
}

// Each frame screen 35 is up: the changes written to the config, and the
// rows that can apply at once applied - the others wait for the next start.
void PlayState::omkOptionsServe() {
    if (!omkOpt_) return;
    auto& o = *omkOpt_;
    const auto edits = o.takeEdits();
    if (!edits.empty()) {
        const bool ok = omk::writeOptionsFile(configFile, edits);
        std::printf("options: OMK - %zu edit(s) %s %s\n", edits.size(), ok ? "written to" : "NOT written to",
                    configFile.c_str());
        omk::event("options.omk.write", {{"path", configFile}, {"edits", static_cast<long>(edits.size())},
                                         {"ok", ok}});
    }
    for (const std::string& key : o.takeChanged()) {
        const bool applied = omkApplyLive(key, o.value(key));
        if (applied) o.settle(key);
        omk::event("options.omk", {{"key", key}, {"value", o.iniValue(key)}, {"live", applied}});
        std::printf("options: %s = %s - %s\n", key.c_str(), o.iniValue(key).c_str(),
                    applied ? "applied" : "at the next start");
    }
}

// ONE CHANGE ON THE RUNNING GAME (`todo/graphics-menu.md` A4, 3a) - true when
// it is drawn from the next frame on, false when it waits for the next start.
// Each is what the start does with the same value, re-applied; a value the
// renderer cannot draw now is refused, as the start refuses it
// (`playsetup_bodies.cpp`'s lighting and shadow checks).
bool PlayState::omkApplyLive(const std::string& key, double v) {
    if (key == "omk.language") {
        settings.language = v > 0.5 ? "en" : "fr";
        omkText_.load(settings.language, textOverride(configFile));
    } else if (key == "enhancements.uiscaling") {
        uiScaling = static_cast<int>(v);
        comp_->setScaling(uiScaling);
        applyTextScale(dispW, dispH);
    } else if (key == "enhancements.textscaling") {
        textScaling = static_cast<int>(v);
        applyTextScale(dispW, dispH);
    } else if (key == "enhancements.radar") {
        radarAlways = v > 0.5;
    } else if (key == "enhancements.framerate") {
        frameRate = static_cast<int>(v);
    } else if (key == "enhancements.animation") {
        smoothAnim = v > 0.5;
        omk::setPoseSmoothing(smoothAnim);
    } else if (key == "enhancements.clipdistance") {
        // as `--clip 0` / the key does at start: no distance, and the fog,
        // whose range it is, off - or back to the options row's
        unlimitedClip = v > 0.5;
        clipInches = unlimitedClip ? std::numeric_limits<double>::infinity() : settings.clipInches();
        clipReport = true;
    } else if (key == "enhancements.shadowquality") {
        // the blobs (classic, fitted) are the frontend's; the mapped pass is
        // the renderer's, and only where it was built
        if (v > 1.5 && !(world_ && world_->drawsShadowMap())) return false;
        shadowQuality = static_cast<int>(v);
    } else if (key == "enhancements.lighting") {
        if (v > 0.5 && !(world_ && world_->drawsPixelLights())) return false;
        lighting = static_cast<int>(v);
    } else if (key.rfind("remaster.", 0) == 0) {
        return remasterApplyLive(key.substr(9), static_cast<float>(v));
    } else {
        return false;
    }
    return true;
}
