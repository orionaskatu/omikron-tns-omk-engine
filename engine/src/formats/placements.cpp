// SPDX-License-Identifier: GPL-3.0-or-later
// See `placements.h` for where every field comes from.
#include "formats/bytes.h"
#include "formats/placements.h"

#include "o3de/worldcam.h"       // rawToWorld

namespace omk {
namespace {

using omk::le::i16;
using omk::le::i32;
using omk::le::u32;

}  // namespace

std::vector<Placement> readPlacements(std::span<const std::byte> b, ChunkKind kind) {
    std::vector<Placement> out;
    // AREA +40 / +72, SCENE +8 / +40 - the same table, thirty-two apart, the
    // offset every AREA/SCENE field pair keeps.
    const std::size_t po = kind == ChunkKind::Area ? 40u : 8u;
    const std::size_t co = kind == ChunkKind::Area ? 72u : 40u;
    if (b.size() < co + 2u) return out;
    const std::size_t p = u32(b, po);
    const int n = i16(b, co);
    if (n <= 0 || p == 0 || p + 20 * std::uint64_t(n) > b.size()) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const std::size_t o = p + 20u * static_cast<std::size_t>(i);
        Placement r;
        r.slot  = i16(b, o);
        r.actor = i16(b, o + 2);
        for (int k = 0; k < 3; ++k)
            r.pos[k] = rawToWorld(i32(b, o + 4u + 4u * static_cast<std::size_t>(k)));
        // `u16(v28, 8) = (int64_t)((double)v34 * 0.087890625)` - `Area_Load`
        // converts the angle IN PLACE and TRUNCATES, exactly as it truncates
        // the three coordinates, and `Actors_SpawnFromTables` then reads the
        // int16 back (`v23 = (float)i16(i, 16)`). The Demon's 4073 is 357.98
        // and the engine's facing is 357, not 358.
        r.facing = static_cast<float>(
            static_cast<std::int32_t>(static_cast<double>(i16(b, o + 16)) * 0.087890625));
        r.bit   = i16(b, o + 18);
        out.push_back(r);
    }
    return out;
}

}  // namespace omk
