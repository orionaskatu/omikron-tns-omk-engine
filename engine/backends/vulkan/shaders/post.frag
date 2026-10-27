// SPDX-License-Identifier: GPL-3.0-or-later
// THE POST-PROCESSING CHAIN's final pass, for the Direct3D 12 and Vulkan backends
// (`todo/enhancements.md` 22): the finished 3D frame - the scene's colour, or
// the upscaler's display-size output - in, a display-size picture out, which
// the present pass then hands to the window with its colour-depth rule
// (`present.frag`), and the 2D interface is laid over AFTER it (`overlay.frag`,
// or the CPU composite over `readback()`): the HUD and the subtitles are
// never bloomed, blurred or graded.
//
// In order, each step only when asked:
//   1. the source pixel, the supersample folded as `present.frag` folds it;
//   2. DEPTH OF FIELD, on cinematic frames alone (the caller decides: a
//      letterboxed strip, conversations and cutscenes): a single-pass
//      scatter-as-gather on a golden-angle spiral (after Dennis Gustafsson,
//      "Bokeh depth of field in a single pass", 2018), the circle of
//      confusion from the depth buffer against the camera's eye-to-target
//      distance, a nearer sample's own circle allowed to spill over (so a
//      blurred foreground covers a sharp subject's edge) and a farther one
//      clamped (so a sharp subject never smears onto the background);
//   3. into LINEAR light (gamma 2.2, the CRT the game was lit for) and
//      EXPANDED: identity below the knee, the inverse of the tone curve's
//      shoulder above it - so the frame's own 8-bit picture maps back onto
//      itself exactly when nothing is added, and a near-white neon texel
//      stands for several times a mid grey's light;
//   4. BLOOM added there (`bloom.frag`'s chain, level 0, half size);
//   5. the TONE CURVE: the shoulder, with white at 1 (SDR) or at the HDR
//      display's peak over paper white (`hdr = on`), back to gamma 2.2 - so
//      added light rolls off instead of clipping;
//   6. COLOUR GRADING, parametric (`colorgrade = cyber|warm|noir|filmic`, the
//      numbers in `o3de/postfx.h`'s `kGrades`): lift / gamma / gain per
//      channel, an S-curve, saturation (less in the highlights for filmic),
//      and a split tone - a tint into the shadows and another into the
//      highlights, each with no luma of its own.
#version 450

layout(set = 0, binding = 0) uniform sampler2D src;     // linear, clamped
layout(set = 0, binding = 1) uniform sampler2D bloom;   // level 0 of the chain
#ifdef OMK_MS
layout(set = 0, binding = 2) uniform sampler2DMS depthTex;
layout(set = 0, binding = 3) uniform usampler2DMS stencilTex;   // the depth image's stencil plane
#else
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform usampler2D stencilTex;     // the depth image's stencil plane
#endif

layout(push_constant) uniform Post {
    vec4 frame;    // xy: 1 / the display size; zw: the picture's uv rectangle (uvMax)
    vec4 depthAB;  // xy: the depth image's size; z, w: A, B - view z = B / (d - A)
    vec4 dof;      // x: focus distance (0: no depth of field), y: largest radius (px),
                   // z: the in-focus band (fraction of the focus), w: the tap step
    vec4 tone;     // x: bloom intensity (0 off), y: white (1 SDR, the HDR peak / paper white),
                   // z: grade on (0/1), w: supersample factor
    vec4 lift;     // rgb lift, w saturation
    vec4 gam;      // rgb gamma, w S-curve amount
    vec4 gain;     // rgb gain, w highlight desaturation
    vec4 tintS;    // rgb shadow tint, w its weight
    vec4 tintH;    // rgb highlight tint, w its weight
    vec4 lut;      // x: the remaster's grade LUT's side (0 none) - the Direct3D 12 build's (-DOMK_DX)
} pc;

#ifdef OMK_DX
// THE REMASTER'S CITY GRADE (`rmpost.frag`'s, here for the Direct3D 12 backend, whose chain this is - and
// the shower's overcast rides on it): a 3D LUT stored as an N*N x N strip (blue slices side by side),
// looked up trilinearly. Applied FIRST, on the 8-bit picture, before this chain's own grade.
layout(set = 0, binding = 4) uniform sampler2D rmLut;
vec3 rmGraded(vec3 c) {
    float n = pc.lut.x;
    c = clamp(c, 0.0, 1.0);
    float bs = c.b * (n - 1.0);
    float b0 = floor(bs);
    float b1 = min(b0 + 1.0, n - 1.0);
    float t = bs - b0;
    vec2 inv = vec2(1.0 / (n * n), 1.0 / n);
    vec2 rg = c.rg * (n - 1.0) + 0.5;
    vec3 a = texture(rmLut, (vec2(b0 * n, 0.0) + rg) * inv).rgb;
    vec3 b = texture(rmLut, (vec2(b1 * n, 0.0) + rg) * inv).rgb;
    return mix(a, b, t);
}
#endif

layout(location = 0) out vec4 o;

const float kKnee = 0.5;
// verbatim in `bloom.frag`
vec3 expand(vec3 L) {
    vec3 u = clamp((L - kKnee) / (1.0 - kKnee), 0.0, 1.0 - 1e-3);
    vec3 e = kKnee - (1.0 - kKnee) * log(1.0 - u);
    return mix(L, e, step(kKnee, L));
}
// its inverse, with the asymptote at `white`
vec3 shoulder(vec3 E, float white) {
    float r = white - kKnee;
    vec3 d = kKnee + r * (1.0 - exp(-(E - kKnee) / r));
    return mix(E, d, step(kKnee, E));
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float viewZ(vec2 uv) {
    ivec2 p = ivec2(min(uv * pc.depthAB.xy, pc.depthAB.xy - 1.0));
#ifdef OMK_MS
    float d = texelFetch(depthTex, p, 0).r;
#else
    float d = texelFetch(depthTex, p, 0).r;
#endif
    return pc.depthAB.w / min(d - pc.depthAB.z, -1e-7);
}
// A TRANSLUCENT BODY (a hologram, a ghost) writes no depth, so its pixels hold
// the wall behind it; the scene pass marked them with stencil 2. Such a pixel
// is as far as the focus - the subject of the picture, sharp.
bool subjectAt(vec2 uv) {
    ivec2 p = ivec2(min(uv * pc.depthAB.xy, pc.depthAB.xy - 1.0));
    return texelFetch(stencilTex, p, 0).g == 2u;
}
// the circle of confusion's radius, in display pixels
float coc(float z, float f) {
    float off = max(abs(z - f) - pc.dof.z * f, 0.0) / max(z, 1e-3);
    return clamp(off * 1.6, 0.0, 1.0) * pc.dof.y;
}

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 uv = gl_FragCoord.xy * pc.frame.xy;
    // 1. the source, folded
    int ss = int(pc.tone.w + 0.5);
    vec3 c = vec3(0.0);
    for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx)
            c += texelFetch(src, ivec2(p.x * ss + sx, p.y * ss + sy), 0).rgb;
    c /= float(ss * ss);
    // 2. depth of field
    if (pc.dof.x != 0.0) {
        vec2 lim = pc.frame.zw - 0.5 * pc.frame.xy;
        // THE FOCUS: where the camera AIMS - its target projects to the
        // strip's centre - at the distance of what is there: the median of
        // five depths about the centre. The eye-to-target distance itself
        // (pc.dof.x) is the floor: an editing's target is often a point in
        // the air before the subject (143 units in the supermarket fight's
        // opening shot, the fighter several times further).
        vec2 cc = 0.5 * pc.frame.zw, dd = 0.04 * pc.frame.zw;
        float focus = -pc.dof.x;   // an EXPLICIT focus (a character's depth): negative
        if (pc.dof.x > 0.0) {
            float z0 = viewZ(cc), z1 = viewZ(cc + vec2(dd.x, 0.0)), z2 = viewZ(cc - vec2(dd.x, 0.0));
            float z3 = viewZ(cc + vec2(0.0, dd.y)), z4 = viewZ(cc - vec2(0.0, dd.y));
            // the median of five
            float zs5[5] = float[5](z0, z1, z2, z3, z4);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4 - i; ++j) {
                    float lo = min(zs5[j], zs5[j + 1]), hi = max(zs5[j], zs5[j + 1]);
                    zs5[j] = lo; zs5[j + 1] = hi;
                }
            focus = max(zs5[2], pc.dof.x);
        }
        float zc = subjectAt(uv) ? focus : viewZ(uv);
        float rc = coc(zc, focus);
        vec3 acc = c;
        float tot = 1.0;
        float step_ = pc.dof.w;
        float radius = step_;
        float ang = 0.0;
        for (int i = 0; i < 96 && radius < pc.dof.y; ++i) {
            vec2 t = min(uv + vec2(cos(ang), sin(ang)) * radius * pc.frame.xy, lim);
            vec3 sc = texture(src, t).rgb;
            float zs = subjectAt(t) ? focus : viewZ(t);
            float rs = coc(zs, focus);
            if (zs > zc) rs = clamp(rs, 0.0, rc * 2.0);
            float m = smoothstep(radius - step_, radius + step_, rs);
            acc += mix(acc / tot, sc, m);
            tot += 1.0;
            radius += step_ * step_ / radius;
            ang += 2.39996323;
        }
        c = acc / tot;
    }
#ifdef OMK_DX
    // the REMASTER's city grade (and the shower's overcast), before everything of this chain's own
    if (pc.lut.x > 1.0) c = rmGraded(c);
#endif
    bool tone = pc.tone.x > 0.0 || pc.tone.y != 1.0;
    if (tone) {
        // 3-5. linear, expanded, bloom, the shoulder, back
        vec3 E = expand(pow(max(c, vec3(0.0)), vec3(2.2)));
        if (pc.tone.x > 0.0) E += pc.tone.x * texture(bloom, min(uv, pc.frame.zw)).rgb;
        c = pow(shoulder(E, pc.tone.y), vec3(1.0 / 2.2));
    }
    // 6. the grade - on the SDR range; an HDR highlight's excess over 1 is
    // carried through untouched
    if (pc.tone.z > 0.0) {
        vec3 hi = max(c - 1.0, 0.0);
        vec3 g = min(c, 1.0);
        g = pow(max(pc.gain.rgb * (g + pc.lift.rgb * (1.0 - g)), 0.0), 1.0 / pc.gam.rgb);
        // the S-curve, pivoted low (0.4: this is a dark game, and a curve
        // about 0.5 crushes most of it): a power toe below, a shoulder above
        const float pv = 0.4;
        vec3 toe = pv * pow(g / pv, vec3(1.0 + 0.5 * pc.gam.w));   // (a gentler toe)
        vec3 sh = 1.0 - (1.0 - pv) * pow(max((1.0 - g) / (1.0 - pv), 0.0), vec3(1.0 + pc.gam.w));
        g = mix(sh, toe, step(g, vec3(pv)));
        float l = luma(g);
        float sat = pc.lift.w * (1.0 - pc.gain.w * smoothstep(0.55, 1.0, l));
        g = mix(vec3(l), g, sat);
        vec3 ts = pc.tintS.rgb - luma(pc.tintS.rgb), th = pc.tintH.rgb - luma(pc.tintH.rgb);
        // (the shadow tint fades out toward black, which it would otherwise own)
        g += ts * (pc.tintS.w * (1.0 - l) * (1.0 - l) * min(l * 6.0, 1.0)) + th * (pc.tintH.w * l * l);
        c = clamp(g, 0.0, 1.0) + hi;
    }
    o = vec4(c, 1.0);
}
