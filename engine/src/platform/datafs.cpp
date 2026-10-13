// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/datafs.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>

#include "platform/hostfs.h"

namespace omk {

namespace {

// ---- the platform layer: `platform/hostfs.h`, one file per host ---------
using hostfs::isDirectory;
using hostfs::pathExists;
using hostfs::listNames;
std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char last = a.back();
    return (last == '/' || last == '\\' || last == ':') ? a + b : a + "/" + b;
}
// The last component and the rest, on either separator.
std::string baseName(const std::string& p) {
    const auto cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}
std::string parentOf(const std::string& p) {
    const auto cut = p.find_last_of("/\\");
    return cut == std::string::npos ? std::string() : p.substr(0, cut);
}
// ".ext" of the last component, empty when it has none.
std::string extensionOf(const std::string& p) {
    const std::string b = baseName(p);
    const auto dot = b.rfind('.');
    return (dot == std::string::npos || dot == 0) ? std::string() : b.substr(dot);
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(std::string_view rel) {
    std::vector<std::string> parts;
    std::string cur;
    const auto flush = [&] {
        // "" and "." both mean the directory itself; ".." is not accepted -
        // nothing in the game's data references a parent, and allowing it
        // would let a data string escape the root
        if (!cur.empty() && cur != ".") parts.push_back(cur);
        cur.clear();
    };
    for (char c : rel) {
        if (c == '/' || c == '\\') { flush(); }   // the data uses backslashes
        else                         { cur.push_back(c); }
    }
    flush();
    return parts;
}

std::vector<std::byte> slurp(const std::string& real) { return readWholeFile(real); }

}  // namespace

const std::map<std::string, std::string>* DataFs::indexOf(
        const std::string& dir) const {
    if (const auto it = index_.find(dir); it != index_.end()) return &it->second;
    if (!isDirectory(dir)) return nullptr;
    std::map<std::string, std::string> m;
    for (const auto& name : listNames(dir))
        m.emplace(lower(name), name);
    return &index_.emplace(dir, std::move(m)).first->second;
}

std::optional<std::string> DataFs::resolve(std::string_view rel) const {
    std::string at = root_;
    for (const auto& part : split(rel)) {
        const auto* idx = indexOf(at);
        if (!idx) return std::nullopt;
        const auto it = idx->find(lower(part));
        if (it == idx->end()) return std::nullopt;
        at = join(at, it->second);
    }
    return at;
}

std::vector<std::byte> DataFs::read(std::string_view rel) const {
    const auto real = resolve(rel);
    return real ? slurp(*real) : std::vector<std::byte>{};
}

std::optional<std::string> DataFs::resolveSibling(std::string_view rel,
                                                  std::string_view ext) const {
    std::string p(rel);
    std::string e(ext);
    if (!e.empty() && e.front() != '.') e.insert(e.begin(), '.');
    // replace_extension: drop the last component's extension, add this one
    const std::string old = extensionOf(p);
    if (!old.empty()) p.erase(p.size() - old.size());
    return resolve(p + e);
}

std::vector<std::byte> DataFs::readSibling(std::string_view rel,
                                           std::string_view ext) const {
    const auto real = resolveSibling(rel, ext);
    return real ? slurp(*real) : std::vector<std::byte>{};
}

std::vector<std::string> DataFs::list(std::string_view relDir,
                                      std::string_view ext) const {
    std::vector<std::string> out;
    const auto dir = resolve(relDir);
    if (!dir) return out;
    const auto* idx = indexOf(*dir);
    if (!idx) return out;
    std::string want = lower(std::string(ext));
    if (!want.empty() && want.front() != '.') want.insert(want.begin(), '.');
    for (const auto& [lc, real] : *idx) {
        const auto dot = lc.rfind('.');
        if (dot == std::string::npos) continue;
        if (lc.substr(dot) == want) out.push_back(join(*dir, real));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> DataFs::subdirs(std::string_view relDir) const {
    std::vector<std::string> out;
    const auto dir = resolve(relDir);
    if (!dir) return out;
    const auto* idx = indexOf(*dir);
    if (!idx) return out;
    for (const auto& [lc, real] : *idx) {
        (void)lc;
        const auto p = join(*dir, real);
        if (isDirectory(p)) out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::byte> DataFs::readPath(const std::string& path) {
    const auto r = findPath(path);
    return r ? slurp(*r) : std::vector<std::byte>{};
}

std::optional<std::string> DataFs::findPath(const std::string& path) {
    if (pathExists(path)) return path;
    // fall back to a case-insensitive lookup of the last component
    const std::string parent = parentOf(path);
    const auto dir = parent.empty() ? std::string(".") : parent;
    DataFs fsys(dir);
    return fsys.resolve(baseName(path));
}

bool safeOutputPath(const std::string& path) {
    // Two independent tests, because either alone has a hole: an extension
    // test misses a data file with an unusual name, and a location test misses
    // a shipped asset copied somewhere else.
    std::string ext = extensionOf(path);
    for (auto& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    static const char* kData[] = {".3DO", ".3DT", ".3DM", ".3DA", ".3DP",
                                  ".SCX", ".SFX", ".CTL", ".ANI", ".ADP",
                                  ".FNT", ".TAG", ".EXE", ".DLL", ".MPG"};
    for (const char* d : kData) {
        if (ext == d) {
            std::fprintf(stderr,
                "refusing to write %s: %s is a shipped game-data extension, so "
                "this is almost certainly an argument in the wrong position\n",
                path.c_str(), ext.c_str());
            return false;
        }
    }
    // And anything inside a directory that looks like the shipped tree.
    const std::string abs = hostfs::comparablePath(path);
    for (const char* dir : {"/MESHES/", "/IAM/", "/SCPTDATA/", "/FONTS/",
                            "/TRACKS/", "/I2D/", "/IMAGES/", "/FLIS/"}) {
        if (abs.find(dir) != std::string::npos) {
            std::fprintf(stderr,
                "refusing to write %s: it is inside the shipped data tree\n",
                path.c_str());
            return false;
        }
    }
    return true;
}

std::vector<std::byte> readWholeFile(const std::string& path) {
    const long long size = fileSize(path);
    if (size < 0) return {};
    std::vector<std::byte> d(static_cast<std::size_t>(size));
    long handle = -1;
    void* file = nullptr;
    if (!hostfs::openRead(path, handle, file)) return {};
    const long long r = hostfs::readAt(handle, file, 0, d.data(), d.size());
    hostfs::closeFile(handle, file);
    const std::size_t got = r > 0 ? static_cast<std::size_t>(r) : 0;
    if (got < d.size()) {
        std::printf("datafs: SHORT READ %s - %lu of %lu bytes\n", path.c_str(),
                    static_cast<unsigned long>(got), static_cast<unsigned long>(d.size()));
        d.resize(got);
    }
    return d;
}

std::vector<std::byte> readFileRange(const std::string& path, std::uint64_t offset,
                                     std::size_t size) {
    const long long fsize = fileSize(path);
    if (fsize < 0 || offset >= static_cast<std::uint64_t>(fsize)) return {};
    const std::uint64_t left = static_cast<std::uint64_t>(fsize) - offset;
    std::vector<std::byte> d(static_cast<std::size_t>(std::min<std::uint64_t>(size, left)));
    long handle = -1;
    void* file = nullptr;
    if (!hostfs::openRead(path, handle, file)) return {};
    const long long r = hostfs::readAt(handle, file, offset, d.data(), d.size());
    hostfs::closeFile(handle, file);
    if (r < 0) return {};                    // the seek failed
    const std::size_t got = static_cast<std::size_t>(r);
    if (got < d.size()) {
        std::printf("datafs: SHORT READ %s - %lu of %lu bytes at %llu\n", path.c_str(),
                    static_cast<unsigned long>(got), static_cast<unsigned long>(d.size()),
                    static_cast<unsigned long long>(offset));
        d.resize(got);
    }
    return d;
}

bool HostFile::open(const std::string& path) {
    close();
    size_ = fileSize(path);
    if (size_ < 0) return false;
    if (!hostfs::openRead(path, handle_, file_)) return false;
    open_ = true;
    return true;
}

void HostFile::close() {
    if (!open_) return;
    hostfs::closeFile(handle_, file_);
    open_ = false;
    handle_ = -1;
    file_ = nullptr;
}

std::size_t HostFile::readAt(std::uint64_t offset, void* dst, std::size_t n) {
    if (!open_ || size_ < 0 || offset >= static_cast<std::uint64_t>(size_)) return 0;
    n = static_cast<std::size_t>(std::min<std::uint64_t>(n, static_cast<std::uint64_t>(size_) - offset));
    const long long r = hostfs::readAt(handle_, file_, offset, static_cast<std::byte*>(dst), n);
    return r > 0 ? static_cast<std::size_t>(r) : 0;
}

std::string readTextFile(const std::string& path) {
    const auto d = readWholeFile(path);
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t j = text.find('\n', i);
        if (j == std::string::npos) { out.push_back(text.substr(i)); break; }
        out.push_back(text.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool writeWholeFile(const std::string& path, const void* data, std::size_t n) {
    std::FILE* f = std::fopen(hostPath(path).c_str(), "wb");
    if (!f) return false;
    const bool all = n == 0 || std::fwrite(data, 1, n, f) == n;
    return std::fclose(f) == 0 && all;
}

}  // namespace omk
