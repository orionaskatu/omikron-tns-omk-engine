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
    remasterBodies_.erase(name);   // a reload starts again
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
        // BOTH RESTS KEPT for the original/remaster switch (B2): the flat one
        // shown while the original's look is - from the start in `mode =
        // original`, which rounds all the same
        RemasterBody& b = remasterBodies_[name];
        b.other = m.flat;
        b.roundShown = true;
        b.active = true;
        if (!remasterShown_ || remasterStartHidden_) {
            std::swap(m.rest, b.other);
            b.roundShown = false;
        }
        m.rest.revision = ++worldGeoRev;
    }
}

// ...and the player's
void PlayState::remasterSmoothPlayer() {
    // the ROUNDED BODIES (`o3de/smoothbody.h`), as `charModelFor` does
    // for everyone else - his face kept for its morph
    remasterPlayerBody_ = RemasterBody{};
    if (smoothes(playerModel)) {
        omk::Geometry flat = playerBody.rest;
        const auto face = omk::faceMeshOf(playerBody.meshes);
        const auto ss = omk::smoothBody(playerBody.rest, face.valid() ? face.base : 0,
                                        face.valid() ? face.base + static_cast<std::size_t>(face.count) : 0,
                                        3, smoothBodies);
        playerBody.rest.revision = ++worldGeoRev;
        std::printf("smooth: the player's %s - %zu of %zu triangles split (%zu kept at a seam, %zu the "
                    "face), %zu corners\n", playerModel.c_str(), ss.split, ss.triangles, ss.keptSeam,
                    ss.keptFace, playerBody.rest.corners.size());
        // ...both kept, as every model's (B2)
        remasterPlayerBody_.other = std::move(flat);
        remasterPlayerBody_.active = true;
        if (!remasterShown_ || remasterStartHidden_) {
            std::swap(playerBody.rest, remasterPlayerBody_.other);
            remasterPlayerBody_.roundShown = false;
            playerBody.rest.revision = ++worldGeoRev;
        }
    }
}

// THE SWITCH's bodies (B2, `playremaster_mode.cpp`): each rounded model's
// rest swapped with the one it kept - every actor wearing it reads the rest
// each frame (`applyPose` re-sizes its posed copy), the crowd's copies are
// cut from the flat rest either way (`lodRestFor`), and the new revision
// sends it to the GPU again
void PlayState::remasterSwapBodies(bool round) {
    for (auto it = remasterBodies_.begin(); it != remasterBodies_.end();) {
        const auto m = charModels.find(it->first);
        if (m == charModels.end() || !it->second.active) { it = remasterBodies_.erase(it); continue; }
        if (it->second.roundShown != round) {
            std::swap(m->second.rest, it->second.other);
            m->second.rest.revision = ++worldGeoRev;
            it->second.roundShown = round;
        }
        ++it;
    }
    RemasterBody& p = remasterPlayerBody_;
    if (p.active && p.roundShown != round) {
        std::swap(playerBody.rest, p.other);
        // the underwater shimmer's phase, as the scene sets it on the rest
        // shown (`playframe_world_scene.cpp`)
        for (std::size_t i = 0; i < playerBody.rest.corners.size(); ++i)
            playerBody.rest.corners[i].phase =
                underwater && i < playerBody.rest.cornerVertex.size() && playerBody.rest.cornerVertex[i] >= 0
                    ? static_cast<float>((2u * static_cast<std::uint32_t>(playerBody.rest.cornerVertex[i])) % 32u)
                    : -1.0f;
        playerBody.rest.revision = ++worldGeoRev;
        p.roundShown = round;
        // ...and THE FEET: the anchor is the posed body's lowest corner,
        // latched (`playframe_world_crowd.cpp`, omk-play 69) - the one the
        // latch measured on this body in the same pose takes its place
        if (playerFeetKnown) std::swap(playerFeet, remasterOtherFeet_);
    }
}

// the latch's pose on the body the switch keeps: its lowest corner, as the
// latch takes the shown one's
void PlayState::remasterFeetLatched(const std::vector<omk::MeshPose>& pose) {
    const RemasterBody& p = remasterPlayerBody_;
    if (!p.active || p.other.cornerMesh.size() != p.other.corners.size()) return;
    omk::Geometry g;
    omk::applyPose(g, p.other, playerBody.meshes, pose);
    remasterOtherFeet_ = -1e9f;
    for (const auto& c : g.corners) remasterOtherFeet_ = std::max(remasterOtherFeet_, c.y);
}

#endif  // OMK_REMASTER
