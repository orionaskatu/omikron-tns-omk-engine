// SPDX-License-Identifier: GPL-3.0-or-later
// FRAME GENERATION's numbers and decisions, shared by the Direct3D 12 and the
// Vulkan windows (`todo/enhancements.md` 27).
//
// The passes are one GLSL source (`backends/vulkan/shaders/fg_cap.frag`,
// `fg_splat.frag`, `fg_resolve.frag`); this header is the other half of "the
// same frame generation": the push blocks those shaders read, the rule that
// decides whether an in-between frame has a refresh to be shown in (the
// self-limiting rule of the user's stutter, 2026-10-19), the camera-cut test,
// and the probe `verify.py: engine: framegen` reads - written once so the two
// backends cannot disagree about when to interpolate or how it is judged.
#pragma once

#include "platform/datafs.h"
#include "platform/events.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace omk::framegen {

// `fg_cap.frag`'s and `fg_splat.frag`'s / `fg_resolve.frag`'s push blocks, field for field
struct CapPush { float pic[4], toRender[4], depthAB[4]; };
struct Push { float pic[4], k[4]; };

// THE DISPLAY'S ROOM: an in-between frame only while a frame's own work (the last
// present's return to this present's call) spans about two refreshes, with a margin
// each way so it does not flicker on and off (a 60 Hz display: 33 ms, 30 real fps or
// fewer; 144 Hz: 14 ms). Measured (2026-10-19, 4K, 60 Hz, ~21 ms of work): without it
// every in-between present blocked 22-25 ms on the v-sync and the loop went from
// 21 ms to 57 ms - the real frames at 17 a second, the player visibly stepping.
// `refreshHz` 0 (unknown) or `always` (the harness's OMK_FRAMEGEN_ALWAYS): always room.
inline bool roomFor(bool hadRoom, double work, int refreshHz, bool always) {
    if (refreshHz <= 0 || always) return true;
    const double V = 1.0 / static_cast<double>(refreshHz);
    return hadRoom ? work > 1.7 * V : work > 1.9 * V;
}

// THE CAMERA CUT: the engine's camera jumps - a cutscene's editing, an area load, a
// scripted placement - and the picture before the jump must not be warped into the one
// after. More than five metres (200 inches) in a frame, or a turn of more than 30
// degrees (`fwd . prevFwd` under 0.866), is a jump.
inline bool cameraCut(const float eye[3], const float prevEye[3], float fwdDotPrev) {
    const float dx = eye[0] - prevEye[0], dy = eye[1] - prevEye[1], dz = eye[2] - prevEye[2];
    return dx * dx + dy * dy + dz * dz > 200.0f * 200.0f || fwdDotPrev < 0.866f;
}

// `OMK_FRAMEGEN_PROBE=<prefix>`: THE CHECK's instrument. Frames N-2 (`a`) and N (`b`)
// interpolated with the vectors doubled (`warp`), laid against the frame that really was
// rendered between them, N-1 (`real`) - beside the plain half-and-half blend of the two
// and frame N repeated. The mean error over the picture (a channel, 0..255) and over its
// MOVING part (where N-2 and N differ), a line and a `framegen.probe` event a frame; the
// last frame's three pictures written to `<prefix>warp.rgba`, `real.rgba`, `blend.rgba`
// (RGBA8, the display size, `stride` pixels a row). `W` x `H` the picture's rectangle.
inline void probeCompare(const std::vector<unsigned char>& warp, const std::vector<unsigned char>& real,
                         const std::vector<unsigned char>& a, const std::vector<unsigned char>& b, int W, int H,
                         int stride, long seq, const char* prefix) {
    std::vector<unsigned char> blend(warp.size(), 255);
    double eW = 0, eB = 0, eR = 0, mW = 0, mB = 0, mR = 0;
    long moving = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(x)) * 4;
            double dw = 0, db = 0, dr = 0;
            int change = 0;
            for (int c = 0; c < 3; ++c) {
                const int bl = (a[i + c] + b[i + c] + 1) / 2;
                blend[i + c] = static_cast<unsigned char>(bl);
                dw += std::abs(warp[i + c] - real[i + c]);
                db += std::abs(bl - real[i + c]);
                dr += std::abs(b[i + c] - real[i + c]);
                change += std::abs(a[i + c] - b[i + c]);
            }
            eW += dw; eB += db; eR += dr;
            if (change > 24) { mW += dw; mB += db; mR += dr; ++moving; }
        }
    const double px = 3.0 * static_cast<double>(std::max(W * H, 1)), mpx = 3.0 * static_cast<double>(std::max(moving, 1L));
    const double share = static_cast<double>(moving) / static_cast<double>(std::max(W * H, 1));
    std::printf("framegen probe: frame %ld - N-2 and N in between, against the real N-1: mean error %.2f warped, "
                "%.2f blended, %.2f repeated; %.1f%% of the picture moving: %.2f, %.2f, %.2f there\n", seq,
                eW / px, eB / px, eR / px, 100.0 * share, mW / mpx, mB / mpx, mR / mpx);
    omk::event("framegen.probe", {{"frame", seq}, {"warp", eW / px}, {"blend", eB / px}, {"repeat", eR / px},
                                  {"moving", share}, {"mwarp", mW / mpx}, {"mblend", mB / mpx}, {"mrepeat", mR / mpx}});
    const auto write = [&](const char* name, const std::vector<unsigned char>& pic) {
        const std::string path = std::string(prefix) + name;
        if (!omk::safeOutputPath(path.c_str())) return;
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(pic.data(), 1, pic.size(), f);
            std::fclose(f);
        }
    };
    write("warp.rgba", warp);
    write("real.rgba", real);
    write("blend.rgba", blend);
}

}  // namespace omk::framegen
