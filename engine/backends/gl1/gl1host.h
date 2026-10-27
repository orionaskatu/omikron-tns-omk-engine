// SPDX-License-Identifier: GPL-3.0-or-later
// THE WINDOW HOST - the calls between the fixed-function backend and the
// frontend whose window it draws into: the CLASSIC Mac frontend
// (`backends/classic/carbonfront.cpp`, `OMK_GL1_AGL`) and the SDL one on Mac
// OS X (`backends/gl1/gl1sdlhost.cpp`, `OMK_GL1_SDL_WINDOW`).
//
// On classic Mac OS the OpenGL context belongs to the WINDOW: AGL attaches it
// to the frontend's window port, and the frame is presented through it
// (`glDrawPixels` and `aglSwapBuffers`) rather than by CopyBits, which an AGL
// surface over the window would hide on Mac OS X. So the frontend makes and
// owns the context, on first ask - a run that never asks (`OMK_GL1=0`, or no
// OpenGL installed) keeps the window plain and presents by CopyBits. The SDL
// host's context is SDL's window's (`SDL_GL_CreateContext`), its CGL object.
#pragma once

namespace omk {
// -> the frontend's context (AGL: an AGLContext, made on the first call and
// current; SDL: the window's CGLContextObj), or nullptr when there is no
// window yet or no OpenGL renderer to be had.
void* gl1WindowContext();
// The window's size in pixels (AGL: its port bounds; SDL: the drawable) -
// what the straight present needs to be 1:1 with the frame; 0 x 0 without
// a window.
void gl1WindowSize(int& w, int& h);
// The swap, on the SDL host (`SDL_GL_SwapWindow`); AGL swaps its own context.
void gl1WindowSwap();
// The overlay texture's upload format, measured when the context was made
// (`CarbonFrontend::probeUploads`): true for BGRA as 8-8-8-8-REV - the
// 32-bit ARGB that Mac OS X's drivers take without converting - false for
// RGBA bytes.
bool gl1OverlayArgb();
// ...and HOW it is sent, the probe's pick as well: through Apple's
// CLIENT STORAGE (the card reads the texture from our memory, no copy into
// the driver), as a RECTANGLE texture (the frame's own size), with the SHARED
// storage hint - each only where the driver names the extension and the
// probe timed it faster. All false: a plain copy into the driver.
struct Gl1UploadPath { bool client = false, rect = false, hint = false; };
Gl1UploadPath gl1OverlayPath();
// The card's FREE TEXTURE MEMORY in bytes - what the original's
// `GetAvailableVidMem` answered (`docs/ASSETS.md` 4b): the renderer's texture
// memory less this window's back and depth buffers. -1 when the driver does
// not say; the backend then sizes the textures as on a large card.
long gl1TextureMemory();
}  // namespace omk
