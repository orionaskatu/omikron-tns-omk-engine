// SPDX-License-Identifier: GPL-3.0-or-later
// The one part of the Vulkan backend this library leaves out: the frame
// generation swapchain (sdk/src/backends/vk/FrameInterpolationSwapchain). Only
// the FSR 3.1 upscaler is wanted, but the backend's interface table points at
// this function, so it must exist; it says "not supported".
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* /*config*/)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}
