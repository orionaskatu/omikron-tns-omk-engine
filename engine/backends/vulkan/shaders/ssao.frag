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

layout(set = 0, binding = 0) uniform sampler2D depthTex;

layout(push_constant) uniform Push {
    vec4 proj;   // tanH, tanV, A, B
    vec4 view;   // viewport w, h (render pixels), radius (world units), strength
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
    vec3 helper = abs(N.y) < 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 T = normalize(cross(helper, N));
    vec3 B = cross(N, T);
    vec3 Tr = T * cos(a) + B * sin(a);
    vec3 Br = cross(N, Tr);

    float R = pc.view.z;
    float occ = 0.0;
    const int K = 16;
    for (int i = 0; i < K; ++i) {
        float t = (float(i) + 0.5) / float(K);
        float phi = float(i) * 2.3999632;                 // the golden angle
        float cosT = sqrt(1.0 - t);                       // cosine-weighted
        float sinT = sqrt(t);
        vec3 h = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
        float len = mix(0.15, 1.0, t * t);                // denser near P
        vec3 S = P + (Tr * h.x + Br * h.y + N * h.z) * (R * len);
        if (S.z <= 1.0) continue;
        vec2 ndc = vec2(S.x / (S.z * pc.proj.x), -S.y / (S.z * pc.proj.y));
        vec2 sp = (ndc * 0.5 + 0.5) * pc.view.xy;
        if (sp.x < 0.0 || sp.y < 0.0 || sp.x >= pc.view.x || sp.y >= pc.view.y) continue;
        float zs = texelFetch(depthTex, ivec2(sp), 0).r;
        if (zs >= 0.99999) continue;
        float sceneZ = viewZ(zs);
        // occluded when the surface there is IN FRONT of the sample point;
        // a surface much nearer than the radius is another object, faded out
        float range = smoothstep(0.0, 1.0, R / max(abs(P.z - sceneZ), 1e-3));
        // the bias grows with distance: a standard (not reversed) depth
        // buffer resolves less the further it looks
        occ += (sceneZ < S.z - (R * 0.04 + P.z * 0.004) ? 1.0 : 0.0) * range;
    }
    float ao = 1.0 - pc.view.w * occ / float(K);
    // the far street is fogged black and its geometry coarse: let it go
    ao = mix(ao, 1.0, smoothstep(1500.0, 4500.0, P.z));
    outAO = clamp(ao, 0.0, 1.0);
}
