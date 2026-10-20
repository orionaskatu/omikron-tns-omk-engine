// SPDX-License-Identifier: GPL-3.0-or-later
// THE TILED POINT LIGHTS - the remaster's emitters' light (`o3de/emitterlight.h`)
// without a cap on the frame. NOT IN THE ORIGINAL.
//
// The per-pixel lighting took the 56 point lights nearest the camera, so a
// far neon lit nothing round it and lights came and went as he walked ("the
// neons look turned off", the reader, 2026-10-20). Here EVERY point light the
// frame hands over (`View::points`, all within the clip distance) is binned
// to screen TILES of `kLightTile` render pixels: each light's sphere is
// projected - conservatively - to a pixel rectangle, and each tile it covers
// lists it. A fragment reads its own tile's list and nothing else, so the
// cost is the lights that can reach it, not all of them.
//
// The projection is `cameraBasis`'s (`o3de/raster.h`), the one the GPU
// backends build their matrices from: screen x = w/2 (1 + (v.s / v.f) / tanH),
// y = h/2 (1 - (v.u / v.f) / tanV), mirrored in x under `flipX`.
//
// ...and in DEPTH: a tile is cut into `kLightSlices` slices of view depth,
// spaced exponentially from `kSliceNear` to `kSliceFar`, and a light joins
// only the slices its sphere reaches - a near light covers most of the screen
// and, binned in 2D alone, filled every tile and crowded the far neons out
// again (880 of 880 tiles full in Qalisar). A fragment takes its slice from
// its own view depth (`vDepth`, the fog's).
//
// The layout, 32-bit words: [0] tiles across, [1] tiles down, [2] slices,
// [3] the tile's size in render pixels, then
// two words a cluster (slice-major, then row, then column) - the offset of its
// list and its count - then the lists, light indices into `View::points`. A
// cluster keeps at most `kTileCap`, nearest the camera first.
#pragma once

#include "o3de/raster.h"
#include "o3de/renderer.h"

#include <cstdint>
#include <span>
#include <vector>

namespace omk {

inline constexpr int kLightTile = 32;     // render pixels a side, at least...
inline constexpr int kMaxAcross = 64;     // ...and at most this many across: the tile GROWS with the
                                          // resolution, or the lists did (past 1512x982 they overflowed
                                          // the buffer and every point light went - the reader, 2026-10-20)
inline constexpr int kLightSlices = 16;   // depth slices a tile
inline constexpr float kSliceNear = 50.0f, kSliceFar = 20000.0f;   // view depth, game units
// the slice of a view depth (the shader's own formula, `scene.frag`)
int depthSlice(float z);
inline constexpr int kTileCap = 64;       // lights a cluster at most, the nearest IT
inline constexpr int kMaxPoints = 4096;   // View::points at most

struct LightTiles {
    std::vector<std::uint32_t> words;
    int across = 0, down = 0, tile = kLightTile;
    long listed = 0;    // entries over all tiles (for the log)
    int full = 0;       // tiles that hit kTileCap
};

// `points` binned for a camera whose picture is `pxW x pxH` render pixels
// (`cam.w x cam.h` times the supersampling).
void binLights(const RCamera& cam, int pxW, int pxH, std::span<const View::GpuLight> points, LightTiles& out);

}  // namespace omk
