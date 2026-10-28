// SPDX-License-Identifier: GPL-3.0-or-later
// THE UPSCALERS - `todo/enhancements.md` row 18, the Direct3D 12 backend's.
//
// An ENHANCEMENT the original never had, off unless `--upscaler` or
// `[Enhancements] upscaler` asks: the frame is drawn at a RENDER size below
// the display size, over a sub-pixel jitter that changes every frame, and a
// temporal upscaler rebuilds the display-size picture from the frame's
// colour, its depth and a motion-vector target the scene pass writes. Two
// SDKs stand behind this one interface:
//
//   * AMD FSR (`upscale_fsr.cpp`), through the FidelityFX API's loader DLL,
//     which picks the newest provider the GPU can run - FSR 4 on RDNA 4, 3.1
//     elsewhere - and says which;
//   * NVIDIA DLSS (`upscale_dlss.cpp`), through Streamline's interposer.
//
// Both are loaded at RUN time (LoadLibrary / GetProcAddress) from the DLLs the
// `Makefile` copies next to the viewer, so a build or a machine without them
// runs, prints why, and draws the native frame. The backend owns everything
// the SDKs are fed - the jitter, the previous camera, the motion target - and
// this header is the whole of what it asks of them; the SDKs' own headers
// stay inside the two `.cpp` files.
//
// CONVENTIONS, settled here once for both:
//   * the MOTION VECTOR (`scene.frag`, -DOMK_MOTION) is PREVIOUS minus CURRENT
//     position in texture-UV units (y down): FSR's scale is the render size,
//     DLSS's 1;
//   * the JITTER is in render PIXELS, as the SDKs return it, in [-0.5, 0.5]; the
//     backend adds `2 x / rw`, `2 y / rh` to clip space (Vulkan's Y-down clip
//     space, which the vertex translation then flips to D3D's - FSR's sign);
//   * matrices are this engine's: COLUMN-major, `cameraBasis`'s camera, the
//     depth NOT inverted and the far plane finite (`pushView`).
#pragma once

#ifndef WIDL_EXPLICIT_AGGREGATE_RETURNS
#  define WIDL_EXPLICIT_AGGREGATE_RETURNS   // the MSVC ABI for COM struct returns (d3d12render.cpp)
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <memory>
#include <string>

#include "o3de/upscaleshared.h"

namespace omk::d3dup {

// the kinds, the quality ladder and the camera are the Vulkan glue's too
// (`o3de/upscaleshared.h`)
using omk::upscale::Kind;
using omk::upscale::kOff;
using omk::upscale::kFsr;
using omk::upscale::kDlss;
using omk::upscale::qualityRatio;
using omk::upscale::qualityName;
using omk::upscale::Camera;

// One dispatch. The resources are in the states named: the inputs readable
// by any shader stage, the output an unordered-access target.
struct Frame {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* colour = nullptr;   // render size, R8G8B8A8 (perceptual, LDR)
    ID3D12Resource* depth = nullptr;    // render size, D32S8 (TYPELESS), not inverted
    ID3D12Resource* motion = nullptr;   // render size, R16G16_FLOAT (the convention above)
    ID3D12Resource* output = nullptr;   // display size, R8G8B8A8, UAV
    D3D12_RESOURCE_STATES inputState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    int rw = 0, rh = 0, dw = 0, dh = 0;
    float jitterX = 0.0f, jitterY = 0.0f;   // render pixels
    bool reset = false;                      // the camera cut: drop the history
    float frameMs = 33.333f;
    Camera cam;
};

class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual const char* name() const = 0;       // "FSR", "DLSS"
    virtual std::string version() const = 0;    // what the provider reported
    // the render size for the display size it was made for
    virtual void renderSize(int& rw, int& rh) = 0;
    // the jitter sequence: its length for this ratio, and its `index`th offset
    virtual int jitterPhases(int rw, int dw) = 0;
    virtual void jitter(int index, int phases, float& x, float& y) = 0;
    // record the upscale into `f.list`. It may change the list's descriptor
    // heaps and root signature: the caller sets its own again afterwards.
    virtual bool dispatch(const Frame& f) = 0;
};

// Each -> null, after one log line saying why, when its SDK was not built in,
// its DLLs are not next to the viewer, or this device cannot run it - and the
// caller then draws natively (or tries the other).
std::unique_ptr<Upscaler> makeFsr(ID3D12Device* dev, int dw, int dh, int quality);
std::unique_ptr<Upscaler> makeDlss(ID3D12Device* dev, IDXGIAdapter1* adapter, int dw, int dh, int quality);

// The viewer's own directory, where the `Makefile` puts the SDKs' DLLs - and
// the ONLY place they are loaded from (never the search path).
std::wstring moduleDir();

}  // namespace omk::d3dup
