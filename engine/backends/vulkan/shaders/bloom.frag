// SPDX-License-Identifier: GPL-3.0-or-later
// THE BLOOM CHAIN of the post-processing passes (shared by the Direct3D 12 and Vulkan backends) (`bloom = on`,
// `todo/enhancements.md` 22) - one shader, three jobs, drawn level by level
// into a chain of R11G11B10_FLOAT images at half, quarter... of the display
// size (`D3D12Renderer::runPost`):
//
//   mode 0  THE FIRST LEVEL, from the frame itself: the 13-tap downsample of
//           Jimenez's "Next generation post processing in Call of Duty:
//           Advanced Warfare" (SIGGRAPH 2014), each of its five 2x2 groups
//           weighted by 1 / (1 + luma) - the KARIS average, so one hot texel
//           cannot flicker the whole halo - over the BRIGHT PASS: the frame's
//           8-bit colour taken to linear light, EXPANDED by `post.frag`'s
//           inverse shoulder (`expand`, kept identical there) so a near-white
//           neon or lamp texel stands for more light than a mid grey, and
//           only what lies above a soft threshold kept;
//   mode 1  every further level: the same 13 taps, plain;
//   mode 2  back UP the chain: a 3x3 tent over the smaller level, added onto
//           the larger by the pipeline's blend (ONE, ONE) - so level 0 ends
//           up holding the sum of every level, each wider than the last.
//
// The taps are clamped to `uvMax`, the picture's own rectangle: a letterboxed
// frame's strip sits in the top-left of the target (`renderer.h`, THE
// LETTERBOX) and the rows under it must not bleed into it.
#version 450

layout(set = 0, binding = 0) uniform sampler2D src;   // linear, clamped

layout(push_constant) uniform Bloom {
    vec4 texel;    // xy: 1 / the INPUT's size; zw: the picture's uv rectangle (uvMax)
    vec4 params;   // x: threshold, y: knee, z: mode (0 first, 1 down, 2 up), w: the up pass's weight
    vec4 target;   // xy: 1 / the TARGET's size (`fsq.vert` gives no uv: gl_FragCoord does)
} pc;

layout(location = 0) out vec4 o;

// THE EXPANSION - verbatim in `post.frag`: identity below the knee `k`, the
// inverse of the exponential shoulder above it, capped where the shoulder
// gives back 255 (u <= 1 - 1e-3).
const float kKnee = 0.5;
vec3 expand(vec3 L) {
    vec3 u = clamp((L - kKnee) / (1.0 - kKnee), 0.0, 1.0 - 1e-3);
    vec3 e = kKnee - (1.0 - kKnee) * log(1.0 - u);
    return mix(L, e, step(kKnee, L));
}

vec3 at(vec2 p) {
    return texture(src, min(p, pc.texel.zw - 0.5 * pc.texel.xy)).rgb;
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 bright(vec3 c) {
    vec3 E = expand(pow(max(c, vec3(0.0)), vec3(2.2)));
    // the soft threshold (a quadratic knee) on the brightness: the luma, or
    // 0.6 of the brightest channel - so a saturated red neon passes, while a
    // pale sky (its blue near 240, its luma not) does not glow over the roofs
    float br = max(luma(E), 0.6 * max(E.r, max(E.g, E.b)));
    float t = pc.params.x, k = pc.params.y;
    float soft = clamp(br - t + k, 0.0, 2.0 * k);
    soft = soft * soft / (4.0 * k + 1e-5);
    float w = max(soft, br - t) / max(br, 1e-4);
    return E * w;
}

void main() {
    vec2 uv = gl_FragCoord.xy * pc.target.xy;
    vec2 d = pc.texel.xy;
    int mode = int(pc.params.z + 0.5);
    if (mode == 2) {
        vec3 s = at(uv) * 4.0;
        s += (at(uv + vec2(-d.x, 0.0)) + at(uv + vec2(d.x, 0.0)) +
              at(uv + vec2(0.0, -d.y)) + at(uv + vec2(0.0, d.y))) * 2.0;
        s += at(uv + vec2(-d.x, -d.y)) + at(uv + vec2(d.x, -d.y)) +
             at(uv + vec2(-d.x, d.y)) + at(uv + vec2(d.x, d.y));
        o = vec4(s * (pc.params.w / 16.0), 1.0);
        return;
    }
    vec3 a = at(uv + d * vec2(-2.0, -2.0)), b = at(uv + d * vec2(0.0, -2.0)), c = at(uv + d * vec2(2.0, -2.0));
    vec3 e = at(uv + d * vec2(-1.0, -1.0)), f = at(uv + d * vec2(1.0, -1.0));
    vec3 g = at(uv + d * vec2(-2.0, 0.0)), h = at(uv), i = at(uv + d * vec2(2.0, 0.0));
    vec3 j = at(uv + d * vec2(-1.0, 1.0)), k = at(uv + d * vec2(1.0, 1.0));
    vec3 l = at(uv + d * vec2(-2.0, 2.0)), m = at(uv + d * vec2(0.0, 2.0)), n = at(uv + d * vec2(2.0, 2.0));
    if (mode == 0) {
        a = bright(a); b = bright(b); c = bright(c); e = bright(e); f = bright(f);
        g = bright(g); h = bright(h); i = bright(i); j = bright(j); k = bright(k);
        l = bright(l); m = bright(m); n = bright(n);
        vec3 g0 = (e + f + j + k) * 0.25, g1 = (a + b + g + h) * 0.25, g2 = (b + c + h + i) * 0.25;
        vec3 g3 = (g + h + l + m) * 0.25, g4 = (h + i + m + n) * 0.25;
        float w0 = 0.5 / (1.0 + luma(g0)), w1 = 0.125 / (1.0 + luma(g1)), w2 = 0.125 / (1.0 + luma(g2));
        float w3 = 0.125 / (1.0 + luma(g3)), w4 = 0.125 / (1.0 + luma(g4));
        o = vec4((g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4), 1.0);
        return;
    }
    vec3 s = (e + f + j + k) * 0.125;
    s += (a + b + g + h) * 0.03125 + (b + c + h + i) * 0.03125;
    s += (g + h + l + m) * 0.03125 + (h + i + m + n) * 0.03125;
    o = vec4(s, 1.0);
}
