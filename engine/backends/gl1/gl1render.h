// SPDX-License-Identifier: GPL-3.0-or-later
// THE FIXED-FUNCTION OpenGL 1.x BACKEND - `todo/classic-mac-port-1999.md`
// step 4, the renderer a Mac port of 2000 would have shipped.
//
// The original drew through Direct3D's fixed-function pipeline: vertices it
// had transformed itself, render states set per bucket, one texture slot per
// batch (`bucketKey & 0x3F`), the device's own linear fog. This backend takes
// the same decisions across `PORTING` A2's boundary and turns them into the
// OpenGL 1.x calls that do the same - no shader anywhere - so it runs on the
// cards of the period (Rage 128, Radeon 9700 under Tiger) as well as on a
// current Mac's legacy OpenGL.
//
// TIER: none (`PORTING` B6, like the Vulkan backend). Its correctness is the
// software reference's, which it mirrors law for law, and a frame from it is
// compared against that reference rather than against the original.
#pragma once

#include "o3de/renderer.h"

namespace omk {
// -> nullptr is never returned; `init()` is what fails when there is no
// accelerated OpenGL to be had.
Renderer* makeGl1Renderer();
// `--full-textures` (an enhancement): every texture at its own size, not the
// size the original's rule gives the card's free memory (`docs/ASSETS.md` 4b).
// Before the first texture pool.
void gl1SetFullTextures(Renderer* r, bool full);
#if defined(OMK_GL1_AGL)
// The world presented straight from the window's back buffer, and the same
// with the composed interface over it (`gl1render.cpp`, tier B of
// todo/cpu-vs-original.md). -> false where the window is not the frame's
// size, and the caller presents the composed frame itself.
bool gl1PresentWorld(Renderer* r, int ww, int wh);
// `probe` logs what the overlay frame held and three back-buffer pixels
// before the swap (the white-screen probe, `todo/handoff-classic-mac.md` 3f
// steps 2j/2k - kept until a run on the iBook confirms the fix).
// `maskRow`: 1 where a row of `mask` may be nonzero (a pass touched it), 0
// where it is all zero and need not be read; null reads every row.
bool gl1PresentOverlay(Renderer* r, const Surface& fb, const std::uint8_t* mask,
                       const std::uint8_t* maskRow, const float fade[4], int ww, int wh,
                       bool probe = false);
#endif
}  // namespace omk
