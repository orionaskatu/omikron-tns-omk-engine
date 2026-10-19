// SPDX-License-Identifier: GPL-3.0-or-later
// FRAME GENERATION's RESOLVE (`todo/enhancements.md` 27), the Direct3D 12 and Vulkan
// backend's own: the in-between picture from the two splat heads
// (`fg_splat.frag`). At each pixel the front-most surface either frame put
// there decides:
//   * both frames' surfaces at one depth (within ~7%): the same surface seen
//     from both sides - the two source pixels blended half and half;
//   * one in front: that frame's pixel, blended with the OTHER frame's picture
//     where its own vector says the point was - when the other frame shows the
//     same surface there (depth within ~13%, a like motion); where it does not
//     (the point hidden in the other frame - a DISOCCLUSION) the one frame alone.
// A pixel neither frame reached directly (a crack where the motion stretches
// the picture) takes the FARTHEST of its eight neighbours' splats - the
// background's, so a crack never grows a foreground edge - and neither at all
// the two pictures blended in place.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D colN;
layout(set = 0, binding = 1) uniform texture2D colP;
layout(set = 0, binding = 2) uniform texture2D auxN;
layout(set = 0, binding = 3) uniform texture2D auxP;
layout(set = 0, binding = 4, r32ui) uniform readonly uimage2D headN;
layout(set = 0, binding = 5, r32ui) uniform readonly uimage2D headP;

layout(push_constant) uniform Fg {
    vec4 pic;   // the picture's width, height (display px)
    vec4 k;     // x the vectors' scale
} pc;

layout(location = 0) out vec4 o;

const uint kEmpty = 0xFFFFFFFFu;
const int kSame = 24;       // depth keys (~227 an octave): one surface from both sides
const int kAgree = 40;      // ...and the other frame's depth at the point's place there

int depthKey(float zq) {
    return int(clamp(log2(max(zq * 4.0, 1.0)) * (4094.0 / 18.0), 0.0, 4094.0));
}
ivec2 offsetOf(uint v) {
    return ivec2(int((v >> 10) & 1023u) - 512, int(v & 1023u) - 512);
}

ivec2 W;

// the farthest of the eight neighbours' splats, kEmpty for none
#define FARTHEST(NAME, HEAD)                                                  \
    uint NAME(ivec2 q) {                                                      \
        uint best = kEmpty;                                                   \
        bool found = false;                                                   \
        for (int dy = -1; dy <= 1; ++dy)                                      \
            for (int dx = -1; dx <= 1; ++dx) {                                \
                ivec2 s = q + ivec2(dx, dy);                                  \
                if ((dx == 0 && dy == 0) || any(lessThan(s, ivec2(0))) ||     \
                    any(greaterThanEqual(s, W))) continue;                    \
                uint v = imageLoad(HEAD, s).r;                                \
                if (v == kEmpty) continue;                                    \
                if (!found || v > best) best = v;                             \
                found = true;                                                 \
            }                                                                 \
        return best;                                                          \
    }
FARTHEST(farthestN, headN)
FARTHEST(farthestP, headP)

// a bilinear read of a picture at a pixel-centred position, clamped to the picture
#define BILINEAR(NAME, TEX)                                                   \
    vec4 NAME(vec2 pos) {                                                     \
        vec2 f = pos - 0.5;                                                   \
        ivec2 i = ivec2(floor(f));                                            \
        vec2 t = f - vec2(i);                                                 \
        ivec2 lo = clamp(i, ivec2(0), W - 1), hi = clamp(i + 1, ivec2(0), W - 1); \
        vec4 a = mix(texelFetch(TEX, lo, 0), texelFetch(TEX, ivec2(hi.x, lo.y), 0), t.x); \
        vec4 b = mix(texelFetch(TEX, ivec2(lo.x, hi.y), 0), texelFetch(TEX, hi, 0), t.x); \
        return mix(a, b, t.y);                                                \
    }
BILINEAR(bilinN, colN)
BILINEAR(bilinP, colP)

// the other frame shows the same surface at `pos`: inside the picture, a like
// depth, and its own vector like the one that led there (steady motion)
bool agrees(vec4 other, int key, vec2 motion) {
    if (other.w < 0.5) return false;
    if (abs(depthKey(other.z) - key) > kAgree) return false;
    return length(pc.k.x * other.xy - motion) <= 2.0 + 0.5 * length(motion);
}
bool inside(vec2 pos) {
    return all(greaterThanEqual(pos, vec2(0.0))) && all(lessThan(pos, vec2(W)));
}

void main() {
    ivec2 q = ivec2(gl_FragCoord.xy);
    W = ivec2(pc.pic.xy);
    if (q.x >= W.x || q.y >= W.y) {
        o = vec4(texelFetch(colN, q, 0).rgb, 1.0);
        return;
    }
    float k = pc.k.x;
    // a neighbour's splat taken over holds only where its offset, applied
    // here, still lands on that surface in its frame - else it would fetch
    // whatever hid the surface there (a ghost of the outline of a body that
    // has moved off it)
    uint a = imageLoad(headN, q).r;
    if (a == kEmpty) {
        a = farthestN(q);
        ivec2 s = clamp(q + offsetOf(a), ivec2(0), W - 1);
        if (a != kEmpty && abs(depthKey(texelFetch(auxN, s, 0).z) - int(a >> 20)) > kSame) a = kEmpty;
    }
    uint b = imageLoad(headP, q).r;
    if (b == kEmpty) {
        b = farthestP(q);
        ivec2 s = clamp(q + offsetOf(b), ivec2(0), W - 1);
        if (b != kEmpty && abs(depthKey(texelFetch(auxP, s, 0).z) - int(b >> 20)) > kSame) b = kEmpty;
    }
    if (a == kEmpty && b == kEmpty) {
        o = vec4(0.5 * (texelFetch(colN, q, 0).rgb + texelFetch(colP, q, 0).rgb), 1.0);
        return;
    }
    int ka = a == kEmpty ? 1 << 20 : int(a >> 20);
    int kb = b == kEmpty ? 1 << 20 : int(b >> 20);
    ivec2 sa = clamp(q + offsetOf(a), ivec2(0), W - 1);
    ivec2 sb = clamp(q + offsetOf(b), ivec2(0), W - 1);
    vec3 c;
    if (a != kEmpty && b != kEmpty && abs(ka - kb) <= kSame) {
        c = 0.5 * (texelFetch(colN, sa, 0).rgb + texelFetch(colP, sb, 0).rgb);
    } else if (ka < kb) {
        // frame N's surface in front: where was it in N-1?
        vec4 an = texelFetch(auxN, sa, 0);
        vec2 pp = vec2(sa) + 0.5 + k * an.xy;
        c = texelFetch(colN, sa, 0).rgb;
        if (inside(pp) && agrees(texelFetch(auxP, ivec2(pp), 0), ka, k * an.xy))
            c = 0.5 * (c + bilinP(pp).rgb);
    } else {
        // frame N-1's surface in front: where is it in N?
        vec4 ap = texelFetch(auxP, sb, 0);
        vec2 pn = vec2(sb) + 0.5 - k * ap.xy;
        c = texelFetch(colP, sb, 0).rgb;
        if (inside(pn) && agrees(texelFetch(auxN, ivec2(pn), 0), kb, k * ap.xy))
            c = 0.5 * (c + bilinN(pn).rgb);
    }
    o = vec4(c, 1.0);
}
