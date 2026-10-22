// SPDX-License-Identifier: GPL-3.0-or-later
// The font pack - see fontpack.h. Not a port of anything in the original.
#include "ui/fontpack.h"

#include "platform/datafs.h"
#include "platform/events.h"
#include "platform/hostfs.h"
#include "platform/pngread.h"
#include "platform/sync.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace omk {

std::uint64_t fontKey(const Font& f) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const std::byte b : f.blob) h = (h ^ static_cast<std::uint8_t>(b)) * 0x100000001b3ull;
    return h;
}

HdLayout hdLayout(const Font& f, int k) {
    HdLayout l;
    l.k = k;
    l.w = kFontAtlasW;
    int x = 0, y = 0, shelf = 0;
    for (std::size_t c = 0; c < kFntGlyphs; ++c) {
        const Glyph& g = f.glyphs[c];
        if (!g.present || g.width <= 0 || g.height <= 0) continue;
        const int w = g.width * k, h = g.height * k;
        if (x > 0 && x + w > kFontAtlasW) { x = 0; y += shelf + kFontGap; shelf = 0; }
        l.x[c] = x; l.y[c] = y;
        x += w + kFontGap;
        shelf = std::max(shelf, h);
    }
    l.h = y + shelf;
    return l;
}

struct FontPack::Impl {
    std::string dir;
    std::set<std::uint64_t> keys;
    std::map<const Font*, std::shared_ptr<const HdFont>> byFont;   // null: looked, none
    Stats st;
    Mutex m;
};

bool FontPack::open(const std::string& dir) {
    active_ = false;
    impl_ = std::make_shared<Impl>();
    impl_->dir = dir;
    if (!hostfs::isDirectory(dir)) return false;
    for (const auto& name : hostfs::listNames(dir)) {
        if (name.size() != 20 || name.substr(16) != ".png") continue;
        char* end = nullptr;
        const unsigned long long k = std::strtoull(name.substr(0, 16).c_str(), &end, 16);
        if (end && *end == '\0') impl_->keys.insert(k);
    }
    active_ = !impl_->keys.empty();
    return active_;
}

std::size_t FontPack::files() const { return impl_ ? impl_->keys.size() : 0; }

FontPack::Stats FontPack::stats() const {
    if (!impl_) return {};
    ScopedLock l(impl_->m);
    return impl_->st;
}

std::shared_ptr<const HdFont> FontPack::find(const Font& f) {
    if (!active_ || !f.valid) return nullptr;
    ScopedLock l(impl_->m);
    const auto it = impl_->byFont.find(&f);
    if (it != impl_->byFont.end()) return it->second;
    std::shared_ptr<const HdFont> out;
    const std::uint64_t key = fontKey(f);
    if (impl_->keys.count(key)) {
        char name[24];
        std::snprintf(name, sizeof name, "/%016llx.png", static_cast<unsigned long long>(key));
        const auto bytes = DataFs::readPath(impl_->dir + name);
        PngImage img;
        if (!bytes.empty() && decodePng(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), img)) {
            for (int k = 2; k <= 16 && !out; k *= 2) {
                auto lay = hdLayout(f, k);
                if (lay.w != img.w || lay.h != img.h) continue;
                auto hd = std::make_shared<HdFont>();
                hd->lay = lay;
                hd->cov.resize(static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h));
                for (std::size_t i = 0; i < hd->cov.size(); ++i) hd->cov[i] = img.rgba[4 * i];
                out = std::move(hd);
            }
        }
        if (out) {
            ++impl_->st.decoded;
            event("fontpack.face", {{"key", std::string(name + 1, 16)}, {"k", std::to_string(out->lay.k)}});
        } else {
            ++impl_->st.refused;   // unreadable, or no `k` lays the face out at its size
        }
    }
    impl_->byFont.emplace(&f, out);
    return out;
}

FontPack& fontPack() {
    static FontPack p;
    return p;
}

}  // namespace omk
