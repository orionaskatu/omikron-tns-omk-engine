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
    o.setTakes(world_ ? world_->takes() : 0u);
    const omk::Settings d;   // the reader's defaults - what a reset key gives
    // ---- [Enhancements]: the values running; a flag holds its own row, and
    // `--enhance-all` every row it raises that has no flag of its own
    const auto enh = [&](const char* key, double running, double def, int flag, const char* flagName,
                         bool underAll = true) {
        const std::string k = std::string("enhancements.") + key;
        o.setValue(k, running);
        o.setDefault(k, def);
        o.setHeld(k, flag >= 0 ? flagName : (underAll && enhanceAll) ? "--enhance-all" : "");
    };
    enh("all", (enhanceAll || settings.enhanceAll) ? 1 : 0, 0, enhanceAll ? 1 : -1, "--enhance-all", false);
    enh("antialiasing", aaSamples, d.antiAliasing, aaFlag, "--aa");
    enh("supersampling", ssaa, d.supersample, ssaaFlag, "--ssaa", false);
    enh("texturefiltering", texFilter, d.textureFilter, filterFlag, "--filter");
    enh("anisotropy", texAniso, d.anisotropy, anisoFlag, "--anisotropy");
    enh("shadowquality", shadowQuality, d.shadowQuality, shadowQFlag, "--shadow-quality");
    enh("lighting", lighting, d.lighting, lightingFlag, "--lighting");
    enh("uiscaling", uiScaling, d.uiScaling, uiScaleFlag, "--ui-scaling");
    enh("textscaling", textScaling, d.textScaling, textScaleFlag, "--text-scaling");
    enh("radar", radarAlways ? 1 : 0, d.radarAlways ? 1 : 0, radarFlag, "--radar");
    enh("framerate", frameRate, d.frameRate, frameRateFlag, "--framerate");
    enh("animation", smoothAnim ? 1 : 0, d.smoothAnimation ? 1 : 0, smoothAnimFlag, "--smooth-anim");
    enh("clipdistance", unlimitedClip ? 1 : 0, 0, clipFlag ? 1 : -1, "--clip");
    // ---- [Remaster]: what the remaster mode RUNS with - the flag, the key,
    // the preset's - read from the file again, since `mode = original` left
    // this run's settings without them
    if (remasterBuilt()) {
        const omk::Settings f = omk::resolveSettings(ini, std::nullopt);
        const bool preset = !remasterFlag.empty() || !f.remasterDir.empty();
        const auto rem = [&](const char* key, float flag, const char* flagName, float iniV,
                             omk::Settings::Source src, float off = 0.0f) {
            const std::string k = std::string("remaster.") + key;
            const float def = preset && std::string(key) != "upscale" ? omk::remasterPresetValue(key) : off;
            o.setValue(k, flag >= 0.0f ? flag : src == omk::Settings::Source::Ini ? iniV : def);
            o.setDefault(k, def);
            o.setHeld(k, flag >= 0.0f ? flagName : "");
        };
        o.setValue("remaster.mode", f.remasterOn ? 1 : 0);
        o.setDefault("remaster.mode", 1);
        rem("bloom", bloomFlag, "--bloom", f.bloom, f.bloomSource);
        rem("water", waterFlag, "--water", f.water, f.waterSource);
        rem("floors", floorsFlag, "--floors", f.floors, f.floorsSource);
        rem("mist", mistFlag, "--mist", f.mist, f.mistSource);
        rem("relief", reliefFlag, "--relief", f.relief, f.reliefSource);
        rem("ao", aoFlag, "--ao", f.ao, f.aoSource);
        rem("bodylight", bodyLightFlag, "--body-light", f.bodyLight, f.bodyLightSource);
        rem("fxlight", fxLightFlag, "--fx-light", f.fxLight, f.fxLightSource);
        rem("smooth", smoothFlag, "--smooth", f.smooth, f.smoothSource);
        rem("upscale", upscaleFlag, "--upscale", f.upscale, f.upscaleSource, 1.0f);
        rem("preload", preloadFlag, "--preload", f.preload, f.preloadSource);
    }
    o.setValue("omk.language", settings.language == "en" ? 1 : 0);
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
        const double v = o.value(key);
        const bool live = o.live(key);
        omk::event("options.omk", {{"key", key}, {"value", o.iniValue(key)}, {"live", live}});
        if (!live) {
            std::printf("options: %s = %s - at the next start\n", key.c_str(), o.iniValue(key).c_str());
            continue;
        }
        std::printf("options: %s = %s - applied\n", key.c_str(), o.iniValue(key).c_str());
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
        }
    }
}
