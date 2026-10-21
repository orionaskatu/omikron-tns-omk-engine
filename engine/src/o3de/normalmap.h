// SPDX-License-Identifier: GPL-3.0-or-later
// THE MATERIAL MAPS' GENERATOR - the remaster's B3 (`todo/remaster.md`). NOT IN
// THE ORIGINAL, and not a port: the game has no normal maps.
//
// A pack texture's NORMAL MAP and HEIGHT from its colour alone, the classical
// way (Materialize's): the texture's brightness read as height, BAND-PASSED -
// detail minus a wide blur - so the light and shade the artists painted in
// (a wall darker at its foot, a lit edge) does not become a slope; the slope
// summed over several scales of blur, so a stone reads as a stone and its
// grain as grain. The texture TILES (the sets wrap their UVs), so every blur
// and every difference wraps; a KEYED texel (alpha 0, a cutout's hole) is
// flat and weighs nothing.
//
// The map's convention is OpenGL's (DeepBump's, Blender's): R along +u (the
// image's right), G along -v (the image's UP), B out of the surface, each
// 0..255 for -1..1; its alpha the band-passed height, 128 the mean.
//
// THE COMBINATION (the reader's pick, 2026-10-21, from a sheet of seven
// textures): an AI generator's normal map - DeepBump's, which reads VOLUMES (a
// sign's disc domed, a grille's slats rounded) but invents (a face's
// blotches, streaks on a plain wall) - BLURRED until its inventions are gone,
// as the large forms, plus this one's slope as the grain. Without a base the
// map is this one's alone, which anyone can rebuild from the repo.
//
// Used by `texpack_tool normals` (`tools/omktexpack.py materials`); the engine
// reads the result as the pack's `<key>_n.png`.
#pragma once

#include <cstdint>
#include <vector>

namespace omk {

struct NormalMapParams {
    float strength = 1.0f;   // the slope's scale: 1 tilts a 10% brightness step ~45 degrees at the finest scale
    int   wide = 48;         // the band-pass's wide blur, texels: shading broader than this is not height
    // the BASE (an AI generator's map, OpenGL's convention, RGBA w x h), null for none
    const std::uint8_t* base = nullptr;
    float baseWeight = 1.0f, ownWeight = 0.7f;
    int   baseBlur = 3;      // texels at 1024 across: two box passes of it, ~ a Gaussian of 4 (the preview's)
    // THE COVERAGE (`texpack_tool islands`): 0 where no face samples the
    // texel, 1 where one does, 2 on the border of a face's UV chart; cw x ch
    // at the ORIGINAL's size, null for all. An ATLAS's regions meet in
    // a step the maps read as a ridge, drawn along the edge of every face
    // mapped up to it (BATITR09, Qalisar's floor); the slope fades to flat
    // over `fade` texels (at 1024 across) from any texel not 1. 8 left
    // Qalisar's line: at a grazing angle the sampler reads a mip whose texel
    // spans 4-16 of these, and DeepBump's ridge is wide; 16 (four of the
    // original's texels) took it, as 32 did.
    const std::uint8_t* cover = nullptr;
    int   cw = 0, ch = 0;
    int   fade = 16;
};

// `rgba` w x h, rows top to bottom -> `out` w x h RGBA8 (normal, height).
void normalMap(const std::uint8_t* rgba, int w, int h, const NormalMapParams& p, std::vector<std::uint8_t>& out);

}  // namespace omk
