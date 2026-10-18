// SPDX-License-Identifier: GPL-3.0-or-later
#include "o3de/texpack.h"

#include "formats/mesh3do.h"
#include "formats/png.h"
#include "platform/datafs.h"
#include "platform/events.h"

#include <cctype>
#include <cstdio>
#include <set>

namespace omk {

namespace {
std::string lastPart(const std::string& p) {
    const auto cut = p.find_last_of("/\\:");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}
std::string lowerOf(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// the decoded replacements kept beyond what the pool uses, before the unused
// ones are let go
constexpr std::size_t kKeepBytes = std::size_t(1) << 30;
}  // namespace

std::vector<std::string> packFileNames(const std::vector<std::string>& names) {
    static const std::string safe =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _+.~-";
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::string s = names[i];
        for (char& c : s)
            if (safe.find(c) == std::string::npos) c = '_';
        for (std::size_t k = s.size(); k > 0 && (s[k - 1] == ' ' || s[k - 1] == '.'); --k) s[k - 1] = '_';
        if (s.empty()) s = "mat" + std::to_string(i);
        if (seen.count(lowerOf(s))) s += "~" + std::to_string(i);
        seen.insert(lowerOf(s));
        out.push_back(s);
    }
    return out;
}

std::uint64_t contentKey(const Texture& t) {
    if (!t.hasPixels()) return 0;
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const std::uint8_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    };
    const std::uint8_t sz[8] = {
        static_cast<std::uint8_t>(t.width), static_cast<std::uint8_t>(t.width >> 8),
        static_cast<std::uint8_t>(t.width >> 16), static_cast<std::uint8_t>(t.width >> 24),
        static_cast<std::uint8_t>(t.height), static_cast<std::uint8_t>(t.height >> 8),
        static_cast<std::uint8_t>(t.height >> 16), static_cast<std::uint8_t>(t.height >> 24)};
    mix(sz, 8);
    mix(t.idx.data(), static_cast<std::size_t>(t.width) * static_cast<std::size_t>(t.height));
    mix(t.pal.data(), 768);
    return h ? h : 1;
}

bool TexturePack::open(const std::string& dir, const std::string& dataRoot) {
    index_.clear();
    seen_.clear();
    const DataFs pack(dir), game(dataRoot);
    int models = 0, rejected = 0, strays = 0;
    std::map<int, int> scales;
    for (const std::string& gp : pack.subdirs(".")) {
        const std::string group = lastPart(gp);
        for (const std::string& mp : pack.subdirs(group)) {
            const std::string model = lastPart(mp);
            const std::string rel = "MESHES/" + group + "/" + model + ".3DO";
            const auto d = game.read(rel);
            const auto h = d.empty() ? std::nullopt : readHeader(d);
            if (!h) {
                std::printf("texture pack: %s/%s names no model of the game's MESHES - ignored\n",
                            group.c_str(), model.c_str());
                ++strays;
                continue;
            }
            const auto tex = textures(d, game.readSibling(rel, ".3dt"));
            std::vector<std::string> names;
            for (int i = 0; i < h->materials; ++i) {
                const auto m = readMaterial(d, *h, i);
                names.push_back(m ? std::string(m->name) : std::string());
            }
            const auto stems = packFileNames(names);
            bool any = false;
            for (std::size_t i = 0; i < tex.size() && i < stems.size(); ++i) {
                const auto png = pack.resolve(group + "/" + model + "/" + stems[i] + ".png");
                if (!png || !tex[i].hasPixels()) continue;
                const auto head = readFileRange(*png, 0, 33);
                int w = 0, ht = 0;
                std::string why;
                if (pngHeader(head, w, ht, why)) {
                    const int k = w / tex[i].width;
                    if (w % tex[i].width || ht % tex[i].height || ht / tex[i].height != k || k < 1 || k > 16)
                        why = std::to_string(w) + "x" + std::to_string(ht) + " is not 1..16 x the original's " +
                              std::to_string(tex[i].width) + "x" + std::to_string(tex[i].height);
                    else {
                        const std::uint64_t key = contentKey(tex[i]);
                        if (!index_.count(key)) {
                            Entry e;
                            e.path = *png; e.w = tex[i].width; e.h = tex[i].height; e.scale = k;
                            index_.emplace(key, std::move(e));
                            scales[k]++;
                        }
                        any = true;
                        continue;
                    }
                }
                std::printf("texture pack: %s - %s; the original stays\n", png->c_str(), why.c_str());
                ++rejected;
            }
            models += any;
        }
    }
    std::string by;
    for (const auto& [k, n] : scales) by += (by.empty() ? "" : ", ") + std::string("x") + std::to_string(k) + " " + std::to_string(n);
    std::printf("texture pack: %s - an ENHANCEMENT: %zu replacements over %d models%s%s, %d rejected, "
                "%d directories matching no model (the Vulkan and Direct3D 12 renderers)\n",
                dir.c_str(), index_.size(), models, by.empty() ? "" : " - ", by.c_str(), rejected, strays);
    event("texpack.load", {{"indexed", static_cast<long>(index_.size())}, {"models", models},
                           {"rejected", rejected}, {"strays", strays}});
    return !index_.empty();
}

bool TexturePack::decode(Entry& e) {
    e.tried = true;
    PngImage img;
    std::string why;
    const auto bytes = DataFs::readPath(e.path);
    if (!decodePng(bytes, img, why)) {
        if (failedTold_++ < 20) std::printf("texture pack: %s - %s; the original stays\n", e.path.c_str(), why.c_str());
        e.failed = true;
        return false;
    }
    if (img.width != e.w * e.scale || img.height != e.h * e.scale) {
        if (failedTold_++ < 20) std::printf("texture pack: %s - changed size since the start; the original stays\n", e.path.c_str());
        e.failed = true;
        return false;
    }
    // THE KEY, `toRgbaKeyed`'s: pure black (or an RGBA file's alpha < 128) is
    // (0,0,0,0), anything else opaque
    const std::size_t n = static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    e.rgba.assign(n * 4, 0);
    std::uint8_t* o = e.rgba.mutableData();
    const std::uint8_t* s = img.px.data();
    const int ch = img.channels;
    for (std::size_t k = 0; k < n; ++k, s += ch, o += 4) {
        const bool key = (s[0] | s[1] | s[2]) == 0 || (ch == 4 && s[3] < 128);
        if (key) continue;
        o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255;
    }
    decodedBytes_ += e.rgba.size();
    return true;
}

std::size_t TexturePack::apply(std::vector<Texture>& pool) {
    if (index_.empty()) return 0;
    ++applies_;
    std::size_t replaced = 0;
    for (Texture& t : pool) {
        if (!t.hasPixels()) continue;
        auto& s = seen_[{t.idx.data(), t.pal.data()}];
        if (!s.key) { s.idx = t.idx; s.pal = t.pal; s.key = contentKey(t); }
        const auto it = index_.find(s.key);
        if (it == index_.end()) continue;
        Entry& e = it->second;
        if (e.w != t.width || e.h != t.height) continue;
        if (!e.tried && !decode(e)) continue;
        if (e.failed || e.rgba.empty()) continue;
        e.age = applies_;
        t.packRgba = e.rgba;
        t.packScale = e.scale;
        ++replaced;
    }
    // what only this cache still holds is gone from every list
    for (auto it = seen_.begin(); it != seen_.end();)
        it = it->second.idx.useCount() == 1 ? seen_.erase(it) : std::next(it);
    // and the decoded pictures no pool uses go once they add up
    if (decodedBytes_ > kKeepBytes)
        for (auto& [key, e] : index_)
            if (!e.rgba.empty() && e.age != applies_) {
                decodedBytes_ -= e.rgba.size();
                e.rgba = PixelBuffer{};
                e.tried = false;
            }
    if (static_cast<long>(replaced) != lastReplaced_) {
        lastReplaced_ = static_cast<long>(replaced);
        std::printf("texture pack: %zu of the pool's %zu slots replaced\n", replaced, pool.size());
        event("texpack.pool", {{"replaced", static_cast<long>(replaced)}, {"slots", static_cast<long>(pool.size())}});
    }
    return replaced;
}

}  // namespace omk
