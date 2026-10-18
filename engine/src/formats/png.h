// SPDX-License-Identifier: GPL-3.0-or-later
// A SMALL PNG READER - for the texture override packs (`o3de/texpack.h`,
// `todo/enhancements.md` 25), written here rather than vendored: zlib's
// INFLATE (stored, fixed and dynamic Huffman blocks, RFC 1951) and PNG's five
// scanline filters, for the one shape a pack needs - 8 bits a channel, RGB
// (colour type 2) or RGBA (6), not interlaced. Anything else is refused with
// the reason, never guessed at: a pack's file that does not decode falls back
// to the game's own texture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omk {

struct PngImage {
    int width = 0, height = 0;
    int channels = 0;                 // 3 (RGB) or 4 (RGBA)
    std::vector<std::uint8_t> px;     // width*height*channels, rows top down
};

// The IHDR alone - the first 33 bytes are enough. -> false (with `why`) when
// it is not a PNG this reader decodes.
bool pngHeader(std::span<const std::byte> d, int& w, int& h, std::string& why);

// The whole picture. -> false with `why` on anything it does not take.
bool decodePng(std::span<const std::byte> d, PngImage& out, std::string& why);

// RFC 1950/1951: a zlib stream's payload, appended to `out`. -> false on a
// malformed stream. Exposed for the probe.
bool zlibInflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out);

}  // namespace omk
