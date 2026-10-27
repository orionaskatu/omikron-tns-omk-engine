// SPDX-License-Identifier: GPL-3.0-or-later
// THE REFLECTION'S CLIP MEMO across a set's reload (`drawWithWater`'s
// `clipAbove`, `src/o3de/renderer.cpp` - the remaster audit's bug 2,
// `todo/remaster.md`). The memo keeps, per geometry and plane height, the runs
// of each set draw that lie above the plane. A world slot reloaded IN PLACE
// keeps its geometry's address, so this drives `drawWithWater` with one
// `Geometry` object holding three sets in turn and a renderer that records
// what it is handed to reflect:
//
//   reload   set A (both triangles above the plane), then set B moved into the
//            same object with a new revision (its second triangle below) -
//            the reflection must take B's one triangle, not A's two;
//   moved    set B's first triangle moved below the plane by the set motion
//            patch (`dirtyFrom`/`dirtyTo`/`dirtyCorners`) - nothing above;
//   kept     a second draw, untouched by the patch, still clipped right.
//
//     clip_memo        prints "@clip <case> <runs> <want> ok|WRONG"; exit 1 on a WRONG
#include "o3de/renderer.h"

#include <cstdio>
#include <span>
#include <utility>
#include <vector>

#if OMK_REMASTER
namespace {

using Runs = std::vector<std::pair<std::size_t, std::size_t>>;

// what a backend's water pass is handed: the draws it reflects
struct Recorder : omk::Renderer {
    Runs above;
    bool init(int, int) override { return true; }
    void setTextures(std::span<const omk::Texture>) override {}
    void begin(const omk::View&) override {}
    void submit(const omk::Draw&) override {}
    void end() override {}
    const omk::Surface& readback() override { return none; }
    omk::RasterStats stats() const override { return {}; }
    const char* name() const override { return "clip_memo"; }
    omk::Surface none;
    bool drawsWater() const override { return true; }
    bool drawWaterScene(const omk::View&, const omk::View&, std::span<const omk::Draw>,
                        std::span<const omk::Draw> a, std::span<const omk::Draw>, float, float) override {
        above.clear();
        for (const auto& d : a) above.emplace_back(d.start, d.count);
        return true;
    }
};

omk::Corner at(float x, float y, float z) {
    omk::Corner c{};
    c.x = x; c.y = y; c.z = z;
    c.phase = -1.0f;
    return c;
}

// two triangles at heights `y0`, `y1` (the game's Y points DOWN: negative is
// above the plane at 0), a third pair at `y2`, `y3`, and a water triangle on
// the plane
std::vector<omk::Corner> set(float y0, float y1, float y2, float y3) {
    std::vector<omk::Corner> c;
    for (float y : {y0, y1, y2, y3}) {
        c.push_back(at(0, y, 0));
        c.push_back(at(10, y, 0));
        c.push_back(at(0, y, 10));
    }
    c.push_back(at(0, 0, 0));
    c.push_back(at(10, 0, 0));
    c.push_back(at(0, 0, 10));
    return c;
}

}  // namespace

int main() {
    omk::Geometry g;
    std::uint64_t rev = 0;
    g.corners = set(-10, -10, -10, -10);
    g.revision = ++rev;
    omk::Draw a, b, w;
    a.geo = b.geo = w.geo = &g;
    a.start = 0; a.count = 6;
    b.start = 6; b.count = 6;
    w.start = 12; w.count = 3; w.blend = omk::Blend::Add;
    const std::vector<omk::Draw> draws{a, b, w};
    omk::View v;
    v.cam.eye[0] = 5; v.cam.eye[1] = -100; v.cam.eye[2] = -50;
    v.cam.at[0] = 5; v.cam.at[1] = 0; v.cam.at[2] = 5;
    omk::WaterPlane wp;
    wp.found = true;
    wp.y = 0.0f;
    Recorder r;
    int wrong = 0;
    const auto frame = [&](const char* name, const omk::Draw& d, const Runs& want) {
        r.above.clear();
        omk::drawWithWater(r, draws, v, &g, wp, 1.0f);
        Runs got;
        for (const auto& [s, n] : r.above)
            if (s >= d.start && s < d.start + d.count) got.emplace_back(s, n);
        std::size_t gn = 0, wn = 0;
        for (const auto& [s, n] : got) gn += n;
        for (const auto& [s, n] : want) wn += n;
        const bool ok = got == want;
        wrong += ok ? 0 : 1;
        std::printf("@clip %s %zu %zu %s\n", name, gn, wn, ok ? "ok" : "WRONG");
    };
    frame("first", a, Runs{{0, 6}});
    // RELOADED IN PLACE: the slot's `w = std::move(L.out)`, then a new revision
    g = omk::Geometry{};
    g.corners = set(-10, 10, -10, -10);
    g.revision = ++rev;
    frame("reload", a, Runs{{0, 3}});
    // THE MOTION PATCH: the first triangle below the plane, from the revision drawn
    g.dirtyFrom = g.revision;
    for (std::size_t i = 0; i < 3; ++i) g.corners[i].y = 10.0f;
    g.revision = ++rev;
    g.dirtyTo = g.revision;
    g.dirtyCorners = {2, 0, 1};   // unsorted, as the patch leaves it
    frame("moved", a, Runs{});
    frame("kept", b, Runs{{6, 6}});
    return wrong ? 1 : 0;
}
#else
int main() {
    std::printf("clip_memo: built without OMK_REMASTER - skipped\n");
    return 0;
}
#endif
