// This file is part of the FidelityFX SDK (v1.1.4, sdk/src/shared/ffx_message.cpp).
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
// The original drops every message on a platform that is not Windows (it only
// knows OutputDebugString). This one hands them to the callback the host set,
// which is how the viewer sees the library's debug checks.
#include <FidelityFX/host/ffx_message.h>

static ffxMessageCallback s_messageCallback;
static uint32_t           s_debugLevel;

void ffxSetPrintMessageCallback(ffxMessageCallback callback, uint32_t debugLevel)
{
    s_messageCallback = callback;
    s_debugLevel      = debugLevel;
}

void ffxPrintMessage(uint32_t type, const wchar_t* message)
{
    if (s_messageCallback)
        s_messageCallback(type, message);
}
