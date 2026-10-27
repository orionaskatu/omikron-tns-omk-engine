#version 450
#ifdef OMK_RT
// HYBRID RAY TRACING (`todo/enhancements.md` 19): the -DOMK_RT build, both
// backends' (`rt.glsl`; the descriptor sets are `OMK_RT_SET`'s three).
#include "rt.glsl"
#endif
// The scene fragment shader, and it is a transcription of `raster.cpp`'s inner
// loop rather than a rendering choice:
//
//   * the texel is the material's own pixel units WRAPPED, which is what lets
//     one atlas tile across a wall - so the UVs arrive unnormalised and are
//     divided by textureSize here, with the sampler set to REPEAT;
//   * the baked vertex value is a COLOUR and multiplies the texel (ASSETS 4c);
//     38.9% of set vertices are not grey, and reading one byte as a brightness
//     renders every set in monochrome, which was this repo's own bug;
//   * `cutout` is flag 0x800 - a COLOUR KEY on black, the engine's
//     SetRenderState(27, 1) arm - and never alpha;
//   * the fog is linear over the clip distance's range, the CPU having
//     already applied the bucket key's exclusions (`renderer.h`, the View).
layout(push_constant) uniform Push {
    mat4  mvp;
    int   cutout;      // bit 0: flag 0x800, a colour key on black, never alpha;
                       // bits 8..: a TEXTURE PACK slot's k (`o3de/texpack.h`), 0 for none
    float fogStart;    // 0 = no fog for this batch; see renderer.h's View
    float fogEnd;
    // A CASTER does not RECEIVE. The engine's shadow only ever darkens the
    // ground, and a map that also shadowed the caster would put its own
    // silhouette across a 1999 character's chest.
    //
    // **It sits HERE, before the vec3, and that is not a style choice.** A
    // `vec3` in a push-constant block is 16-BYTE ALIGNED, so `fogColour`
    // lands at offset 80 whatever precedes it; the C++ struct had it packed at
    // 76 and the two had disagreed since the fog landed. Nobody saw it because
    // the shipped fog colour is BLACK (ASSETS, "The fog"), so the four bytes
    // that arrived in the wrong place were zeros either way. Adding an int
    // after the vec3 is what made it visible: `caster` read the padding and
    // every character shadowed itself.
    int   caster;
    vec3  fogColour;
    // Row 7: this batch is LIT per pixel, so its baked vertex colour is not
    // its shading and the light starts from BLACK.
    //
    // **AFTER the vec3, and that is forced.** `fogColour` is 16-byte aligned,
    // so it sits at offset 80 and anything put in front of it pushes it to
    // 96 - which silently broke this the first time: `lit` was never 1 and the
    // whole enhancement drew nothing. The scalars before it fill 64..80
    // exactly; a new one goes here.
    int   lit;          // 92
    // The BACK-FACE CULL (`geom3do.h` kTwoSided): 0 draws both sides, +1
    // discards where !gl_FrontFacing, -1 where gl_FrontFacing - the engine's
    // software test in `Render_SubmitMesh`, which the device never sees.
    int   cull;         // 96
    int   posed;        // 100, a body posed in `scene.vert` (set 2)
#ifdef OMK_MOTION
    // THE UPSCALER's two (`d3d12render.cpp`, built with -DOMK_MOTION for the
    // Direct3D 12 backend only - Vulkan compiles this block without them, so
    // its push block stays 104 bytes): the sub-pixel JITTER in clip units,
    // added after the transform, and the PREVIOUS frame's view-projection,
    // for the motion vectors. After `posed`, so nothing above moves.
    vec2  jitter;      // 104
    mat4  prevMvp;     // 112
#endif
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;

// ---- the MAPPED shadow, `todo/enhancements.md` 6 - an ENHANCEMENT ----------
//
// Set 1 is per FRAME, not per texture: the light's transform and the depth
// map it filled. `strength` of 0 means the frame has no shadow light, which is
// the default and every classic and fitted frame.
// Up to THREE maps (layers), one per light reaching the casters, each with
// its own strength - 0 for a layer with no light this frame.
layout(set = 1, binding = 0) uniform Shadow {
    mat4  lightMvp[3];
    vec4  strength;    // per layer; 0 = no shadow from that layer
    vec4  lightPos[3]; // xyz lamp position (w 1) or travel direction (w 0)
    vec4  params[3];   // near, far, penumbra scale, 1 = perspective
    float texel;       // 1 / the map's side, for the PCF tap spacing
    float bias;        // depth slop, in the map's own 0..1 units
    float shimmer;     // THE SHIMMER's clock - see the note below
    float pad;
} sh;
layout(set = 1, binding = 1) uniform sampler2DArray shadowMap;

// ---- PER-PIXEL LIGHTING, `todo/enhancements.md` row 7 ----------------------
//
// `sub_493E40` transcribed, evaluated per fragment. Not a new law: the same
// squared-radius reach test, the same LINEAR falloff between the two radii,
// the same `k = intensity * 256 * fall`, the same `-(N.L)` and the same
// `(t * c) >> 8` ramp `o3de/vertexlight.cpp` runs per vertex.
struct GpuLight {
    vec4 posA;      // xyz position, w outer radius
    vec4 dirB;      // xyz direction, w inner radius
    vec4 colourI;   // rgb colour 0..1, a intensity
};
layout(set = 1, binding = 2) uniform Lights {
    GpuLight l[8];
    int   count;
    float base;        // View::litBase - the scene's +416, what a lit batch starts from
    int   grey;        // View::grey - the greyscale bank, ops 150/151
    int   pad2;
    vec4  eye;         // the camera, for vehicle shine's view vector
#ifdef OMK_SOFT
    vec4  soft;        // SOFT PARTICLES: x A, y B (view z = B / (depth - A)), z 1 / the fade's range
#endif
} lg;

#ifdef OMK_SOFT
// SOFT PARTICLES (`todo/enhancements.md` 23), an ENHANCEMENT: the Direct3D 12
// backend's -DOMK_SOFT build, drawn for the effect sprites alone (`kDrawSoft`,
// additive or multiply): the OPAQUE depth, read through a read-only depth view
// of the very buffer the sprite is depth-tested against. Vulkan never defines it.
// (a descriptor SET of its own: Direct3D 12 spaces it as 6, Vulkan, with fewer sets
// to spare, as 3 - `-DOMK_SOFT_SET=3`)
#ifndef OMK_SOFT_SET
#define OMK_SOFT_SET 6
#endif
layout(set = OMK_SOFT_SET, binding = 0) uniform sampler2D sceneDepth;
#endif

#ifdef OMK_RT
// The ray-traced frame's constants (`d3d12render.cpp` `RtFrame`) and the rays'
// tally - one atomic per 8x8 pixels, scaled, for the log - in the -DOMK_RT_COUNT
// build alone, drawn one frame in 60: a shader that writes a UAV loses the
// EARLY depth test, and every hidden body fragment would cast its rays.
layout(set = OMK_RT_SET, binding = 2) uniform RtFrame {
    vec4 sky;      // rgb what a ray leaving the set sees; a the indirect term's strength (0 off)
    vec4 eye;      // xyz the eye; w unused
    vec4 params;   // x indirect reach; y, z, w unused
    vec4 refl;     // RAY-TRACED REFLECTIONS: x strength (0 off), y F0, z reach, w a puddle's roughness
    vec4 refl2;    // x the share of the ground wet, y a bright texel's boost, z a damp patch's
                   // roughness, w the strength a damp patch keeps
    vec4 skyUp;    // rgb the sky's mean colour (what an upward ray leaving the set sees), w 1 if any
} rtf;
#ifdef OMK_RT_COUNT
layout(std430, set = OMK_RT_SET, binding = 4) buffer RtCount { uint n[]; } rtCount;
#endif
#endif

vec3 litColour(vec3 n, vec3 w) {
    vec3 c = vec3(0.0);
    for (int i = 0; i < lg.count; ++i) {
        vec3  p  = lg.l[i].posA.xyz;
        float rA = lg.l[i].posA.w, rB = lg.l[i].dirB.w;
        vec3  d  = w - p;
        float d2 = dot(d, d);
        if (d2 > rA * rA) continue;      // the engine's own reach test
        if (rA <= rB) continue;          // a degenerate pair lights nothing
        float fall = 1.0 - (sqrt(d2) - rB) / (rA - rB);
        fall = min(fall, 1.0);
        float k = lg.l[i].colourI.a * 256.0 * fall;
        if (k <= 0.0) continue;
        // `-(N.L)`, with the falloff folded into L exactly as the engine
        // folds `v46` in before the dot.
        float t = -dot(n, lg.l[i].dirB.xyz * k);
        if (t <= 0.0) continue;
        // ...and the ramp, `(t * c) >> 8` per channel, saturating.
        c += min(t * lg.l[i].colourI.rgb / 256.0, vec3(1.0));
    }
    return min(c, vec3(1.0));
}

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vCol;
layout(location = 2) in float vDepth;
layout(location = 3) in vec3 vWorld;
layout(location = 4) in vec3 vNrm;
layout(location = 5) in float vPhase;
layout(location = 0) out vec4 outColour;
#ifdef OMK_MOTION
// THE MOTION VECTOR (`scene.vert`), into the scene pass's second target:
// PREVIOUS minus CURRENT position in texture-UV units (y down, as the rows
// run), which is the direction both upscalers read - `d3d12render.cpp` gives
// FSR the render size as its scale and DLSS 1.
layout(location = 6) in vec4 vCurClip;
layout(location = 7) in vec4 vPrevClip;
layout(location = 1) out vec2 outMotion;
#endif

// -> 1 fully lit, 0 fully shadowed. Outside the map's slab a fragment is LIT
// by definition, which is what keeps a characters-only shadow from darkening
// the far end of a street: the slab is fitted to the casters and nothing else.
// SOFT SHADOWS (PCSS): a blocker search finds how far the occluder is from
// the receiver, the penumbra widens with that gap - sharp at the feet, soft
// and fading further out, as under a real lamp of some size - and a rotated
// Poisson disc filters it without banding. A receiver facing away from the
// light is not lit by it, so it takes no shadow from it either.
const vec2 kDisc[16] = vec2[16](
    vec2(-0.942, -0.399), vec2( 0.946, -0.769), vec2(-0.094, -0.929), vec2( 0.345,  0.294),
    vec2(-0.916,  0.458), vec2(-0.815, -0.879), vec2(-0.383,  0.277), vec2( 0.975,  0.756),
    vec2( 0.443, -0.975), vec2( 0.537, -0.474), vec2(-0.265, -0.419), vec2( 0.792,  0.191),
    vec2(-0.242,  0.997), vec2(-0.814,  0.914), vec2( 0.200,  0.786), vec2( 0.144, -0.141));

float linDepth(int L, float z) {
    vec4 q = sh.params[L];
    return q.w > 0.5 ? q.x * q.y / (q.y - z * (q.y - q.x)) : z * q.y;
}

float layerLit(int L, float strength) {
    vec4 lp = sh.lightMvp[L] * vec4(vWorld, 1.0);
    if (lp.w <= 0.0) return 1.0;
    vec3 p = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 1.0;
    if (p.z < 0.0 || p.z > 1.0) return 1.0;
    bool persp = sh.params[L].w > 0.5;
    vec3 toL = persp ? sh.lightPos[L].xyz - vWorld : -sh.lightPos[L].xyz;
    float facing = 1.0, slope = 0.0;
    float nl = length(vNrm);
    if (nl > 1e-3) {
        float ndl = dot(vNrm / nl, normalize(toL));
        facing = smoothstep(-0.05, 0.25, ndl);
        slope = 1.0 - clamp(ndl, 0.0, 1.0);
    }
    if (facing <= 0.0) return 1.0;
    float zR = linDepth(L, p.z);
    float bias = sh.bias * (1.0 + 3.0 * slope);
    float a = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    mat2 rot = mat2(cos(a), sin(a), -sin(a), cos(a));
    // 1. the blockers' mean depth
    float search = persp ? clamp(sh.params[L].z * (zR - sh.params[L].x) / (sh.params[L].x * zR), 3.0 * sh.texel, 0.04)
                         : 0.03;
    float zB = 0.0, nB = 0.0;
    for (int k = 0; k < 16; ++k) {
        float d = linDepth(L, texture(shadowMap, vec3(uv + rot * kDisc[k] * search, float(L))).r);
        if (d < zR - bias) { zB += d; nB += 1.0; }
    }
    if (nB < 1.0) return 1.0;
    zB /= nB;
    float gap = zR - zB;
    // 2. the penumbra, by similar triangles from the lamp's size
    float pen = persp ? sh.params[L].z * gap / (zB * zR) : sh.params[L].z * gap;
    pen = clamp(pen, 1.5 * sh.texel, 0.05);
    float lit = 0.0;
    for (int k = 0; k < 16; ++k) {
        float d = linDepth(L, texture(shadowMap, vec3(uv + rot * kDisc[k] * pen, float(L))).r);
        lit += (zR - bias <= d) ? 1.0 : 0.0;
    }
    lit /= 16.0;
    // 3. darkest at contact, thinning as the occluder gets further away
    float fade = mix(1.0, 0.3, smoothstep(0.0, 260.0, gap));
    float edge = smoothstep(0.0, 0.05, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
    return mix(1.0 - strength * fade * facing * edge, 1.0, lit);
}

// -> 1 fully lit, 0 fully shadowed: the layers' attenuations multiplied, so
// two lights casting over the same ground darken it more than either alone.
float litness() {
    if (pc.caster != 0) return 1.0;
    float l = 1.0;
    for (int L = 0; L < 3; ++L)
        if (sh.strength[L] > 0.0) l *= layerLit(L, sh.strength[L]);
    return l;
}

#ifdef OMK_RT
// the mean over the pixel's 2x2 quad, read from the fine derivatives: four
// pixels' rays for the price of one - the cheap spatial filter that keeps a
// few rays a pixel from reading as grain
vec4 rtQuad(vec4 v) {
    float sx = (int(gl_FragCoord.x) & 1) == 0 ? 1.0 : -1.0;
    float sy = (int(gl_FragCoord.y) & 1) == 0 ? 1.0 : -1.0;
    vec4 vx = v + dFdxFine(v) * sx;
    vec4 vy = v + dFdyFine(v) * sy;
    vec4 vxy = vx + dFdyFine(vx) * sy;
    return 0.25 * (v + vx + vy + vxy);
}

// The character's smooth normal for its rays: the vertex normal, turned to
// face the eye by the fragment's own geometric normal - never the geometric
// normal itself, which is a low-poly face's flat triangle.
vec3 rtCharNormal(vec3 nIn) {
    vec3 ng = cross(dFdx(vWorld), dFdy(vWorld));
    ng = dot(ng, ng) > 1e-12 ? normalize(ng) : vec3(0.0, -1.0, 0.0);
    if (dot(ng, rtf.eye.xyz - vWorld) < 0.0) ng = -ng;
    vec3 N = dot(nIn, nIn) > 1e-6 ? normalize(nIn) : ng;
    if (dot(N, ng) < -0.25) N = -N;
    return N;
}

// A CHARACTER LIT BY ITS SURROUNDINGS. `shade` is what the game gave the
// fragment (its lamps, from the scene's grey) and it KEEPS it: a character
// receives no ray-traced SHADOW - no lamp shadow ray, no near occlusion, and
// `ssao.frag` leaves its pixels out - because on a body of a few big
// triangles every such term read as a weird dark patch on the face. Its
// light is the game's own law, what `raytracing = off` gives it. To it the
// INDIRECT light alone is ADDED: cosine-weighted rays into the set, each
// hit giving what the eye sees there - the baked vertex colour times its
// texel - and a miss the sky; their mean. Light only ever added, never taken.
vec3 rtCharacter(vec3 shade, vec3 nIn) {
    vec3 N = rtCharNormal(nIn);
    const int K = 6;
    vec3 sum = vec3(0.0);
    if (rtf.sky.a > 0.0) {
        float a = 6.2831853 * rtNoise(gl_FragCoord.xy);
        vec3 o = vWorld + N * 2.0;
        for (int i = 0; i < K; ++i) {
            vec3 d = rtCosine(N, i, K, a, rtNoise(gl_FragCoord.yx + vec2(3.0, 11.0)));
            vec3 rad = rtf.sky.rgb;
            if (rtTrace(o, d, rtf.params.x, kRtSet, false)) {
                vec2 uv; vec3 col; uint tex;
                rtSurface(rtI, rtP, rtG, rtB, uv, col, tex);
                rad = mix(col * rtTexel(tex, uv).rgb, rtf.sky.rgb, smoothstep(0.5, 1.0, rtT / rtf.params.x));
            }
            sum += rad;
        }
    }
    // The quad mean counts only the lanes that really traced: a HELPER lane (a
    // pixel of the quad outside this triangle, run only for the derivatives)
    // may run no ray query and answers nothing, and averaged in it drew a
    // dark line along every triangle edge of a face. The reflections do the same.
    float w = gl_HelperInvocation ? 0.0 : 1.0;
    vec3 q = rtQuad(vec4(sum / float(K) * w, 0.0)).rgb / max(rtQuad(vec4(w)).x, 1e-3);
#ifdef OMK_RT_COUNT
    if (((int(gl_FragCoord.x) | int(gl_FragCoord.y)) & 7) == 0) {
        if (rtf.sky.a > 0.0) atomicAdd(rtCount.n[1], uint(K) * 64u);
    }
#endif
    return shade + rtf.sky.a * q;
}

// RAY-TRACED REFLECTIONS (`todo/enhancements.md` 26), an ENHANCEMENT: a
// GROUND draw (`kDrawFloor` - the set's road, pavement and floor meshes,
// `o3de/floormesh.h`) is wet in patches and mirrors what is above it.
//   * THE WET: value noise over the world's XZ in three octaves (patches of
//     ~18 m, puddles of a few), fixed to the ground - never the frame; a
//     share `refl2.x` of it is puddled (near-mirror, the albedo darkened as
//     wet stone darkens), the rest damp (rough, weak);
//   * ONE RAY a pixel about the mirror direction, turned within a cone as wide
//     as the roughness by a stable per-pixel angle (`rtNoise`), against the
//     set AND the bodies (the passers-by, the traffic); a hit gives what the
//     eye would see there - the baked colour times the texel, a body's texel
//     under a middling light - and a bright texel (a neon tube, a lamp) is
//     BOOSTED so the post chain's bloom picks its reflection up; a miss, or a
//     hit far off, fades to the sky: the sky plane's mean colour (`View::skyMean`) overhead, the fog colour toward the horizon;
//   * Schlick's Fresnel - weak looking down, strong at grazing - and the 2x2
//     quad mean (`rtQuad`) over the ground pixels as the spatial filter.
// Only an UPWARD face reflects (a kerb's side in a ground mesh does not).
uint rrHash(ivec2 p) {
    uint h = uint(p.x) * 1597334677u ^ uint(p.y) * 3812015801u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    return h ^ (h >> 13u);
}
float rrValue(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    vec2 u = f * f * (3.0 - 2.0 * f);
    ivec2 c = ivec2(i);
    float a = float(rrHash(c) & 0xFFFFu), b = float(rrHash(c + ivec2(1, 0)) & 0xFFFFu);
    float d = float(rrHash(c + ivec2(0, 1)) & 0xFFFFu), e = float(rrHash(c + ivec2(1, 1)) & 0xFFFFu);
    return mix(mix(a, b, u.x), mix(d, e, u.x), u.y) / 65535.0;
}
// -> 0 damp .. 1 a puddle
float rrWet(vec2 xz) {
    float n = 0.55 * rrValue(xz / 700.0) + 0.3 * rrValue(xz / 230.0 + vec2(17.3, 5.1))
            + 0.15 * rrValue(xz / 75.0 + vec2(3.7, 41.9));
    float t = 1.0 - rtf.refl2.x;   // the noise's level the wet share starts above
    return smoothstep(t - 0.035, t + 0.035, n);
}
uint rrRays = 0u;
vec3 rtReflect(vec3 c) {
    vec3 ng = cross(dFdx(vWorld), dFdy(vWorld));
    ng = dot(ng, ng) > 1e-12 ? normalize(ng) : vec3(0.0, 1.0, 0.0);
    if (ng.y > 0.0) ng = -ng;                    // up is -Y
    vec3 V = normalize(rtf.eye.xyz - vWorld);
    float use = (ng.y < -0.9 && dot(V, ng) > 0.0) ? 1.0 : 0.0;
    float wet = rrWet(vWorld.xz);
    float rough = mix(rtf.refl2.z, rtf.refl.w, wet);
    vec3 R = reflect(-V, ng);
    vec3 T = normalize(cross(abs(R.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), R));
    vec3 B = cross(R, T);
    float a = 6.2831853 * rtNoise(gl_FragCoord.xy + vec2(29.0, 5.0));
    float r = rough * sqrt(rtNoise(gl_FragCoord.yx + vec2(17.0, 41.0)));
    vec3 d = normalize(R + (T * cos(a) + B * sin(a)) * r);
    if (dot(d, ng) < 0.02) d = normalize(d - ng * (dot(d, ng) - 0.02));
    // a miss: the sky plane overhead (its mean colour), the fog toward the horizon
    vec3 rad = rtf.skyUp.w > 0.0 ? mix(rtf.sky.rgb, rtf.skyUp.rgb, smoothstep(0.03, 0.35, -d.y)) : rtf.sky.rgb;
    if (use > 0.0) {
        ++rrRays;
        float reach = rtf.refl.z;
        if (rtTrace(vWorld + ng * 1.5, d, reach, kRtSet | kRtBody, false)) {
            vec2 uv; vec3 col; uint tex;
            rtSurface(rtI, rtP, rtG, rtB, uv, col, tex);
            vec3 tx = rtTexel(tex, uv).rgb;
            vec3 h = rtInst.i[rtI].z == kRtBody ? tx * clamp(col, 0.0, 1.0) * 0.4 : col * tx;
            // a bright texel BOOSTED in the puddles alone, and a damp patch's
            // scattered ray clamped - one lamp texel in a rough cone is a
            // firefly, not a reflection
            float m = max(h.r, max(h.g, h.b));
            h *= 1.0 + rtf.refl2.y * wet * smoothstep(0.55, 0.95, m);
            h = min(h, vec3(mix(0.35, 4.0, wet)));
            rad = mix(h, rtf.sky.rgb, smoothstep(0.55, 1.0, rtT / reach));
        }
    }
    float ndv = clamp(dot(ng, V), 0.0, 1.0);
    float F = rtf.refl.y + (1.0 - rtf.refl.y) * pow(1.0 - ndv, 5.0);
    float k = rtf.refl.x * mix(rtf.refl2.w, 1.0, wet) * F;
    // a HELPER lane (the quad's pixel past the triangle's edge, run for the
    // derivatives alone) traces nothing useful - its ray query may not run -
    // so it takes no part in the mean, or every edge draws a seam of sky
    float w = gl_HelperInvocation ? 0.0 : use;
    vec4 q = rtQuad(vec4(rad * k * w, w));
    vec3 add = q.a > 0.0 ? q.rgb / q.a : vec3(0.0);
#ifdef OMK_RT_COUNT
    if (((int(gl_FragCoord.x) | int(gl_FragCoord.y)) & 7) == 0 && rrRays > 0u) atomicAdd(rtCount.n[2], rrRays * 64u);
#endif
    return use > 0.0 ? c * (1.0 - 0.3 * wet * rtf.refl.x) * (1.0 - 0.6 * k) + add : c;
}
#endif

void main() {
    if ((pc.cull > 0 && !gl_FrontFacing) || (pc.cull < 0 && gl_FrontFacing)) discard;
    // Alpha is the KEY, written at upload: 0 on a black texel, 1 elsewhere.
    // Under the nearest sampler (the original's) this is exactly the old
    // `rgb == 0` test on the same texel. Under the bilinear ENHANCEMENT the
    // key texels are (0,0,0,0), so the sample is premultiplied: a fragment
    // more than half key is discarded and the rest is un-premultiplied,
    // which is what keeps the cutout edge free of a dark fringe.
    //
    // THE TEXTURE PACK (`todo/enhancements.md` 25, an ENHANCEMENT): a slot
    // whose page a pack replaced holds an image k times the original's size,
    // and the UVs are still in the ORIGINAL's texel units - so the size they
    // are divided by is the image's over k (k rides in the cutout word's bits
    // 8.., the push block having no room on Direct3D 12). Without a pack the
    // arithmetic is the old one exactly.
    vec2 texSz = vec2(textureSize(tex, 0));
    int packK = pc.cutout >> 8;
    if (packK > 1) texSz /= float(packK);
    vec4 t = texture(tex, vUV / texSz);
    if ((pc.cutout & 1) != 0) {
        if (t.a < 0.5) discard;
        t.rgb /= t.a;
    }
    // A LIT batch starts from black and takes its shading from the lights;
    // every other one keeps the colour baked into its vertices.
    // 1 from black, 2 added to the baked colour - see `renderer.h`'s Draw.
    // THE SHIMMER - mesh flag 0x8000000, and it is the GAME's, not an
    // enhancement. `sub_4947F0` adds one entry of a 32-byte signed table into
    // all three colour bytes; 233 set meshes carry the flag, all of them
    // distant scenery, and neighbouring vertices sit 1/16 of a cycle apart so
    // the wave travels. The table is duplicated here from `o3de/shimmer.h`
    // because a shader cannot include it, and `verify.py: shimmer table`
    // asserts the two against the executable.
    float wave = 0.0;
    if (vPhase >= 0.0) {
        int wi = ((int(sh.shimmer) >> 2) + int(vPhase)) & 31;
        float tbl[32] = float[32](
              0.0,  10.0,  13.0,  18.0,  28.0,  32.0,  48.0,  56.0,
             64.0,  56.0,  48.0,  32.0,  28.0,  18.0,  13.0,  10.0,
              0.0, -10.0, -13.0, -18.0, -28.0, -32.0, -48.0, -56.0,
            -64.0, -56.0, -48.0, -32.0, -28.0, -18.0, -13.0, -10.0);
        wave = tbl[wi] / 255.0;
    }
#ifdef OMK_RT
#endif
    vec3 shade = (pc.lit & 3) == 1 ? min(vec3(lg.base) + litColour(normalize(vNrm), vWorld), vec3(1.0))
               : (pc.lit & 3) == 2 ? min(vCol + litColour(normalize(vNrm), vWorld), vec3(1.0))
                             : vCol + vec3(wave);
#ifdef OMK_RT
    // a CHARACTER (a caster - the bodies) takes the traced terms; the set
    // keeps its baked light untouched, the artists' own
    if (pc.caster != 0) shade = rtCharacter(shade, vNrm);
#endif
    // THE GREYSCALE BANK (ops 150/151, `o3de/greybank.h`): `sub_42FF80`
    // writes each vertex colour as its luma, `(299 R + 587 G + 114 B) / 1000`
    // on the BYTES, truncated - here on the colour the batch ends with, after
    // its shimmer and lights. The texture came greyed (its palette); the fog
    // colour too. `+ 0.5` keeps the floor exact under an approximate divide.
    if (lg.grey != 0) {
        float y = floor((dot(floor(clamp(shade, 0.0, 1.0) * 255.0 + 0.5),
                             vec3(299.0, 587.0, 114.0)) + 0.5) / 1000.0);
        shade = vec3(y / 255.0);
    }
    vec3 c = clamp(t.rgb * shade * litness(), 0.0, 1.0);
#ifdef OMK_RT
    if ((pc.lit & 16) != 0 && rtf.refl.x > 0.0) c = clamp(rtReflect(c), 0.0, 1.0);
#endif
    // VEHICLE SHINE (`vehicleshine`, `todo/enhancements.md` 20), an ENHANCEMENT:
    // the paint gets what the original painted into the texture and never
    // lit - a Blinn-Phong highlight from each lamp in reach, and a sky
    // reflection (a gradient from the scene's own ambient and fog colour,
    // darker below the horizon; the game's Y points DOWN) weighted by Fresnel.
    if ((pc.lit & 4) != 0) {
        vec3 N = normalize(vNrm);
        vec3 V = normalize(lg.eye.xyz - vWorld);
        if (dot(N, V) < 0.0) N = -N;
        float ndv = clamp(dot(N, V), 0.0, 1.0);
        float fres = 0.22 + 0.78 * pow(1.0 - ndv, 4.0);
        vec3 R = reflect(-V, N);
        vec3 amb = vec3(lg.base) * 1.4 + pc.fogColour + vec3(0.12);
        vec3 env = mix(amb * 0.35, amb * 1.8, smoothstep(-0.25, 0.55, -R.y));
        vec3 spec = vec3(0.0);
        for (int i = 0; i < lg.count; ++i) {
            vec3  p  = lg.l[i].posA.xyz;
            float rA = lg.l[i].posA.w, rB = lg.l[i].dirB.w;
            vec3  d  = p - vWorld;
            float d2 = dot(d, d);
            if (d2 > rA * rA || rA <= rB) continue;
            float fall = min(1.0 - (sqrt(d2) - rB) / (rA - rB), 1.0);
            vec3 Hh = normalize(normalize(d) + V);
            float s = pow(max(dot(N, Hh), 0.0), 40.0) * smoothstep(0.0, 0.2, dot(N, normalize(d)));
            spec += lg.l[i].colourI.rgb * min(lg.l[i].colourI.a, 1.5) * fall * s;
        }
        float paint = mix(0.55, 1.0, dot(t.rgb, vec3(0.299, 0.587, 0.114)));
        c = clamp(c + (env * fres * 1.0 + spec * 1.2) * paint, 0.0, 1.0);
    }
    // THE FOG, and it is raster.cpp's line transcribed. Linear -
    // `FOGTABLEMODE` 3 at density 1.0, the only mode the engine sets - over
    // the range the clip distance sizes. The CPU side has already applied the
    // bucket key's two exclusions and any doubling, so a zero end means this
    // batch is not fogged.
    if (pc.fogEnd > pc.fogStart && vDepth > pc.fogStart) {
        float f = clamp((pc.fogEnd - vDepth) / (pc.fogEnd - pc.fogStart), 0.0, 1.0);
        c = mix(pc.fogColour, c, f);
    }
#ifdef OMK_SOFT
    // the sprite FADES as it nears what is behind it: by the view-depth gap
    // between this fragment and the opaque surface at its pixel, over the
    // fade's range - so a puff of smoke meets the ground in a soft edge
    // instead of a hard line. Both blends take it on the colour: an additive
    // sprite adds less, a multiply one (`dst * (1 - c)`) darkens less.
    {
        float zs = lg.soft.y / (texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r - lg.soft.x);
        float zf = lg.soft.y / (gl_FragCoord.z - lg.soft.x);
        c *= clamp((zs - zf) * lg.soft.z, 0.0, 1.0);
    }
#endif
    outColour = vec4(c, 1.0);
#ifdef OMK_MOTION
    // a corner behind the eye a frame ago has no previous screen position:
    // no motion, rather than a vector to infinity
    outMotion = vPrevClip.w > 1e-3
              ? (vPrevClip.xy / vPrevClip.w - vCurClip.xy / vCurClip.w) * 0.5
              : vec2(0.0);
#endif
}
