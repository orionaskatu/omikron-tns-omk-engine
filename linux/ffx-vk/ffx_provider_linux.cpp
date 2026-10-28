// This file is part of the FidelityFX SDK (v1.1.4, ffx-api/src/ffx_provider.cpp).
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// The Linux build of the Vulkan library (this repository's `linux/ffx-vk/`)
// reduces that file to what it uses: the FSR 3.1 upscaler provider. The
// original also lists the FSR 2 and frame generation providers, and asks the
// Windows driver (AMD's D3D12 extension) for its own - none exists here.
#include "ffx_provider.h"

#include "ffx_provider_fsr3upscale.h"

#include <cstdint>

static constexpr ffxProvider* providers[] = {
    &ffxProvider_FSR3Upscale::Instance,
};
static constexpr size_t providerCount = sizeof(providers) / sizeof(providers[0]);

const ffxProvider* GetffxProvider(ffxStructType_t descType, uint64_t overrideId, void* /*device*/)
{
    for (size_t i = 0; i < providerCount; ++i)
    {
        if (overrideId && providers[i]->GetId() != overrideId)
            continue;
        if (providers[i]->CanProvide(descType))
            return providers[i];
    }
    return nullptr;
}

const ffxProvider* GetAssociatedProvider(ffxContext* context)
{
    const InternalContextHeader* hdr = (const InternalContextHeader*)(*context);
    return hdr->provider;
}

uint64_t GetProviderCount(ffxStructType_t descType, void* device)
{
    return GetProviderVersions(descType, device, UINT64_MAX, nullptr, nullptr);
}

uint64_t GetProviderVersions(ffxStructType_t descType, void* /*device*/, uint64_t capacity, uint64_t* versionIds, const char** versionNames)
{
    uint64_t count = 0;
    for (size_t i = 0; i < providerCount; ++i)
    {
        if (count >= capacity) break;
        if (providers[i]->CanProvide(descType))
        {
            if (versionIds)
                versionIds[count] = providers[i]->GetId();
            if (versionNames)
                versionNames[count] = providers[i]->GetVersionName();
            count++;
        }
    }
    return count;
}
