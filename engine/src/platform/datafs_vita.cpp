// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST FILE SYSTEM on the PS Vita: `sceIo*` (`platform/hostfs.h`).
#if defined(__vita__)

#include "platform/datafs.h"
#include "platform/hostfs.h"

#include <algorithm>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

namespace omk {

std::string hostPath(const std::string& p) {
    return p;
}

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return true;
    // each prefix in turn - `sceIoMkdir` makes one level, and an existing one
    // is not an error for this
    for (std::size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            const std::string p = dir.substr(0, i);
            if (!p.empty() && p.back() != ':' && !hostfs::isDirectory(p)) sceIoMkdir(p.c_str(), 0777);
        }
    }
    return hostfs::isDirectory(dir);
}

long long fileSize(const std::string& path) {
    SceIoStat st;
    if (sceIoGetstat(path.c_str(), &st) < 0) return -1;
    return static_cast<long long>(st.st_size);
}

namespace hostfs {

bool isDirectory(const std::string& p) {
    SceIoStat st;
    return sceIoGetstat(p.c_str(), &st) >= 0 && SCE_S_ISDIR(st.st_mode);
}

bool pathExists(const std::string& p) {
    SceIoStat st;
    return sceIoGetstat(p.c_str(), &st) >= 0;
}

std::vector<std::string> listNames(const std::string& dir) {
    std::vector<std::string> out;
    const SceUID d = sceIoDopen(dir.c_str());
    if (d < 0) return out;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        const std::string n = e.d_name;
        if (n != "." && n != "..") out.push_back(n);
    }
    sceIoDclose(d);
    return out;
}

std::string comparablePath(const std::string& path) {
    return path;   // no canonical form to take: the Vita's device paths are absolute
}

bool openRead(const std::string& path, long& handle, void*&) {
    const SceUID fd = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd < 0) return false;
    handle = fd;
    return true;
}

void closeFile(long handle, void*) {
    sceIoClose(static_cast<SceUID>(handle));
}

long long readAt(long handle, void*, std::uint64_t offset, std::byte* dst, std::size_t n) {
    const SceUID fd = static_cast<SceUID>(handle);
    if (sceIoLseek(fd, static_cast<SceOff>(offset), SCE_SEEK_SET) < 0) return -1;
    std::size_t got = 0;
    while (got < n) {
        // at most 1 MiB a call: a kernel read need not deliver all it is asked
        const std::size_t want = std::min<std::size_t>(n - got, 1u << 20);
        const int r = sceIoRead(fd, dst + got, static_cast<SceSize>(want));
        if (r <= 0) break;
        got += static_cast<std::size_t>(r);
    }
    return static_cast<long long>(got);
}

}  // namespace hostfs
}  // namespace omk

#endif
