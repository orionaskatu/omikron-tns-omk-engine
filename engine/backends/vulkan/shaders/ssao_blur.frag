// SPDX-License-Identifier: GPL-3.0-or-later
// SSAO's second half (`ssao.frag`): a 4x4 depth-aware average of the AO -
// the window the hemisphere's rotation repeats over, so its pattern cancels -
// written as a GREY that the pipeline's blend MULTIPLIES into the frame
// (dst * src), so nothing but the darkening is touched.
#version 450

// Under MSAA the depth is MULTISAMPLED and read at its first sample (the
// `_ms` build of this file, glslc -DOMK_MS); `texelFetch` reads both alike.
#ifdef OMK_MS
layout(set = 0, binding = 0) uniform sampler2DMS depthTex;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTex;
#endif
layout(set = 0, binding = 1) uniform sampler2D aoTex;

layout(push_constant) uniform Push {
    vec4 proj;   // tanH, tanV, A, B
    vec4 view;   // viewport w, h, radius, strength
} pc;

layout(location = 0) out vec4 outColour;

float viewZ(float z) { return pc.proj.w / (z - pc.proj.z); }

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 lim = ivec2(pc.view.xy) - 1;
    if (px.x > lim.x || px.y > lim.y) { outColour = vec4(1.0); return; }
    float z0 = texelFetch(depthTex, px, 0).r;
    if (z0 >= 0.99999) { outColour = vec4(1.0); return; }
    float v0 = viewZ(z0);
    float sum = 0.0, wsum = 0.0;
    for (int j = -2; j <= 1; ++j)
        for (int i = -2; i <= 1; ++i) {
            ivec2 q = clamp(px + ivec2(i, j), ivec2(0), lim);
            float zq = texelFetch(depthTex, q, 0).r;
            float w = zq >= 0.99999 ? 0.0 : exp(-abs(viewZ(zq) - v0) / (0.08 * v0 + 2.0));
            sum += texelFetch(aoTex, q, 0).r * w;
            wsum += w;
        }
    float ao = wsum > 0.0 ? sum / wsum : 1.0;
    ao = ao * ao;   // contrast: open stays open, creases deepen
    outColour = vec4(vec3(ao), 1.0);
}
