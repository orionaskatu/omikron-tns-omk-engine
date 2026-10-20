// SPDX-License-Identifier: GPL-3.0-or-later
// PNG, READ ONLY - for the remaster's texture pack (`todo/remaster.md` B4).
//
// NOT A PORT. Nothing the game ships is a PNG and the original reads no
// image format but its own; this exists because a texture pack is made by
// tools that write PNG (an AI upscaler, an image editor), and a pack a
// person can open and fix one file of is worth more than a packed blob.
// PORTING A8: no dependency - the inflate is written here (RFC 1950/1951,
// table-driven Huffman), so `make` still needs nothing installed.
// `verify.py: engine: texture pack` differences it against Python's zlib.
//
// What is read: bit depth 8, colour types 0 (grey), 2 (RGB), 3 (palette,
// with tRNS), 4 (grey + alpha) and 6 (RGBA), not interlaced. Anything else
// is refused and SAID through `why`, never guessed at. CRCs are not checked -
// the zlib stream's Adler-32 is, and a pack is local files, not a download.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omk {

struct PngImage {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;   // w * h * 4, rows top to bottom
};

// A zlib stream -> its bytes. False on any malformed input, a bad Adler-32
// included. `expect`, when not 0, is the exact size wanted: it reserves, and
// a stream that would produce more is refused (a PNG's image data is exactly
// it) - with the input's end checked on every symbol, a stream that runs off
// its data is refused rather than decoded for ever.
bool zlibInflate(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& out,
                 std::size_t expect = 0);

// A PNG -> RGBA8. False (and `why` set) on anything outside the subset above.
bool decodePng(const std::uint8_t* src, std::size_t n, PngImage& out, std::string* why = nullptr);

// The size alone, from IHDR, without decoding anything.
bool pngSize(const std::uint8_t* src, std::size_t n, int& w, int& h);

}  // namespace omk
