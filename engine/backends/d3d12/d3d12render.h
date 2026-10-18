// SPDX-License-Identifier: GPL-3.0-or-later
// THE DIRECT3D 12 BACKEND'S ENTRY POINTS, for the window layer.
//
// `PORTING` A8 rule 2 keeps a graphics API's headers inside its own backend
// directory, so this declares the seam the way `playgpu_vulkan.h` declares
// Vulkan's: opaque handles (`hwnd` is a Win32 `HWND`, passed as a pointer)
// and the renderer behind `omk::Renderer`. Nothing here includes <d3d12.h>,
// so the frontend compiles the same with or without the backend linked;
// `OMK_D3D12` (Windows, and the tools found - `Makefile`) says whether it is.
#pragma once

#include "o3de/renderer.h"

#include <vector>

namespace omk {

// The factory. Like `makeVulkanRenderer`, the object is inert until `init`,
// so the enhancement setters (`setMultisample` and the rest) go between.
Renderer* makeD3d12Renderer();
// The adapter's name, UTF-8 ("" when `r` is not the D3D12 renderer).
const char* d3d12DeviceName(Renderer* r);
// A window to present into - a DXGI flip-model swapchain on it. BEFORE `init`;
// without one the renderer is offscreen (`--world-d3d12`, the harness).
bool d3d12AttachWindow(Renderer* r, void* hwnd);
// The four presents `playgpu_vulkan.cpp` already makes on Vulkan, with the
// same meaning: the frame as drawn, a CPU-composited picture, the GPU present
// pass (dither + 565 expansion, the letterbox's rows `vy`..`vy + vh`), and
// that pass read back for `OMK_VERIFY_GPU_PRESENT`.
bool d3d12Present(Renderer* r);
bool d3d12PresentSurface(Renderer* r, const Surface& s);
bool d3d12PresentWorld(Renderer* r, int vy, int vh);
bool d3d12WorldPicture(Renderer* r, int vy, int vh, std::vector<unsigned char>& rgba);
// options row 2: the frame's size changed (and the window's, with it)
bool d3d12Resize(Renderer* r, int w, int h);
// what the frame is drawn with: MSAA samples (1 off) and the device's most
int d3d12Samples(Renderer* r);
int d3d12MaxSamples(Renderer* r);

}  // namespace omk
