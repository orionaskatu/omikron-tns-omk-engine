#version 450
// The scene vertex shader. The transform is handed down as a finished matrix
// from `cameraBasis` - the CPU side owns the conventions (world up is (0,-1,0)
// because the game's Y points DOWN; hfov is HORIZONTAL) so that this file
// cannot disagree with the software rasterizer about them.
layout(push_constant) uniform Push {
    mat4  mvp;
    int   cutout;      // flag 0x800: a colour key on black, never alpha
    float fogStart;    // 0 = no fog for this batch; see renderer.h's View
    float fogEnd;
    int   caster;      // a caster does not receive; see scene.frag
    vec3  fogColour;   // 16-BYTE ALIGNED at offset 80 - see scene.frag
    int   lit;         // 92, and it must come AFTER the vec3
    int   cull;        // 96, the back-face cull - see scene.frag
    int   posed;       // 100, a body POSED here from set 2
#ifndef OMK_DX
    int   texScaled;   // 104, a TEXTURE PACK replacement - see scene.frag
    float texScaleU;   // 108
    float texScaleV;   // 112
    int   bloomMode;   // 116, the BLOOM's light map - see scene.frag
    int   water;       // 120, the WATER reflection - see scene.frag
    float waterStrength; // 124
#endif
#ifdef OMK_MOTION
    // THE UPSCALER's two (`d3d12render.cpp`, built with -DOMK_MOTION - the Direct3D 12 backend, and
    // Vulkan's frame generation): the sub-pixel JITTER in clip units, added after the transform, and
    // the PREVIOUS frame's view-projection, for the motion vectors. After everything else, so
    // nothing above moves.
#ifdef OMK_DX
    vec2  jitter;      // 104
    mat4  prevMvp;     // 112
#else
    vec2  jitter;      // 128
    mat4  prevMvp;     // 144
#endif
#endif
#ifdef OMK_DX
    // THE DIRECT3D 12 BUILD (-DOMK_DX) has no bloom and no water pass, and a root signature of 64
    // dwords: it carries the texture pack's scale alone, after the motion block (104, or 176 with it)
    uint  texScale;    // the pack's scale per axis, minus 1: u in bits 0..3, v in 4..7 (0: none)
#endif
} pc;

// THE POSE BUFFER (`vkrender.cpp` `posesBodies`, 2026-10-11 - as GLES, GL1 and
// the 3DS pose): a posed draw's corners are its REST corners and this block,
// at the body's offset - d[0] (lights, from black, base grey, affines), then
// kLights lights of two vec4 (direction x strength, colour bytes), then three
// vec4 an affine: world = R rest + t, the rows of a 3x4 (`renderer.h`), then
// one vec4 the body light's tint (rgb).
const int kLights = 16;
layout(std430, set = 2, binding = 0) readonly buffer Pose { vec4 d[]; } pose;

layout(location = 0) in vec3 inPos;    // world position
layout(location = 1) in vec2 inUV;     // TEXEL units, as the shipped data stores them
layout(location = 2) in vec3 inCol;    // the baked light - a COLOUR, not a brightness
layout(location = 3) in vec3 inNrm;    // the vertex NORMAL, for row 7's lighting
layout(location = 4) in float inPhase; // the SHIMMER's phase, -1 = none
layout(location = 5) in float inMesh;  // the corner's mesh, -1 = none (a posed draw's affine)

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vCol;
// The VIEW-SPACE depth, in world units, for the fog. Row 3 of `mvp` is
// `f . (world - eye)` - the forward axis dotted with the offset from the eye -
// so `gl_Position.w` is exactly the `z` raster.cpp's inner loop fogs on.
layout(location = 2) out float vDepth;
// The WORLD position, which the fragment stage needs to look itself up in the
// shadow map. Carried rather than reconstructed: the map's matrix is a
// separate transform and there is no inverse of `mvp` here.
layout(location = 3) out vec3 vWorld;
layout(location = 4) out vec3 vNrm;
layout(location = 5) out float vPhase;
#ifdef OMK_MOTION
// THE MOTION VECTOR's two ends, in clip space, divided per FRAGMENT
// (`scene.frag`): where the corner is now and where it was a frame ago -
// unjittered both, so the vector carries motion and no jitter.
layout(location = 6) out vec4 vCurClip;
layout(location = 7) out vec4 vPrevClip;
#endif

void main() {
    vec3 pos = inPos, nrm = inNrm, col = inCol;
#ifdef OMK_MOTION
    // the corner's world position a frame ago: where it is now, unless a
    // POSED body's previous affines say otherwise (below). The set does not
    // move; only the camera did, which `prevMvp` carries.
    vec3 prevPos = inPos;
#endif
    if (pc.posed != 0) {
        // PLACED by its mesh's affine (a corner no mesh owns stays at rest),
        // the normal turned and not moved
        vec4 h = pose.d[0];
        int m = int(floor(inMesh + 0.5));
        if (m >= 0 && float(m) < h.w) {
            int b = 1 + 2 * kLights + 3 * m;
            vec4 r0 = pose.d[b], r1 = pose.d[b + 1], r2 = pose.d[b + 2];
            pos = vec3(dot(r0.xyz, inPos) + r0.w, dot(r1.xyz, inPos) + r1.w, dot(r2.xyz, inPos) + r2.w);
            nrm = vec3(dot(r0.xyz, inNrm), dot(r1.xyz, inNrm), dot(r2.xyz, inNrm));
#ifdef OMK_MOTION
            // ...and its PREVIOUS affines, which the D3D12 and Vulkan backends write
            // after the current ones AND the body light's tint vec4 (the current
            // again for a body not seen before: no object motion rather than a wrong one)
            int pb = 1 + 2 * kLights + 3 * int(h.w) + 1 + 3 * m;
            vec4 p0 = pose.d[pb], p1 = pose.d[pb + 1], p2 = pose.d[pb + 2];
            prevPos = vec3(dot(p0.xyz, inPos) + p0.w, dot(p1.xyz, inPos) + p1.w, dot(p2.xyz, inPos) + p2.w);
#endif
        }
        // ...and LIT, corner by corner as `applyLights` walks it: from the
        // scene's grey where the body starts black, else from its colour; t =
        // -(N.L) truncated toward zero and clamped to 0..255, the ramp
        // `(t * c) >> 8` (exact in float), each light added and clamped
        // ...from a start colour times THE BODY LIGHT's tint (the remaster's
        // A5, `o3de/lightprobe.h`), the vec4 after the affines - 1, 1, 1
        // unless it is on, which multiplies nothing
        vec3 tint = pose.d[1 + 2 * kLights + 3 * int(h.w + 0.5)].rgb;
        col = inCol * tint;
        int n = int(h.x);
        if (n > 0 || h.y > 0.5) {
            vec3 lit = (h.y > 0.5 ? vec3(h.z) : inCol) * tint;
            for (int i = 0; i < kLights; ++i) {
                if (i >= n) break;
                vec4 L = pose.d[1 + 2 * i], C = pose.d[2 + 2 * i];
                float t = -dot(nrm, L.xyz);
                float ti = clamp(t < 0.0 ? ceil(t) : floor(t), 0.0, 255.0);
                vec3 a = floor(ti * C.rgb / 256.0) / 255.0;
                lit = min(lit + a, vec3(1.0));
            }
            col = lit;
        }
    }
    vUV  = inUV;
    vCol = col;
    vWorld = pos;
    vNrm = nrm;
    // A TIE LOSER (`bakeDepthTie`) carries its phase moved down by 8192: -1
    // becomes -8193, a phase p becomes p - 8192, both decoded exactly here
    float ph = inPhase;
    float tie = 0.0;
    if (ph < -4096.0) { ph += 8192.0; tie = 1.0; }
    vPhase = ph;
    gl_Position = pc.mvp * vec4(pos, 1.0);
    vDepth = gl_Position.w;
    // ...drawn TWO 16-bit depth steps back (Vulkan's window depth IS the NDC
    // z), so the earlier coincident face keeps every pixel under LESS whatever
    // the noise between the two triangulations: the band a later face must
    // beat under the engine's strict test on its quantised buffer (ASSETS 4b)
    gl_Position.z += tie * (2.0 / 65535.0) * gl_Position.w;
#ifdef OMK_MOTION
    vCurClip = pc.mvp * vec4(pos, 1.0);
    vPrevClip = pc.prevMvp * vec4(prevPos, 1.0);
    // THE JITTER, last: the rasterised picture moves by a sub-pixel, the
    // motion vector's two ends do not
    gl_Position.xy += pc.jitter * gl_Position.w;
#endif
}
