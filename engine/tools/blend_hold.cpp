// SPDX-License-Identifier: GPL-3.0-or-later
// A BLENDED TRANSITION HOLDS TWO FIXED KEYS, IT DOES NOT REPLAY THE TARGET.
//
//     blend_hold <gamedata>
//
// `Actor_BlendToClip` (0x0045C510) and `sub_471820` slerp the key the old
// clip was left on toward the target clip's START key, by frame/blendFrames.
// H1Avnt's group 111 - what `player.move 111` installs before the sneak call
// at the Telis lunch (SCENE 53 pc 1148) - is H_SNKON (19 frames), then a
// pass-through that blends 40 frames into H_SNKON AT FRAME 19, its own last
// pose: the engine HOLDS the raised wrist. A port that plays the clip behind
// the pass-through on the blend counter raises it a second time, which is
// what a reader saw.
//
// Prints, per phase, how many ticks it lasted and the largest angle (degrees,
// over every bone) between the pose handed to the poser and H_SNKON's last
// frame. The RISE must move (the wrist comes up, so the measure can see
// motion at all); the BLEND and the HOLD after it must not.
#include "actor/player.h"
#include "actor/pose.h"
#include "formats/ctl.h"
#include "formats/mesh3do.h"
#include "o3de/geom3do.h"
#include "platform/datafs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {
// the largest bone angle between row `f` of `t` and row `g` of `ref`
double maxDev(const omk::NodeTracks& t, int f, const omk::NodeTracks& ref, int g) {
    double worst = 0.0;
    const auto& rt = t.quats[static_cast<std::size_t>(f)];
    const auto& rr = ref.quats[static_cast<std::size_t>(g)];
    for (std::size_t i = 0; i < t.ids.size() && i < rt.size(); ++i)
        for (std::size_t k = 0; k < ref.ids.size() && k < rr.size(); ++k) {
            if (t.ids[i] != ref.ids[k]) continue;
            const auto& a = rt[i];
            const auto& b = rr[k];
            double d = std::fabs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
            d = std::min(1.0, d);
            worst = std::max(worst, 2.0 * std::acos(d) * 180.0 / 3.14159265358979);
        }
    return worst;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: blend_hold <gamedata>\n"); return 2; }
    const omk::DataFs fs(argv[1]);
    const auto ctlData = fs.read("ANIMS/H1AVNT.CTL");
    const auto modData = fs.read("MESHES/PERSOS/HO1_FN.3DO");
    const auto setData = fs.read("MESHES/DECORS/ANEKBAH.3DO");
    const omk::CtlFile ctl = omk::readCtl(ctlData);
    std::vector<omk::Mesh> meshes;
    if (const auto h = omk::readHeader(modData)) meshes = omk::readMeshes(modData, *h);
    const auto soup = omk::collisionSoup(setData, omk::SoupKind::Walkable);
    if (!ctl.valid || meshes.empty() || soup.empty()) {
        std::printf("blend_hold: inputs missing\n"); return 1;
    }
    omk::PlayerController::Setup su;
    su.ctl = &ctl; su.ctlData = ctlData; su.meshes = &meshes; su.soup = &soup;
    su.pos[0] = 1804.0f; su.pos[1] = 0.0f; su.pos[2] = -6890.0f;
    su.facing = 336.0f;
    omk::PlayerController pc(su);
    for (int i = 0; i < 5; ++i) pc.tick(1.0f, 0);
    if (!pc.goToMove(111)) { std::printf("blend_hold: no group 111\n"); return 1; }

    const int snk = pc.clip();
    const omk::NodeTracks* ref = pc.clipTracks(snk);
    if (pc.ctlStateName() != "H_SNKON" || !ref) {
        std::printf("blend_hold: group 111 did not start on H_SNKON\n"); return 1;
    }
    const int last = ref->frames - 1;
    // rise: the default entry; blend: the nameless pass-through; hold: after
    int n[3] = {0, 0, 0};
    double dev[3] = {0.0, 0.0, 0.0};
    int phase = 0;
    for (int t = 0; t < 120; ++t) {
        const std::string nm = pc.ctlStateName();
        if (phase == 0 && nm.empty()) phase = 1;
        else if (phase == 1 && !nm.empty()) phase = 2;
        if (const omk::NodeTracks* pt = pc.poseTracks()) {
            const int f = std::min(pc.poseFrame(), pt->frames - 1);
            dev[phase] = std::max(dev[phase], maxDev(*pt, f, *ref, last));
        }
        ++n[phase];
        pc.tick(1.0f, 0);
    }
    std::printf("clip H_SNKON %d frames %d\n", snk, ref->frames);
    std::printf("rise ticks %d dev %.1f\n", n[0], dev[0]);
    std::printf("blend ticks %d dev %.1f\n", n[1], dev[1]);
    std::printf("hold ticks %d dev %.1f\n", n[2], dev[2]);
    return 0;
}
