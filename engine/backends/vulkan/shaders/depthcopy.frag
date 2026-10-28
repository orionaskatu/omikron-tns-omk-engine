// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
// THE TEMPORAL UPSCALER's DEPTH (`vkrender.cpp` `upscale`, `upscale_vk.h`): the scene's depth - the depth
// aspect of the depth+stencil image the mirror pass needs - copied, texel for texel, into a plain
// R32_SFLOAT image the SDK takes. FidelityFX's Vulkan backend views a depth input as D32_SFLOAT and names the
// depth aspect alone in its barriers, neither legal on a D32_SFLOAT_S8_UINT image, and a copy command cannot
// convert between the two formats; a draw can.
layout(set = 0, binding = 0) uniform sampler2D depthTex;
layout(location = 0) out float outDepth;
void main() {
    outDepth = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
}
