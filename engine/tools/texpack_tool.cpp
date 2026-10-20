// SPDX-License-Identifier: GPL-3.0-or-later
// THE REMASTER'S SIX ENGINE-SIDE HELPERS (`o3de/texpack.h`,
// `todo/remaster.md` B4) - for `tools/omktexpack.py` and for
// `verify.py: engine: texture pack`.
//
//     texpack_tool keys <gamedata>          every .3DO/.3DT pair under MESHES:
//                                           "<path> <material> <key> <w>x<h>"
//     texpack_tool png <in.png> <out.raw>   the engine's PNG reader's answer:
//                                           int32 w, int32 h, then w*h RGBA8
//     texpack_tool pngpar <in.png> <n>      the reader on the THREAD POOL: the
//                                           file decoded n times at once, first
//                                           use included - "pngpar n same k"
//     texpack_tool water <model.3DO>...     the remaster's water plane per set
//                                           (`o3de/renderer.h` waterPlane):
//                                           "<model> found y area triangles"
//     texpack_tool cube <file.cube>         the engine's .cube reader's answer:
//                                           "cube N first r g b last r g b", or why not
//     texpack_tool grey <pack> <model.3DO>  the GREYSCALE BANK through a pack:
//                                           each texture greyed as the viewer
//                                           greys it, aliased, and the pack's
//                                           answer compared with the engine's
//                                           own greyed texture enlarged by
//                                           nearest neighbour (a nearest pack)
//
// `keys` is the engine's half of a differential: the Python tool keys the same
// textures from its own decoder, and the two must agree on every one, or a
// pack made by one is never found by the other. `png` is the engine's reader
// on a file, so the Python tool can read an upscaler's PNG fast and the check
// can difference the reader against Python's zlib.
#include "formats/tex3dt.h"
#include "o3de/colourgrade.h"
#include "o3de/geom3do.h"
#include "o3de/greybank.h"
#include "o3de/renderer.h"
#include "o3de/texpack.h"
#include "platform/datafs.h"
#include "platform/pngread.h"
#include "platform/threads.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

int keys(const std::string& data) {
    fs::path meshes;
    for (const auto& e : fs::directory_iterator(data))
        if (e.is_directory() && lower(e.path().filename().string()) == "meshes") meshes = e.path();
    if (meshes.empty()) { std::fprintf(stderr, "no MESHES under %s\n", data.c_str()); return 1; }
    std::vector<fs::path> models;
    for (const auto& e : fs::recursive_directory_iterator(meshes))
        if (e.is_regular_file() && lower(e.path().extension().string()) == ".3do") models.push_back(e.path());
    std::sort(models.begin(), models.end());
    long n = 0;
    for (const auto& m : models) {
        fs::path t;
        for (const char* ext : {".3dt", ".3DT", ".3Dt"}) {
            auto c = m; c.replace_extension(ext);
            if (fs::exists(c)) { t = c; break; }
        }
        if (t.empty()) continue;
        const auto d = omk::DataFs::readPath(m.string());
        const auto tb = omk::DataFs::readPath(t.string());
        const auto tex = omk::textures(d, tb);
        const std::string rel = fs::relative(m, data).generic_string();
        for (std::size_t i = 0; i < tex.size(); ++i) {
            if (!tex[i].hasPixels()) continue;
            std::printf("%s %zu %s %dx%d\n", rel.c_str(), i,
                        omk::textureKeyHex(omk::textureKey(tex[i])).c_str(), tex[i].width, tex[i].height);
            ++n;
        }
    }
    std::fprintf(stderr, "%ld textures in %zu models\n", n, models.size());
    return 0;
}

int png(const std::string& in, const std::string& out) {
    const auto b = omk::readWholeFile(in);
    omk::PngImage img;
    std::string why;
    if (b.empty() || !omk::decodePng(reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), img, &why)) {
        std::fprintf(stderr, "%s: %s\n", in.c_str(), b.empty() ? "unreadable" : why.c_str());
        return 1;
    }
    if (!omk::safeOutputPath(out)) return 2;
    std::FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return 1;
    const std::int32_t hdr[2] = {img.w, img.h};
    std::fwrite(hdr, sizeof hdr, 1, f);
    std::fwrite(img.rgba.data(), 1, img.rgba.size(), f);
    std::fclose(f);
    return 0;
}

// A NEAREST pack (`omktexpack.py build --nearest N`) answers a grey texture
// with exactly the grey texture N times larger: the luma is per texel, so
// greying the enlargement is enlarging the grey - and the key (alpha 0 where
// the grey is 0) is `toRgbaKeyed` of the grey palette.
int grey(const std::string& pack, const std::string& model) {
    if (!omk::texturePack().open(pack)) return 1;
    fs::path m = model, t;
    for (const char* ext : {".3DT", ".3dt"}) { auto c = m; c.replace_extension(ext); if (fs::exists(c)) t = c; }
    const auto tex = omk::textures(omk::DataFs::readPath(model), omk::DataFs::readPath(t.string()));
    std::vector<omk::Texture> greyed;
    for (const auto& c : tex) {
        if (!c.hasPixels()) continue;
        omk::Texture g = c;
        g.pal = omk::greyPalette(c.pal);
        omk::texturePack().aliasGrey(c, g);
        greyed.push_back(g);
    }
    std::vector<const omk::Texture*> ptrs;
    for (const auto& g : greyed) ptrs.push_back(&g);
    const auto reps = omk::texturePack().replacements(ptrs);
    long replaced = 0, exact = 0;
    std::vector<std::uint8_t> want;
    for (std::size_t i = 0; i < greyed.size(); ++i) {
        if (!reps[i].ok) continue;
        ++replaced;
        const auto& g = greyed[i];
        const int n = reps[i].w / g.width;
        want.assign(static_cast<std::size_t>(g.width) * g.height * 4, 0);
        g.toRgbaKeyed(want.data());
        bool same = reps[i].h == g.height * n;
        for (int y = 0; same && y < reps[i].h; ++y)
            for (int x = 0; same && x < reps[i].w; ++x)
                same = std::memcmp(&reps[i].rgba[(static_cast<std::size_t>(y) * reps[i].w + x) * 4],
                                   &want[(static_cast<std::size_t>(y / n) * g.width + x / n) * 4], 4) == 0;
        exact += same;
    }
    std::printf("grey: %zu textures, %ld replaced, %ld exact\n", greyed.size(), replaced, exact);
    return 0;
}

// `TexturePack::replacements` decodes a pool on the pool: the reader's
// one-time state must survive its FIRST use being parallel (the fixed codes'
// race, 2026-10-20). Nothing is decoded before the parallel pass.
int pngpar(const std::string& in, int n) {
    const auto b = omk::readWholeFile(in);
    if (b.empty()) return 1;
    std::vector<omk::PngImage> out(static_cast<std::size_t>(n));
    std::vector<int> ok(static_cast<std::size_t>(n), 0);
    omk::Threads::shared().parallelFor(0, out.size(), 1, [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i)
            ok[i] = omk::decodePng(reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), out[i]) ? 1 : 0;
    });
    int same = 0;
    for (std::size_t i = 0; i < out.size(); ++i)
        same += ok[i] && ok[0] && out[i].rgba == out[0].rgba ? 1 : 0;
    std::printf("pngpar %d same %d\n", n, same);
    return 0;
}

int water(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) {
        const auto d = omk::DataFs::readPath(argv[i]);
        if (d.empty()) continue;
        const auto g = omk::buildGeometry(d, omk::DrawFilter::Engine);
        const auto w = omk::waterPlane(g);
        std::printf("%s %d %.1f %.0f %ld\n", fs::path(argv[i]).filename().string().c_str(), w.found ? 1 : 0,
                    static_cast<double>(w.y), w.area, w.triangles);
    }
    return 0;
}

int cube(const std::string& path) {
    omk::ColourLut lut;
    std::string why;
    if (!omk::parseCube(omk::readTextFile(path), lut, &why)) { std::printf("refused: %s\n", why.c_str()); return 0; }
    const std::size_t k = lut.rgb.size() - 3;
    std::printf("cube %d first %.6f %.6f %.6f last %.6f %.6f %.6f\n", lut.n, lut.rgb[0], lut.rgb[1], lut.rgb[2],
                lut.rgb[k], lut.rgb[k + 1], lut.rgb[k + 2]);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "keys" && argc == 3) return keys(argv[2]);
    if (mode == "png" && argc == 4) return png(argv[2], argv[3]);
    if (mode == "grey" && argc == 4) return grey(argv[2], argv[3]);
    if (mode == "cube" && argc == 3) return cube(argv[2]);
    if (mode == "pngpar" && argc == 4) return pngpar(argv[2], std::atoi(argv[3]));
    if (mode == "water" && argc >= 3) return water(argc, argv);
    std::fprintf(stderr, "usage: texpack_tool keys <gamedata>\n"
                         "       texpack_tool png <in.png> <out.raw>\n"
                         "       texpack_tool grey <pack> <model.3DO>\n");
    return 2;
}
