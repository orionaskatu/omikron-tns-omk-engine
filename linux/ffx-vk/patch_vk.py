#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patches a copy of FidelityFX SDK v1.1.4's sdk/src/backends/vk/ffx_vk.cpp for the Linux build.

    patch_vk.py <ffx_vk.cpp of the SDK> <patched copy to write>

Each change asserts that the line it edits is still there (the SDK is pinned to
v1.1.4, so a mismatch means the pin moved):

  1. `alignas(32)` off the per-effect context. It lives in the scratch buffer at 4
     byte aligned offsets: MSVC never minded, gcc -O2 zeroes it with aligned vector
     stores and crashes. Its data needs 8.
  2. A device-local-only request falls back to a host-visible device-local memory
     type when that is the only one (software Vulkan, some integrated GPUs): the
     SDK skips those to keep the visible heap free and then finds no memory at all.
  3. The Vulkan call that failed is said on stderr (the SDK only returns a code).
"""
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src, newline="").read()


def swap(old, new):
    global s
    if old not in s:
        raise SystemExit("ffx_vk.cpp is not what patch_vk.py expects: " + old[:60])
    s = s.replace(old, new, 1)


swap("struct alignas(32) EffectContext", "struct EffectContext")

swap("""    return bestCandidate;
}

VkBufferUsageFlags ffxGetVKBufferUsageFlagsFromResourceUsage""",
     """    if (bestCandidate == UINT32_MAX && requestedProperties == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        // only host-visible device-local memory exists: take it
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((memRequirements.memoryTypeBits & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & requestedProperties)) {
                outProperties = memProperties.memoryTypes[i].propertyFlags;
                return i;
            }
        }
    }

    return bestCandidate;
}

VkBufferUsageFlags ffxGetVKBufferUsageFlagsFromResourceUsage""")

n = s.count("return FFX_ERROR_BACKEND_API_ERROR;")
if n < 10:
    raise SystemExit("ffx_vk.cpp has fewer backend error returns than expected")
s = s.replace("return FFX_ERROR_BACKEND_API_ERROR;",
              '{ fprintf(stderr, "ffx_vk.cpp:%d: a Vulkan call failed\\n", __LINE__); return FFX_ERROR_BACKEND_API_ERROR; }')

open(dst, "w", newline="").write(s)
