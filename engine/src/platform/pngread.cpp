// SPDX-License-Identifier: GPL-3.0-or-later
// PNG, read only - see pngread.h. Not a port of anything in the original.
// (the remaster's: compiled empty without OMK_REMASTER - `engine/Makefile`)
#if OMK_REMASTER

#include "platform/pngread.h"

#include <algorithm>
#include <cstring>

namespace omk {
namespace {

// ---- the bit reader: LSB first, as deflate packs them ---------------------
//
// Past the end it feeds zeros and counts them, so a decoder never reads out
// of bounds; `overrun()` then says whether any of those zeros were USED.
struct Bits {
    const std::uint8_t* p;
    const std::uint8_t* end;
    std::uint64_t buf = 0;
    int cnt = 0;
    std::size_t fed = 0;   // zero bytes fed past the end

    Bits(const std::uint8_t* s, std::size_t n) : p(s), end(s + n) {}
    void refill() {
        while (cnt <= 56) {
            std::uint64_t b = 0;
            if (p < end) b = *p++; else ++fed;
            buf |= b << cnt;
            cnt += 8;
        }
    }
    std::uint32_t get(int n) {
        if (n == 0) return 0;
        if (cnt < n) refill();
        const std::uint32_t v = static_cast<std::uint32_t>(buf & ((1ull << n) - 1));
        buf >>= n; cnt -= n;
        return v;
    }
    void alignByte() { const int k = cnt % 8; buf >>= k; cnt -= k; }
    // the zeros fed past the end that are no longer in the buffer were read
    bool overrun() const { return fed * 8 > static_cast<std::size_t>(cnt); }
    // the input position of the next unread byte (the buffer handed back)
    const std::uint8_t* bytePos() const { return p - (cnt / 8) + static_cast<std::ptrdiff_t>(fed); }
};

// ---- canonical Huffman: a 10-bit table, then puff's walk for longer codes --
constexpr int kFast = 10;
struct Huff {
    std::uint16_t fast[1 << kFast];   // sym | len << 9, 0 for "longer than kFast"
    std::uint16_t count[16];
    std::uint16_t symbol[320];

    bool build(const std::uint8_t* len, int n) {
        std::memset(fast, 0, sizeof fast);
        std::memset(count, 0, sizeof count);
        for (int s = 0; s < n; ++s) ++count[len[s]];
        if (count[0] == n) return true;   // no codes: valid only if never read
        int left = 1;
        for (int l = 1; l < 16; ++l) {
            left <<= 1;
            left -= count[l];
            if (left < 0) return false;   // over-subscribed
        }
        std::uint16_t offs[16];
        offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<std::uint16_t>(offs[l] + count[l]);
        for (int s = 0; s < n; ++s)
            if (len[s]) symbol[offs[len[s]]++] = static_cast<std::uint16_t>(s);
        // the fast table: each code's bits REVERSED (deflate sends a code's
        // first bit first, into the reader's lowest bit)
        std::uint16_t next[16];
        int code = 0;
        for (int l = 1; l < 16; ++l) { next[l] = static_cast<std::uint16_t>(code); code = (code + count[l]) << 1; }
        for (int s = 0; s < n; ++s) {
            const int l = len[s];
            if (!l) continue;
            const int c = next[l]++;
            if (l > kFast) continue;
            int r = 0;
            for (int i = 0; i < l; ++i) r |= ((c >> i) & 1) << (l - 1 - i);
            for (int k = r; k < (1 << kFast); k += 1 << l)
                fast[k] = static_cast<std::uint16_t>(s | (l << 9));
        }
        return true;
    }
    int decode(Bits& b) const {
        if (b.cnt < 16) b.refill();
        const std::uint16_t e = fast[b.buf & ((1u << kFast) - 1)];
        if (e) {
            const int l = e >> 9;
            b.buf >>= l; b.cnt -= l;
            return e & 0x1FF;
        }
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= static_cast<int>(b.get(1));
            const int c = count[l];
            if (code - c < first) return symbol[index + (code - first)];
            index += c; first += c;
            first <<= 1; code <<= 1;
        }
        return -1;
    }
};

constexpr std::uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                         4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                         7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// `limit`: the most bytes the stream may produce (0: no limit). Every symbol
// is checked against the input's end too - past it the reader feeds zeros,
// and zeros decode as literals for ever (a pack PNG hung the viewer so).
bool codes(Bits& b, const Huff& lit, const Huff& dist, std::vector<std::uint8_t>& out, std::size_t limit) {
    for (;;) {
        const int s = lit.decode(b);
        if (s < 0 || b.overrun()) return false;
        if (s < 256) {
            if (limit && out.size() >= limit) return false;
            out.push_back(static_cast<std::uint8_t>(s));
            continue;
        }
        if (s == 256) return !b.overrun();
        const int li = s - 257;
        if (li >= 29) return false;
        const std::size_t len = kLenBase[li] + b.get(kLenExtra[li]);
        const int ds = dist.decode(b);
        if (ds < 0 || ds >= 30) return false;
        const std::size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
        if (d > out.size() || b.overrun() || (limit && out.size() + len > limit)) return false;
        const std::size_t from = out.size() - d;
        out.resize(out.size() + len);
        std::uint8_t* o = out.data();
        const std::size_t to = out.size() - len;
        for (std::size_t i = 0; i < len; ++i) o[to + i] = o[from + i];   // overlap is the point
    }
}

// THE FIXED CODES (RFC 1951 3.2.6), built ONCE and thread-safely: a pool's
// replacements decode on several threads at a time (`TexturePack::
// replacements`), and the first version built these lazily behind a plain
// bool - two workers rebuilt them at once, one decoded through a table the
// other was rewriting, and the viewer hung (2026-10-20). A function-local
// static's initialisation is the language's own once.
struct FixedCodes {
    Huff lit, dist;
    FixedCodes() {
        std::uint8_t l[288];
        for (int s = 0; s < 144; ++s) l[s] = 8;
        for (int s = 144; s < 256; ++s) l[s] = 9;
        for (int s = 256; s < 280; ++s) l[s] = 7;
        for (int s = 280; s < 288; ++s) l[s] = 8;
        lit.build(l, 288);
        std::uint8_t d[30];
        for (int s = 0; s < 30; ++s) d[s] = 5;
        dist.build(d, 30);
    }
};

bool inflateRaw(Bits& b, std::vector<std::uint8_t>& out, std::size_t limit) {
    static const FixedCodes fixed;
    const Huff& fixedLit = fixed.lit;
    const Huff& fixedDist = fixed.dist;
    int last = 0;
    do {
        last = static_cast<int>(b.get(1));
        const int type = static_cast<int>(b.get(2));
        if (type == 0) {
            b.alignByte();
            const std::uint32_t len = b.get(16), nlen = b.get(16);
            if ((len ^ 0xFFFFu) != nlen) return false;
            if (limit && out.size() + len > limit) return false;
            for (std::uint32_t i = 0; i < len; ++i) out.push_back(static_cast<std::uint8_t>(b.get(8)));
            if (b.overrun()) return false;
        } else if (type == 1) {
            if (!codes(b, fixedLit, fixedDist, out, limit)) return false;
        } else if (type == 2) {
            const int nlen = static_cast<int>(b.get(5)) + 257;
            const int ndist = static_cast<int>(b.get(5)) + 1;
            const int ncode = static_cast<int>(b.get(4)) + 4;
            if (nlen > 286 || ndist > 30) return false;
            static constexpr std::uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                                      11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::uint8_t cl[19] = {};
            for (int i = 0; i < ncode; ++i) cl[order[i]] = static_cast<std::uint8_t>(b.get(3));
            Huff lc;
            if (!lc.build(cl, 19)) return false;
            std::uint8_t lens[320] = {};
            int i = 0;
            while (i < nlen + ndist) {
                const int s = lc.decode(b);
                if (s < 0) return false;
                if (s < 16) { lens[i++] = static_cast<std::uint8_t>(s); continue; }
                int rep = 0; std::uint8_t v = 0;
                if (s == 16) { if (i == 0) return false; v = lens[i - 1]; rep = 3 + static_cast<int>(b.get(2)); }
                else if (s == 17) rep = 3 + static_cast<int>(b.get(3));
                else rep = 11 + static_cast<int>(b.get(7));
                if (i + rep > nlen + ndist) return false;
                while (rep--) lens[i++] = v;
            }
            if (lens[256] == 0) return false;   // no end-of-block code
            Huff lit, dist;
            if (!lit.build(lens, nlen) || !dist.build(lens + nlen, ndist)) return false;
            if (!codes(b, lit, dist, out, limit)) return false;
        } else {
            return false;
        }
        if (b.overrun()) return false;
    } while (!last);
    return !b.overrun();
}

std::uint32_t adler32(const std::uint8_t* p, std::size_t n) {
    std::uint32_t a = 1, s = 0;
    while (n) {
        const std::size_t k = n < 5552 ? n : 5552;
        for (std::size_t i = 0; i < k; ++i) { a += p[i]; s += a; }
        a %= 65521u; s %= 65521u;
        p += k; n -= k;
    }
    return (s << 16) | a;
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

bool fail(std::string* why, const char* w) { if (why) *why = w; return false; }

constexpr std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

}  // namespace

bool zlibInflate(const std::uint8_t* src, std::size_t n, std::vector<std::uint8_t>& out,
                 std::size_t expect) {
    out.clear();
    if (n < 6) return false;
    const unsigned cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return false;
    // reserved at most at deflate's own ratio (1032:1) over the bytes there
    // are: a header naming 16384x16384 over a few bytes of data reserved
    // ~1 GiB before inflating anything (the remaster audit's item 7)
    if (expect) out.reserve(std::min(expect, n * 1032));
    Bits b(src + 2, n - 2);
    // `expect`, when given, is also the LIMIT: a PNG's image data is exactly it
    if (!inflateRaw(b, out, expect)) return false;
    b.alignByte();
    const std::uint8_t* tail = b.bytePos();
    if (tail + 4 > src + n) return false;
    return be32(tail) == adler32(out.data(), out.size());
}

bool pngSize(const std::uint8_t* src, std::size_t n, int& w, int& h) {
    if (n < 33 || std::memcmp(src, kSig, 8) != 0 || std::memcmp(src + 12, "IHDR", 4) != 0) return false;
    const std::uint32_t W = be32(src + 16), H = be32(src + 20);
    if (W == 0 || H == 0 || W > 16384 || H > 16384) return false;
    w = static_cast<int>(W); h = static_cast<int>(H);
    return true;
}

bool decodePng(const std::uint8_t* src, std::size_t n, PngImage& out, std::string* why) {
    out = PngImage{};
    int W = 0, H = 0;
    if (!pngSize(src, n, W, H)) return fail(why, "not a PNG, or no usable IHDR");
    const int depth = src[24], ctype = src[25], interlace = src[28];
    if (depth != 8) return fail(why, "bit depth is not 8");
    if (interlace != 0) return fail(why, "interlaced");
    int ch = 0;
    switch (ctype) {
        case 0: ch = 1; break;
        case 2: ch = 3; break;
        case 3: ch = 1; break;
        case 4: ch = 2; break;
        case 6: ch = 4; break;
        default: return fail(why, "unknown colour type");
    }
    std::vector<std::uint8_t> idat;
    std::uint8_t plte[256 * 3] = {};
    std::uint8_t trns[256];
    std::memset(trns, 255, sizeof trns);
    int nplte = 0;
    std::size_t at = 8;
    bool ended = false;
    while (at + 12 <= n) {
        const std::uint32_t len = be32(src + at);
        const std::uint8_t* type = src + at + 4;
        const std::uint8_t* data = src + at + 8;
        if (len > n - at - 12) return fail(why, "a chunk runs past the file");
        if (!std::memcmp(type, "IDAT", 4)) idat.insert(idat.end(), data, data + len);
        else if (!std::memcmp(type, "PLTE", 4)) {
            if (len % 3 || len > 768) return fail(why, "bad PLTE");
            std::memcpy(plte, data, len); nplte = static_cast<int>(len / 3);
        } else if (!std::memcmp(type, "tRNS", 4) && ctype == 3) {
            std::memcpy(trns, data, len < 256 ? len : 256);
        } else if (!std::memcmp(type, "IEND", 4)) { ended = true; break; }
        at += 12 + len;
    }
    if (!ended) return fail(why, "no IEND");
    if (ctype == 3 && nplte == 0) return fail(why, "palette image without PLTE");
    const std::size_t stride = static_cast<std::size_t>(W) * ch;
    std::vector<std::uint8_t> raw;
    if (!zlibInflate(idat.data(), idat.size(), raw, (stride + 1) * H))
        return fail(why, "the image data does not inflate");
    if (raw.size() != (stride + 1) * H) return fail(why, "the image data is the wrong size");

    // un-filter in place, row by row (PNG 9.2): Sub, Up, Average, Paeth
    for (int y = 0; y < H; ++y) {
        std::uint8_t* row = raw.data() + y * (stride + 1);
        const std::uint8_t* prev = y ? row - (stride + 1) + 1 : nullptr;
        const int f = row[0];
        std::uint8_t* r = row + 1;
        switch (f) {
            case 0: break;
            case 1: for (std::size_t i = ch; i < stride; ++i) r[i] = static_cast<std::uint8_t>(r[i] + r[i - ch]); break;
            case 2: if (prev) for (std::size_t i = 0; i < stride; ++i) r[i] = static_cast<std::uint8_t>(r[i] + prev[i]); break;
            case 3:
                for (std::size_t i = 0; i < stride; ++i) {
                    const int a = i >= static_cast<std::size_t>(ch) ? r[i - ch] : 0, b = prev ? prev[i] : 0;
                    r[i] = static_cast<std::uint8_t>(r[i] + ((a + b) >> 1));
                }
                break;
            case 4:
                for (std::size_t i = 0; i < stride; ++i) {
                    const int a = i >= static_cast<std::size_t>(ch) ? r[i - ch] : 0;
                    const int b = prev ? prev[i] : 0;
                    const int c = (prev && i >= static_cast<std::size_t>(ch)) ? prev[i - ch] : 0;
                    const int p = a + b - c;
                    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
                    const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    r[i] = static_cast<std::uint8_t>(r[i] + pr);
                }
                break;
            default: return fail(why, "unknown row filter");
        }
    }
    out.w = W; out.h = H;
    out.rgba.resize(static_cast<std::size_t>(W) * H * 4);
    std::uint8_t* o = out.rgba.data();
    for (int y = 0; y < H; ++y) {
        const std::uint8_t* r = raw.data() + y * (stride + 1) + 1;
        for (int x = 0; x < W; ++x, o += 4) {
            switch (ctype) {
                case 0: o[0] = o[1] = o[2] = r[x]; o[3] = 255; break;
                case 2: o[0] = r[3 * x]; o[1] = r[3 * x + 1]; o[2] = r[3 * x + 2]; o[3] = 255; break;
                case 3: {
                    const int k = r[x];
                    if (k >= nplte) return fail(why, "a palette index past PLTE");
                    o[0] = plte[3 * k]; o[1] = plte[3 * k + 1]; o[2] = plte[3 * k + 2]; o[3] = trns[k];
                    break;
                }
                case 4: o[0] = o[1] = o[2] = r[2 * x]; o[3] = r[2 * x + 1]; break;
                default: std::memcpy(o, r + 4 * x, 4); break;
            }
        }
    }
    return true;
}

}  // namespace omk

#endif  // OMK_REMASTER
