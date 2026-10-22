// SPDX-License-Identifier: GPL-3.0-or-later
// THE FONT PACK's maker (`ui/fontpack.h`, the remaster's Tier E) - for
// `tools/omktexpack.py fonts`.
//
//     font_hd <gamedata> <tables> <outdir> [k [ttf FACE,FACE...]]
//
// For every face of the font table, `<outdir>/<key>.raw` - int32 w, int32 h,
// then w*h coverage bytes (0..255), laid out by `hdLayout` at `k` (8) - and a
// line "@fonthd NAME key k glyphs N ttf T residual R mass M", the residual the
// mean |coverage - original| once the HD glyphs are brought back down to the
// face's size (0..1, of full coverage), `mass` their summed coverage over the
// original's. Two sources, both the shipped data's:
//
//   * SUPER-RESOLUTION of the glyph's own coverage. The 0..31 levels are
//     ANTIALIASING - each says how much of the texel the letter covered - so
//     they place its contour to under a texel. A smooth (Catmull-Rom)
//     enlargement, its half-coverage contour drawn sharp: what the runtime's
//     own `fit` does with a bilinear one, so the gain is a rounder contour,
//     not new detail - there is no vector source for these faces.
//   * THE GAME'S VECTOR FONT, for the faces named after `ttf`: the disc
//     carries `FONTS/OMIKRON.TTF` ("Omikron One Medium", Loic Normand,
//     1998), the alien script MENUINTR and SNEAK were rasterised from. Each
//     glyph is drawn from its outline at a size fitted per face and moved to
//     where the shipped glyph's mass is; a glyph the outline does not match
//     (residual over `kTtfWorst`), or that the font lacks, falls back to the
//     super-resolution. Drawn by stb_truetype (`third_party/`), here only:
//     the engine never reads the TTF.
#include "formats/fnt.h"
#include "platform/datafs.h"
#include "ui/fontpack.h"
#include "ui/text.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr float kTtfWorst = 0.06f;   // a mean |difference| past this is another letter

struct Img {
    int w = 0, h = 0;
    std::vector<float> v;
    Img() = default;
    Img(int w_, int h_) : w(w_), h(h_), v(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0.0f) {}
    float& at(int x, int y) { return v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
    float get(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
        return v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    }
};

float cubic(float a, float b, float c, float d, float t) {   // Catmull-Rom
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

// `src` enlarged k times, each HD pixel's centre sampled (zero outside)
Img enlarge(const Img& src, int k) {
    Img o(src.w * k, src.h * k);
    for (int Y = 0; Y < o.h; ++Y) {
        const float fy = (Y + 0.5f) / k - 0.5f;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - y0;
        for (int X = 0; X < o.w; ++X) {
            const float fx = (X + 0.5f) / k - 0.5f;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - x0;
            float r[4];
            for (int j = 0; j < 4; ++j) {
                const int yy = y0 - 1 + j;
                r[j] = cubic(src.get(x0 - 1, yy), src.get(x0, yy), src.get(x0 + 1, yy), src.get(x0 + 2, yy), tx);
            }
            o.at(X, Y) = cubic(r[0], r[1], r[2], r[3], ty);
        }
    }
    return o;
}

Img reduce(const Img& hd, int k) {   // k x k averages
    Img o(hd.w / k, hd.h / k);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            float s = 0;
            for (int j = 0; j < k; ++j)
                for (int i = 0; i < k; ++i) s += hd.get(x * k + i, y * k + j);
            o.at(x, y) = s / static_cast<float>(k * k);
        }
    return o;
}

// the steep curve: about one and a half HD pixels of edge, the contour where
// the field crosses half
Img sharpen(const Img& f, int k) {
    Img o(f.w, f.h);
    const float g = static_cast<float>(k) / 1.5f;
    for (std::size_t i = 0; i < f.v.size(); ++i) {
        float t = std::clamp((f.v[i] - 0.5f) * g + 0.5f, 0.0f, 1.0f);
        o.v[i] = t * t * (3.0f - 2.0f * t);
    }
    return o;
}

float residual(const Img& a, const Img& b) {
    float s = 0;
    for (std::size_t i = 0; i < a.v.size(); ++i) s += std::fabs(a.v[i] - b.v[i]);
    return a.v.empty() ? 0.0f : s / static_cast<float>(a.v.size());
}

float mass(const Img& a) {
    float s = 0;
    for (const float x : a.v) s += x;
    return s;
}

// the glyph `k` times finer: its coverage enlarged smoothly and its contour -
// where that crosses half - drawn sharp. TRIED AND DROPPED (2026-10-22):
// back-projecting the difference after (Irani and Peleg) until the HD glyph
// averages down to the shipped one - it fits the 16 coverage levels'
// quantisation as if it were shape, and lumps the contours and BREAKS thin
// strokes (COMPUTER's "p", GENERIC1); a blur between passes did not cure it
Img superResolve(const Img& c, int k) {
    return sharpen(enlarge(c, k), k);
}

// the character code's Unicode: the faces index Windows-1252
int unicodeOf(int c) {
    static const int hi[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                               0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013,
                               0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};
    return c >= 0x80 && c < 0xA0 ? hi[c - 0x80] : c;
}

struct Ttf {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    bool ok = false;
};

// the outline of `cp` at `px` pixels an em, `k` times enlarged, with its mass
// moved to the centroid of `c` (in c's texels); empty when the font lacks it
Img fromOutline(const Ttf& t, int cp, float px, const Img& c, int k) {
    const int gi = stbtt_FindGlyphIndex(&t.info, cp);
    if (!gi) return {};
    const float sc = stbtt_ScaleForMappingEmToPixels(&t.info, px * static_cast<float>(k));
    // the shipped glyph's centroid, HD pixels
    float m = 0, cx = 0, cy = 0;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) { const float v = c.get(x, y); m += v; cx += v * (x + 0.5f); cy += v * (y + 0.5f); }
    if (m <= 0) return {};
    cx = cx / m * k; cy = cy / m * k;
    // the outline's, drawn once to find it, then again shifted to land on it
    const auto draw = [&](float sx, float sy, int& x0, int& y0, Img& out) {
        int x1, y1;
        stbtt_GetGlyphBitmapBoxSubpixel(&t.info, gi, sc, sc, sx, sy, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w <= 0 || h <= 0) return false;
        std::vector<unsigned char> b(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));
        stbtt_MakeGlyphBitmapSubpixel(&t.info, b.data(), w, h, w, sc, sc, sx, sy, gi);
        out = Img(w, h);
        for (std::size_t i = 0; i < b.size(); ++i) out.v[i] = b[i] / 255.0f;
        return true;
    };
    int x0, y0;
    Img r;
    if (!draw(0, 0, x0, y0, r)) return {};
    float rm = 0, rx = 0, ry = 0;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) { const float v = r.get(x, y); rm += v; rx += v * (x + 0.5f); ry += v * (y + 0.5f); }
    if (rm <= 0) return {};
    // where the outline's origin must go: its centroid onto the glyph's
    const float ox = cx - rx / rm - static_cast<float>(x0), oy = cy - ry / rm - static_cast<float>(y0);
    const float fx = ox - std::floor(ox), fy = oy - std::floor(oy);
    if (!draw(fx, fy, x0, y0, r)) return {};
    Img o(c.w * k, c.h * k);
    const int bx = static_cast<int>(std::floor(ox)) + x0, by = static_cast<int>(std::floor(oy)) + y0;
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x) {
            const int X = bx + x, Y = by + y;
            if (X >= 0 && Y >= 0 && X < o.w && Y < o.h) o.at(X, Y) = r.get(x, y);
        }
    return o;
}

Img coverageOf(const omk::Font& f, unsigned char code) {
    const omk::Glyph& g = f.glyphs[code];
    const auto cov = f.coverage(code);
    Img c(g.width, g.height);
    for (std::size_t i = 0; i < cov.size(); ++i) c.v[i] = static_cast<float>(static_cast<std::uint8_t>(cov[i]) & 31) / 31.0f;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: font_hd <gamedata> <tables> <outdir> [k [ttf FACE,FACE...]]\n");
        return 2;
    }
    const std::string data = argv[1], tables = argv[2], out = argv[3];
    const int k = argc > 4 ? std::atoi(argv[4]) : 8;
    if (k < 2 || k > 16 || (k & (k - 1))) { std::fprintf(stderr, "k: a power of two, 2..16\n"); return 2; }
    Ttf ttf;
    std::set<std::string> ttfFaces;
    if (argc > 6) {
        ttf.bytes = [&] {
            const auto b = omk::DataFs::readPath(argv[5]);
            return std::vector<unsigned char>(reinterpret_cast<const unsigned char*>(b.data()),
                                              reinterpret_cast<const unsigned char*>(b.data()) + b.size());
        }();
        ttf.ok = !ttf.bytes.empty() && stbtt_InitFont(&ttf.info, ttf.bytes.data(), stbtt_GetFontOffsetForIndex(ttf.bytes.data(), 0));
        if (!ttf.ok) { std::fprintf(stderr, "%s: not a TrueType font\n", argv[5]); return 1; }
        std::stringstream ss(argv[6]);
        for (std::string n; std::getline(ss, n, ',');) ttfFaces.insert(n);
    }
    const auto table = omk::FontTable::loadJson(tables + "/ui.json");
    const omk::DataFs fs(data);
    int faces = 0;
    for (const auto& face : table.all()) {
        const omk::Font f = omk::readFnt(fs.read("FONTS/" + face.name + ".FNT"));
        if (!f.valid) { std::fprintf(stderr, "%s: unreadable\n", face.name.c_str()); continue; }
        const auto lay = omk::hdLayout(f, k);
        std::vector<std::uint8_t> atlas(static_cast<std::size_t>(lay.w) * static_cast<std::size_t>(lay.h), 0);
        const bool useTtf = ttf.ok && ttfFaces.count(face.name);
        // the outline's size: the em, in the face's pixels, that fits its
        // letters and digits best - searched over a range of the face's height
        float em = 0;
        if (useTtf) {
            float best = 1e9f;
            for (float px = face.height * 0.5f; px <= face.height * 2.5f; px += 0.25f) {
                float err = 0;
                int n = 0;
                for (int c = '0'; c <= 'z'; ++c) {
                    if (!std::isalnum(c) || !f.glyphs[c].present) continue;
                    const Img cv = coverageOf(f, static_cast<unsigned char>(c));
                    const Img r = fromOutline(ttf, c, px, cv, 2);
                    if (r.v.empty()) continue;
                    err += residual(reduce(r, 2), cv);
                    ++n;
                }
                if (n && err / n < best) { best = err / n; em = px; }
            }
        }
        int glyphs = 0, fromTtf = 0;
        double res = 0, mIn = 0, mOut = 0, pixels = 0;
        for (int code = 0; code < static_cast<int>(omk::kFntGlyphs); ++code) {
            const omk::Glyph& g = f.glyphs[code];
            if (!g.present || g.width <= 0 || g.height <= 0 || f.coverage(static_cast<unsigned char>(code)).empty())
                continue;
            const Img c = coverageOf(f, static_cast<unsigned char>(code));
            Img hd;
            if (useTtf) {
                Img r = fromOutline(ttf, unicodeOf(code), em, c, k);
                if (!r.v.empty() && residual(reduce(r, k), c) <= kTtfWorst) { hd = std::move(r); ++fromTtf; }
            }
            if (hd.v.empty()) hd = superResolve(c, k);
            const Img back = reduce(hd, k);
            res += residual(back, c) * static_cast<double>(c.v.size());
            pixels += static_cast<double>(c.v.size());
            mIn += mass(c); mOut += mass(back);
            for (int y = 0; y < hd.h; ++y)
                for (int x = 0; x < hd.w; ++x)
                    atlas[static_cast<std::size_t>(lay.y[code] + y) * static_cast<std::size_t>(lay.w) +
                          static_cast<std::size_t>(lay.x[code] + x)] =
                        static_cast<std::uint8_t>(std::clamp(hd.get(x, y), 0.0f, 1.0f) * 255.0f + 0.5f);
            ++glyphs;
        }
        char key[17];
        std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(omk::fontKey(f)));
        const std::string path = out + "/" + key + ".raw";
        std::FILE* o = omk::safeOutputPath(path) ? std::fopen(path.c_str(), "wb") : nullptr;
        if (!o) { std::fprintf(stderr, "%s: cannot write\n", path.c_str()); return 1; }
        const std::int32_t wh[2] = {lay.w, lay.h};
        std::fwrite(wh, 4, 2, o);
        std::fwrite(atlas.data(), 1, atlas.size(), o);
        std::fclose(o);
        std::printf("@fonthd %s key %s k %d glyphs %d ttf %d em %.2f residual %.4f mass %.4f\n", face.name.c_str(), key,
                    k, glyphs, fromTtf, static_cast<double>(em), pixels > 0 ? res / pixels : 0.0,
                    mIn > 0 ? mOut / mIn : 0.0);
        ++faces;
    }
    return faces ? 0 : 1;
}
