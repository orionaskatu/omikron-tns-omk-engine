// SPDX-License-Identifier: GPL-3.0-or-later
// SSAO, `todo/enhancements.md` 15 - an ENHANCEMENT, off by default.
//
// Screen-space ambient occlusion from the depth buffer alone: each pixel's
// view-space position is rebuilt from its depth through the scene's own
// projection (`pushView`: x_ndc = vx / (vz tanH), y_ndc = -vy / (vz tanV),
// z = A + B / vz), its normal from the neighbouring depths, and 16 points of a
// hemisphere around it are tested against the depth there. The rotation of the
// hemisphere repeats over 4x4 pixels, which `ssao_blur.frag`'s 4x4 window
// averages out exactly. The result is a factor, 1 = open, written to an R8
// target and multiplied over the frame by the blur.
#version 450
#ifdef OMK_RT
// RAY-TRACED AO (`todo/enhancements.md` 19): the Direct3D 12 backend's -DOMK_RT
// build replaces the depth-buffer hemisphere with a few short rays against the
// frame's TLAS (`backends/d3d12/shaders/rt.glsl`) - the set AND the bodies,
// on screen or not. Vulkan never defines it.
#include "rt.glsl"
#endif

// Under MSAA the depth is MULTISAMPLED and read at its first sample (the
// `_ms` build of this file, glslc -DOMK_MS); `texelFetch` reads both alike.
#ifdef OMK_MS
layout(set = 0, binding = 0) uniform sampler2DMS depthTex;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTex;
#endif

layout(push_constant) uniform Push {
    vec4 proj;   // tanH, tanV, A, B
    vec4 view;   // viewport w, h (render pixels), radius (world units), strength
#ifdef OMK_RT
    vec4 eye;    // the camera in WORLD space: xyz the eye, w the rays' instance mask
    vec4 s;      // ...and its basis (`cameraBasis`): view x = s, view y = u,
    vec4 u;      //    view z = f, so world = eye + s x + u y + f z
    vec4 f;
#endif
} pc;

layout(location = 0) out float outAO;

float viewZ(float z) { return pc.proj.w / (z - pc.proj.z); }

vec3 viewPos(ivec2 p) {
    float z = texelFetch(depthTex, p, 0).r;
    float vz = viewZ(z);
    vec2 ndc = (vec2(p) + 0.5) / pc.view.xy * 2.0 - 1.0;
    return vec3(ndc.x * pc.proj.x * vz, -ndc.y * pc.proj.y * vz, vz);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 lim = ivec2(pc.view.xy) - 1;
    if (px.x > lim.x || px.y > lim.y) { outAO = 1.0; return; }
    float z0 = texelFetch(depthTex, px, 0).r;
    if (z0 >= 0.99999) { outAO = 1.0; return; }          // the sky / nothing drawn
    vec3 P = viewPos(px);

    // the normal, from whichever neighbour on each axis is nearer in depth -
    // so a silhouette edge does not tilt it
    vec3 r = viewPos(min(px + ivec2(1, 0), lim)) - P;
    vec3 l = P - viewPos(max(px - ivec2(1, 0), ivec2(0)));
    vec3 d = viewPos(min(px + ivec2(0, 1), lim)) - P;
    vec3 u = P - viewPos(max(px - ivec2(0, 1), ivec2(0)));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(d.z) < abs(u.z) ? d : u;
    vec3 N = normalize(cross(dx, dy));
    if (dot(N, P) > 0.0) N = -N;                          // facing the eye

    // a basis around N, turned by the 4x4 ordered pattern
    const float bayer[16] = float[16](0., 8., 2., 10., 12., 4., 14., 6.,
                                      3., 11., 1., 9., 15., 7., 13., 5.);
    float a = bayer[(px.y & 3) * 4 + (px.x & 3)] / 16.0 * 6.2831853;
#ifdef OMK_RT
    {
        // the pixel and its normal in the world, four rays as long as SSAO's
        // radius, the 4x4 pattern turning them as it turns the hemisphere
        // (and `ssao_blur.frag` averaging it out); a near hit occludes most
        vec3 Pw = pc.eye.xyz + pc.s.xyz * P.x + pc.u.xyz * P.y + pc.f.xyz * P.z;
        vec3 Nw = normalize(pc.s.xyz * N.x + pc.u.xyz * N.y + pc.f.xyz * N.z);
        float Rr = pc.view.z;
        vec3 o = Pw + Nw * (0.5 + P.z * 0.002);
        const int KR = 4;
        float occR = 0.0;
        for (int i = 0; i < KR; ++i)
            if (rtTrace(o, rtCosine(Nw, i, KR, a, rtNoise(gl_FragCoord.xy)), Rr, uint(pc.eye.w), true))
                occR += 1.0 - smoothstep(0.3 * Rr, Rr, rtT);
        float aoR = 1.0 - pc.view.w * occR / float(KR);
        aoR = mix(aoR, 1.0, smoothstep(3500.0, 8000.0, P.z));
        outAO = clamp(aoR, 0.0, 1.0);
        return;
    }
#endif
    vec3 helper = abs(N.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 T = normalize(cross(helper, N));
    vec3 B = cross(N, T);
    vec3 Tr = T * cos(a) + B * sin(a);
    vec3 Br = cross(N, Tr);

    float R = pc.view.z;
    float occ = 0.0, wsum = 0.0;
    const int K = 16;
    for (int i = 0; i < K; ++i) {
        float t = (float(i) + 0.5) / float(K);
        float phi = float(i) * 2.3999632;                 // the golden angle
        float cosT = sqrt(1.0 - 0.9 * t);                 // cosine-weighted, kept off the tangent
        float sinT = sqrt(t);
        vec3 h = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        float len = mix(0.15, 1.0, t * t);                // denser near P
        vec3 S = P + (Tr * h.x + Br * h.y + N * h.z) * (R * len);
        // the near samples weigh most: a crease occludes them all, while a
        // body standing a metre off only reaches the outer ones - so the
        // architecture reads and a character's darkening stays at its feet
        float w = 1.0 - 0.6 * len;
        wsum += w;
        if (S.z <= 1.0) continue;
        vec2 ndc = vec2(S.x / (S.z * pc.proj.x), -S.y / (S.z * pc.proj.y));
        vec2 sp = (ndc * 0.5 + 0.5) * pc.view.xy;
        if (sp.x < 0.0 || sp.y < 0.0 || sp.x >= pc.view.x || sp.y >= pc.view.y) continue;
        float zs = texelFetch(depthTex, ivec2(sp), 0).r;
        if (zs >= 0.99999) continue;
        float sceneZ = viewZ(zs);
        // occluded when the surface there is IN FRONT of the sample point -
        // but only by a surface within the radius IN 3D: depth alone takes a
        // body as infinitely thick, and darkened the floor two metres in
        // front of the player
        vec2 qn = (floor(sp) + 0.5) / pc.view.xy * 2.0 - 1.0;
        vec3 Q = vec3(qn.x * pc.proj.x * sceneZ, -qn.y * pc.proj.y * sceneZ, sceneZ);
        float range = 1.0 - smoothstep(0.8 * R, 1.4 * R, length(Q - P));
        // the bias grows with distance: a standard (not reversed) depth
        // buffer resolves less the further it looks
        occ += (sceneZ < S.z - (R * 0.04 + P.z * 0.004) ? 1.0 : 0.0) * range * w;
    }
    float ao = 1.0 - pc.view.w * occ / max(wsum, 1e-3);
    // the far street is fogged black and its geometry coarse: let it go
    ao = mix(ao, 1.0, smoothstep(3500.0, 8000.0, P.z));
    outAO = clamp(ao, 0.0, 1.0);
}
