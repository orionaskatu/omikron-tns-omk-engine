// SPDX-License-Identifier: GPL-3.0-or-later
// BC7 / BC4 decoded on the CPU - the texture pack's KTX2 (`formats/ktx2.h`)
// read back to pixels where a GPU cannot sample its blocks: the GLES backend
// on a desktop (the Mac's GL stops before BPTC), a grey alias (its pixels are
// recomputed), a normal map whose own KTX2 is missing. With it a pack needs
// no PNG beside each KTX2 (`o3de/texpack.h`). The vendored decoders
// (`third_party/bc7enc`: bc7decomp, rgbcx) are compiled HERE, once - the
// encoder tool (`tools/tex_encode.cpp`) links them from this file.
// Not a port of anything in the original; the remaster's (OMK_REMASTER).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omk {

// One level of BC7 blocks -> RGBA8 `w x h` (a partial edge block clipped).
// False when `blocks` is shorter than the level needs.
bool decodeBc7(std::span<const std::uint8_t> blocks, int w, int h, std::vector<std::uint8_t>& rgba);

// One level of BC4 blocks -> one byte a texel, `w x h`.
bool decodeBc4(std::span<const std::uint8_t> blocks, int w, int h, std::vector<std::uint8_t>& r);

}  // namespace omk
