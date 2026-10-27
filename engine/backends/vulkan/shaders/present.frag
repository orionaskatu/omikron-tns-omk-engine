#version 450
// THE PRESENT PASS (todo/optimization.md step 4b) - the adventure frame's last
// CPU work, done where the frame already is.
//
// On a frame with nothing drawn over the 3D, the CPU path was: read the colour
// attachment back, `quantise888DitherRow` it into the RGB565 framebuffer at the
// letterbox's row offset (bands black), expand every 565 value to RGBA8 by bit
// replication (`expand565Rgba`) and upload it again. This computes the same
// bytes per pixel, from the same attachment, in integer arithmetic:
//
//   * `quantise888Dither` (ui/surface.h): the 4x4 Bayer offset t - 8, scaled to
//     one output step per channel with C++'s TRUNCATING division - written out
//     below rather than trusting a signed `/` - clamped, then
//     `(v * 31 + 127) / 255` (and 63 for green), all non-negative;
//   * the dither's cell is the WORLD picture's (x, y), not the window's: the
//     readback dithers the picture before it is placed `vy` rows down;
//   * with the dither off, `rgb565`'s truncation;
//   * then `(r5 << 3) | (r5 >> 2)`, `(g6 << 2) | (g6 >> 4)`, `(b5 << 3) | (b5 >> 3)`,
//     alpha 255, written as k / 255 into a UNORM8 target - which stores k.
//   * SUPERSAMPLED (`ss` > 1, todo/optimization.md "The 4K benchmark"): the
//     attachment is `ss` times the picture each way, and the pixel is first
//     the ROUNDED integer mean of its `ss x ss` block, `(sum + n/2) / n` on the
//     8-bit values - `readback`'s resolve - and only then dithered ONCE, at the
//     PICTURE's (x, y). That loop used to run on the CPU after reading the whole
//     oversized attachment back: 530 MB a frame at 4K with 4x4.
//
//   * COLOUR DEPTH 32 (`depth` = 32, `colordepth = 32`, todo/enhancements.md
//     21): the mean above goes out AS IT IS, 8 bits a channel - no dither, no
//     565 quantisation, no bit replication. Any other value is the 16-bit
//     arithmetic above, unchanged.
//
// `engine/tools/present_probe.cpp` checks it against the tables over every
// colour at every matrix cell, and `OMK_VERIFY_GPU_PRESENT` against the real
// framebuffer in play (`verify.py: engine: gpu present`).
//
//   * UPSCALED (`rcas` 1, `todo/remaster.md` A6): `world` is then EASU's
//     output, already at the picture's size (`easu.frag`), and the pixel is
//     AMD FSR 1's RCAS sharpening of it (`third_party/ffx_fsr1.h`, MIT) -
//     then dithered as any other. `raw` 1 writes the 8-bit value undithered
//     and unquantised: `readback`'s, whose CPU path quantises it itself.
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D world;
layout(push_constant) uniform Present {
    int vy;       // the picture's first row in the window
    int vh;       // its height; rows outside are the black bands
    int w;        // the picture's width (RCAS's clamp)
    int dither;   // the View's DITHERENABLE
    int ss;       // the supersample factor, 1 off
    int depth;    // the colour depth: 32 passes the picture through, else 565
    int rcas;     // 1: the upscale's sharpening, `world` EASU's
    uint rcasCon; // `omk::fsrRcasCon`
    int raw;      // 1: the 8-bit value, for `readback`
} pc;
layout(location = 0) out vec4 o;

// (the Direct3D 12 build, -DOMK_NO_RCAS, has no upscale: the sharpening is left out of it)
#ifndef OMK_NO_RCAS
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(world, clamp(p, ASU2(0), ASU2(pc.w - 1, pc.vh - 1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "ffx_fsr1.h"
#endif

const int kBayer4[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);

int divTrunc(int a, int b) { return a >= 0 ? a / b : -((-a) / b); }
int clampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int wy = p.y - pc.vy;
    if (wy < 0 || wy >= pc.vh) {
        o = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    int r = 0, g = 0, b = 0;
#ifndef OMK_NO_RCAS
    if (pc.rcas != 0) {
        AF1 fr, fg, fb;
        FsrRcasF(fr, fg, fb, AU2(p.x, wy), AU4(pc.rcasCon, 0u, 0u, 0u));
        r = clampByte(int(round(fr * 255.0)));
        g = clampByte(int(round(fg * 255.0)));
        b = clampByte(int(round(fb * 255.0)));
    } else
#endif
    {
        for (int sy = 0; sy < pc.ss; ++sy)
            for (int sx = 0; sx < pc.ss; ++sx) {
                vec4 s = texelFetch(world, ivec2(p.x * pc.ss + sx, wy * pc.ss + sy), 0);
                r += int(round(s.r * 255.0));
                g += int(round(s.g * 255.0));
                b += int(round(s.b * 255.0));
            }
        int n = pc.ss * pc.ss;
        r = (r + n / 2) / n;
        g = (g + n / 2) / n;
        b = (b + n / 2) / n;
    }
    if (pc.raw != 0 || pc.depth == 32) {   // 32: the picture passes through (`colordepth`)
        o = vec4(float(r), float(g), float(b), 255.0) / 255.0;
        return;
    }
    int r5, g6, b5;
    if (pc.dither != 0) {
        int t = kBayer4[(wy & 3) * 4 + (p.x & 3)] - 8;
        int d5 = divTrunc(t * 8, 16);
        int d6 = divTrunc(t * 4, 16);
        r5 = (clampByte(r + d5) * 31 + 127) / 255;
        g6 = (clampByte(g + d6) * 63 + 127) / 255;
        b5 = (clampByte(b + d5) * 31 + 127) / 255;
    } else {
        r5 = r >> 3;
        g6 = g >> 2;
        b5 = b >> 3;
    }
    int R = (r5 << 3) | (r5 >> 2);
    int G = (g6 << 2) | (g6 >> 4);
    int B = (b5 << 3) | (b5 >> 3);
    o = vec4(float(R), float(G), float(B), 255.0) / 255.0;
}
