// SPDX-License-Identifier: GPL-3.0-or-later
// THE TEXTURE PACK'S TWO ENGINE-SIDE HELPERS (`o3de/texpack.h`,
// `todo/remaster.md` B4) - for `tools/omktexpack.py` and for
// `verify.py: engine: texture pack`.
//
//     texpack_tool keys <gamedata>          every .3DO/.3DT pair under MESHES:
//                                           "<path> <material> <key> <w>x<h>"
//     texpack_tool png <in.png> <out.raw>   the engine's PNG reader's answer:
//                                           int32 w, int32 h, then w*h RGBA8
//
// `keys` is the engine's half of a differential: the Python tool keys the same
// textures from its own decoder, and the two must agree on every one, or a
// pack made by one is never found by the other. `png` is the engine's reader
// on a file, so the Python tool can read an upscaler's PNG fast and the check
// can difference the reader against Python's zlib.
#include "formats/tex3dt.h"
#include "o3de/texpack.h"
#include "platform/datafs.h"
#include "platform/pngread.h"

#include <algorithm>
#include <cstdio>
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

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "keys" && argc == 3) return keys(argv[2]);
    if (mode == "png" && argc == 4) return png(argv[2], argv[3]);
    std::fprintf(stderr, "usage: texpack_tool keys <gamedata>\n"
                         "       texpack_tool png <in.png> <out.raw>\n");
    return 2;
}
