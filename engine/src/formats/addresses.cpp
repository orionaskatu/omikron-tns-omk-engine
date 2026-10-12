// SPDX-License-Identifier: GPL-3.0-or-later
// See `addresses.h` for where every field comes from.
#include "formats/bytes.h"
#include "formats/addresses.h"

#include "o3de/worldcam.h"

namespace omk {
namespace {

using omk::le::i16;
using omk::le::i32;

}  // namespace

std::vector<Address> readAddresses(std::span<const std::byte> b) {
    std::vector<Address> out;
    if (b.size() < 84) return out;
    const auto at = i32(b, 60);
    const auto n  = i16(b, 82);
    if (n <= 0 || at <= 0) return out;
    const auto base = static_cast<std::size_t>(at);
    if (base + 16 * std::uint64_t(n) > b.size()) return out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const std::size_t o = base + 16u * static_cast<std::size_t>(i);
        Address a;
        for (int k = 0; k < 3; ++k)
            a.pos[k] = rawToWorld(i32(b, o + 4u * static_cast<std::size_t>(k)));
        // The same 4096-per-turn field a camera's roll uses, wrapped the same
        // way - a heading is an angle and angles wrap (CLAUDE.md 1).
        a.yaw = angle4096(i16(b, o + 12));
        a.id  = i16(b, o + 14);
        for (int k = 0; k < 3; ++k) {
            const std::int32_t raw = i32(b, o + 4u * static_cast<std::size_t>(k));
            a.memPos[k] = static_cast<std::int32_t>(static_cast<std::int64_t>(
                double(100 * raw) * 0.00390625 * 0.3937007874015748 - 1.0));
        }
        a.memYaw = static_cast<std::int32_t>(static_cast<std::int64_t>(
            double(i16(b, o + 12)) * 0.087890625));
        out.push_back(a);
    }
    return out;
}

}  // namespace omk
