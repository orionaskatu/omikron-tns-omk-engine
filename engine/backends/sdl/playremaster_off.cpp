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
void PlayState::remasterPrepareSet(SetLoad&) {}
void PlayState::remasterSmoothModel(CharModel&, const std::string&) {}
void PlayState::remasterSmoothPlayer() {}
bool PlayState::remasterTakePreload(int, const std::string&, int, long) { return false; }
void PlayState::phasePreload() {}
void PlayState::remasterFinish() {}
void PlayState::preloadPin() {}
void PlayState::remasterSceneViewer() {}
void PlayState::remasterEmitterLights(WorldSlot&) {}
void PlayState::remasterGrade() {}
void PlayState::remasterAliasGrey(const omk::Texture&, const omk::Texture&) {}
void PlayState::remasterConfigure(omk::Renderer&) {}
bool PlayState::remasterOpenFilm(const std::string&, omk::Movie&, const char*) { return false; }
void PlayState::remasterViewMist(omk::View& view) { view.mist = 0.0f; }
// the per-pixel lighting as it always was: no night, no relief, no point lights
int PlayState::remasterViewLights(omk::View& view, bool& fxPix, float& kFxView) {
    fxPix = false;
    kFxView = 0.0f;
    view.setDim = 1.0f;
    view.relief = 0.0f;
    view.points.clear();
    return 0;
}
void PlayState::remasterPointLights(omk::View&, std::vector<std::pair<float, const omk::Light3do*>>&, float, int,
                                    float&) {}
void PlayState::remasterLightsTold(const omk::View&, const std::vector<std::pair<float, const omk::Light3do*>>&,
                                   const float[3]) {}
void PlayState::remasterLitSet(std::vector<omk::Draw>&) {}
bool PlayState::remasterDrawReflected(omk::Renderer&, std::span<const omk::Draw>, const omk::View&) { return false; }
void PlayState::bodyTint(const float[3], float out[3]) const { out[0] = out[1] = out[2] = 1.0f; }

#endif  // !OMK_REMASTER
