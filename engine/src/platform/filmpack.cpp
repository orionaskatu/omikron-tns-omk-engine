// SPDX-License-Identifier: GPL-3.0-or-later
// The film pack - see filmpack.h. Not a port of anything in the original.
#include "platform/filmpack.h"

#include "platform/datafs.h"
#include "platform/hostfs.h"

#include <cstdio>
#include <cstdlib>

namespace omk {

std::uint64_t filmKey(const std::string& path) {
    std::FILE* f = std::fopen(hostPath(path).c_str(), "rb");
    if (!f) return 0;
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint8_t b) { h = (h ^ b) * 0x100000001b3ull; };
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return 0; }
    for (int k = 0; k < 8; ++k)
        mix(static_cast<std::uint8_t>((static_cast<std::uint64_t>(size) >> (8 * k)) & 0xFF));
    unsigned char buf[65536];
    std::size_t left = 1u << 20;
    while (left > 0) {
        const std::size_t n = std::fread(buf, 1, left < sizeof buf ? left : sizeof buf, f);
        if (n == 0) break;
        for (std::size_t i = 0; i < n; ++i) mix(buf[i]);
        left -= n;
    }
    std::fclose(f);
    return h;
}

bool FilmPack::open(const std::string& dir) {
    dir_ = dir;
    keys_.clear();
    if (!hostfs::isDirectory(dir)) return false;
    for (const auto& name : hostfs::listNames(dir)) {
        if (name.size() != 20 || name.substr(16) != ".mpg") continue;
        char* end = nullptr;
        const unsigned long long k = std::strtoull(name.substr(0, 16).c_str(), &end, 16);
        if (end && *end == '\0') keys_.insert(k);
    }
    return active();
}

std::string FilmPack::find(const std::string& path) const {
    if (keys_.empty()) return "";
    const std::uint64_t k = filmKey(path);
    if (!k || !keys_.count(k)) return "";
    char name[24];
    std::snprintf(name, sizeof name, "/%016llx.mpg", static_cast<unsigned long long>(k));
    return dir_ + name;
}

FilmPack& filmPack() {
    static FilmPack p;
    return p;
}

}  // namespace omk
