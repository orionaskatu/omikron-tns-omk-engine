// SPDX-License-Identifier: GPL-3.0-or-later
// VOLUMETRIC LAMP LIGHT, the march (`todo/enhancements.md` 24), for the
// Direct3D 12 and Vulkan backends (one source): an ENHANCEMENT, off unless `volumetrics = on`.
//
// At HALF the render size, for each pixel: the eye ray to the opaque surface
// (rebuilt from the depth through the scene's own projection - `ssao.frag`'s
// `viewPos`, the camera basis taking it into the world), and the light each of
// the frame's lamps in the air (`View::airLights`) scatters towards the eye
// along it. Single scattering in a thin, uniform air:
//
//   * the lamp's light at a point x falls as 1 / (rc^2 + r^2) - inverse square
//     with a CORE of radius rc, so it stays finite at the lamp - times a window
//     that takes it to zero at the record's outer reach (1 - r/R)^2, so a lamp
//     lights the air round it and not the whole street;
//   * a Henyey-Greenstein phase (g = 0.35): more light scattered on towards the
//     eye when it looks at the lamp, so a lamp seen across the street wears a
//     halo and the air beside it a faint glow;
//   * the scene's LINEAR FOG (`View::fog*`) takes what lies far off, as it
//     takes the surfaces there.
//
// Only the stretch of the ray inside the lamp's reach is marched, and it is
// marched in EQUAL ANGLES seen from the lamp (equiangular sampling, Kulla &
// Fajardo 2012): the inverse square integrates exactly, rc^2 / H (thb - tha)
// with H^2 = h^2 + rc^2 and h the ray's closest approach, and the 8 steps per
// lamp carry only the slow terms (the window, the phase, the fog) - so a lamp
// a pixel wide and a lamp filling the screen are both sampled where their
// light is. The steps are jittered per pixel by interleaved gradient noise
// (Jimenez 2014), which `volmix.frag`'s 4x4 depth-aware filter averages out.
// No shadowing: the ray stops at the opaque surface, so a wall in front of a
// lamp hides the air behind it, and that is all.
//
// Out: RGBA16F - the in-scattered light, and the surface's view depth / 1000
// for the filter. Under MSAA the depth is read at its first sample (`_ms`).
#version 450

#ifdef OMK_MS
layout(set = 0, binding = 0) uniform sampler2DMS depthTex;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTex;
#endif

struct Lamp {
    vec4 posR;    // xyz the lamp, w the reach of its glow R (a share of the record's outer reach)
    vec4 colI;    // rgb colour x intensity (the record's +32, capped)
    vec4 shape;   // x the core radius rc
};
layout(set = 0, binding = 2) uniform Lamps {
    Lamp l[16];
    ivec4 count;  // x lamps
} lm;

layout(push_constant) uniform Push {
    vec4 proj;    // tanH, tanV, A, B (view z = B / (depth - A))
    vec4 view;    // xy the picture's size in render pixels, z the frame (the jitter turns), w the near cut
    vec4 eye;     // xyz the eye in the world, w the air's density
    vec4 s;       // xyz the camera's right, w fog start
    vec4 u;       // xyz its up,          w fog end (<= start: no fog)
    vec4 f;       // xyz its forward,     w the farthest view z marched
} pc;

layout(location = 0) out vec4 o;

const float kG = 0.35;
const int kSteps = 8;

void main() {
    ivec2 lim = ivec2(pc.view.xy) - 1;
    ivec2 p = min(ivec2(gl_FragCoord.xy) * 2, lim);
#ifdef OMK_MS
    float d = texelFetch(depthTex, p, 0).r;
#else
    float d = texelFetch(depthTex, p, 0).r;
#endif
    float zs = pc.proj.w / (d - pc.proj.z);
    vec2 ndc = (vec2(p) + 0.5) / pc.view.xy * 2.0 - 1.0;
    // the world step per unit of view z, and the ray's unit direction
    vec3 D = pc.s.xyz * (ndc.x * pc.proj.x) - pc.u.xyz * (ndc.y * pc.proj.y) + pc.f.xyz;
    float dl = length(D);
    vec3 Dn = D / dl;
    float tEnd = min(zs, pc.f.w);
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))) +
                      0.61803399 * pc.view.z);
    bool fog = pc.u.w > pc.s.w;
    vec3 L = vec3(0.0);
    for (int i = 0; i < lm.count.x; ++i) {
        vec3 C = lm.l[i].posR.xyz;
        float R = lm.l[i].posR.w;
        // the ray in WORLD length s from the eye: x = eye + Dn s; its stretch
        // inside the reach, cut at the near plane and at the surface
        vec3 oc = pc.eye.xyz - C;
        float b = dot(Dn, oc);
        float disc = b * b - (dot(oc, oc) - R * R);
        if (disc <= 0.0) continue;
        float sq = sqrt(disc);
        float s0 = max(-b - sq, pc.view.w * dl), s1 = min(-b + sq, tEnd * dl);
        if (s1 <= s0) continue;
        // equiangular about the closest approach sc, at "height" H
        float sc = -b;
        float rc = lm.l[i].shape.x;
        float h2 = max(dot(oc, oc) - b * b, 0.0);
        float H = sqrt(h2 + rc * rc);
        float ta = atan((s0 - sc) / H), tb = atan((s1 - sc) / H);
        float acc = 0.0;
        for (int k = 0; k < kSteps; ++k) {
            float th = mix(ta, tb, (float(k) + jit) / float(kSteps));
            float s = sc + H * tan(th);
            vec3 v = oc + Dn * s;                 // the point, from the lamp
            float r = length(v);
            float w = max(1.0 - r / R, 0.0);
            w *= w;
            float ct = -dot(v, Dn) / max(r, 1e-3);  // the light's way on . the way back to the eye
            float ph = (1.0 - kG * kG) / pow(1.0 + kG * kG - 2.0 * kG * ct, 1.5);
            float fv = fog ? clamp((pc.u.w - s / dl) / (pc.u.w - pc.s.w), 0.0, 1.0) : 1.0;
            acc += w * ph * fv;
        }
        // the exact integral of rc^2 / (rc^2 + r^2) over the stretch, times
        // the mean of the slow terms
        L += lm.l[i].colI.rgb * (acc / float(kSteps)) * (rc * rc / H) * (tb - ta);
    }
    o = vec4(L * pc.eye.w, min(zs, 60000.0) * 1e-3);
}
