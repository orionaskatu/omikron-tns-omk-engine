// SPDX-License-Identifier: GPL-3.0-or-later
// VOLUMETRIC LAMP LIGHT, the composite (`todo/enhancements.md` 24), for the
// Direct3D 12 backend: `vol.frag`'s half-size in-scattered light brought up to
// the render size and ADDED over the scene's colour (the blend: one + one),
// before the upscaler and the post chain - so the halo is upscaled, bloomed and
// graded with the rest of the frame.
//
// A 4x4 gather of the half-size texels round the pixel, each weighted by a
// Gaussian of its distance and by how near its depth lies to this pixel's own
// (a joint bilateral upsample): the march's per-pixel jitter is averaged away,
// and a halo behind a character does not bleed over his silhouette. Where no
// texel shares the pixel's depth (a pole one pixel wide) the plain Gaussian
// stands in. A dither of half a level keeps the faint gradient from banding
// in the 8-bit target.
#version 450

#ifdef OMK_MS
layout(set = 0, binding = 0) uniform sampler2DMS depthTex;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTex;
#endif
layout(set = 0, binding = 1) uniform sampler2D volTex;

layout(push_constant) uniform Push {
    vec4 proj;    // tanH, tanV, A, B
    vec4 view;    // xy the picture's size in render pixels, zw the half-size image's
} pc;

layout(location = 0) out vec4 o;

void main() {
    ivec2 p = min(ivec2(gl_FragCoord.xy), ivec2(pc.view.xy) - 1);
    float d = texelFetch(depthTex, p, 0).r;
    float zp = min(pc.proj.w / (d - pc.proj.z), 60000.0) * 1e-3;
    vec2 hc = (vec2(p) + 0.5) * 0.5 - 0.5;     // this pixel in half-size texels
    ivec2 h0 = ivec2(floor(hc));
    ivec2 hl = ivec2(pc.view.zw) - 1;
    vec3 sb = vec3(0.0), sg = vec3(0.0);
    float wb = 0.0, wg = 0.0;
    for (int y = -1; y <= 2; ++y)
        for (int x = -1; x <= 2; ++x) {
            ivec2 q = clamp(h0 + ivec2(x, y), ivec2(0), hl);
            vec4 v = texelFetch(volTex, q, 0);
            vec2 dd = vec2(h0 + ivec2(x, y)) - hc;
            float g = exp(-0.5 * dot(dd, dd));
            float rz = (v.a - zp) / max(zp, 1e-3);
            float wz = exp(-rz * rz * 60.0);
            sb += v.rgb * g * wz; wb += g * wz;
            sg += v.rgb * g;      wg += g;
        }
    vec3 c = wb > 0.05 * wg ? sb / wb : sg / wg;
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    c = max(c + (n - 0.5) / 255.0 * step(0.5 / 255.0, max(c.r, max(c.g, c.b))), 0.0);
    o = vec4(c, 0.0);
}
