// SPDX-License-Identifier: GPL-3.0-or-later
// The reflecting floors - see floorreflect.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "o3de/floorreflect.h"

#include "platform/datafs.h"   // upperAscii

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace omk {

bool FloorList::open(const std::string& path) {
    byKey_.clear();
    count_ = 0;
    opened_ = false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string ln(line);
        if (const auto h = ln.find('#'); h != std::string::npos) ln.resize(h);
        const auto eq = ln.find('=');
        if (eq == std::string::npos) continue;
        char set[128] = {}, tx[128] = {};
        float s = 0.0f;
        if (std::sscanf(ln.substr(0, eq).c_str(), "%127s %127s", set, tx) != 2) continue;
        if (std::sscanf(ln.substr(eq + 1).c_str(), "%f", &s) != 1 || !(s > 0.0f)) continue;
        byKey_[upperAscii(set) + " " + upperAscii(tx)] = std::min(s, 1.0f);
        ++count_;
    }
    std::fclose(f);
    opened_ = true;
    return true;
}

float FloorList::strength(const std::string& set, const std::string& texture) const {
    if (!opened_) return 0.0f;
    const std::string t = upperAscii(texture);
    if (const auto it = byKey_.find(upperAscii(set) + " " + t); it != byKey_.end()) return it->second;
    if (const auto it = byKey_.find("* " + t); it != byKey_.end()) return it->second;
    return 0.0f;
}

FloorList& floorList() {
    static FloorList l;
    return l;
}

FloorPlanes floorPlanes(const Geometry& g, std::span<const Texture> tex, const FloorList& list,
                        const std::string& set) {
    FloorPlanes out;
    if (!list.active()) return out;
    out.strength.assign(tex.size(), 0.0f);
    bool named = false;
    for (std::size_t i = 0; i < tex.size(); ++i)
        if (tex[i].hasPixels()) named |= (out.strength[i] = list.strength(set, tex[i].name)) > 0.0f;
    if (!named) { out.strength.clear(); return out; }
    // the listed LEVEL opaque triangles, by height in steps of 4 units
    std::map<int, FloorPlanes::Level> byHeight;
    forLevelTriangles(g, [&](const Batch& b) {
        return b.blend == Blend::Opaque && b.material >= 0 && static_cast<std::size_t>(b.material) < out.strength.size() &&
               out.strength[static_cast<std::size_t>(b.material)] > 0.0f;
    }, [&](const Corner& a, const Corner& c1, const Corner& c2) {
        const double area = std::fabs((c1.x - a.x) * (c2.z - a.z) - (c1.z - a.z) * (c2.x - a.x)) * 0.5;
        auto& L = byHeight[static_cast<int>(std::lround(a.y / 4.0f))];
        L.area += area;
        L.tris.insert(L.tris.end(), {(a.x + c1.x + c2.x) / 3.0f, (a.z + c1.z + c2.z) / 3.0f,
                                     static_cast<float>(area)});
    });
    // ...merged where they lie within `kFloorMerge` of each other, at the
    // HIGHEST of them (the smallest y; ascending here): the others lie below
    // the plane and the reflection pass clips them - at a mean height the top
    // one would be drawn into its own reflection
    for (auto& [k, L] : byHeight) {
        const float y = static_cast<float>(k) * 4.0f;
        if (!out.levels.empty() && y - out.levels.back().y <= kFloorMerge) {
            auto& m = out.levels.back();
            m.area += L.area;
            m.tris.insert(m.tris.end(), L.tris.begin(), L.tris.end());
        } else {
            L.y = y;
            out.levels.push_back(std::move(L));
        }
    }
    return out;
}

WaterPlane chooseFloor(const FloorPlanes& f, const float eye[3]) {
    WaterPlane out;
    double best = 0.0;
    const float r2 = kFloorNear * kFloorNear;
    for (const auto& L : f.levels) {
        if (eye[1] >= L.y - 1.0f) continue;   // not below the eye (Y is down)
        double near = 0.0;
        for (std::size_t i = 0; i + 2 < L.tris.size(); i += 3) {
            const float dx = L.tris[i] - eye[0], dz = L.tris[i + 1] - eye[2];
            if (dx * dx + dz * dz <= r2) near += L.tris[i + 2];
        }
        if (near > best) { best = near; out.found = true; out.y = L.y; out.area = near; }
    }
    return out;
}

WaterPlane chooseFloorNear(const FloorPlanes& f, const float eye[3], float refY) {
    WaterPlane out;
    float bestGap = 0.0f;
    const float r2 = kFloorNear * kFloorNear;
    for (const auto& L : f.levels) {
        if (eye[1] >= L.y - 1.0f) continue;   // not below the eye (Y is down)
        double near = 0.0;
        for (std::size_t i = 0; i + 2 < L.tris.size(); i += 3) {
            const float dx = L.tris[i] - eye[0], dz = L.tris[i + 1] - eye[2];
            if (dx * dx + dz * dz <= r2) near += L.tris[i + 2];
        }
        if (near < kWetPlaneMin) continue;
        const float gap = std::fabs(L.y - refY);
        if (!out.found || gap < bestGap) { bestGap = gap; out.found = true; out.y = L.y; out.area = near; }
    }
    return out;
}

WaterPlane chooseFloorSecond(const FloorPlanes& f, const float eye[3], const float at[3], float firstY) {
    WaterPlane out;
    const float r2 = kFloorNear * kFloorNear;
    // the way the camera looks, on the ground: floor behind it is never drawn
    float fx = at[0] - eye[0], fz = at[2] - eye[2];
    const float fl = std::sqrt(fx * fx + fz * fz);
    if (fl > 0.0f) { fx /= fl; fz /= fl; }
    for (const auto& L : f.levels) {
        if (eye[1] >= L.y - 1.0f) continue;                              // not below the eye (Y is down)
        if (std::fabs(L.y - firstY) <= 2.0f * kWetPlaneBand) continue;   // the first plane's
        double near = 0.0;
        for (std::size_t i = 0; i + 2 < L.tris.size(); i += 3) {
            const float dx = L.tris[i] - eye[0], dz = L.tris[i + 1] - eye[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 > r2) continue;
            // within `kSecondCone` of the look (a camera looking straight down: all)
            const float ahead = dx * fx + dz * fz;
            if (fl > 0.0f && (ahead <= 0.0f || ahead * ahead < kSecondCone * kSecondCone * d2)) continue;
            near += L.tris[i + 2];
        }
        if (near >= kWetPlaneMin && near > out.area) { out.found = true; out.y = L.y; out.area = near; }
    }
    return out;
}

}  // namespace omk

#endif  // OMK_REMASTER
