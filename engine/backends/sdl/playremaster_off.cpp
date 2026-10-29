// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER, NOT BUILT - the `PlayState::remaster*` methods (and the
// preload's) as empty stubs for every build without OMK_REMASTER: the Vita,
// the classic Mac, the 3DS, the PowerPC Mac, and `make noremaster`. Their
// build files compile every `backends/sdl/*.cpp`, so this file is how they
// link without the remaster; the real methods are `backends/remaster/`.
// The `playvr_off.cpp` pattern (`todo/quest-port.md` §5).
#if !OMK_REMASTER

#include "playstate.h"

#include <cstdio>

// the remaster's options are parsed everywhere (the settings and the command
// line are shared); without it, one that asks for something is SAID
void PlayState::remasterSetup() {
    if (opt.anyRemaster() || settings.anyRemaster())
        std::printf("remaster: not built (a build without OMK_REMASTER) - its options are ignored\n");
}
bool PlayState::remasterBuilt() const { return false; }   // OMK's options offer no Remaster page
bool PlayState::remasterApplyLive(const std::string&, float) { return false; }
bool PlayState::remasterShow(bool, const char*) { return false; }
void PlayState::remasterModeFrame(bool) {}
bool PlayState::remasterSwitchable() const { return false; }
float PlayState::remasterArmed(const char*) const { return 0.0f; }
float PlayState::remasterRunning(const char*, float v) const { return v; }
float* PlayState::remasterStrength(const std::string&) { return nullptr; }
void PlayState::remasterSwapBodies(bool) {}
void PlayState::remasterFeetLatched(const std::vector<omk::MeshPose>&) {}
void PlayState::remasterPrepareSet(SetLoad&) {}
void PlayState::remasterSmoothModel(CharModel&, const std::string&) {}
void PlayState::remasterSmoothPlayer() {}
bool PlayState::remasterTakePreload(int, const std::string&, int, long) { return false; }
void PlayState::phasePreload() {}
void PlayState::remasterFinish() {}
void PlayState::preloadPin() {}
void PlayState::preloadReleaseAll() {}
void PlayState::remasterSceneViewer() {}
void PlayState::remasterGrade() {}
void PlayState::remasterAliasGrey(const omk::Texture&, const omk::Texture&) {}
void PlayState::remasterConfigure(omk::Renderer&) {}
bool PlayState::remasterOpenFilm(const std::string&, omk::Movie&, const char*) { return false; }
void PlayState::remasterViewMist(omk::View& v) { v.mist = 0.0f; v.rain = 0.0f; v.dust = 0.0f; v.veil = 0.0f; }
bool PlayState::remasterRainTake(const std::vector<omk::WeatherDrop>&, unsigned) { return false; }
void PlayState::remasterRainDraws(std::vector<omk::Draw>&, const omk::View&) {}
void PlayState::remasterPoolRain(std::vector<omk::Texture>&) {}
bool PlayState::remasterWeatherRoofless() const { return false; }
void PlayState::remasterSnowCover(std::vector<omk::Draw>&) {}
void PlayState::remasterWetBodies(std::vector<omk::Draw>&) {}
bool PlayState::remasterDrawReflected(omk::Renderer&, std::span<const omk::Draw>, const omk::View&) { return false; }

#endif  // !OMK_REMASTER
