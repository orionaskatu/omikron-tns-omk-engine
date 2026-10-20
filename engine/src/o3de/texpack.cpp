// SPDX-License-Identifier: GPL-3.0-or-later
// The texture pack - see texpack.h. Not a port of anything in the original.
#include "o3de/texpack.h"

#include "platform/datafs.h"
#include "platform/events.h"
#include "platform/hostfs.h"
#include "platform/pngread.h"

#include <cstdio>

namespace omk {
namespace {

bool hexKey(const std::string& s, std::uint64_t& k) {
    if (s.size() != 16) return false;
    k = 0;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        k = (k << 4) | static_cast<std::uint64_t>(v);
    }
    return true;
}

// 1, 2, 4 ... 16
bool powerOfTwoFactor(int big, int small) {
    if (small <= 0 || big % small) return false;
    const int f = big / small;
    return f >= 1 && f <= 16 && !(f & (f - 1));
}

}  // namespace

std::uint64_t textureKey(const Texture& t) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint8_t b) { h ^= b; h *= 0x100000001b3ull; };
    for (const int v : {t.width, t.height})
        for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * i)));
    const std::size_t n = static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height);
    const std::uint8_t* idx = t.idx.data();
    for (std::size_t i = 0; i < n && i < t.idx.size(); ++i) mix(idx[i]);
    const std::uint8_t* pal = t.pal.data();
    for (std::size_t i = 0; i < 768; ++i) mix(i < t.pal.size() ? pal[i] : 0);
    return h;
}

std::string textureKeyHex(std::uint64_t key) {
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(key));
    return b;
}

bool TexturePack::open(const std::string& dir) {
    files_.clear();
    refused_.clear();
    opened_ = false;
    dir_ = dir;
    if (!hostfs::isDirectory(dir)) {
        std::fprintf(stderr, "texture pack: %s is not a directory - the textures stay as shipped\n",
                     dir.c_str());
        return false;
    }
    for (const auto& name : hostfs::listNames(dir)) {
        if (name.size() != 20) continue;
        const std::string ext = name.substr(16);
        if (ext != ".png" && ext != ".PNG") continue;
        std::uint64_t k;
        if (hexKey(name.substr(0, 16), k)) files_[k] = dir + "/" + name;
    }
    opened_ = true;
    return true;
}

bool TexturePack::replacement(const Texture& t, std::vector<std::uint8_t>& rgba, int& w, int& h) const {
    if (!opened_ || files_.empty() || !t.hasPixels()) return false;
    const std::uint64_t key = textureKey(t);
    const auto it = files_.find(key);
    if (it == files_.end() || refused_.count(key)) return false;
    const auto bytes = readWholeFile(it->second);
    PngImage img;
    std::string why;
    if (bytes.empty() ||
        !decodePng(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), img, &why)) {
        std::fprintf(stderr, "texture pack: %s - %s; %s drawn as shipped\n", it->second.c_str(),
                     bytes.empty() ? "unreadable" : why.c_str(), t.name.c_str());
        refused_.insert(key);
        return false;
    }
    if (!powerOfTwoFactor(img.w, t.width) || !powerOfTwoFactor(img.h, t.height)) {
        std::fprintf(stderr, "texture pack: %s is %dx%d, not %dx%d times a power of two "
                             "(1..16) each way; %s drawn as shipped\n",
                     it->second.c_str(), img.w, img.h, t.width, t.height, t.name.c_str());
        refused_.insert(key);
        return false;
    }
    w = img.w; h = img.h;
    rgba = std::move(img.rgba);
    return true;
}

TexturePack& texturePack() {
    static TexturePack p;
    return p;
}

bool openTexturePack(const std::string& dir) {
    if (!texturePack().open(dir)) return false;
    std::printf("texture pack: %s - %zu replacement textures. A REMASTER feature: it "
                "replaces the shipped pixels, drawn by Vulkan and GLES only (the software "
                "reference draws what the original drew)\n",
                dir.c_str(), texturePack().size());
    event("texpack.open", {{"dir", dir}, {"entries", static_cast<long>(texturePack().size())}});
    return true;
}

}  // namespace omk
