// SPDX-License-Identifier: GPL-3.0-or-later
// THE PACKS' KEYS (the remaster's, `todo/remaster.md`): 64-bit FNV-1a over
// each pack's own bytes - a texture's indices and palette (`o3de/texpack.h`),
// a 2D bitmap's size and pixels (`ui/uipack.h`), a font's blob
// (`ui/fontpack.h`), a film's size and first megabyte (`platform/filmpack.h`)
// - and a key written as its file's name, sixteen hex digits. One copy of
// each, where the four modules had their own (the audit's cleanliness,
// 2026-10-26); the keys are the same bits.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace omk {

struct Fnv1a {
    std::uint64_t h = 0xcbf29ce484222325ull;
    void mix(std::uint8_t b) { h = (h ^ b) * 0x100000001b3ull; }
    // the low `n` bytes of `v`, the least significant first
    void mixLe(std::uint64_t v, int n) {
        for (int k = 0; k < n; ++k) mix(static_cast<std::uint8_t>(v >> (8 * k)));
    }
};

inline std::string keyHex(std::uint64_t k) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(k));
    return b;
}

}  // namespace omk
