// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST FILE SYSTEM on classic Mac OS: the File Manager
// (`platform/hostfs.h`). Reads go through it READ-ONLY: the C library's `open`
// asks for read-WRITE whatever the mode, which a locked file or a CD refuses.
#if defined(macintosh)

#include "platform/datafs.h"
#include "platform/hostfs.h"

#include <algorithm>
#include <cstring>

#include <Files.h>
#include <TextUtils.h>

namespace omk {

std::string hostPath(const std::string& p) {
    if (p.empty()) return p;
    const bool hfsRoot = p.find(':') != std::string::npos;   // the caller wrote HFS
    const bool absolute = hfsRoot || p[0] == '/';
    std::string out = absolute ? "" : ":";
    std::size_t i = 0;
    while (i <= p.size()) {
        const std::size_t j = p.find_first_of("/\\", i);
        const std::string part = p.substr(i, (j == std::string::npos ? p.size() : j) - i);
        i = (j == std::string::npos) ? p.size() + 1 : j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {                                  // "::" climbs one
            if (out.empty() || out.back() != ':') out += ':';
            out += ':';
            continue;
        }
        if (!out.empty() && out.back() != ':') out += ':';
        out += part;
    }
    // a bare volume name is a file in the default folder until it has its ':'
    if (absolute && out.find(':') == std::string::npos) out += ':';
    return out;
}

namespace {

// The File Manager's view of a path: its FSSpec, and its catalog record.
// -> false when the path does not name something that exists.
bool catalogOf(const std::string& p, FSSpec& spec, CInfoPBRec& pb) {
    const std::string h = hostPath(p);
    if (h.empty() || h.size() > 255) return false;
    Str255 name;
    c2pstrcpy(name, h.c_str());
    if (FSMakeFSSpec(0, 0, name, &spec) != noErr) return false;
    std::memset(&pb, 0, sizeof pb);
    pb.hFileInfo.ioNamePtr = spec.name;
    pb.hFileInfo.ioVRefNum = spec.vRefNum;
    pb.hFileInfo.ioDirID = spec.parID;
    pb.hFileInfo.ioFDirIndex = 0;
    return PBGetCatInfoSync(&pb) == noErr;
}

}  // namespace

bool makeDirectories(const std::string& dir) {
    if (dir.empty()) return true;
    // each prefix in turn, as on the Vita: `FSpDirCreate` makes one level
    for (std::size_t i = 0; i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
            const std::string p = dir.substr(0, i);
            if (p.empty() || hostfs::isDirectory(p)) continue;
            const std::string h = hostPath(p);
            if (h.size() > 255) return false;
            Str255 name;
            c2pstrcpy(name, h.c_str());
            FSSpec spec;
            if (FSMakeFSSpec(0, 0, name, &spec) == fnfErr) {
                long made = 0;
                FSpDirCreate(&spec, smSystemScript, &made);
            }
        }
    }
    return hostfs::isDirectory(dir);
}

long long fileSize(const std::string& path) {
    FSSpec spec; CInfoPBRec pb;
    if (!catalogOf(path, spec, pb) || (pb.hFileInfo.ioFlAttrib & kioFlAttribDirMask)) return -1;
    return static_cast<long long>(pb.hFileInfo.ioFlLgLen);   // the data fork
}

namespace hostfs {

bool isDirectory(const std::string& p) {
    FSSpec spec; CInfoPBRec pb;
    return catalogOf(p, spec, pb) && (pb.hFileInfo.ioFlAttrib & kioFlAttribDirMask) != 0;
}

bool pathExists(const std::string& p) {
    FSSpec spec; CInfoPBRec pb;
    return catalogOf(p, spec, pb);
}

std::vector<std::string> listNames(const std::string& dir) {
    std::vector<std::string> out;
    // the folder's entries by index, 1 up, until the File Manager runs out
    FSSpec spec; CInfoPBRec pb;
    if (!catalogOf(dir, spec, pb) || !(pb.hFileInfo.ioFlAttrib & kioFlAttribDirMask)) return out;
    const long dirID = pb.dirInfo.ioDrDirID;
    for (short k = 1;; ++k) {
        Str255 name;
        std::memset(&pb, 0, sizeof pb);
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = spec.vRefNum;
        pb.hFileInfo.ioDirID = dirID;        // reset: an indexed call overwrites it
        pb.hFileInfo.ioFDirIndex = k;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        out.emplace_back(reinterpret_cast<const char*>(name + 1), name[0]);
    }
    return out;
}

std::string comparablePath(const std::string& path) {
    return path;   // no canonical form to take
}

bool openRead(const std::string& path, long& handle, void*&) {
    FSSpec spec; CInfoPBRec pb;
    if (!catalogOf(path, spec, pb)) return false;
    short ref = 0;
    if (FSpOpenDF(&spec, fsRdPerm, &ref) != noErr) return false;
    handle = ref;
    return true;
}

void closeFile(long handle, void*) {
    FSClose(static_cast<short>(handle));
}

long long readAt(long handle, void*, std::uint64_t offset, std::byte* dst, std::size_t n) {
    const short ref = static_cast<short>(handle);
    if (SetFPos(ref, fsFromStart, static_cast<long>(offset)) != noErr) return -1;
    std::size_t got = 0;
    while (got < n) {
        long want = static_cast<long>(std::min<std::size_t>(n - got, 1u << 20));
        const OSErr e = FSRead(ref, &want, dst + got);
        if (want <= 0) break;
        got += static_cast<std::size_t>(want);
        if (e != noErr) break;
    }
    return static_cast<long long>(got);
}

}  // namespace hostfs
}  // namespace omk

#endif
