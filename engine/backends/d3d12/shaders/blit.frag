// SPDX-License-Identifier: GPL-3.0-or-later
// THE WINDOW BLIT, for the Direct3D 12 backend's swapchain present.
//
// Vulkan presents with `vkCmdBlitImage`: the finished `w x h` frame (or the
// present pass's picture) scaled into the window at its own aspect, centred,
// over a black clear - NEAREST at supersample 1, LINEAR when there is
// something to average. D3D12 has no scaled blit, so this is that blit as a
// full-screen triangle (`fsq.vert`) drawn with the viewport set to the
// destination rectangle. GLSL like every other shader here, and compiled the
// same way (glslc -> spirv-cross -> dxc, `Makefile`), so there is still ONE
// shading language in the tree; only the D3D12 backend uses this one.
//
// NEAREST is Vulkan's blit rule exactly: the source texel is
// floor((dst + 0.5) * src / dstSize), which `gl_FragCoord`'s pixel centre
// already carries.
#version 450

// a LINEAR, clamped sampler (the root signature's static one)
layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Blit {
    vec4 dst;      // x0, y0, width, height of the destination rectangle
    vec4 srcSize;  // the source region's width, height; the texture's width, height
    int  linear;   // 1 LINEAR (supersampled), 0 NEAREST
} pc;

layout(location = 0) out vec4 o;

void main() {
    vec2 rel = (gl_FragCoord.xy - pc.dst.xy) / pc.dst.zw;
    vec2 s = rel * pc.srcSize.xy;
    if (pc.linear != 0)
        o = texture(src, s / pc.srcSize.zw);
    else
        o = texelFetch(src, ivec2(min(floor(s), pc.srcSize.xy - 1.0)), 0);
    o.a = 1.0;
}
