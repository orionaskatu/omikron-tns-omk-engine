// SPDX-License-Identifier: GPL-3.0-or-later
// `dcbz` and its measured line - see cacheline.h.
#include "platform/cacheline.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__POWERPC__) || defined(__ppc__) || defined(__powerpc__)
#  define OMK_HAVE_DCBZ 1
#endif

namespace omk {
namespace {

#if defined(OMK_HAVE_DCBZ)
inline void dcbz(void* p) { __asm__ volatile("dcbz 0,%0" : : "r"(p) : "memory"); }

// One `dcbz` in the middle of a buffer of 0xFF, 128-aligned: the run of zeros
// it leaves is the line, provided the run starts at the address given and is
// 32, 64 or 128 bytes with nothing else touched.
std::size_t measureLine() {
    unsigned char raw[3 * 128 + 128];
    unsigned char* base = reinterpret_cast<unsigned char*>(
        (reinterpret_cast<std::uintptr_t>(raw) + 127u) & ~static_cast<std::uintptr_t>(127u));
    std::memset(base, 0xFF, 3 * 128);
    dcbz(base + 128);
    std::size_t first = 3 * 128, last = 0;
    for (std::size_t i = 0; i < 3 * 128; ++i) {
        if (base[i] == 0) {
            if (i < first) first = i;
            last = i;
        } else if (base[i] != 0xFF) {
            return 0;
        }
    }
    if (first != 128) return 0;
    const std::size_t line = last + 1 - first;
    for (std::size_t i = first; i <= last; ++i)
        if (base[i] != 0) return 0;            // one run, no 0xFF inside it
    return line == 32 || line == 64 || line == 128 ? line : 0;
}
#endif

}  // namespace

std::size_t zeroLineSize() {
#if defined(OMK_HAVE_DCBZ)
    static const std::size_t line = [] {
        const std::size_t l = measureLine();
        std::printf("cache: dcbz clears %zu-byte lines%s\n", l,
                    l ? " - whole lines written without being read first" : " (refused: not used)");
        return l;
    }();
    return line;
#else
    return 0;
#endif
}

void claimLines(void* p, std::size_t n) {
#if defined(OMK_HAVE_DCBZ)
    const std::size_t line = zeroLineSize();
    if (!line || n < line) return;
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    std::uintptr_t q = (a + line - 1) & ~static_cast<std::uintptr_t>(line - 1);
    const std::uintptr_t end = (a + n) & ~static_cast<std::uintptr_t>(line - 1);
    for (; q < end; q += line) dcbz(reinterpret_cast<void*>(q));
#else
    (void)p;
    (void)n;
#endif
}

void zeroBytes(void* p, std::size_t n) {
#if defined(OMK_HAVE_DCBZ)
    const std::size_t line = zeroLineSize();
    if (line && n >= 2 * line) {
        const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t q0 = (a + line - 1) & ~static_cast<std::uintptr_t>(line - 1);
        const std::uintptr_t q1 = (a + n) & ~static_cast<std::uintptr_t>(line - 1);
        std::memset(p, 0, q0 - a);
        for (std::uintptr_t q = q0; q < q1; q += line) dcbz(reinterpret_cast<void*>(q));
        std::memset(reinterpret_cast<void*>(q1), 0, a + n - q1);
        return;
    }
#endif
    std::memset(p, 0, n);
}

}  // namespace omk
