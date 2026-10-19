// SPDX-License-Identifier: GPL-3.0-or-later
// THE WINDOW BLIT, for the Direct3D 12 backend's swapchain present - and, built
// with -DOMK_HDR, the Vulkan backend's blit into an scRGB swapchain.
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
    float white;   // HDR (-DOMK_HDR): scRGB's value for paper white (nits / 80)
    float peak;    // ...and for the display's peak, where it is clamped
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
#ifdef OMK_HDR
    // HDR OUTPUT (`hdr = on`, `todo/enhancements.md` 22): the swapchain is
    // scRGB - FP16, linear, BT.709 primaries, 1.0 = 80 nits
    // (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709). Everything this blit is
    // handed is gamma 2.2 with 1.0 = paper white - the 2D, a CPU frame, and
    // the post chain's picture, whose highlights alone run past 1.0 - so it
    // goes to linear light and is scaled to paper white's nits.
    o.rgb = min(pow(max(o.rgb, vec3(0.0)), vec3(2.2)) * pc.white, vec3(pc.peak));
#endif
}
