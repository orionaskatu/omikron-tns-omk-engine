// SPDX-License-Identifier: GPL-3.0-or-later
// THE PRELOAD RADIUS (`todo/remaster.md`; first planned for the Quest,
// `todo/quest-port.md` §4) - NOT IN THE ORIGINAL, and nothing of the game's
// logic moves: the engine still loads into its two resident slots at its own
// moments (`Area_TickLoad`, the transitions); what changes is that the data
// those loads ask for is already PREPARED when they ask.
//
// Every few frames: the doors of the resident pair (`Session::doorTargets`,
// read statically, two hops - a street's door opens on an airlock or a
// corridor, a "Sas", and the room is behind it; from inside, the sas and the
// street its load evicted), each at the zone the player walks to. A set
// whose nearest door is within `preloadMetres` is prepared on a thread
// (`prepareSet`, the same job a load starts) and, once ready, its textures
// handed to the renderer, which uploads a few a frame and keeps them
// (`Renderer::setPreloadTextures` / `preloadStep`). Past `preloadMetres` + 10
// it is dropped - the gap stops a player on the edge from preparing and
// dropping the same set. `askSet` takes a prepared set instead of starting
// one. At most `kHeld` are held, the nearest first: a city's tunnel opens on
// the next city - and a NEARER door takes the place of the farthest held (the
// audit's bug 3, 2026-10-23: three doors within reach filled the six, and the
// door walked to next waited until one of them was past the keep radius).
#if OMK_REMASTER

#include "../sdl/playstate.h"

#include "platform/env.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr double kInchesPerMetre = 39.37;   // the game's unit
constexpr std::size_t kHeld = 6;
constexpr int kEvery = 10;                  // frames between distance passes
constexpr int kUploadsPerFrame = 3;         // textures the renderer takes a frame
}  // namespace

// `askSet`'s first question: the set the preload radius prepared for this
// area, its job done or still running - `integrateSet` waits on it exactly
// as on one started there. -> whether it was taken.
bool PlayState::remasterTakePreload(int slot, const std::string& stem, int area, long frame) {
    const auto it = preloads.find(stem);
    if (it == preloads.end() || it->second.area != area || !it->second.L) return false;
    auto P = it->second.L;
    preloads.erase(it);   // its pins stay until the next refresh: the pool takes them first
    P->slot = slot;
    P->askedFrame = frame;
    setLoads[slot] = P;
    ++preloadTaken;
    std::printf("preload: %s (AREA %d) taken - %s\n", stem.c_str(), area,
                P->job && !P->job->ready() ? "still preparing, waited for" : "prepared ahead");
    omk::event("preload.take", {{"set", stem}, {"area", area}, {"ready", !P->job || P->job->ready()}});
    return true;
}

void PlayState::preloadPin() {
    std::vector<omk::Texture> pins;
    for (const auto& [stem, p] : preloads)
        if (p.sent && p.L && p.L->found)
            pins.insert(pins.end(), p.L->out.tex.begin(), p.L->out.tex.end());
    if (world_) world_->setPreloadTextures(pins);
}

void PlayState::phasePreload() {
    if (preloadMetres <= 0.0f || !session_ || !world_) return;
    auto& session = *session_;
    const auto& fs = *fs_;
    // the renderer's share of the work: a few uploads a frame, spread
    world_->preloadStep(kUploadsPerFrame);
    // the doors, read again when the resident pair changes
    const int k0 = session.residentSlot(0).area, k1 = session.residentSlot(1).area;
    if (k0 != preloadDoorsKey[0] || k1 != preloadDoorsKey[1]) {
        preloadDoorsKey[0] = k0;
        preloadDoorsKey[1] = k1;
        preloadDoors = session.doorTargets(2);
        int second = 0;
        for (const auto& d : preloadDoors) second += d.via >= 0 ? 1 : 0;
        std::printf("preload: areas %d and %d resident - %zu destinations behind their doors (%d through an "
                    "airlock or corridor)\n", k0, k1, preloadDoors.size(), second);
        omk::event("preload.doors", {{"area0", k0}, {"area1", k1}, {"targets", static_cast<long>(preloadDoors.size())},
                                     {"second", second}});
    }
    // ...and the distances only once the player stands somewhere: before
    // that his position is the origin, and every door near it looks close
    if (frameNo % kEvery != 0 || !session.playerPlaced()) return;
    const float* p = session.playerPos();
    const double reach = preloadMetres * kInchesPerMetre, keep = (preloadMetres + 10.0) * kInchesPerMetre;
    // each destination's nearest door
    std::map<std::string, std::pair<double, int>> near;   // stem -> (distance, area)
    for (const auto& d : preloadDoors) {
        const double dx = d.pos[0] - p[0], dy = d.pos[1] - p[1], dz = d.pos[2] - p[2];
        const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        auto it = near.find(d.set);
        if (it == near.end() || dist < it->second.first) near[d.set] = {dist, d.area};
    }
    bool pinsChanged = false;
    // released: out of the keep radius, or no door leads there any more
    for (auto it = preloads.begin(); it != preloads.end();) {
        const auto n = near.find(it->first);
        if (n == near.end() || n->second.first > keep || n->second.second != it->second.area) {
            std::printf("preload: %s (AREA %d) released - %s\n", it->first.c_str(), it->second.area,
                        n == near.end() ? "no door leads there now" : "past the keep radius");
            omk::event("preload.drop", {{"set", it->first}, {"area", it->second.area},
                                        {"metres", n == near.end() ? -1.0 : n->second.first / kInchesPerMetre}});
            pinsChanged = pinsChanged || it->second.sent;
            it = preloads.erase(it);
        } else {
            it->second.dist = n->second.first;
            ++it;
        }
    }
    // wanted: within reach, nearest first, up to `kHeld` held - a want
    // nearer than the farthest held taking its place (`OMK_PRELOAD_HELD=n`,
    // an instrument: the cap n, so a short walk reaches it)
    static const std::size_t held =
        static_cast<std::size_t>(std::max(1L, omk::envInt("OMK_PRELOAD_HELD", static_cast<long>(kHeld))));
    std::vector<std::pair<double, std::string>> want;
    for (const auto& [stem, dn] : near)
        if (dn.first <= reach && !preloads.count(stem)) want.push_back({dn.first, stem});
    std::sort(want.begin(), want.end());
    for (const auto& [dist, stem] : want) {
        if (preloads.size() >= held) {
            const auto far = std::max_element(preloads.begin(), preloads.end(), [](const auto& a, const auto& b) {
                return a.second.dist < b.second.dist;
            });
            if (far->second.dist <= dist) break;   // every held is nearer: the rest wait
            std::printf("preload: %s (AREA %d) released - %s's door is nearer\n", far->first.c_str(),
                        far->second.area, stem.c_str());
            omk::event("preload.drop", {{"set", far->first}, {"area", far->second.area}, {"for", stem},
                                        {"metres", far->second.dist / kInchesPerMetre},
                                        {"forMetres", dist / kInchesPerMetre}});
            pinsChanged = pinsChanged || far->second.sent;
            preloads.erase(far);
        }
        const int area = near[stem].second;
        const auto o = fs.resolve("MESHES/DECORS/" + stem + ".3DO");
        if (!o) continue;
        auto L = std::make_shared<SetLoad>();
        L->stem = stem;
        L->area = area;
        L->dayNight = omk::dayNightAt(0, session.areaChunkOf(area)).floorByClock;
        L->askedFrame = frameNo;
        L->path3do = *o;
        if (const auto t = fs.resolve("MESHES/DECORS/" + stem + ".3DT")) L->path3dt = *t;
        L->job = std::make_unique<omk::BackgroundJob>([this, L] { prepareSet(*L); });
        preloads[stem] = Preloaded{area, L, false, dist};
        ++preloadMade;
        std::printf("preload: %s (AREA %d) - its door %.1f m away, prepared on a thread\n", stem.c_str(), area,
                    dist / kInchesPerMetre);
        omk::event("preload.want", {{"set", stem}, {"area", area}, {"metres", dist / kInchesPerMetre}});
    }
    // prepared: its textures to the renderer
    for (auto& [stem, pl] : preloads)
        if (!pl.sent && pl.L && (!pl.L->job || pl.L->job->ready())) {
            pl.sent = true;
            pinsChanged = true;
        }
    if (pinsChanged) preloadPin();
}

#endif  // OMK_REMASTER
