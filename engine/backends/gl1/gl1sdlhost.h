// SPDX-License-Identifier: GPL-3.0-or-later
// THE SDL BUILD'S GL WINDOW for the fixed-function backend on Mac OS X
// (`OMK_GL1_SDL_WINDOW`, `ppc-darwin.mk`) - the AGL host's twin
// (`backends/gl1/gl1host.h`): the window carries an OpenGL context instead of
// an `SDL_Renderer`, the GL1 backend draws the world into its back buffer and
// swaps it, and a frame composed on the CPU (a menu, a film, a screen that
// reads the picture) is presented through the same context as a texture.
#pragma once

#include "ui/surface.h"

namespace omk {
class SdlFrontend;
// Opens the GL window (`w x h`, the frontend's flags) and hands it to the
// frontend (`attachWindow`). -> false, and nothing open, where SDL gives no GL
// window (the dummy video driver, a refused context): the caller then keeps
// the frontend's own window and the backend its offscreen target.
bool sdlGl1OpenWindow(SdlFrontend& front, int w, int h);
// Whether the GL window is up.
bool sdlGl1Window();
// The composed frame `fb` to the window, fitted at its aspect, and swapped.
void sdlGl1PresentFrame(const Surface& fb);
}  // namespace omk
