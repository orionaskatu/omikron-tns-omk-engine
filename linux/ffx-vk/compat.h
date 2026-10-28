// SPDX-License-Identifier: GPL-3.0-or-later
// What AMD's FidelityFX SDK v1.1.4 takes from MSVC and glibc does not have,
// read in with `-include`: the "_s" string functions, `_countof`, and
// `__declspec(dllexport)` (an exported symbol, with -fvisibility=hidden for the rest).
#pragma once
// MSVC's headers pull these in on their own
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <locale>    // std::wstring_convert
#include <codecvt>

#ifndef _WIN32

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define __declspec(x) __attribute__((visibility("default")))
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#define sprintf_s snprintf

// into a wchar_t array of a known size
template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
    wcsncpy(dst, src, N - 1);
    dst[N - 1] = L'\0';
    return 0;
}
inline int wcscpy_s(wchar_t* dst, size_t n, const wchar_t* src) {
    if (!dst || n == 0) return 22;
    wcsncpy(dst, src, n - 1);
    dst[n - 1] = L'\0';
    return 0;
}
inline int strcpy_s(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return 22;
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
    return 0;
}
inline int wcstombs_s(size_t* converted, char* dst, size_t dstSize, const wchar_t* src, size_t maxCount) {
    if (!dst || dstSize == 0) return 22;
    const size_t limit = maxCount < dstSize - 1 ? maxCount : dstSize - 1;
    size_t n = wcstombs(dst, src, limit);
    if (n == static_cast<size_t>(-1)) n = 0;
    dst[n] = '\0';
    if (converted) *converted = n + 1;
    return 0;
}

#endif  // !_WIN32
