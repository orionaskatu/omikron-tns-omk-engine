// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST FILE SYSTEM on the desktops and the 3DS: `std::filesystem` and C
// stdio (`platform/hostfs.h`). The Vita and classic Mac OS have their own.
#if !defined(__vita__) && !defined(macintosh)

#include "platform/datafs.h"
#include "platform/hostfs.h"

#include <cstdio>
#include <filesystem>
namespace fs = std::filesystem;

namespace omk {

std::string hostPath(const std::string& p) {
    return p;
}

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return true;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return fs::is_directory(dir, ec);
}

long long fileSize(const std::string& path) {
    std::error_code ec;
    const auto n = fs::file_size(path, ec);
    return ec ? -1 : static_cast<long long>(n);
}

namespace hostfs {

bool isDirectory(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool pathExists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::vector<std::string> listNames(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        out.push_back(e.path().filename().string());
    return out;
}

std::string comparablePath(const std::string& path) {
    std::error_code ec;
    return fs::weakly_canonical(fs::path(path), ec).string();
}

bool openRead(const std::string& path, long&, void*& file) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    file = f;
    return true;
}

void closeFile(long, void* file) {
    std::fclose(static_cast<std::FILE*>(file));
}

long long readAt(long, void* file, std::uint64_t offset, std::byte* dst, std::size_t n) {
    std::FILE* f = static_cast<std::FILE*>(file);
    // an archive is far under 2 GB (`IAM\AREA` 1.2 MB), so `long` holds it
    if (std::fseek(f, static_cast<long>(offset), SEEK_SET) != 0) return -1;
    std::size_t got = 0;
    while (got < n) {
        const std::size_t r = std::fread(dst + got, 1, n - got, f);
        if (r == 0) break;
        got += r;
    }
    return static_cast<long long>(got);
}

}  // namespace hostfs
}  // namespace omk

#endif
