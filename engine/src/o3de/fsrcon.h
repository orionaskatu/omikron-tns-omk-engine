// SPDX-License-Identifier: GPL-3.0-or-later
// THE UPSCALE's constants - the remaster's A6 (`todo/remaster.md`), AMD's FSR 1.
// NOT IN THE ORIGINAL: it drew at the display's size.
//
// The two passes are AMD's own shader code, vendored verbatim (MIT,
// `third_party/ffx_fsr1.h`); what they need from the CPU is a handful of
// float BIT PATTERNS, which `FsrEasuCon` / `FsrRcasCon` compute in that header
// under `A_CPU`. That path drags in the whole of `ffx_a.h`'s C side (hundreds
// of unused-function warnings), so the same arithmetic is written out here and
// `verify.py: engine: upscale` compiles the two side by side and compares the
// words - a transcription that drifts is red.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace omk {

inline std::uint32_t fsrBits(float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

// `FsrEasuCon`: the input VIEWPORT (the part of the image drawn), the input
// IMAGE's size, the OUTPUT's size, all in pixels -> con0..con3, 16 words
inline void fsrEasuCon(std::uint32_t con[16], float inVpW, float inVpH, float inW, float inH,
                       float outW, float outH) {
    con[0] = fsrBits(inVpW * (1.0f / outW));
    con[1] = fsrBits(inVpH * (1.0f / outH));
    con[2] = fsrBits(0.5f * inVpW * (1.0f / outW) - 0.5f);
    con[3] = fsrBits(0.5f * inVpH * (1.0f / outH) - 0.5f);
    con[4] = fsrBits(1.0f / inW);
    con[5] = fsrBits(1.0f / inH);
    con[6] = fsrBits(1.0f * (1.0f / inW));
    con[7] = fsrBits(-1.0f * (1.0f / inH));
    con[8] = fsrBits(-1.0f * (1.0f / inW));
    con[9] = fsrBits(2.0f * (1.0f / inH));
    con[10] = fsrBits(1.0f * (1.0f / inW));
    con[11] = fsrBits(2.0f * (1.0f / inH));
    con[12] = fsrBits(0.0f * (1.0f / inW));
    con[13] = fsrBits(4.0f * (1.0f / inH));
    con[14] = con[15] = 0;
}

// `FsrRcasCon`'s first word (the 32-bit path reads no other): the sharpness in
// STOPS - 0 the most, each 1 halving it - as a linear factor
inline std::uint32_t fsrRcasCon(float stops) { return fsrBits(std::exp2(-stops)); }

}  // namespace omk
