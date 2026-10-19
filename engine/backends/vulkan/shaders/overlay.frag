// SPDX-License-Identifier: GPL-3.0-or-later
// THE INTERFACE AS AN OVERLAY, for the Direct3D 12 backend at colour depth 32
// (`colordepth = 32`, `todo/enhancements.md` 21) - the GLES window's
// `kOverlayFrag` (`backends/gles/glesrender.cpp`, `todo/vita-port.md` G6
// step 2) on this backend's present image.
//
// A frame with an interface over the 3D (a HUD, a subtitle, a fade, a screen)
// is not composed over a 565 readback of the world: the CPU composes the 2D
// over a KEY (`ui/overlay.h`), `omk::resolveOverlayPlanes` resolves it into C
// (the 565 frame) and M ("how much of the world shows through", 8 bits), and
// this draws `C + world * M` over the full-precision world the present pass
// already wrote - the blend is (ONE, SRC_ALPHA), set by the pipeline. The
// colour fade goes over everything: new = old * (1 - f) + colour * f.
//
// C arrives as the raw 565 words (R16_UINT) and is expanded here by bit
// replication exactly as `omk::expand565Rgba` does on the CPU path (blue by
// `>> 3`, as there); an untouched key
// pixel (0xF81F) is "the world shows here" by itself. The 2D stays 565-sourced
// - it is the game's art - but the world under it is never re-quantised.
#version 450

layout(set = 0, binding = 0) uniform usampler2D pic;   // C, the 565 words
layout(set = 0, binding = 1) uniform sampler2D mask;   // M, R8_UNORM

layout(push_constant) uniform Overlay {
    vec4 fade;     // the colour fade: rgb, weight
} pc;

layout(location = 0) out vec4 o;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    uint v = texelFetch(pic, p, 0).r;
    float m = texelFetch(mask, p, 0).r;
    vec4 c;
    if (v == 0xF81Fu) {
        c = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        uint r5 = v >> 11, g6 = (v >> 5) & 63u, b5 = v & 31u;
        uint R = (r5 << 3) | (r5 >> 2), G = (g6 << 2) | (g6 >> 4), B = (b5 << 3) | (b5 >> 3);
        c = vec4(float(R) / 255.0, float(G) / 255.0, float(B) / 255.0, m);
    }
    o = vec4(c.rgb * (1.0 - pc.fade.a) + pc.fade.rgb * pc.fade.a, c.a * (1.0 - pc.fade.a));
}
