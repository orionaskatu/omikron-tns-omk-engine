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
#ifndef OMK_DX
    // THE TEXTURE PACK (`o3de/texpack.h`): the bound texture is a replacement
    // this many times the original's size, while the UVs are still in the
    // ORIGINAL's texels. 0 on every shipped texture, which then divides by
    // its own size exactly as before.
    int   texScaled;    // 104
    float texScaleU;    // 108
    float texScaleV;    // 112
    // THE BLOOM (`post.frag`): 0 off - alpha 1, as always; 1 on - the alpha
    // is this texel's LIGHT, the texture pack's light map (binding 1)
    int   bloomMode;    // 116
    // THE WATER REFLECTION (`todo/remaster.md` A3): 1 this draw is WATER and
    // adds the reflected scene (binding 2) at its own screen position, times
    // `waterStrength`; 2 this is the REFLECTION pass, and `waterStrength` is
    // the water's height - what lies below it is discarded; 3 a REFLECTING
    // FLOOR's draw, as 1 with its relief (`o3de/floorreflect.h`); 0 otherwise.
    // Those are the LOW FOUR BITS: above them a WET floor's darkening, 0..255
    // (`o3de/wetfloor.h`), and above that the RAIN's, 0..255 (`View::rain`, its
    // ripples) - `waterMode`, `wetFilm` and `rainNow` below
    int   water;          // 120
    float waterStrength;  // 124
#endif
#ifdef OMK_MOTION
    // THE UPSCALER's two (`d3d12render.cpp`, built with -DOMK_MOTION for the
    // Direct3D 12 backend only - Vulkan compiles this block without them, so
    // its push block stays 104 bytes): the sub-pixel JITTER in clip units,
    // added after the transform, and the PREVIOUS frame's view-projection,
    // for the motion vectors. After `posed`, so nothing above moves.
#ifdef OMK_DX
    vec2  jitter;      // 104
    mat4  prevMvp;     // 112
#else
    vec2  jitter;      // 128
    mat4  prevMvp;     // 144
#endif
#endif
#ifdef OMK_DX
    // the Direct3D 12 build (-DOMK_DX): no bloom, and 64 root dwords - ONE word for the pack's scale and
    // the water's per-draw bits (the rest of the water is in `lg.wat`)
    uint  texScale;    // 0..7 the pack's scale per axis, minus 1: u in bits 0..3, v in 4..7 (0: none);
                       // 8..19 the water's mode and wet film (`water`'s bits 0..11, raised by 8);
                       // 20, 21 snow lying, a wet body (`water`'s own bits); 22..29 a floor's reflection x255
#endif
} pc;
#ifdef OMK_DX
#define PC_BLOOM 0
// `water` as Vulkan's push block has it: the rain's strength (bits 12..19) is the frame's, in `lg.wat.y`
#define PC_WATER (int(((pc.texScale >> 8) & 4095u) | (uint(lg.wat.y) << 12) | (pc.texScale & 0x300000u)))
// ...and `waterStrength`: the water's (`lg.wat.x`), a floor's share of it, or - in the reflection pass - its height
#define PC_WSTR ((PC_WATER & 15) == 2 ? lg.wat.z :                  lg.wat.x * ((PC_WATER & 15) == 3 ? float((pc.texScale >> 22) & 255u) / 255.0 : 1.0))
#define PC_TEXDIV vec2(float(pc.texScale & 15u) + 1.0, float((pc.texScale >> 4) & 15u) + 1.0)
#else
#define PC_BLOOM pc.bloomMode
#define PC_WSTR pc.waterStrength
#define PC_WATER pc.water
#define PC_TEXDIV (pc.texScaled != 0 ? vec2(pc.texScaleU, pc.texScaleV) : vec2(1.0))
#endif

layout(set = 0, binding = 0) uniform sampler2D tex;
// the texture's LIGHT MAP (`o3de/texpack.h`), black where the pack has none;
// read only with the bloom on
#ifndef OMK_DX
layout(set = 0, binding = 1) uniform sampler2D emis;
// the WATER pass's reflection, the scene through the camera reflected in the
// water plane (black unless a water pass made one); read only by water draws
layout(set = 0, binding = 2) uniform sampler2D refl;
#else
// ...Direct3D 12's is the FRAME's, in the table that holds the shadow map (set 1, binding 3: t3)
layout(set = 1, binding = 3) uniform sampler2D refl;
#endif
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
    GpuLight l[8];     // View::kMaxGpuLights
    int   count;
    float base;        // View::litBase - the scene's +416, what a lit batch starts from
    int   grey;        // View::grey - the greyscale bank, ops 150/151
    int   pad0;        // unused: std140 puts the vec4 below on a 16-byte boundary
    vec4  eye;         // the camera, for the water's Fresnel (xyz)
    vec4  fx;          // x unused; y, z, w: the MIST's strength, ground and height (View::mist...), both
    vec4  dust;        // THE SAND'S DUST (View::dust): strength, the drift's x and z, the distance scale
    vec4  dustColour;  // ...its colour, lit as the scene (rgb)
    vec4  veil;        // THE RAIN'S VEILS (View::veil): strength, the fall's offset, the distance scale;
                       // w the PUDDLES' strength (View::puddles)
    vec4  veilColour;  // ...their colour, lit as the scene (rgb)
#ifdef OMK_DX
    vec4  wat;         // THE WATER's frame constants (Vulkan's push block has them a draw): x the strength, y the
                       // rain's 0..255 (the wet floors' ripples), z the plane's height in the reflection pass
#endif
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


// (the fragment's VIEW depth, the fog's)
layout(location = 2) in float vDepth;

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

// THE RAIN'S RIPPLES on a wet floor (`o3de/wetfloor.h`, the remaster's): the
// floor in cells of 48 units, each with at most one drop - at a hashed place,
// and kept only where its hash is below the rain's strength, so a light shower
// has few - whose ring spreads to its own size (24..48 units) over 64 of the
// shimmer's clock (32 frames; the clock wraps at 256, a whole number of
// rings), ONE soft crest `(1 - x^2)^2` across 6 units, rising in its first
// tenth and fading as it goes. -> xy the rings' slope at `p` (the reflection's
// bend), z their crest (the light). GLES's twin is in
// `glesrender_remaster_shaders.inl` - keep the two in step.
// THE SAND'S DUST (the restored weather's, the remaster's - `View::dust`): a
// fog toward an ochre lit as the scene, thickening with the storm and with
// distance, its density broken into drifting clouds - a 3D value noise moved
// along the wind - so gusts pass. GLES's twin in glesrender_remaster_shaders.inl.
float dustHash(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float dustNoise(vec3 x) {
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(dustHash(i), dustHash(i + vec3(1, 0, 0)), f.x),
                   mix(dustHash(i + vec3(0, 1, 0)), dustHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(dustHash(i + vec3(0, 0, 1)), dustHash(i + vec3(1, 0, 1)), f.x),
                   mix(dustHash(i + vec3(0, 1, 1)), dustHash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// -> how much of the dust's colour a point `w` seen from `e` takes - with
// `kDustNear` of dust in front of everything, so it drifts by the player too
// (the reader: "make the moving fog go closer to the player")
// The clouds are STRETCHED along the wind (x, the arm's: long that way,
// narrow across), sampled near the eye too, and contrasted - gaps and thick
// gusts sweeping past (the reader: "make the fog movements more visible").
const float kDustNear = 300.0;
float dustCloud(vec3 p) {
    float g = dustNoise(p * vec3(0.0012, 0.004, 0.004));
    return g * g * 2.2;                                   // 0..2.2, most of it thin
}
float dustAmount(vec3 e, vec3 w, vec4 dust) {
    vec3 drift = vec3(dust.y, 0.0, dust.z);
    float d = length(w - e);
    vec3 near = e + (w - e) * min(1.0, 250.0 / max(d, 1.0));
    float n = 0.1 + (dustCloud(near + drift) + dustCloud(mix(e, w, 0.5) + drift) + dustCloud(w + drift)) / 3.0 * 1.6;
    return 1.0 - exp(-dust.x * n * (d + kDustNear) / dust.w);
}

// THE RAIN'S VEILS (the remaster's, `View::veil`): far rain reads as sheets,
// not streaks - long bands slanted by the wind and falling, sampled IN THE AIR
// along the ray (at 500, 1200 and 1900 units, those nearer than the surface),
// a light grey lit as the scene, thicker with distance. GLES's twin in
// glesrender_remaster_shaders.inl.
const float kVeilFirst = 500.0;         // the first sample in the air, units from the eye
const float kVeilStep = 700.0;          // ...and between the samples
const float kVeilSlant = 0.577;         // the bands' slant with the wind (30 degrees)
const float kVeilAcross = 0.008;        // the bands' noise across, per unit
const float kVeilAlong = 0.0007;        // ...and along the fall: long sheets
const float kVeilLow = 0.5;             // the noise where a sheet starts...
const float kVeilHigh = 0.85;           // ...and where it is full
const float kVeilMax = 0.85;            // the veils' strength at full, far off
float veilAmount(vec3 e, vec3 w, vec4 veil) {
    float d = length(w - e);
    if (d < kVeilFirst) return 0.0;
    vec3 dir = (w - e) / d;
    float sum = 0.0;
    for (int k = 0; k < 3; ++k) {
        float t = kVeilFirst + float(k) * kVeilStep;
        if (t > d) break;
        vec3 p = e + dir * t;
        vec3 q = vec3((p.x + kVeilSlant * p.y) * kVeilAcross, (p.y - veil.y) * kVeilAlong, p.z * kVeilAcross);
        sum += smoothstep(kVeilLow, kVeilHigh, dustNoise(q));
    }
    return veil.x * (1.0 - exp(-d / veil.z)) * sum / 3.0 * kVeilMax;
}

// THE PUDDLES' MASK: 0 between them, 1 in one - a value noise of the ground's
// x and z (about 2.5 m patches), its edge soft over a fifth of its range.
// GLES's twin in glesrender_remaster_shaders.inl.
const float kPuddleBroad = 0.004;   // the patches' noise, a cycle in 250 units
const float kPuddleFine = 0.011;    // ...and their edges' detail
const float kPuddleFineShare = 0.3; // ...its share of the noise
const float kPuddleDry = 0.52;      // the noise where a puddle starts...
const float kPuddleFull = 0.68;     // ...and where it is full
// what a puddle does, as (between them, in one): the wet film's darkening,
// the reflection's strength, the ripples'; and the Fresnel's floor in one
const vec2 kPuddleFilm = vec2(0.5, 1.8);
const vec2 kPuddleMirror = vec2(0.5, 2.2);
const vec2 kPuddleRipples = vec2(0.6, 1.6);
const float kPuddleFresnel = 0.18;
float puddleMask(vec2 xz) {
    float n = dustNoise(vec3(xz.x * kPuddleBroad, 0.0, xz.y * kPuddleBroad)) * (1.0 - kPuddleFineShare) +
              dustNoise(vec3(xz.x * kPuddleFine, 3.7, xz.y * kPuddleFine)) * kPuddleFineShare;
    return smoothstep(kPuddleDry, kPuddleFull, n);
}

const float kRippleLight = 0.13;   // a ring's crest, added light (GLES: the same)
vec3 rippleHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yxz + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}
vec3 ripples(vec2 p, float clock, float rain) {
    const float cell = 48.0;
    vec2 base = floor(p / cell);
    vec3 g = vec3(0.0);
    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j) {
            vec2 cc = base + vec2(float(i), float(j));
            vec3 h = rippleHash(cc);
            if (h.z > rain) continue;
            float life = fract(clock / 64.0 + h.z * 4.0);
            vec2 d = p - (cc + 0.15 + 0.7 * h.xy) * cell;
            float r = length(d);
            float x = (r - life * (24.0 + 24.0 * fract(h.z * 13.0))) / 3.0;
            if (abs(x) < 1.0) {
                float a = (1.0 - life) * (1.0 - life) * smoothstep(0.0, 0.1, life);
                float u = 1.0 - x * x;
                g.z += u * u * a;
                g.xy += d / max(r, 0.001) * (-4.0 * x * u) * a * 0.5;
            }
        }
    return g;
}

// A WET BODY (`Draw::wetBody`) - GLES's twin in glesrender_remaster_shaders.inl
const float kWetBodyDark = 0.3;         // its darkening at full wet: wet cloth and skin go darker...
const float kWetBodySat = 0.35;         // ...and deeper in colour (the saturation's gain)
const float kWetBodySheen = 0.75;       // its SHEEN, the lit street mirrored by the water, at full wet
const float kWetBodyRim = 5.0;          // ...toward the grazing angles (the Fresnel's power)...
const float kWetBodyRimFilm = 0.25;     // ...faint where no bead sits: an even rim is a plastic film
const float kWetBodySky = 1.0;          // ...and where a bead faces up, mirroring the sky's glow
const float kWetBodySkyPower = 16.0;    // ...in a tight highlight (the power on the reflection's lift)
const float kWetBodyBeadScale = 48.0;   // the BEADS of water, a noise on the body's texture (it moves
const float kWetBodyBeadLo = 0.5;       // with the body, where the world's would slide over it)...
const float kWetBodyBeadHi = 0.8;       // ...thresholded to patches between these

// THE TRICKLES on a WET WALL (`o3de/wetfloor.h`, the reader's "wet walls and
// characters", 2026-10-28): thin lines of water down some columns of the
// wall - a bead running down each, a glint, its wet trail above it darker
// (-> trail, bead) - on the ripples'
// clock. GLES's twin in glesrender_remaster_shaders.inl.
const float kWetWallFilm = 0.6;       // a wet wall's film against a floor's: the water runs off
const float kTrickleAcross0 = 0.83;   // a trickle's columns run along this level direction...
const float kTrickleAcross1 = 0.56;   // ...(any wall but one rare heading)
const float kTrickleSpan = 5.0;       // ...one column every this many units
const float kTrickleShare = 0.45;     // ...this share of them running at the full shower
const float kTrickleWidth = 0.12;     // ...a line's half width, of its column
const float kTrickleLength = 40.0;    // ...a bead and its wet trail, units of fall
const float kTrickleRate = 0.0625;    // ...cycles a clock step: 16 in the clock's 256, so it wraps whole
const float kTrickleDark = 0.35;        // ...the wet trail above a bead, darker by this
const float kTrickleRuns = 0.5;       // ...a cycle's run there or not, this share running
const float kTrickleMeander = 0.04;   // ...wandering this much of its column either way
const float kTrickleWave = 0.07;      // ...over this many radians a unit of fall
const float kTrickleFar = 900.0;      // ...gone this far from the eye, finer than a pixel
const float kTrickleLight = 0.22;      // ...a bead's light, added
vec2 trickles(vec3 w, float clock, float rain) {
    float u = (w.x * kTrickleAcross0 + w.z * kTrickleAcross1) / kTrickleSpan;
    float col = floor(u);
    vec3 h = rippleHash(vec2(col, 17.0));
    if (h.z > kTrickleShare * rain) return vec2(0.0);
    float speed = kTrickleRate * (1.0 + floor(h.y * 2.0));
    float v = w.y / kTrickleLength - clock * speed + h.x * 7.0;
    // ...a run now and then down the column, not every cycle
    if (rippleHash(vec2(col, floor(v))).x > kTrickleRuns) return vec2(0.0);
    float f = fract(v);
    // ...meandering a little as it runs
    float centre = 0.3 + 0.4 * h.x + kTrickleMeander * sin(w.y * kTrickleWave + h.y * 6.28);
    float line = 1.0 - smoothstep(kTrickleWidth * 0.5, kTrickleWidth, abs(fract(u) - centre));
    float bead = smoothstep(0.82, 0.93, f) * (1.0 - smoothstep(0.93, 0.97, f));
    float trail = smoothstep(0.1, 0.93, f) * (1.0 - smoothstep(0.93, 0.97, f));
    return line * vec2(trail, bead);
}

// THE CROWD'S COLOUR VARIATION (`pedvariety`, `todo/enhancements.md` 29, `o3de/pedtint.h` - the
// same math in C++, and `verify.py: crowd variety` compares the constants): a walker's own hue
// turn, saturation and brightness on the texel, in `pc.lit` bits 6..21 (bit 5 says it is there).
// Skin (a window of hue and saturation) and near-white are kept; so, by what they are, are
// the near-blacks. The constants carry `pedtint.h`'s names and values.
const float kPedHueCentre = 0.04;
const float kPedHueFull = 0.075;
const float kPedHueNone = 0.115;
const float kPedSatLow0 = 0.05;
const float kPedSatLow1 = 0.12;
const float kPedSatHigh0 = 0.65;
const float kPedSatHigh1 = 0.85;
const float kPedDarkV0 = 0.30;
const float kPedDarkV1 = 0.55;
const float kPedDarkBy = 0.30;
const float kPedWhiteV0 = 0.65;
const float kPedWhiteV1 = 0.85;
const float kPedWhiteS0 = 0.08;
const float kPedWhiteS1 = 0.2;
const float kPedSatBase = 0.75;
const float kPedSatStep = 0.045;
const float kPedValBase = 0.75;
const float kPedValStep = 0.04;
const float kPedChromaCap0 = 0.35;
const float kPedChromaCap1 = 0.90;
const float kPedChromaCapBy = 0.45;
const float kPedSpreadMax = 0.27;
const float kPedPhaseStep = 0.125;
const float kPedSkinBase = 0.78;
const float kPedSkinStep = 0.11;
const float kPedDyeMax = 0.45;
const float kPedDyeFloor = 0.18;
const float kPedDyeLift = 1.2;
const float kPedSkinNudge = 0.012;
const float kPedDyeS0 = 0.08;
const float kPedDyeS1 = 0.35;
const float kPedDyeV0 = 0.06;
const float kPedDyeV1 = 0.16;
const float kPedDyeLuma = 3.0;
const float kPedScaleMin = 0.93;
const float kPedScaleMax = 1.07;
vec3 pedHueRgb(float h) {
    float x = (h - floor(h)) * 6.0;
    return clamp(vec3(abs(x - 3.0) - 1.0, 2.0 - abs(x - 2.0), 2.0 - abs(x - 4.0)), 0.0, 1.0);
}
vec3 pedTint(vec3 c, int word) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = d / max(mx, 1e-4);
    float h = 0.0;
    if (d >= 1e-4) {
        if (mx == c.r) { float x = (c.g - c.b) / d; h = (x - 6.0 * floor(x / 6.0)) / 6.0; }
        else if (mx == c.g) h = ((c.b - c.r) / d + 2.0) / 6.0;
        else h = ((c.r - c.g) / d + 4.0) / 6.0;
    }
    float hx = h - kPedHueCentre + 0.5;
    float dh = abs((hx - floor(hx)) - 0.5);
    float wh = 1.0 - smoothstep(kPedHueFull, kPedHueNone, dh);
    float dark = 1.0 - smoothstep(kPedDarkV0, kPedDarkV1, mx);
    float ws = smoothstep(kPedSatLow0, kPedSatLow1, s) *
               (1.0 - smoothstep(kPedSatHigh0 + kPedDarkBy * dark, kPedSatHigh1 + kPedDarkBy * dark, s));
    float skin = wh * ws;
    float white = smoothstep(kPedWhiteV0, kPedWhiteV1, mx) * (1.0 - smoothstep(kPedWhiteS0, kPedWhiteS1, s));
    float prot = max(skin, white);
    // the complexion: what is kept as skin has its value scaled (code 2 is the shipped skin)
    float tone = kPedSkinBase + float((word >> 29) & 3) * kPedSkinStep;
    float toneMix = skin * (1.0 - white);
    float toneMul = 1.0 + (tone - 1.0) * toneMix;
    // ...and leans the skin's hue a little (code 2: none)
    float nudge = float(((word >> 29) & 3) - 2) * kPedSkinNudge * 6.28318530718 * toneMix;
    float ncs = cos(nudge), nsn = sin(nudge);
    float nk = (1.0 - ncs) / 3.0, nq = 0.57735026919 * nsn;
    vec3 kept = clamp(vec3((ncs + nk) * c.r + (nk - nq) * c.g + (nk + nq) * c.b,
                           (nk + nq) * c.r + (ncs + nk) * c.g + (nk - nq) * c.b,
                           (nk - nq) * c.r + (nk + nq) * c.g + (ncs + nk) * c.b) * toneMul, 0.0, 1.0);
    if (prot >= 1.0) return kept;
    // the turn leans by the pixel's own hue (the spread), so two garments are not turned alike
    float base = float((word >> 6) & 255) / 256.0;
    float spread = (float((word >> 22) & 15) - 7.5) / 7.5 * kPedSpreadMax;
    float phase = float((word >> 26) & 7) * kPedPhaseStep;
    float a = (base + spread * sin(6.28318530718 * (h + phase))) * 6.28318530718;
    float cs = cos(a), sn = sin(a);
    float k = (1.0 - cs) / 3.0, q = 0.57735026919 * sn;
    vec3 o = vec3((cs + k) * c.r + (k - q) * c.g + (k + q) * c.b,
                  (k + q) * c.r + (cs + k) * c.g + (k - q) * c.b,
                  (k - q) * c.r + (k + q) * c.g + (cs + k) * c.b);
    // the turn keeps the mean of the three, not the luma: put the luma back
    float y0 = dot(c, vec3(0.299, 0.587, 0.114));
    o = clamp(o + (y0 - dot(o, vec3(0.299, 0.587, 0.114))), 0.0, 1.0);
    float satCode = float((word >> 14) & 15);
    float sc = kPedSatBase + satCode * kPedSatStep;
    sc *= 1.0 - kPedChromaCapBy * smoothstep(kPedChromaCap0, kPedChromaCap1, s);
    float vc = kPedValBase + float((word >> 18) & 15) * kPedValStep;
    float y = dot(o, vec3(0.299, 0.587, 0.114));
    // the dye: a grey cloth (low saturation, not black) has no hue to turn - a chroma is
    // added along a hue that follows the grey's own brightness
    float wDye = (1.0 - smoothstep(kPedDyeS0, kPedDyeS1, s)) * smoothstep(kPedDyeV0, kPedDyeV1, mx);
    vec3 dye = pedHueRgb(base + spread * sin(6.28318530718 * (kPedDyeLuma * y0 + phase)));
    float dyeY = dot(dye, vec3(0.299, 0.587, 0.114));
    float dyeBy = kPedDyeMax * (satCode / 15.0) * wDye * (kPedDyeFloor + kPedDyeLift * y0);
    o = clamp((y + (o - y) * sc + (dye - dyeY) * dyeBy) * vc, 0.0, 1.0);
    return o * (1.0 - prot) + kept * prot;
}

void main() {
    int waterMode = PC_WATER & 15;                            // the water's mode (`Push`)
    float wetFilm = float((PC_WATER >> 4) & 255) / 255.0;     // a wet floor's darkening
    float rainNow = float((PC_WATER >> 12) & 255) / 255.0;    // ...and the rain on it
    bool snowLying = (PC_WATER & (1 << 20)) != 0;              // ...or snow lying on it, bit 20
    bool wetBody = (PC_WATER & (1 << 21)) != 0;                // ...or a BODY in the rain, bit 21
    // the open sky at this point, the corners' answers blended (`encodeOpenSky`)
    // - a WALL's (`-6 - open`) below -5.5, a floor's (`-2 - open`) below -1.5
    bool wetWall = vPhase <= -5.5;
    float openSky = wetWall ? clamp(-6.0 - vPhase, 0.0, 1.0) : vPhase <= -1.5 ? clamp(-2.0 - vPhase, 0.0, 1.0) : 1.0;
#ifdef OMK_LATE_DEPTH
    // THE CUTOUT'S DEPTH, decided AFTER the shader (the cutout pipelines'
    // module, -DOMK_LATE_DEPTH, bound for an opaque cutout draw alone): on
    // Apple's GPUs a module that can `discard` its keyed texels still let some
    // of them write depth once it grew past a size, so the cutout's holes hid what lies behind them
    // DIFFERENTLY EACH RUN, the texture atlas's neighbours showing through
    // (Qalisar without MSAA, ~2000 pixels behind the pink sign; found
    // 2026-10-26). Writing the depth makes it the shader's: a discarded
    // fragment writes none. The value is the rasterised one, so nothing moves.
    // Not every draw: it costs the hidden-surface removal (5.4 ms of 13 at 4K
    // for the whole scene, measured).
    gl_FragDepth = gl_FragCoord.z;
#endif
    if ((pc.cull > 0 && !gl_FrontFacing) || (pc.cull < 0 && gl_FrontFacing)) discard;
    vec2 texels = vec2(textureSize(tex, 0));
    texels /= PC_TEXDIV;
    // THE SCREEN DERIVATIVES the remaster's reads take, here - before the
    // water's and the cutout's DISCARDS (the cull's takes a whole primitive).
    // Past a discard that took a neighbour the quad is no longer whole:
    // `dFdx` and a `texture()`'s implicit LOD are UNDEFINED there. On the M1 that drew the
    // glow's light map differently EACH RUN on a wall whose quads a cutout
    // edge split (found 2026-10-25). The reads after the discards take these through
    // `textureGrad`; the colour read above them stays as the original's.
    vec2 gradUV[2] = vec2[2](dFdx(vUV / texels), dFdy(vUV / texels));
    if (waterMode == 2 && vWorld.y > PC_WSTR) discard;   // below the water (Y is down)
    // Alpha is the KEY, written at upload: 0 on a black texel, 1 elsewhere.
    // Under the nearest sampler (the original's) this is exactly the old
    // `rgb == 0` test on the same texel. Under the bilinear ENHANCEMENT the
    // key texels are (0,0,0,0), so the sample is premultiplied: a fragment
    // more than half key is discarded and the rest is un-premultiplied,
    // which is what keeps the cutout edge free of a dark fringe.
    vec4 t = texture(tex, vUV / texels);
    if (pc.cutout != 0) {
        if (t.a < 0.5) discard;
        t.rgb /= t.a;
    }
    // a crowd walker's own colours (`pedvariety`)
    if ((pc.lit & 32) != 0) t.rgb = pedTint(t.rgb, pc.lit);
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
    vec3 nL = normalize(vNrm);
    vec3 shade = (pc.lit & 3) == 1 ? min(vec3(lg.base) + litColour(nL, vWorld), vec3(1.0))
               : (pc.lit & 3) == 2 ? min(vCol + litColour(nL, vWorld), vec3(1.0))
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
    // SNOW LYING (`o3de/wetfloor.h` coverRuns): the floor turned white under
    // its own light - `c / texel`, the light alone - by the cover times the
    // open sky here, before the fog takes it like the rest
    // A BODY IN THE RAIN (`Draw::wetBody`): darker and deeper in colour, and
    // a SHEEN - the water mirroring the lit street around it, so lit by the
    // body's own light (`shade`), in BEADS on its texture (an even film read
    // as plastic: the reader, 2026-10-28): strongest at the grazing angles
    // (Fresnel) and where the reflected ray rises to the sky (Y is down: -y).
    // `reflect` takes either sign of the normal alike. GLES alike
    if (wetBody && wetFilm > 0.0) {
        float y = dot(c, vec3(0.299, 0.587, 0.114));
        c = max(mix(vec3(y), c, 1.0 + kWetBodySat * wetFilm), 0.0) * (1.0 - kWetBodyDark * wetFilm);
        vec3 v = normalize(lg.eye.xyz - vWorld);
        float bead = smoothstep(kWetBodyBeadLo, kWetBodyBeadHi, dustNoise(vec3(vUV * kWetBodyBeadScale, 0.5)));
        float rim = pow(1.0 - abs(dot(nL, v)), kWetBodyRim) * mix(kWetBodyRimFilm, 1.0, bead);
        float sky = pow(max(-reflect(-v, nL).y, 0.0), kWetBodySkyPower) * kWetBodySky * bead;
        c = clamp(c + kWetBodySheen * wetFilm * (rim + sky) * shade, 0.0, 1.0);
        wetFilm = 0.0;   // no floor's film, no ripples, on a body
    }
    if (snowLying && wetFilm > 0.0) {
        // ...the more the darker the texel: a snow texture keeps its own
        // drawing, stone and wood go white
        vec3 lightHere = c / max(t.rgb, vec3(0.04));
        float darkTexel = clamp((1.0 - dot(t.rgb, vec3(0.299, 0.587, 0.114))) * 1.4, 0.0, 1.0);
        c = mix(c, clamp(lightHere * 0.9, 0.0, 1.0), wetFilm * openSky * darkTexel);
        wetFilm = 0.0;   // no rain's film, no ripples, on snow
    }
    // RAY-TRACED FLOOR REFLECTIONS (the enhancement) and the remaster's WET-floor reflection
    // (`waterMode == 3`, below) share the ground: where the rays reflect it, the planar
    // lookup is not added over them - the rain's darkening and ripples still are
    bool rtFloor = false;
#ifdef OMK_RT
    if ((pc.lit & 16) != 0 && rtf.refl.x > 0.0) { c = clamp(rtReflect(c), 0.0, 1.0); rtFloor = true; }
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
    // THE MIST (the remaster's A4, `View::mist`): a height fog in the same
    // colour, on what the game fogs - its density 1 at the set's GROUND and
    // falling as exp(-h / height) above it (Y is down: h = ground - y), the
    // ray's mean density in closed form, one unit of it the 15 m that takes
    // 63% of the light at strength 1 (40 m read as nothing over a street). Not in the water's reflection pass
    // (its eye is the frame's); 0 nothing. The batches it takes are the ones
    // the game fogs - or, its fog off, the ones the backend marks -1 / -1.
    if (lg.fx.y > 0.0 && (pc.fogEnd > pc.fogStart || pc.fogEnd < 0.0) && waterMode != 2) {
        float H = max(lg.fx.w, 1.0);
        float h0 = max(lg.fx.z - lg.eye.y, 0.0), h1 = max(lg.fx.z - vWorld.y, 0.0);
        float dist = length(vWorld - lg.eye.xyz);
        float mean = abs(h1 - h0) > 1.0 ? H * (exp(-h0 / H) - exp(-h1 / H)) / (h1 - h0) : exp(-h0 / H);
        float m = 1.0 - exp(-lg.fx.y * dist * mean / 1500.0);
        c = mix(c, pc.fogColour, m);
    }
    // ...and THE SAND'S DUST (`dustAmount` above), on the same batches
    if (lg.dust.x > 0.0 && (pc.fogEnd > pc.fogStart || pc.fogEnd < 0.0) && waterMode != 2)
        c = mix(c, lg.dustColour.rgb, dustAmount(lg.eye.xyz, vWorld, lg.dust));
    // ...and THE RAIN'S VEILS (`veilAmount` above), on the same batches
    if (lg.veil.x > 0.0 && (pc.fogEnd > pc.fogStart || pc.fogEnd < 0.0) && waterMode != 2)
        c = mix(c, lg.veilColour.rgb, veilAmount(lg.eye.xyz, vWorld, lg.veil));
    // THE FRESNEL (Schlick, F0 0.02 - water's): the water mirrors little
    // looked down into and nearly all at grazing, so `waterStrength` is the
    // GRAZING strength. The plane is level: cos theta is the eye ray's height.
    // A REFLECTING FLOOR (`o3de/floorreflect.h`) is mode 3: the same, its
    // lookup broken up by the rain's ripples. The water's mode 1 is as it was.
    // A WET floor (`o3de/wetfloor.h`): the rain's film darkens it, and its
    // reflection, if it has one, is added over the darker ground - all of it
    // times how open to the sky this point is, the corners' answers blended
    // (`encodeOpenSky`: `-2 - open` in the phase; any other phase is open)
    // PUDDLES (the reader's "more realistic rain", 2 of 3): patches of standing
    // water across the wet floor - a noise mask on the ground (`puddleMask`):
    // in one, the film darker, the reflection much stronger, the ripples more;
    // between them, the film and the reflection lighter
    // A WALL takes a lighter film and its TRICKLES instead - no puddle, no ripples
    float puddle = wetFilm > 0.0 && !wetWall ? puddleMask(vWorld.xz) * lg.veil.w : 0.0;
    float wetHere = wetFilm > 0.0 ? openSky * mix(kPuddleMirror.x, kPuddleMirror.y, puddle) : 1.0;
    c *= 1.0 - wetFilm * openSky * (wetWall ? kWetWallFilm : mix(kPuddleFilm.x, kPuddleFilm.y, puddle));
    // ...and while it RAINS, its RIPPLES (`ripples` above): a little light on
    // their crests, and bending the reflection below
    vec3 rip = vec3(0.0);
    if (wetFilm > 0.0 && rainNow > 0.0 && openSky > 0.0 && wetWall) {
        float near = 1.0 - smoothstep(kTrickleFar * 0.6, kTrickleFar, length(lg.eye.xyz - vWorld));
        vec2 tr = trickles(vWorld, sh.shimmer, rainNow) * openSky * near;
        c = c * (1.0 - kTrickleDark * tr.x) + vec3(kTrickleLight * tr.y);
    } else if (wetFilm > 0.0 && rainNow > 0.0 && openSky > 0.0) {
        rip = ripples(vWorld.xz, sh.shimmer, rainNow) * openSky * mix(kPuddleRipples.x, kPuddleRipples.y, puddle);
        c += vec3(kRippleLight * rip.z);
    }
    if (waterMode == 1 || waterMode == 3) {
        float ct = abs(normalize(lg.eye.xyz - vWorld).y);
        float f = 0.02 + 0.98 * pow(1.0 - ct, 5.0);
        f = max(f, kPuddleFresnel * puddle);   // standing water mirrors, looked into too
        vec2 rs = vec2(textureSize(refl, 0));
        // (a floor's reflection is at HALF size, in the image's top-left quarter)
        float k = waterMode == 3 ? 0.5 : 1.0;
        vec2 at = gl_FragCoord.xy * k;
        if (waterMode == 3) at += rip.xy * rs.y * 0.02 * k;   // the ripples' slope
        if (waterMode == 3) at = clamp(at, vec2(0.5), rs * 0.5 - 0.5);   // inside its quarter
        if (!(waterMode == 3 && rtFloor)) c += texture(refl, at / rs).rgb * PC_WSTR * f * wetHere;
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
    // THE ALPHA, read by the post-pass (`post.frag`, `ao.frag`): 0 on a BODY
    // (`caster`: a character, the shadow map's - posed here or on the CPU),
    // and on the set 1/255 plus its light map's value (the bloom's glow) in
    // the 254 steps above - so a body neither glows nor takes or casts the
    // ambient occlusion. A body marked by the top of the range would collide
    // with full glow: additive draws ADD their alpha.
#ifdef OMK_DX
    float glowV = 0.0;
#else
    float glowV = pc.bloomMode != 0 ? textureGrad(emis, vUV / texels, gradUV[0], gradUV[1]).r : 0.0;
#endif
    outColour = vec4(c, pc.caster != 0 ? 0.0 : (1.0 + 254.0 * glowV) / 255.0);
#ifdef OMK_MOTION
    // a corner behind the eye a frame ago has no previous screen position:
    // no motion, rather than a vector to infinity
    outMotion = vPrevClip.w > 1e-3
              ? (vPrevClip.xy / vPrevClip.w - vCurClip.xy / vCurClip.w) * 0.5
              : vec2(0.0);
#endif
}
