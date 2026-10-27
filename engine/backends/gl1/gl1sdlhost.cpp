// SPDX-License-Identifier: GPL-3.0-or-later
// THE SDL BUILD'S GL WINDOW - see gl1sdlhost.h.
//
// WHY. The SDL build drew the world into a framebuffer object on a context of
// its own, read every frame back with `glReadPixels`, dithered it to RGB565
// on the CPU and presented it through an `SDL_Renderer`. On an iBook G4
// (1.42 GHz, Mobility Radeon 9550, the reader's profile of 2026-10-27) that
// readback was 135 of a 183 ms street frame - the frame waits there for the
// card to finish, and the bytes come back through the driver. The classic
// build had already left that path for its AGL window (`todo/
// cpu-vs-original.md` tier B: ~5 -> ~23 fps on the emulated Tiger); this is
// the same move for the SDL build: the backend's AGL paths, with the context
// SDL makes for the window.
//
// SDL 2 only: this file is built by `ppc-darwin.mk` (Tiger's SDL 2.0.3, and
// a current Mac's SDL 2 for `verify.py: engine: gl1 backend`).
#include "gl1sdlhost.h"

#include "../sdl/sdlfront.h"
#include "gl1host.h"
#include "platform/env.h"
#include "platform/profile.h"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

SDL_Window* g_win = nullptr;
CGLContextObj g_ctx = nullptr;

// THE COMPOSED FRAME'S TEXTURE: the frame's size rounded up to powers of two
// (OpenGL 1.x), 32-bit ARGB - BGRA as 8-8-8-8-REV, the format Mac OS X's
// drivers take without converting (the classic build's upload probe picked
// it on the emulated Radeon, `carbonfront.cpp` probeUploads) - through
// Apple's CLIENT STORAGE where the driver offers it: the texture's memory is
// ours and the card reads it there, as SDL 2.0.3's own renderer sends its
// window texture. A plain `glTexSubImage2D` copy of the same words drew black
// as BLUE on the emulated Radeon (`todo/handoff-classic-mac.md` 3g; the
// emulator's byte order, not the card's - the same words through client
// storage draw right there, and SDL's did on the reader's iBook G4). Only the
// band of rows that changed since the last composed frame is sent: a menu's
// frame and text stand still while its cloud moves.
GLuint g_tex = 0;
int g_texW = 0, g_texH = 0, g_fbW = 0, g_fbH = 0;
bool g_client = false;                  // GL_APPLE_client_storage: `g_store` is the texture's memory
std::vector<std::uint16_t> g_last;      // the last frame sent, to find the changed rows
std::vector<std::uint32_t> g_store;     // the texture's texels, `g_texW` a row

unsigned pow2(int v) {
    unsigned p = 1;
    while (p < static_cast<unsigned>(v)) p <<= 1;
    return p;
}

// The caller's context out, ours in - and back on scope exit (the backend's
// `Current`; the same context in practice, SDL's being the only one).
struct Ours {
    CGLContextObj prev = CGLGetCurrentContext();
    Ours() { if (prev != g_ctx) CGLSetCurrentContext(g_ctx); }
    ~Ours() { if (CGLGetCurrentContext() != prev) CGLSetCurrentContext(prev); }
};

}  // namespace

namespace omk {

bool sdlGl1OpenWindow(SdlFrontend& front, int w, int h) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) return false;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
    // hidden for a measurement run, as the GLES window is (CLAUDE.md 5)
    const Uint32 flags = SDL_WINDOW_OPENGL |
        (omk::envFlag("OMK_HIDDEN_WINDOW") ? static_cast<Uint32>(SDL_WINDOW_HIDDEN) : front.windowFlags());
    g_win = SDL_CreateWindow("OMK Engine (OpenGL 1.x)", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, w, h, flags);
    if (!g_win) return false;
    SDL_GLContext c = SDL_GL_CreateContext(g_win);   // and made current
    g_ctx = c ? CGLGetCurrentContext() : nullptr;
    if (!g_ctx) {
        std::printf("gl1: no GL context on an SDL window (%s) - the world offscreen\n", SDL_GetError());
        if (c) SDL_GL_DeleteContext(c);
        SDL_DestroyWindow(g_win);
        g_win = nullptr;
        return false;
    }
    // one black frame, so the window holds nothing stale before the first
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(g_win);
    front.attachWindow(g_win);   // F11, the resolution row and the title act on it
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    std::printf("display: an OpenGL context on the SDL window (%dx%d drawable); the world is "
                "swapped from its back buffer, a composed frame drawn as a texture\n", dw, dh);
    return true;
}

bool sdlGl1Window() { return g_ctx != nullptr; }

void sdlGl1PresentFrame(const Surface& fb) {
    if (!g_ctx || fb.w <= 0 || fb.h <= 0) return;
    OMK_ZONE("present: gl frame");
    Ours ours;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(g_win, &ww, &wh);
    const std::size_t n = static_cast<std::size_t>(fb.w) * static_cast<std::size_t>(fb.h);
    bool all = false;
    if (!g_tex || fb.w != g_fbW || fb.h != g_fbH) {
        const int tw = static_cast<int>(pow2(fb.w)), th = static_cast<int>(pow2(fb.h));
        if (g_tex) glDeleteTextures(1, &g_tex);   // a new one: client storage binds its memory
        glGenTextures(1, &g_tex);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        g_client = ext && std::strstr(ext, "GL_APPLE_client_storage") && !omk::envFlag("OMK_GL1_NO_CLIENT");
        g_store.assign(static_cast<std::size_t>(tw) * static_cast<std::size_t>(th), 0xFF000000u);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (g_client) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_STORAGE_HINT_APPLE, GL_STORAGE_CACHED_APPLE);
            glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_TRUE);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_store.data());
        if (g_client) glPixelStorei(GL_UNPACK_CLIENT_STORAGE_APPLE, GL_FALSE);
        std::printf("present: the composed frame as a %dx%d ARGB8888 texture%s\n", tw, th,
                    g_client ? ", client storage" : "");
        g_texW = tw; g_texH = th; g_fbW = fb.w; g_fbH = fb.h;
        g_last.assign(n, 0);
        all = true;
    }
    // the band of rows that differ from the last frame sent
    int lo = fb.h, hi = -1;
    const std::size_t rowBytes = static_cast<std::size_t>(fb.w) * sizeof(std::uint16_t);
    for (int y = 0; y < fb.h; ++y) {
        const std::uint16_t* src = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
        std::uint16_t* seen = g_last.data() + static_cast<std::size_t>(y) * fb.w;
        if (!all && std::memcmp(src, seen, rowBytes) == 0) continue;
        std::memcpy(seen, src, rowBytes);
        lo = std::min(lo, y);
        hi = y;
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (hi >= lo) {
        // the card may still be reading last frame's texels from our memory
        if (g_client) glFinish();
        // 565 widened by replicating the high bits down, as the rest of the
        // port widens a 565 colour; a host word, so endian-free
        for (int y = lo; y <= hi; ++y) {
            const std::uint16_t* s = fb.px.data() + static_cast<std::size_t>(y) * fb.w;
            std::uint32_t* d = g_store.data() + static_cast<std::size_t>(y) * g_texW;
            for (int x = 0; x < fb.w; ++x) {
                const unsigned p = s[x];
                const unsigned r5 = (p >> 11) & 31, g6 = (p >> 5) & 63, b5 = p & 31;
                d[x] = 0xFF000000u | (r5 << 3 | r5 >> 2) << 16 | (g6 << 2 | g6 >> 4) << 8 | (b5 << 3 | b5 >> 2);
            }
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, g_texW);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, lo, fb.w, hi - lo + 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                        g_store.data() + static_cast<std::size_t>(lo) * g_texW);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    // the 2D state for one quad; the backend sets all of its own in begin(),
    // its texture cache included
    glDrawBuffer(GL_BACK);
    glViewport(0, 0, ww, wh);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, ww, 0, wh, -1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_FOG);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_SUM);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glColor4f(1, 1, 1, 1);
    // fitted at its own aspect, centred - the software window's logical size
    const float sc = std::min(static_cast<float>(ww) / fb.w, static_cast<float>(wh) / fb.h);
    const float dw = fb.w * sc, dh = fb.h * sc;
    const float x0 = (ww - dw) * 0.5f, y0 = (wh - dh) * 0.5f;
    const float s1 = static_cast<float>(fb.w) / g_texW, t1 = static_cast<float>(fb.h) / g_texH;
    glBegin(GL_QUADS);                      // texture row 0 is the frame's TOP
    glTexCoord2f(0, 0);   glVertex2f(x0, y0 + dh);
    glTexCoord2f(s1, 0);  glVertex2f(x0 + dw, y0 + dh);
    glTexCoord2f(s1, t1); glVertex2f(x0 + dw, y0);
    glTexCoord2f(0, t1);  glVertex2f(x0, y0);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    OMK_ZONE("present: swap");
    SDL_GL_SwapWindow(g_win);
}

// ---- the window host (`backends/gl1/gl1host.h`) ----
void* gl1WindowContext() { return g_ctx; }
void gl1WindowSize(int& w, int& h) {
    w = h = 0;
    if (g_win) SDL_GL_GetDrawableSize(g_win, &w, &h);
}
void gl1WindowSwap() {
    if (g_win) SDL_GL_SwapWindow(g_win);
}
// the overlay's upload: 32-bit ARGB, which every Mac OS X driver takes as it
// is, through client storage where the driver offers it - as the composed
// frame above, and for the same reason (the plain copy's colours on the
// emulated Radeon)
bool gl1OverlayArgb() { return true; }
Gl1UploadPath gl1OverlayPath() {
    static const Gl1UploadPath path = [] {
        Gl1UploadPath p;
        Ours ours;
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        p.client = ext && std::strstr(ext, "GL_APPLE_client_storage") && !omk::envFlag("OMK_GL1_NO_CLIENT");
        return p;
    }();
    return path;
}
// not asked: the textures stay whole, as they were offscreen
long gl1TextureMemory() { return -1; }

}  // namespace omk
