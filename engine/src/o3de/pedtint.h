// SPDX-License-Identifier: GPL-3.0-or-later
// THE CROWD'S COLOUR VARIATION (`pedvariety = on|off`, `todo/enhancements.md`
// 29), an ENHANCEMENT: "the pedestrians are always the same, there is very
// low diversity (like 4 models/textures)". The `.OPT` crowd wears twelve
// men's and nine women's PERSOS models, and every model has ONE palettised
// texture page set, so 200 walkers share a handful of coats. With this on each
// walker has his own look: the clothes' colours turned round the hue circle,
// less or more saturated, darker or lighter - decided once per walker from
// `Pedestrian::look`, a hash of the area's seed and his slot, so he keeps it
// for as long as he is there and the next load of the street dresses him
// differently.
//
// THE DESIGN, and what it was chosen over. The look is a WORD in
// `Draw::lit` (`kDrawTint` and the bits above it, `o3de/renderer.h`), which
// `scene.frag` applies to the texel it has just sampled - the texture is
// the shipped one, or the texture pack's replacement, whichever the backend
// bound. A recoloured COPY of each page (6-8 per model) was the alternative
// and was rejected: 33 pages across the 21 models, a pack's replacement at
// 4x being 1024x1024 - ~5.6 MB with its mips - so 6 looks would hold another
// gigabyte on the card, each copy a key the pack does not know (it would have
// to be decoded and tinted per copy), and the ray-traced bodies' records
// would name 7x the slots. A word in the draw costs no memory, no draw and no
// upload, works the same with a pack at any k, and puts nothing in the
// Direct3D 12 root signature (the push block's `lit` word already travels).
//
// WHAT IS KEPT (measured over every page of the 21 crowd models - the
// contact sheets of `todo/enhancements.md` 29): the SKIN, in a window of hue
// (-20..49 degrees, fading out by -40 and 68), of saturation (0.12..0.65,
// fading below 0.05 and above 0.85 - and up to 0.95..1.15 for a DARK colour,
// where a brown hair or the shade of a hand reads as saturated) and any value
// - which keeps the faces, hands, the brown and the blond hair and the tan,
// khaki, beige and rust cloth that is the same colour as skin; the near-WHITE (value above 0.65 and saturation
// under 0.2 - the eyes, the teeth, a white shirt); and, since a hue turn and a
// scale of a black are black, the near-black stays black (the hair, the boots,
// a black coat keeps its shine and leans a little to the new hue only).
// Everything else - the greys with some colour in them, the reds, the greens,
// the blues, the oranges - is turned.
//
// THE MATH is in this file and, written out again, in `scene.frag` (a shader
// cannot include a header): `verify.py: crowd variety` compares the constants
// of the two and runs the probe (`tools/pedtint_probe.cpp`) over every page.
#pragma once

#include "o3de/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace omk {

// ---- the constants `scene.frag` repeats (same names, same values)
inline constexpr float kPedHueCentre = 0.04f;    // the skin's hue, in turns (14 degrees)
inline constexpr float kPedHueFull = 0.095f;     // within this of it (-20..49 degrees): all skin
inline constexpr float kPedHueNone = 0.15f;      // beyond this (-40..68): none
inline constexpr float kPedSatLow0 = 0.05f;      // saturation under this: none
inline constexpr float kPedSatLow1 = 0.12f;      // ...over this: all
inline constexpr float kPedSatHigh0 = 0.65f;     // ...and above this it fades
inline constexpr float kPedSatHigh1 = 0.85f;     // ...to none
inline constexpr float kPedDarkV0 = 0.30f;       // ...but a DARK colour (a brown hair, the shade of a hand:
inline constexpr float kPedDarkV1 = 0.55f;       // saturation reads high at a low value) - value under 0.3, none over 0.55 -
inline constexpr float kPedDarkBy = 0.30f;       // ...has both of those limits pushed up this far
inline constexpr float kPedWhiteV0 = 0.65f;      // near-white: value from here
inline constexpr float kPedWhiteV1 = 0.85f;      // ...fully from here
inline constexpr float kPedWhiteS0 = 0.08f;      // ...with a saturation under this: all
inline constexpr float kPedWhiteS1 = 0.2f;       // ...none over this
inline constexpr float kPedSatBase = 0.55f;      // the saturation scale: base + code * step
inline constexpr float kPedSatStep = 0.035f;     // (code 0..15: 0.55 .. 1.075 - muted, the city is dark)
inline constexpr float kPedValBase = 0.80f;      // the brightness scale likewise
inline constexpr float kPedValStep = 0.035f;     // (0.80 .. 1.325)
inline constexpr float kPedChromaCap0 = 0.35f;   // a colour more saturated than this is pulled
inline constexpr float kPedChromaCap1 = 0.90f;   // ...down, by up to
inline constexpr float kPedChromaCapBy = 0.45f;  // ...this share (an orange coat is not a neon green one)

// ---- the word
inline constexpr int kPedTintHueShift = 6;       // bits 6..13: the hue turn, 0..255 = 0..360 degrees
inline constexpr int kPedTintSatShift = 14;      // bits 14..17: the saturation code
inline constexpr int kPedTintValShift = 18;      // bits 18..21: the brightness code
inline constexpr int kPedHueMin = 30;            // the turn is never less than 42 degrees
inline constexpr int kPedHueSpan = 197;          // ...nor more than 318: no walker keeps his colours

// The word for a walker's `look` (a hash: every bit is as good as another).
inline int pedTintWord(std::uint32_t look) {
    const int hue = kPedHueMin + static_cast<int>((look & 0xFFFFu) % static_cast<std::uint32_t>(kPedHueSpan));
    const int sat = static_cast<int>((look >> 16) & 15u);
    const int val = static_cast<int>((look >> 20) & 15u);
    return kDrawTint | (hue << kPedTintHueShift) | (sat << kPedTintSatShift) | (val << kPedTintValShift);
}

namespace pedtint_detail {
inline float smoothstep(float a, float b, float x) {
    const float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f);
    return t * (3.0f - 2.0f * t);
}
}  // namespace pedtint_detail

// How much of `c` is protected, 0 (turned) .. 1 (kept): skin or near-white.
inline float pedProtect(const float c[3]) {
    using pedtint_detail::smoothstep;
    const float r = c[0], g = c[1], b = c[2];
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const float d = mx - mn;
    const float s = d / std::max(mx, 1e-4f);
    float h = 0.0f;
    if (d >= 1e-4f) {
        if (mx == r) { const float x = (g - b) / d; h = (x - 6.0f * std::floor(x / 6.0f)) / 6.0f; }
        else if (mx == g) h = ((b - r) / d + 2.0f) / 6.0f;
        else h = ((r - g) / d + 4.0f) / 6.0f;
    }
    const float x = h - kPedHueCentre + 0.5f;
    const float dh = std::fabs((x - std::floor(x)) - 0.5f);
    const float wh = 1.0f - smoothstep(kPedHueFull, kPedHueNone, dh);
    const float dark = 1.0f - smoothstep(kPedDarkV0, kPedDarkV1, mx);
    const float ws = smoothstep(kPedSatLow0, kPedSatLow1, s) *
                     (1.0f - smoothstep(kPedSatHigh0 + kPedDarkBy * dark, kPedSatHigh1 + kPedDarkBy * dark, s));
    const float white = smoothstep(kPedWhiteV0, kPedWhiteV1, mx) * (1.0f - smoothstep(kPedWhiteS0, kPedWhiteS1, s));
    return std::max(wh * ws, white);
}

// `scene.frag`'s `pedTint`: the texel `in` (0..1) under tint word `word`.
// `protect` false is the PROBE's failure injection only (what the check shows
// it catches): the shader has no such switch.
inline void pedTintRgb(const float in[3], int word, float out[3], bool protect = true) {
    using pedtint_detail::smoothstep;
    const float r = in[0], g = in[1], b = in[2];
    const float prot = protect ? pedProtect(in) : 0.0f;
    const float a = static_cast<float>((word >> kPedTintHueShift) & 255) * (6.28318530718f / 256.0f);
    const float cs = std::cos(a), sn = std::sin(a);
    const float k = (1.0f - cs) / 3.0f, q = 0.57735026919f * sn;
    float o[3] = {(cs + k) * r + (k - q) * g + (k + q) * b,
                  (k + q) * r + (cs + k) * g + (k - q) * b,
                  (k - q) * r + (k + q) * g + (cs + k) * b};
    // the turn keeps the mean of the three, not the luma: put the luma back
    const float y0 = 0.299f * r + 0.587f * g + 0.114f * b;
    const float y1 = 0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2];
    for (float& v : o) v = std::min(std::max(v + (y0 - y1), 0.0f), 1.0f);
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const float sIn = (mx - mn) / std::max(mx, 1e-4f);
    float sc = kPedSatBase + static_cast<float>((word >> kPedTintSatShift) & 15) * kPedSatStep;
    sc *= 1.0f - kPedChromaCapBy * smoothstep(kPedChromaCap0, kPedChromaCap1, sIn);
    const float vc = kPedValBase + static_cast<float>((word >> kPedTintValShift) & 15) * kPedValStep;
    const float y = 0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2];
    for (int i = 0; i < 3; ++i) {
        float v = y + (o[i] - y) * sc;
        v = std::min(std::max(v * vc, 0.0f), 1.0f);
        out[i] = v * (1.0f - prot) + in[i] * prot;
    }
}

}  // namespace omk
