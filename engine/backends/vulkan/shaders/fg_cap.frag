// SPDX-License-Identifier: GPL-3.0-or-later
// FRAME GENERATION's CAPTURE (`todo/enhancements.md` 27), the Direct3D 12
// and Vulkan windows' (one source): at the end of every rendered frame the finished WORLD picture
// (the post chain's, the upscaler's or the scene's - `displaySource`, the
// supersample folded) is kept at the display size, and beside it what the
// in-between frame needs of the scene - the MOTION VECTOR in display pixels and
// the VIEW DEPTH - read from the render-size targets at the pixel's place.
// Two frames of these are what `fg_splat.frag` and `fg_resolve.frag` warp.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D world;    // the display source
layout(set = 0, binding = 1) uniform texture2D depth;    // the scene's depth (render size)
layout(set = 0, binding = 2) uniform texture2D motion;   // PREVIOUS minus CURRENT, uv of the viewport

layout(push_constant) uniform Cap {
    vec4 pic;       // the picture's width, height (display px), the source's supersample, -
    vec4 toRender;  // render viewport / picture, x and y
    vec4 depthAB;   // the projection's A, B (view depth = B / (z - A))
} pc;

layout(location = 0) out vec4 oCol;
layout(location = 1) out vec4 oAux;   // motion (display px), view depth / 4, 1 inside the picture

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    int ss = int(pc.pic.z);
    vec4 c = vec4(0.0);
    for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx)
            c += texelFetch(world, ivec2(p.x * ss + sx, p.y * ss + sy), 0);
    oCol = vec4(c.rgb / float(ss * ss), 1.0);
    if (p.x >= int(pc.pic.x) || p.y >= int(pc.pic.y)) {
        oAux = vec4(0.0);
        return;
    }
    ivec2 r = ivec2((vec2(p) + 0.5) * pc.toRender.xy);
    float z = texelFetch(depth, r, 0).r;
    vec2 mv = texelFetch(motion, r, 0).rg;
    float vz = pc.depthAB.y / min(z - pc.depthAB.x, -1e-9);
    oAux = vec4(mv * pc.pic.xy, vz * 0.25, 1.0);
}
