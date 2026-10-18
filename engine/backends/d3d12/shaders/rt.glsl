// SPDX-License-Identifier: GPL-3.0-or-later
// HYBRID RAY TRACING's shared half (`todo/enhancements.md` 19) - included by
// `scene.frag` and `ssao.frag` when they are built -DOMK_RT, which only the
// Direct3D 12 backend does (`Makefile`: glslc -std=460, spirv-cross shader
// model 6.5, dxc ps_6_5 - DXR 1.1's INLINE ray queries). Vulkan never sees
// this file, so its shaders compile exactly as before.
//
// The scene the rays see (`d3d12render.cpp`, "HYBRID RAY TRACING"):
//   * ONE TLAS a frame (t0 space3): an instance per static SET geometry (mask
//     1), its BLAS built once from the geometry's opaque and cutout batches,
//     and an instance per POSED BODY (mask 2), rebuilt every frame from its
//     posed corners. Identity transforms - every corner is already in world
//     space, as the raster draws it.
//   * Each BLAS has two geometries: 0 the opaque triangles, 1 the CUTOUT ones
//     (non-opaque, so a ray asks this file whether the texel is the colour
//     key). Blended faces (additive glows, multiply shadows) are not in it.
//   * Per instance a uvec4 (t1 space3): the heap slot of its triangle
//     RECORDS, the count of opaque triangles (geometry 1's first record), the
//     mask. A record is 16 words: three corners of (u, v, r, g, b) - the
//     texel coordinates and the BAKED vertex colour, the artists' lighting -
//     and the texture's heap slot (the white texture when no draw has said).
//   * The records and the textures are read BINDLESS: two unbounded tables
//     over the whole shader-visible heap (t0 space4 raw buffers, t0 space5
//     textures), indexed by heap slot.
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 3, binding = 0) uniform accelerationStructureEXT rtScene;
layout(std430, set = 3, binding = 1) readonly buffer RtInst { uvec4 i[]; } rtInst;
layout(set = 3, binding = 3) uniform sampler rtSmp;   // NEAREST, WRAP - the original's
layout(std430, set = 4, binding = 0) readonly buffer RtRec { uint d[]; } rtRec[];
layout(set = 5, binding = 0) uniform texture2D rtTex[];

const uint kRtSet = 1u, kRtBody = 2u;

// the hit `rtTrace` committed
float rtT;
int rtI, rtP, rtG;
vec2 rtB;

uint rtWord(uint slot, uint w) { return rtRec[nonuniformEXT(slot)].d[w]; }

// A hit triangle's surface: its texel coordinates and baked colour at the
// barycentrics, and its texture's heap slot.
void rtSurface(int inst, int prim, int geom, vec2 b, out vec2 uv, out vec3 col, out uint tex) {
    uvec4 ii = rtInst.i[inst];
    uint base = (uint(prim) + (geom == 1 ? ii.y : 0u)) * 16u;
    vec3 wv = vec3(1.0 - b.x - b.y, b.x, b.y);
    uv = vec2(0.0);
    col = vec3(0.0);
    for (int k = 0; k < 3; ++k) {
        uint o = base + uint(k) * 5u;
        uv += wv[k] * vec2(uintBitsToFloat(rtWord(ii.x, o)), uintBitsToFloat(rtWord(ii.x, o + 1u)));
        col += wv[k] * vec3(uintBitsToFloat(rtWord(ii.x, o + 2u)), uintBitsToFloat(rtWord(ii.x, o + 3u)),
                            uintBitsToFloat(rtWord(ii.x, o + 4u)));
    }
    tex = rtWord(ii.x, base + 15u);
}

// the texel, in the material's own pixel units as `scene.frag` samples it.
// `word` is the record's texture word: the heap index in bits 0..23 and, for a
// TEXTURE PACK's replacement (`o3de/texpack.h`), k - 1 in bits 24.. - the
// image is k x the original and the UVs stay in the original's units
vec4 rtTexel(uint word, vec2 uv) {
    uint tex = word & 0xFFFFFFu;
    vec2 sz = vec2(textureSize(sampler2D(rtTex[nonuniformEXT(tex)], rtSmp), 0)) / float((word >> 24) + 1u);
    return textureLod(sampler2D(rtTex[nonuniformEXT(tex)], rtSmp), uv / max(sz, vec2(1.0)), 0.0);
}

// -> true on a hit (`rtT`..`rtB` say where). A CUTOUT candidate is taken only
// where its texel is not the key - a grille lets the ray through its holes.
// `first`: any hit ends the search (shadow and occlusion rays).
bool rtTrace(vec3 o, vec3 d, float tmax, uint mask, bool first) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, rtScene, first ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT,
                          mask, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q)) {
        if (rayQueryGetIntersectionTypeEXT(q, false) == gl_RayQueryCandidateIntersectionTriangleEXT) {
            vec2 uv; vec3 col; uint tex;
            rtSurface(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false),
                      rayQueryGetIntersectionPrimitiveIndexEXT(q, false),
                      rayQueryGetIntersectionGeometryIndexEXT(q, false),
                      rayQueryGetIntersectionBarycentricsEXT(q, false), uv, col, tex);
            if (rtTexel(tex, uv).a >= 0.5) rayQueryConfirmIntersectionEXT(q);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
    rtT = rayQueryGetIntersectionTEXT(q, true);
    rtI = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    rtP = rayQueryGetIntersectionPrimitiveIndexEXT(q, true);
    rtG = rayQueryGetIntersectionGeometryIndexEXT(q, true);
    rtB = rayQueryGetIntersectionBarycentricsEXT(q, true);
    return true;
}

// a stable per-pixel angle (interleaved gradient noise): the rays turn from
// pixel to pixel, never from frame to frame - no shimmer without a history
float rtNoise(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

// a cosine-weighted direction about N, ray i of K turned by angle a, its
// stratum (how far from the normal) offset by j in [0,1) - which keeps a few
// rays from drawing spokes round a body's feet
vec3 rtCosine(vec3 N, int i, int K, float a, float j) {
    vec3 T = normalize(cross(abs(N.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), N));
    vec3 B = cross(N, T);
    float u = (float(i) + j) / float(K);
    float phi = a + float(i) * 2.3999632;   // the golden angle
    float r = sqrt(u);
    return normalize(T * (cos(phi) * r) + B * (sin(phi) * r) + N * sqrt(max(1.0 - u, 0.0)));
}
