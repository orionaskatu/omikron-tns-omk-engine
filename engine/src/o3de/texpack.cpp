// SPDX-License-Identifier: GPL-3.0-or-later
// The texture pack - see texpack.h. Not a port of anything in the original.
#include "o3de/texpack.h"

#include "o3de/greybank.h"
#include "platform/datafs.h"
#include "platform/events.h"
#include "platform/hostfs.h"
#include "platform/pngread.h"
#include "platform/threads.h"

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
    lights_.clear();
    refused_.clear();
    greyOf_.clear();
    opened_ = false;
    dir_ = dir;
    if (!hostfs::isDirectory(dir)) {
        std::fprintf(stderr, "texture pack: %s is not a directory - the textures stay as shipped\n",
                     dir.c_str());
        return false;
    }
    for (const auto& name : hostfs::listNames(dir)) {
        if (name.size() != 20 && name.size() != 22) continue;
        const std::string ext = name.substr(name.size() - 4);
        if (ext != ".png" && ext != ".PNG") continue;
        const bool light = name.size() == 22;
        if (light && name.compare(16, 2, "_e") != 0) continue;
        std::uint64_t k;
        if (hexKey(name.substr(0, 16), k)) (light ? lights_ : files_)[k] = dir + "/" + name;
    }
    opened_ = true;
    return true;
}

bool TexturePack::replacement(const Texture& t, std::vector<std::uint8_t>& rgba, int& w, int& h) const {
    const Texture* one[1] = {&t};
    auto r = replacements(one);
    if (!r[0].ok) return false;
    w = r[0].w; h = r[0].h;
    rgba = std::move(r[0].rgba);
    return true;
}

std::vector<PackImage> TexturePack::replacements(std::span<const Texture* const> ts, bool colour) const {
    std::vector<PackImage> out(ts.size());
    if (!opened_ || (files_.empty() && lights_.empty())) return out;
    // the lookups, here: the key and the file of every texture the pack holds
    struct Job { std::size_t at; std::uint64_t key; const std::string* file; const std::string* light;
                 std::string why; bool grey; };
    std::vector<Job> jobs;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        const Texture* t = ts[i];
        if (!t || !t->hasPixels()) continue;
        std::uint64_t key = textureKey(*t);
        // (a texture the pack holds nothing for is still a job for the hook)
        bool grey = false;
        if (!files_.count(key) && !lights_.count(key))
            if (const auto g = greyOf_.find(key); g != greyOf_.end()) { key = g->second; grey = true; }
        const auto it = files_.find(key);
        const auto lt = lights_.find(key);
        if (((it == files_.end() && lt == lights_.end()) && !lightHook_) || refused_.count(key)) continue;
        jobs.push_back({i, key, it != files_.end() ? &it->second : nullptr,
                        lt != lights_.end() ? &lt->second : nullptr, {}, grey});
    }
    // the reads and the decodes, on the pool: each job writes only its own
    // result and its own reason
    Threads::shared().parallelFor(0, jobs.size(), 1, [&](std::size_t from, std::size_t to) {
        for (std::size_t j = from; j < to; ++j) {
            Job& jb = jobs[j];
            const Texture& t = *ts[jb.at];
            PackImage& o = out[jb.at];
            if (jb.light) {
                // the light map: its red channel, any size the rule allows
                const auto lb = readWholeFile(*jb.light);
                PngImage li;
                std::string lwhy;
                if (!lb.empty() &&
                    decodePng(reinterpret_cast<const std::uint8_t*>(lb.data()), lb.size(), li, &lwhy) &&
                    powerOfTwoFactor(li.w, t.width) && powerOfTwoFactor(li.h, t.height)) {
                    o.ew = li.w; o.eh = li.h;
                    o.emissive.resize(static_cast<std::size_t>(li.w) * li.h);
                    for (std::size_t q = 0; q < o.emissive.size(); ++q) o.emissive[q] = li.rgba[4 * q];
                }
            }
            if (!jb.file || !colour) continue;
            const auto bytes = readWholeFile(*jb.file);
            PngImage img;
            if (bytes.empty()) { jb.why = "unreadable"; continue; }
            if (!decodePng(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), img, &jb.why))
                continue;
            if (!powerOfTwoFactor(img.w, t.width) || !powerOfTwoFactor(img.h, t.height)) {
                char b[96];
                std::snprintf(b, sizeof b, "%dx%d, not %dx%d times a power of two (1..16) each way",
                              img.w, img.h, t.width, t.height);
                jb.why = b;
                continue;
            }
            if (jb.grey) {
                std::uint8_t* p = img.rgba.data();
                for (std::size_t k = 0; k + 3 < img.rgba.size(); k += 4) {
                    const int y = lumaGrey(p[k], p[k + 1], p[k + 2]);
                    p[k] = p[k + 1] = p[k + 2] = static_cast<std::uint8_t>(y);
                    if (y == 0) p[k + 3] = 0;
                }
            }
            o.ok = true; o.w = img.w; o.h = img.h; o.rgba = std::move(img.rgba);
        }
    });
    if (lightHook_)
        Threads::shared().parallelFor(0, jobs.size(), 1, [&](std::size_t from, std::size_t to) {
            for (std::size_t j = from; j < to; ++j) lightHook_(*ts[jobs[j].at], out[jobs[j].at]);
        });
    // the refusals, here, once per key
    for (const Job& jb : jobs) {
        if (out[jb.at].ok || !jb.file || !colour) continue;
        std::fprintf(stderr, "texture pack: %s - %s; %s drawn as shipped\n", jb.file->c_str(),
                     jb.why.c_str(), ts[jb.at]->name.c_str());
        refused_.insert(jb.key);
    }
    return out;
}

void TexturePack::aliasGrey(const Texture& colour, const Texture& grey) {
    if (!opened_ || !colour.hasPixels() || !grey.hasPixels()) return;
    const std::uint64_t c = textureKey(colour);
    if (files_.count(c) || lights_.count(c)) greyOf_[textureKey(grey)] = c;
}

TexturePack& texturePack() {
    static TexturePack p;
    return p;
}

bool openTexturePack(const std::string& dir) {
    if (!texturePack().open(dir)) return false;
    std::printf("texture pack: %s - %zu replacement textures, %zu light maps. A REMASTER feature: it "
                "replaces the shipped pixels, drawn by Vulkan and GLES only (the software "
                "reference draws what the original drew)\n",
                dir.c_str(), texturePack().size(), texturePack().lightMaps());
    event("texpack.open", {{"dir", dir}, {"entries", static_cast<long>(texturePack().size())},
                           {"lights", static_cast<long>(texturePack().lightMaps())}});
    return true;
}

}  // namespace omk
