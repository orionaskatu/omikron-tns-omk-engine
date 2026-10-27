// SPDX-License-Identifier: GPL-3.0-or-later
// THE FONT PACK - the remaster's Tier E (`todo/remaster.md`), beside the
// interface pack (`ui/uipack.h`): the `.FNT` faces' glyphs drawn from a
// coverage `k` times finer when the text is ENLARGED (`textscaling fit`).
// NOT IN THE ORIGINAL: it drew every glyph at its own size, and this port's
// `fit` enlarges the 0..31 coverage itself (`TextLayout::drawGlyphScaled`),
// which at 4K is a 4.5x blow-up of a 17-pixel face.
//
// One file per face, `<key>.png`: the key `fontKey`, FNV-1a 64 over the
// `.FNT` blob as read, so a pack made from another language's fonts is never
// taken for this one's. The image is GREY in its red channel (0 empty, 255
// covered), every present glyph at `k` times its size, laid out by
// `hdLayout` - which the engine recomputes from the face alone, so the file
// carries no table of its own; `k` is the one power of two whose layout has
// the image's size. `tools/font_hd.cpp` makes them (`omktexpack.py fonts`).
//
// Off unless a pack is open: `find` returns null at once and nothing drawn
// changes. Each face is decoded on its first enlarged glyph and held.
#pragma once

#include "formats/fnt.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace omk {

std::uint64_t fontKey(const Font& f);

// Where each glyph of a face sits in its HD image: present glyphs in code
// order, left to right on shelves `kFontAtlasW` wide, `kFontGap` apart.
inline constexpr int kFontAtlasW = 2048, kFontGap = 2;
struct HdLayout {
    int k = 0, w = 0, h = 0;
    std::array<int, kFntGlyphs> x{}, y{};   // the glyph's top-left, HD pixels
};
HdLayout hdLayout(const Font& f, int k);

struct HdFont {
    HdLayout lay;
    std::vector<std::uint8_t> cov;   // lay.w * lay.h, 0..255
    std::uint8_t at(unsigned char code, int x, int y) const {
        return cov[static_cast<std::size_t>(lay.y[code] + y) * static_cast<std::size_t>(lay.w) +
                   static_cast<std::size_t>(lay.x[code] + x)];
    }
};

class FontPack {
public:
    bool open(const std::string& dir);   // false (and inactive) when absent or empty
    bool active() const { return active_; }
    std::size_t files() const;
    // the HD coverage of `f`, or null: no pack, no file for it, or a file
    // whose size no `k` lays out (counted `refused`)
    std::shared_ptr<const HdFont> find(const Font& f);
    // the ORIGINAL/REMASTER switch (`todo/graphics-menu.md` B): hidden, `find`
    // answers nothing and the shipped faces draw - the pack stays open
    void setShown(bool on) { shown_ = on; }
    struct Stats { std::size_t decoded = 0, refused = 0; };
    Stats stats() const;
private:
    bool active_ = false;
    bool shown_ = true;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

FontPack& fontPack();

}  // namespace omk
