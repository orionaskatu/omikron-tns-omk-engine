#version 450
#ifdef OMK_RT
// HYBRID RAY TRACING (`todo/enhancements.md` 19): the Direct3D 12 backend's
// -DOMK_RT build only (`backends/d3d12/shaders/rt.glsl`); Vulkan never defines it.
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
    int   cutout;      // flag 0x800: a colour key on black, never alpha
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
} lg;

#ifdef OMK_RT
// The ray-traced frame's constants (`d3d12render.cpp` `RtFrame`) and the rays'
// tally - one atomic per 8x8 pixels, scaled, for the log - in the -DOMK_RT_COUNT
// build alone, drawn one frame in 60: a shader that writes a UAV loses the
// EARLY depth test, and every hidden body fragment would cast its rays.
layout(set = 3, binding = 2) uniform RtFrame {
    vec4 sky;      // rgb what a ray leaving the set sees; a the indirect term's strength (0 off)
    vec4 eye;      // xyz the eye; w the near occlusion's strength
    vec4 params;   // x indirect reach, y occlusion reach, z lamp radius, w lamp margin (< 0: no shadow rays)
} rtf;
#ifdef OMK_RT_COUNT
layout(std430, set = 3, binding = 4) buffer RtCount { uint n[]; } rtCount;
#endif
bool rtShadowed = false;   // `litColour` traces its lamps (a character's fragment)
uint rtLampRays = 0u;
// SHADOW RAYS FOR THE LAMPS: the lamp at p is hidden from w when the SET is
// in the way - one ray to a point jittered over a small lamp radius, stopped
// short of the lamp by a margin (a lamp record sits inside its fixture). A
// record whose reach spans the city (Anekbah's two reach 35787 units from
// 20-30 km up) is the set's SKY FILL, the light every street is drawn under -
// not a lamp, and not traced: shadowing it would put the whole player in the
// shade of whichever block stands between him and the sky.
bool rtLampHidden(vec3 w, vec3 n, vec3 p, float reach) {
    if (reach > 8000.0) return false;
    float a = rtNoise(gl_FragCoord.xy + vec2(7.0, 13.0));
    vec3 j = vec3(fract(a * 7.31) , fract(a * 3.17), fract(a * 5.73)) * 2.0 - 1.0;
    vec3 o = w + n * 2.0;
    vec3 d = p + j * rtf.params.z - o;
    float L = length(d);
    if (rtf.params.w < 0.0 || L <= rtf.params.w + 1.0) return false;
    ++rtLampRays;
    return rtTrace(o, d / L, L - rtf.params.w, kRtSet, true);
}
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
#ifdef OMK_RT
        if (rtShadowed && rtLampHidden(w, n, p, rA)) continue;   // the light went through a wall
#endif
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

// A CHARACTER LIT BY ITS SURROUNDINGS. `shade` is what the game gave the
// fragment (its lamps, from the scene's grey); to it:
//   1. a vertex-lit body (the engine's default) loses the share of its lamps
//      the shadow rays find behind the set - the vertex light cannot be
//      traced itself (`writePose` hands it as directions), so the frame's
//      lamps are evaluated twice, open and traced, and the ratio scales it;
//   2. the INDIRECT light: cosine-weighted rays into the set, each hit giving
//      what the eye sees there - the baked vertex colour times its texel -
//      and a miss the sky; their mean added, and the rays stopped OVERHEAD
//      within the occlusion reach taken from his ambient (under an arch).
vec3 rtCharacter(vec3 shade, vec3 nIn) {
    vec3 ng = cross(dFdx(vWorld), dFdy(vWorld));
    ng = dot(ng, ng) > 1e-12 ? normalize(ng) : vec3(0.0, -1.0, 0.0);
    if (dot(ng, rtf.eye.xyz - vWorld) < 0.0) ng = -ng;
    vec3 N = dot(nIn, nIn) > 1e-6 ? normalize(nIn) : ng;
    if (dot(N, ng) < -0.25) N = -N;
    if ((pc.lit & 3) == 0 && lg.count > 0) {
        rtShadowed = false;
        vec3 open = litColour(N, vWorld);
        rtShadowed = true;
        vec3 seen = litColour(N, vWorld);
        vec3 b = vec3(lg.base);
        shade *= (b + seen) / max(b + open, vec3(1e-3));
    }
    const int K = 6;
    vec3 sum = vec3(0.0);
    float near = 0.0;
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
                // an occluder OVERHEAD (the game's Y points down): the ground
                // under his feet is where he stands, not a roof
                if (d.y < -0.3) near += 1.0 - smoothstep(0.5 * rtf.params.y, rtf.params.y, rtT);
            }
            sum += rad;
        }
    }
    vec4 q = rtQuad(vec4(sum, near) / float(K));
#ifdef OMK_RT_COUNT
    if (((int(gl_FragCoord.x) | int(gl_FragCoord.y)) & 7) == 0) {
        if (rtLampRays > 0u) atomicAdd(rtCount.n[0], rtLampRays * 64u);
        if (rtf.sky.a > 0.0) atomicAdd(rtCount.n[1], uint(K) * 64u);
    }
#endif
    return shade * (1.0 - rtf.eye.w * q.w) + rtf.sky.a * q.rgb;
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
    vec4 t = texture(tex, vUV / vec2(textureSize(tex, 0)));
    if (pc.cutout != 0) {
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
    rtShadowed = pc.caster != 0;   // a lit character's lamps are traced
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
    outColour = vec4(c, 1.0);
#ifdef OMK_MOTION
    // a corner behind the eye a frame ago has no previous screen position:
    // no motion, rather than a vector to infinity
    outMotion = vPrevClip.w > 1e-3
              ? (vPrevClip.xy / vPrevClip.w - vCurClip.xy / vCurClip.w) * 0.5
              : vec2(0.0);
#endif
}
