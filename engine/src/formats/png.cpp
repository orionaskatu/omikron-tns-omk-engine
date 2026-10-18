// SPDX-License-Identifier: GPL-3.0-or-later
#include "formats/png.h"

#include <cstdlib>
#include <cstring>

namespace omk {
namespace {

// ---- INFLATE (RFC 1951) ------------------------------------------------------

struct Bits {
    const std::uint8_t* p;
    std::size_t n, at = 0;
    std::uint32_t buf = 0;
    int cnt = 0;
    bool over = false;
    int need(int k) {                        // k <= 24 bits, LSB first
        while (cnt < k) {
            if (at >= n) { over = true; return 0; }
            buf |= static_cast<std::uint32_t>(p[at++]) << cnt;
            cnt += 8;
        }
        const int v = static_cast<int>(buf & ((1u << k) - 1u));
        buf >>= k; cnt -= k;
        return v;
    }
};

// A canonical Huffman code as `puff` keeps it: the count of codes of each
// length and the symbols in code order.
struct Huff {
    std::uint16_t count[16]{};
    std::uint16_t sym[320]{};
};

// -> 0, or nonzero when the lengths over-subscribe the code (an incomplete
// code is allowed, as zlib allows it for a single distance code)
int build(Huff& h, const std::uint8_t* len, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int s = 0; s < n; ++s) h.count[len[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return -1;
    }
    std::uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<std::uint16_t>(offs[l] + h.count[l]);
    for (int s = 0; s < n; ++s)
        if (len[s]) h.sym[offs[len[s]]++] = static_cast<std::uint16_t>(s);
    return 0;
}

int decodeSym(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        code |= b.need(1);
        if (b.over) return -1;
        const int c = h.count[l];
        if (code - c < first) return h.sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr std::uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, std::vector<std::uint8_t>& out, const Huff& lit, const Huff& dist) {
    for (;;) {
        int s = decodeSym(b, lit);
        if (s < 0) return false;
        if (s < 256) { out.push_back(static_cast<std::uint8_t>(s)); continue; }
        if (s == 256) return true;
        s -= 257;
        if (s >= 29) return false;
        const int len = kLenBase[s] + b.need(kLenExtra[s]);
        const int ds = decodeSym(b, dist);
        if (ds < 0 || ds >= 30) return false;
        const std::size_t d = kDistBase[ds] + static_cast<std::size_t>(b.need(kDistExtra[ds]));
        if (b.over || d > out.size()) return false;
        const std::size_t from = out.size() - d;
        for (int k = 0; k < len; ++k) out.push_back(out[from + static_cast<std::size_t>(k)]);
    }
}

}  // namespace

bool zlibInflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) {
    if (in.size() < 2) return false;
    const unsigned cmf = in[0], flg = in[1];
    if ((cmf & 15) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    Bits b{in.data() + 2, in.size() - 2};
    int last = 0;
    do {
        last = b.need(1);
        const int type = b.need(2);
        if (b.over) return false;
        if (type == 0) {                                    // stored
            b.buf = 0; b.cnt = 0;                           // to the byte boundary
            if (b.at + 4 > b.n) return false;
            const unsigned len = b.p[b.at] | (b.p[b.at + 1] << 8);
            const unsigned nlen = b.p[b.at + 2] | (b.p[b.at + 3] << 8);
            b.at += 4;
            if ((len ^ 0xFFFFu) != nlen || b.at + len > b.n) return false;
            out.insert(out.end(), b.p + b.at, b.p + b.at + len);
            b.at += len;
        } else if (type == 1) {                             // fixed Huffman
            struct Fixed { Huff lit, dist; };
            static const Fixed fx = [] {
                Fixed f;
                std::uint8_t l[288];
                int s = 0;
                for (; s < 144; ++s) l[s] = 8;
                for (; s < 256; ++s) l[s] = 9;
                for (; s < 280; ++s) l[s] = 7;
                for (; s < 288; ++s) l[s] = 8;
                build(f.lit, l, 288);
                for (s = 0; s < 30; ++s) l[s] = 5;
                build(f.dist, l, 30);
                return f;
            }();
            if (!codes(b, out, fx.lit, fx.dist)) return false;
        } else if (type == 2) {                             // dynamic Huffman
            const int nlen = b.need(5) + 257, ndist = b.need(5) + 1, ncode = b.need(4) + 4;
            if (b.over || nlen > 286 || ndist > 30) return false;
            static constexpr std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                       11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t lens[320]{};
            for (int k = 0; k < ncode; ++k) lens[order[k]] = static_cast<std::uint8_t>(b.need(3));
            Huff lc;
            if (b.over || build(lc, lens, 19) != 0) return false;
            int k = 0;
            std::uint8_t all[320]{};
            while (k < nlen + ndist) {
                const int s = decodeSym(b, lc);
                if (s < 0) return false;
                if (s < 16) { all[k++] = static_cast<std::uint8_t>(s); continue; }
                int rep = 0;
                std::uint8_t v = 0;
                if (s == 16) {
                    if (k == 0) return false;
                    v = all[k - 1]; rep = 3 + b.need(2);
                } else if (s == 17) {
                    rep = 3 + b.need(3);
                } else {
                    rep = 11 + b.need(7);
                }
                if (b.over || k + rep > nlen + ndist) return false;
                while (rep--) all[k++] = v;
            }
            if (all[256] == 0) return false;                // no end-of-block code
            Huff lit, dist;
            if (build(lit, all, nlen) != 0 || build(dist, all + nlen, ndist) != 0) return false;
            if (!codes(b, out, lit, dist)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

namespace {
std::uint32_t be32(const std::byte* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
constexpr unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
}  // namespace

bool pngHeader(std::span<const std::byte> d, int& w, int& h, std::string& why) {
    if (d.size() < 33 || std::memcmp(d.data(), kSig, 8) != 0 ||
        std::memcmp(d.data() + 12, "IHDR", 4) != 0) {
        why = "not a PNG";
        return false;
    }
    const std::uint32_t W = be32(d.data() + 16), H = be32(d.data() + 20);
    const int depth = static_cast<int>(d[24]), ctype = static_cast<int>(d[25]);
    const int inter = static_cast<int>(d[28]);
    if (depth != 8 || (ctype != 2 && ctype != 6) || inter != 0) {
        why = "PNG of bit depth " + std::to_string(depth) + ", colour type " + std::to_string(ctype) +
              (inter ? ", interlaced" : "") + " - only 8-bit RGB or RGBA, not interlaced, is read";
        return false;
    }
    if (W == 0 || H == 0 || W > 16384 || H > 16384) {
        why = "PNG of " + std::to_string(W) + "x" + std::to_string(H);
        return false;
    }
    w = static_cast<int>(W);
    h = static_cast<int>(H);
    return true;
}

bool decodePng(std::span<const std::byte> d, PngImage& out, std::string& why) {
    int w = 0, h = 0;
    if (!pngHeader(d, w, h, why)) return false;
    const int ch = static_cast<int>(d[25]) == 6 ? 4 : 3;
    std::vector<std::uint8_t> z;
    std::size_t o = 8;
    while (o + 12 <= d.size()) {
        const std::uint32_t n = be32(d.data() + o);
        if (n > d.size() - o - 12) { why = "a chunk runs past the end of the file"; return false; }
        const char* tag = reinterpret_cast<const char*>(d.data() + o + 4);
        if (std::memcmp(tag, "IDAT", 4) == 0) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(d.data() + o + 8);
            z.insert(z.end(), p, p + n);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        o += 12 + n;
    }
    const std::size_t stride = static_cast<std::size_t>(w) * static_cast<std::size_t>(ch);
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * static_cast<std::size_t>(h));
    if (!zlibInflate(z, raw)) { why = "the image data does not inflate"; return false; }
    if (raw.size() < (stride + 1) * static_cast<std::size_t>(h)) {
        why = "the image data is short";
        return false;
    }
    out.width = w; out.height = h; out.channels = ch;
    out.px.assign(stride * static_cast<std::size_t>(h), 0);
    const std::size_t bpp = static_cast<std::size_t>(ch);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* src = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        std::uint8_t* row = out.px.data() + static_cast<std::size_t>(y) * stride;
        const std::uint8_t* up = y ? row - stride : nullptr;
        const int ft = src[0];
        ++src;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0;
            const int b = up ? up[i] : 0;
            const int c = (up && i >= bpp) ? up[i - bpp] : 0;
            int pr = 0;
            switch (ft) {
            case 0: pr = 0; break;
            case 1: pr = a; break;
            case 2: pr = b; break;
            case 3: pr = (a + b) >> 1; break;
            case 4: {
                const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: why = "scanline filter " + std::to_string(ft); return false;
            }
            row[i] = static_cast<std::uint8_t>(src[i] + pr);
        }
    }
    return true;
}

}  // namespace omk
