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
#if defined(OMK_GL1_AGL)
// The world presented straight from the window's back buffer, and the same
// with the composed interface over it (`gl1render.cpp`, tier B of
// todo/cpu-vs-original.md). -> false where the window is not the frame's
// size, and the caller presents the composed frame as before.
bool gl1PresentWorld(Renderer* r, int ww, int wh);
// `variant` and `probe` are the WHITE-SCREEN DIAGNOSTIC of 2026-10-10
// (`todo/handoff-classic-mac.md` 3f, step 2j): 0 draws the overlay as it
// always has, 1 without blending (an alpha test - exact while the overlay's
// alpha is 0 or 255 only), 2 through a 16-bit ARGB1555 texture, 3 not at all;
// `probe` logs the band, the texture's residency and three back-buffer pixels.
bool gl1PresentOverlay(Renderer* r, const Surface& fb, const std::uint8_t* mask,
                       const float fade[4], int ww, int wh, int variant = 0, bool probe = false);
#endif
}  // namespace omk
