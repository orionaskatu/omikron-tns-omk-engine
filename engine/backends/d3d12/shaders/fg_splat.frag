// SPDX-License-Identifier: GPL-3.0-or-later
// FRAME GENERATION's SPLAT (`todo/enhancements.md` 27), the Direct3D 12
// backend's own. Every pixel of the two kept frames is pushed half-way along
// its motion to where it stands at the IN-BETWEEN time - frame N's back along
// its own vector (N -> N-1), frame N-1's forward along its own vector negated
// (N-1 -> N-2, turned round: the motion taken as steady over two frames) - and
// written there with a DEPTH TEST: one 32-bit word a pixel and frame, the
// nearest surface's, `imageAtomicMin` of
//     depth key (12 bits, log2 of the view depth, ~0.3%) << 20
//   | the source pixel's offset from the target, x + 512 (10 bits), y + 512 (10 bits)
// so what the in-between frame shows at a pixel is the front-most surface
// that either frame puts there. `pc.k.x` scales the vectors (1; 2 for the
// probe's N-2/N pair). `pc.k.y` 1: the CLEAR, both heads emptied.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 2) uniform texture2D auxN;
layout(set = 0, binding = 3) uniform texture2D auxP;
layout(set = 0, binding = 4, r32ui) uniform uimage2D headN;
layout(set = 0, binding = 5, r32ui) uniform uimage2D headP;

layout(push_constant) uniform Fg {
    vec4 pic;   // the picture's width, height (display px)
    vec4 k;     // x the vectors' scale, y 1 = clear
} pc;

const uint kEmpty = 0xFFFFFFFFu;

uint depthKey(float zq) {
    return uint(clamp(log2(max(zq * 4.0, 1.0)) * (4094.0 / 18.0), 0.0, 4094.0));
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (pc.k.y > 0.5) {
        imageStore(headN, p, uvec4(kEmpty));
        imageStore(headP, p, uvec4(kEmpty));
        return;
    }
    ivec2 W = ivec2(pc.pic.xy);
    if (p.x >= W.x || p.y >= W.y) return;
    vec4 a = texelFetch(auxN, p, 0);
    if (a.w > 0.5) {
        ivec2 q = ivec2(floor(vec2(p) + 0.5 + 0.5 * pc.k.x * a.xy));
        ivec2 off = p - q;
        if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, W)) && all(lessThanEqual(abs(off), ivec2(511))))
            imageAtomicMin(headN, q, (depthKey(a.z) << 20) | (uint(off.x + 512) << 10) | uint(off.y + 512));
    }
    vec4 b = texelFetch(auxP, p, 0);
    if (b.w > 0.5) {
        ivec2 q = ivec2(floor(vec2(p) + 0.5 - 0.5 * pc.k.x * b.xy));
        ivec2 off = p - q;
        if (all(greaterThanEqual(q, ivec2(0))) && all(lessThan(q, W)) && all(lessThanEqual(abs(off), ivec2(511))))
            imageAtomicMin(headP, q, (depthKey(b.z) << 20) | (uint(off.x + 512) << 10) | uint(off.y + 512));
    }
}
