// SPDX-License-Identifier: GPL-3.0-or-later
// THE INTERFACE PACK - the remaster's Tier E (`todo/remaster.md`): the
// 640x480 interface bitmaps (`I2D/bitmaps`, `IMAGES`) replaced by upscaled
// ones when the window is larger. NOT IN THE ORIGINAL: it drew them at their
// own size and stretched nothing.
//
// The texture pack's way (`o3de/texpack.h`), for the 2D: a directory of
// `<key>.png`, the key a hash of the bitmap as LOADED (`uiKey`: FNV-1a 64
// over its size and its 565 pixels), each the original's size times a power
// of two, RGBA - alpha 0 where the original's colour key (pure black) was.
// `tools/omktexpack.py ui` makes it; `texpack_tool uikeys` gives it the keys,
// so they are the engine's own.
//
// Where it acts: `surfaceFromBmp` hands every bitmap it loads to `attach`,
// which notes it (its pixel buffer, which outlives the move out) when the pack
// has its key; `blt` - the ONE place a bitmap's pixels are sampled at the
// output's size - asks `find` for the HD version when it ENLARGES and draws
// from that, the rectangle scaled. Off unless a pack is open: then `attach`
// and `find` return at once, and nothing drawn changes.
//
// The HD bitmap is 565 like every surface, keyed as the original is: a texel
// whose alpha is under half is 0 (the key), any other that would come out 0
// is nudged to 1 - so the key draws exactly what it drew, with a smooth edge.
// At most `kCached` are held decoded; one evicted is read again when drawn.
#pragma once

#include "ui/surface.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace omk {

std::uint64_t uiKey(const Surface& s);

class UiPack {
public:
    static constexpr std::size_t kCached = 16;
    bool open(const std::string& dir);   // false (and inactive) when absent or empty
    bool active() const { return active_; }
    std::size_t files() const;           // `<key>.png` in the pack
    // a bitmap just loaded: noted when the pack has its key
    void attach(const Surface& s);
    // the HD version of `src` when it is a noted bitmap, unchanged since; null otherwise
    std::shared_ptr<const Surface> find(const Surface& src);
    // the ORIGINAL/REMASTER switch (`todo/graphics-menu.md` B): hidden, `find`
    // answers nothing and the shipped bitmaps draw - the pack stays open
    void setShown(bool on) { shown_ = on; }
    struct Stats { std::size_t attached = 0, decoded = 0, refused = 0, drawn = 0; };
    Stats stats() const;
private:
    bool active_ = false;
    bool shown_ = true;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// the session's pack
UiPack& uiPack();

}  // namespace omk
