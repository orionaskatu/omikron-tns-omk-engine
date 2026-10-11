// SPDX-License-Identifier: GPL-3.0-or-later
// THE ORIGINAL'S TEXTURE SIZE RULE (`sub_45F4F0`, 0x0045F4F0;
// `docs/ASSETS.md` 4b) - one transcription, for every backend whose card has
// little memory (the classic Mac's GL1 on an 8 MB iBook, the 3DS's 6 MB).
//
// The game's spec is a 4 MB card, and the original made its 58 texture slots
// fit one: it asks the card's FREE texture memory (`GetAvailableVidMem` on
// DDSCAPS_TEXTURE | DDSCAPS2_TEXTUREMANAGE, after its screen buffers are
// made) and, while `(slots + 5) << 17` bytes of 16-bit textures (`<< 16` for
// an 8-bit format) times the two scale factors exceed it, halves one factor:
// the WIDTH's (`flt_6A5388`) while it is not the smaller, else the height's
// (`flt_6A5384`). Each texture is then made at `size * factor` a side and the
// page stretch-blitted into it (`sub_463420`). So a 4 MB card drew 128x128,
// an 8 MB one 128x256. The square-only arm (`dword_53ADEC`, a D3D device
// cap) has no counterpart on the port's backends.
#pragma once

#include <cstdlib>

namespace omk {

struct TextureScale {
    float w = 1.0f, h = 1.0f;
    bool scaled() const { return w != 1.0f || h != 1.0f; }
};

// `freeBytes`: the card's free texture memory, or <= 0 when the host does not
// say - the textures then stay whole, as on a large card. `OMK_TEXTURE_MEMORY`
// (bytes) stands in for a card's answer, so the rule can run where the host
// gives none (`verify.py: engine: gl1 backend`).
inline TextureScale originalTextureScale(long freeBytes, int bytesPerTexel = 2) {
    if (const char* e = std::getenv("OMK_TEXTURE_MEMORY")) freeBytes = std::atol(e);
    TextureScale s;
    if (freeBytes <= 0) return s;
    constexpr long kSlots = 58;                                 // `SetMaterialsMemory(58, 0)`
    const float need = static_cast<float>((kSlots + 5) << (bytesPerTexel >= 2 ? 17 : 16));
    while (s.w * s.h * need > static_cast<float>(freeBytes)) {
        if (s.w >= s.h) s.w *= 0.5f; else s.h *= 0.5f;
    }
    return s;
}

// The rule's need in bytes, for a log line.
inline long originalTextureNeed(int bytesPerTexel = 2) {
    return 63L << (bytesPerTexel >= 2 ? 17 : 16);
}

}  // namespace omk
