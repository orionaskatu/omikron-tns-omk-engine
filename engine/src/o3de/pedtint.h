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
// THE SECOND HALF (2026-10-29, "way more variety"): the same word also carries a hue
// SPREAD (the turn leans with the pixel's own hue, so a walker's jacket and trousers
// are turned apart), a COMPLEXION (the kept skin's value x0.78..1.11 - code 2 is the
// shipped skin) and a DYE (a grey cloth has no hue to turn: a chroma is added along a
// hue that follows its brightness). `pedScale` is his build, 0.93..1.07 of the height,
// and the motos wear the word of their mover's `look` (a bike and its rider).
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
inline constexpr float kPedHueFull = 0.075f;     // within this of it (-13..41 degrees): all skin
inline constexpr float kPedHueNone = 0.115f;     // beyond this (-27..55): none - the tan and khaki cloth above 41 is turned
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
inline constexpr float kPedSatBase = 0.75f;      // the saturation scale: base + code * step
inline constexpr float kPedSatStep = 0.045f;     // (code 0..15: 0.75 .. 1.425)
inline constexpr float kPedValBase = 0.75f;      // the brightness scale likewise
inline constexpr float kPedValStep = 0.04f;      // (0.75 .. 1.35)
inline constexpr float kPedChromaCap0 = 0.35f;   // a colour more saturated than this is pulled
inline constexpr float kPedChromaCap1 = 0.90f;   // ...down, by up to
inline constexpr float kPedChromaCapBy = 0.45f;  // ...this share (an orange coat is not a neon green one)

// ---- the second half of the variety (a garment of its own, the skin, the greys, the build)
inline constexpr float kPedSpreadMax = 0.27f;    // the hue turn leans up to this far (97 degrees) either way with the input hue
inline constexpr float kPedPhaseStep = 0.125f;   // ...from a phase on the hue circle, in eighths of a turn
inline constexpr float kPedSkinBase = 0.78f;     // the complexion: the skin's value x (base + code * step)
inline constexpr float kPedSkinStep = 0.11f;     // (code 0..3: 0.78 0.89 1.00 1.11 - code 2 is the shipped skin)
inline constexpr float kPedDyeMax = 0.45f;       // a GREY cloth is dyed up to this much chroma (scaled by the saturation code)
inline constexpr float kPedDyeFloor = 0.18f;     // ...and by (floor + lift * the grey's brightness): a dark coat is dyed too
inline constexpr float kPedDyeLift = 1.2f;
inline constexpr float kPedSkinNudge = 0.012f;   // the complexion also leans the skin's hue, this many turns a code step
inline constexpr float kPedDyeS0 = 0.08f;        // ...a colour under this saturation: all grey
inline constexpr float kPedDyeS1 = 0.35f;        // ...over this: not grey
inline constexpr float kPedDyeV0 = 0.06f;        // ...and under this value it is black and stays black
inline constexpr float kPedDyeV1 = 0.16f;        // ...(fully dyed from here)
inline constexpr float kPedDyeLuma = 3.0f;       // the dye's hue follows the grey's brightness: a pale and a dark grey differ
inline constexpr float kPedScaleMin = 0.93f;     // the walker's build, as a share of the model's height
inline constexpr float kPedScaleMax = 1.07f;

// ---- the word
inline constexpr int kPedTintHueShift = 6;       // bits 6..13: the hue turn, 0..255 = 0..360 degrees
inline constexpr int kPedTintSatShift = 14;      // bits 14..17: the saturation code
inline constexpr int kPedTintValShift = 18;      // bits 18..21: the brightness code
inline constexpr int kPedTintSpreadShift = 22;   // bits 22..25: the hue spread code (-97..+97 degrees)
inline constexpr int kPedTintPhaseShift = 26;    // bits 26..28: its phase
inline constexpr int kPedTintSkinShift = 29;     // bits 29..30: the complexion code
inline constexpr int kPedHueMin = 30;            // the base turn is never less than 42 degrees
inline constexpr int kPedHueSpan = 197;          // ...nor more than 318 (the spread then leans it per garment)

namespace pedtint_detail {
// A 32-bit finaliser: a hash of `x`, every bit as good as another.
inline std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}
}  // namespace pedtint_detail

// A walker's (or a moto's) `look`: a hash of the area's seed and the slot, its low 16 bits replaced by
// `slot` times the golden ratio (+ a seed offset) - well spread hues. No draw from the pool's `rng_`.
inline std::uint32_t pedLook(std::uint32_t seed, std::uint32_t slot) {
    const std::uint32_t h = pedtint_detail::mix(seed ^ (slot * 0x9E3779B1u));
    return (h & 0xFFFF0000u) | ((slot * 40503u + (pedtint_detail::mix(seed) & 0xFFFFu)) & 0xFFFFu);
}

// The word for a walker's `look`.
inline int pedTintWord(std::uint32_t look) {
    // the low 16 bits are a golden-ratio sequence over the slot (`pedLook`), so neighbours in the
    // pool are far apart on the hue circle - a plain hash left three greens in seven walkers
    const int hue = kPedHueMin + static_cast<int>((look & 0xFFFFu) * static_cast<std::uint32_t>(kPedHueSpan) >> 16);
    const int sat = static_cast<int>((look >> 16) & 15u);
    const int val = static_cast<int>((look >> 20) & 15u);
    const std::uint32_t h2 = pedtint_detail::mix(look ^ 0x68E31DA4u);
    const int spread = static_cast<int>(h2 & 15u);
    const int phase = static_cast<int>((h2 >> 4) & 7u);
    const int skin = static_cast<int>((h2 >> 7) & 3u);
    return kDrawTint | (hue << kPedTintHueShift) | (sat << kPedTintSatShift) | (val << kPedTintValShift) |
           (spread << kPedTintSpreadShift) | (phase << kPedTintPhaseShift) | (skin << kPedTintSkinShift);
}

// The walker's build: a scale about his feet, 0.93..1.07, from the same `look`.
inline float pedScale(std::uint32_t look) {
    const std::uint32_t h = pedtint_detail::mix(look ^ 0x2545F491u);
    return kPedScaleMin + (kPedScaleMax - kPedScaleMin) * (static_cast<float>(h & 0xFFFFu) / 65535.0f);
}

namespace pedtint_detail {
inline float smoothstep(float a, float b, float x) {
    const float t = std::min(std::max((x - a) / (b - a), 0.0f), 1.0f);
    return t * (3.0f - 2.0f * t);
}
}  // namespace pedtint_detail

// The hue of `c` in turns (0 when it has no chroma).
inline float pedHueOf(const float c[3]) {
    const float r = c[0], g = c[1], b = c[2];
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const float d = mx - mn;
    float h = 0.0f;
    if (d >= 1e-4f) {
        if (mx == r) { const float x = (g - b) / d; h = (x - 6.0f * std::floor(x / 6.0f)) / 6.0f; }
        else if (mx == g) h = ((b - r) / d + 2.0f) / 6.0f;
        else h = ((r - g) / d + 4.0f) / 6.0f;
    }
    return h;
}

// How much of `c` is kept as it is, split in the two things the rule keeps: SKIN (hue,
// saturation window) and near-WHITE. The shader's `skin`/`white` locals.
inline void pedKeep(const float c[3], float& skin, float& white) {
    using pedtint_detail::smoothstep;
    const float mx = std::max(c[0], std::max(c[1], c[2])), mn = std::min(c[0], std::min(c[1], c[2]));
    const float s = (mx - mn) / std::max(mx, 1e-4f);
    const float x = pedHueOf(c) - kPedHueCentre + 0.5f;
    const float dh = std::fabs((x - std::floor(x)) - 0.5f);
    const float wh = 1.0f - smoothstep(kPedHueFull, kPedHueNone, dh);
    const float dark = 1.0f - smoothstep(kPedDarkV0, kPedDarkV1, mx);
    const float ws = smoothstep(kPedSatLow0, kPedSatLow1, s) *
                     (1.0f - smoothstep(kPedSatHigh0 + kPedDarkBy * dark, kPedSatHigh1 + kPedDarkBy * dark, s));
    skin = wh * ws;
    white = smoothstep(kPedWhiteV0, kPedWhiteV1, mx) * (1.0f - smoothstep(kPedWhiteS0, kPedWhiteS1, s));
}

// How much of `c` is protected, 0 (turned) .. 1 (kept): skin or near-white.
inline float pedProtect(const float c[3]) {
    float skin, white;
    pedKeep(c, skin, white);
    return std::max(skin, white);
}

// The pure colour of hue `h` (turns, any real), value 1 saturation 1.
inline void pedHueRgb(float h, float out[3]) {
    const float x = (h - std::floor(h)) * 6.0f;
    const auto cl = [](float v) { return std::min(std::max(v, 0.0f), 1.0f); };
    out[0] = cl(std::fabs(x - 3.0f) - 1.0f);
    out[1] = cl(2.0f - std::fabs(x - 2.0f));
    out[2] = cl(2.0f - std::fabs(x - 4.0f));
}

// `scene.frag`'s `pedTint`: the texel `in` (0..1) under tint word `word`.
// `protect` false is the PROBE's failure injection only (what the check shows
// it catches): the shader has no such switch.
inline void pedTintRgb(const float in[3], int word, float out[3], bool protect = true) {
    using pedtint_detail::smoothstep;
    const float r = in[0], g = in[1], b = in[2];
    float skin = 0.0f, white = 0.0f;
    if (protect) pedKeep(in, skin, white);
    const float prot = std::max(skin, white);
    // THE COMPLEXION: what is kept as skin is not kept as light as it was - the
    // value scales (code 2 is the shipped skin, exactly)
    const float tone = kPedSkinBase + static_cast<float>((word >> kPedTintSkinShift) & 3) * kPedSkinStep;
    const float toneMix = skin * (1.0f - white);
    const float toneMul = 1.0f + (tone - 1.0f) * toneMix;
    const float nudge = static_cast<float>(((word >> kPedTintSkinShift) & 3) - 2) * kPedSkinNudge * 6.28318530718f * toneMix;
    float kept[3];
    {
        const float ncs = std::cos(nudge), nsn = std::sin(nudge);
        const float nk = (1.0f - ncs) / 3.0f, nq = 0.57735026919f * nsn;
        const float kr = (ncs + nk) * r + (nk - nq) * g + (nk + nq) * b;
        const float kg = (nk + nq) * r + (ncs + nk) * g + (nk - nq) * b;
        const float kb = (nk - nq) * r + (nk + nq) * g + (ncs + nk) * b;
        const float kv[3] = {kr, kg, kb};
        for (int i = 0; i < 3; ++i) kept[i] = std::min(std::max(kv[i] * toneMul, 0.0f), 1.0f);
    }
    // THE TURN: the base hue turn leans, by the pixel's own hue, so two garments of
    // one walker are not turned alike (the spread)
    const float base = static_cast<float>((word >> kPedTintHueShift) & 255) / 256.0f;
    const float spread = (static_cast<float>((word >> kPedTintSpreadShift) & 15) - 7.5f) / 7.5f * kPedSpreadMax;
    const float phase = static_cast<float>((word >> kPedTintPhaseShift) & 7) * kPedPhaseStep;
    const float a = (base + spread * std::sin(6.28318530718f * (pedHueOf(in) + phase))) * 6.28318530718f;
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
    const float satCode = static_cast<float>((word >> kPedTintSatShift) & 15);
    float sc = kPedSatBase + satCode * kPedSatStep;
    sc *= 1.0f - kPedChromaCapBy * smoothstep(kPedChromaCap0, kPedChromaCap1, sIn);
    const float vc = kPedValBase + static_cast<float>((word >> kPedTintValShift) & 15) * kPedValStep;
    const float y = 0.299f * o[0] + 0.587f * o[1] + 0.114f * o[2];
    // THE DYE: a grey cloth (low saturation, not black) has no hue to turn, so it is dyed -
    // a chroma added along a hue that follows the grey's own brightness
    const float wDye = (1.0f - smoothstep(kPedDyeS0, kPedDyeS1, sIn)) * smoothstep(kPedDyeV0, kPedDyeV1, mx);
    float dye[3];
    pedHueRgb(base + spread * std::sin(6.28318530718f * (kPedDyeLuma * y0 + phase)), dye);
    const float dyeY = 0.299f * dye[0] + 0.587f * dye[1] + 0.114f * dye[2];
    const float dyeBy = kPedDyeMax * (satCode / 15.0f) * wDye * (kPedDyeFloor + kPedDyeLift * y0);
    for (int i = 0; i < 3; ++i) {
        float v = y + (o[i] - y) * sc + (dye[i] - dyeY) * dyeBy;
        v = std::min(std::max(v * vc, 0.0f), 1.0f);
        out[i] = v * (1.0f - prot) + kept[i] * prot;
    }
}

}  // namespace omk
