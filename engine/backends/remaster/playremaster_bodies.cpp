// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER'S BODIES (`todo/remaster.md`) - the rounded bodies (D1) and
// the bodies lit by their surroundings (A5); moved out of the shared frontend
// so a build without OMK_REMASTER carries none of it
// (`backends/sdl/playremaster_off.cpp`).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "o3de/smoothbody.h"
#include "platform/events.h"

#include <algorithm>
#include <cstdio>

// a character model's rest geometry, rounded at load when the option names it
void PlayState::remasterSmoothModel(CharModel& m, const std::string& name) {
    // the ROUNDED BODIES (`o3de/smoothbody.h`, the remaster's D1): the rest
    // subdivided once, here, so every actor wearing it and every crowd
    // copy cut from it (`lodRestFor`) is rounded - the face kept as it is
    if (smoothes(name)) {
        const std::size_t before = m.rest.corners.size();
        m.flat = m.rest;   // the crowd's (`lodRestFor`)
        const auto ss = omk::smoothBody(m.rest, m.face.valid() ? m.face.base : 0,
                                        m.face.valid() ? m.face.base + static_cast<std::size_t>(m.face.count) : 0,
                                        3, smoothBodies);
        omk::event("body.smooth", {{"model", name}, {"triangles", static_cast<long>(ss.triangles)},
                                   {"split", static_cast<long>(ss.split)},
                                   {"seam", static_cast<long>(ss.keptSeam)},
                                   {"face", static_cast<long>(ss.keptFace)},
                                   {"pinned", static_cast<long>(ss.pinned)},
                                   {"unpulled", static_cast<long>(ss.unpulled)},
                                   {"folded", static_cast<long>(ss.folded)},
                                   {"positions", static_cast<long>(ss.positions)},
                                   {"corners", static_cast<long>(m.rest.corners.size())}});
        std::printf("smooth: %s - %zu of %zu triangles split (%zu kept at a seam, %zu the face), "
                    "%zu -> %zu corners\n", name.c_str(), ss.split, ss.triangles, ss.keptSeam,
                    ss.keptFace, before, m.rest.corners.size());
    }
}

// ...and the player's
void PlayState::remasterSmoothPlayer() {
    // the ROUNDED BODIES (`o3de/smoothbody.h`), as `charModelFor` does
    // for everyone else - his face kept for its morph
    if (smoothes(playerModel)) {
        const auto face = omk::faceMeshOf(playerBody.meshes);
        const auto ss = omk::smoothBody(playerBody.rest, face.valid() ? face.base : 0,
                                        face.valid() ? face.base + static_cast<std::size_t>(face.count) : 0,
                                        3, smoothBodies);
        playerBody.rest.revision = ++worldGeoRev;
        std::printf("smooth: the player's %s - %zu of %zu triangles split (%zu kept at a seam, %zu the "
                    "face), %zu corners\n", playerModel.c_str(), ss.split, ss.triangles, ss.keptSeam,
                    ss.keptFace, playerBody.rest.corners.size());
    }
}

void PlayState::bodyTint(const float p[3], float out[3]) const {
    out[0] = out[1] = out[2] = 1.0f;
    if (bodyLight <= 0.0f || !world_ || !world_->drawsBodyTint() || !session_) return;
    const WorldSlot& ws = worldSlots[static_cast<std::size_t>(session_->shownSlot() & 1)];
    // the grey every body starts from: the active scene's (`+416`)
    const float base = static_cast<float>(std::clamp(activeAmbientGrey, 0, 255)) / 255.0f;
    if (ws.probes.built()) ws.probes.tint(p, bodyLight, base, out);
}

#endif  // OMK_REMASTER
