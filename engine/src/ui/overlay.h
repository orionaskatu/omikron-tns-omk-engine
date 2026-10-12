// SPDX-License-Identifier: GPL-3.0-or-later
// THE OVERLAY'S SIDE PLANES (`todo/vita-port.md` G6 steps 2 and 3). On the GLES
// window a frame with an interface on it is not composed over a readback of
// the world: the world's rows of the frame hold a KEY, and the GPU blends the
// finished frame over its own picture as `C + world * M`. Opaque drawing simply
// overwrites the key (C = the pixel, M = 0). The passes that READ the picture -
// the two dialogue boxes, the two screen fades, the HUD's blended quads - are
// all affine in it (`new = A * old + B`), so on a key pixel they run on these
// planes instead: the law itself on C, which starts at 0, and A on M, which
// starts at 255. Exact up to the GPU's 8-bit rounding, not a case per pass.
// SPARSE: a row of the planes is initialised the first time a pass touches it
// (`row`), so a frame costs the rows its boxes and bands cover, not the screen.
//
// This is the PORT's own device, not the engine's: the original blends on the
// card, which is what the planes hand back to the GPU.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <vector>

namespace omk {

constexpr std::uint16_t kOverlayKey = 0xF81F;

struct OverlayPlanes {
    bool on = false;
    int w = 0;
    std::vector<std::uint16_t> c;
    std::vector<std::uint8_t>  m;
    std::vector<std::uint8_t>  rowInit;
    // ...and WHICH 32-PIXEL BLOCKS of each row a pass touched (bit b = x in
    // 32b..32b+31; past 2048 pixels the last bit): a reader may leave the
    // rest of a touched row alone - an untouched pixel is the key there, and
    // the planes' c = 0, m = 255 says the same thing (`playgpu_gl1.cpp`; the
    // iBook's fight HUD, two gauges at the screen's two edges, touches ~300
    // rows a few blocks wide each, which spares the glue all 640 of each)
    std::vector<std::uint64_t> rowBlocks;
    std::size_t curY = 0;
    // the row the last `row` call landed in, [lo, hi) as pixel indices: every
    // pass walks the frame row by row, so this spares the DIVIDE per pixel -
    // which the Vita's Cortex-A9 has no instruction for (a library call; the
    // black fade's bands alone cost 46-57 ms a frame on a console that way)
    std::size_t lo = 1, hi = 0;
    void begin(int width, int height) {
        on = true;
        w = width;
        const std::size_t count = static_cast<std::size_t>(width) * height;
        if (c.size() != count) { c.assign(count, 0); m.assign(count, 255); }
        rowInit.assign(static_cast<std::size_t>(height), 0);
        rowBlocks.assign(static_cast<std::size_t>(height), 0);
        lo = 1; hi = 0;
    }
    void row(std::size_t index) {
        if (index >= lo && index < hi) {          // initialised by the call that set them
            const std::size_t b = (index - lo) >> 5;
            rowBlocks[curY] |= std::uint64_t{1} << (b < 63 ? b : 63);
            return;
        }
        const std::size_t y = index / static_cast<std::size_t>(w);
        lo = y * static_cast<std::size_t>(w);
        hi = lo + static_cast<std::size_t>(w);
        curY = y;
        {
            const std::size_t b = (index - lo) >> 5;
            rowBlocks[y] |= std::uint64_t{1} << (b < 63 ? b : 63);
        }
        if (rowInit[y]) return;
        rowInit[y] = 1;
        std::fill(c.begin() + y * w, c.begin() + (y + 1) * w, std::uint16_t(0));
        std::fill(m.begin() + y * w, m.begin() + (y + 1) * w, std::uint8_t(255));
    }
};

// the one set of planes, off unless a frontend began an overlay frame.
// At NAMESPACE scope, not a function-local static: the passes call this per
// PIXEL, and a local static with a constructor is a thread-safe guard on every
// call - five `dmb ish` barriers in the subtitle box's pixel loop on the Vita
// (read in its object). Nothing touches it before `main`.
// ONE strong definition in the .cpp, not an `inline` variable: an inline
// variable with a constructor is a WEAK object, and Retro68's XCOFF linker
// gives a weak .bss object NO STORAGE - its constructor then writes over
// whatever follows (`todo/classic-mac-port-1999.md` 3d-iv).
extern OverlayPlanes g_overlayPlanes;
inline OverlayPlanes& overlayPlanes() { return g_overlayPlanes; }

// THE GLUE every GPU window runs before its overlay pass (GL1, GLES, the
// 3DS): the planes resolved into the frame - C into `px` where it holds the
// key, M into `mask` - ONLY in the 32-pixel blocks a pass touched (`rowBlocks`;
// an untouched pixel stays the key, which every backend sends as "the world":
// the same as the planes' c = 0, m = 255), `maskRow` 1 on a row whose mask may
// be nonzero, and a block's mask zeroed ONCE when it stops being touched
// (`maskBlocks` remembers which may be nonzero). Without the blocks it walks
// every pixel of a touched row, and on the 3DS zero-fills every untouched row
// every frame - the fight HUD's gauges touch ~300 rows a few blocks wide (the
// iBook, `todo/handoff-classic-mac.md` 3f step 3g).
inline void resolveOverlayPlanes(std::uint16_t* px, int w, int h, const OverlayPlanes& ov,
                                 std::vector<std::uint8_t>& mask, std::vector<std::uint8_t>& maskRow,
                                 std::vector<std::uint64_t>& maskBlocks) {
    const std::size_t W = static_cast<std::size_t>(w), H = static_cast<std::size_t>(h);
    if (mask.size() != W * H) {
        mask.assign(W * H, std::uint8_t(0));
        maskRow.assign(H, 0);
        maskBlocks.assign(H, 0);
    }
    const auto span = [&](std::size_t b, std::size_t& x0, std::size_t& x1) {
        x0 = b * 32;
        x1 = b == 63 ? W : (x0 + 32 < W ? x0 + 32 : W);
    };
    for (std::size_t y = 0; y < H; ++y) {
        const std::size_t o = y * W;
        const std::uint64_t now = ov.rowInit[y] ? ov.rowBlocks[y] : 0u;
        if (const std::uint64_t gone = maskBlocks[y] & ~now) {
            for (std::size_t b = 0; b < 64; ++b) {
                if (!(gone >> b & 1u)) continue;
                std::size_t x0, x1;
                span(b, x0, x1);
                if (x0 < x1) std::fill(mask.begin() + static_cast<long>(o + x0),
                                       mask.begin() + static_cast<long>(o + x1), std::uint8_t(0));
            }
        }
        maskBlocks[y] = now;
        maskRow[y] = now ? 1 : 0;
        if (!now) continue;
        for (std::size_t b = 0; b < 64; ++b) {
            if (!(now >> b & 1u)) continue;
            std::size_t x0, x1;
            span(b, x0, x1);
            for (std::size_t i = o + x0; i < o + x1; ++i) {
                if (px[i] == kOverlayKey) { px[i] = ov.c[i]; mask[i] = ov.m[i]; }
                else mask[i] = 0;
            }
        }
    }
}

// Two checksums of a run of bytes, a word at a time - what the overlays
// compare a block of a row by instead of a kept copy of it (the iBook is
// memory-bound: reading the copy cold cost more than the arithmetic). Of two
// kinds (a*31 + w, and a rotate-xor), so a change both miss is not a
// practical case. `h1`, `h2` carry on from the caller's seeds.
inline void overlayBlockSums(const void* p, std::size_t bytes, std::uint32_t& h1, std::uint32_t& h2) {
    const unsigned char* q = static_cast<const unsigned char*>(p);
    std::size_t i = 0;
    for (; i + 4 <= bytes; i += 4) {
        std::uint32_t v;
        std::memcpy(&v, q + i, 4);
        h1 = (h1 << 5) - h1 + v;
        h2 = ((h2 << 7) | (h2 >> 25)) ^ v;
    }
    for (; i < bytes; ++i) { h1 = (h1 << 5) - h1 + q[i]; h2 = ((h2 << 7) | (h2 >> 25)) ^ q[i]; }
}

}  // namespace omk
