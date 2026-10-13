// SPDX-License-Identifier: GPL-3.0-or-later
// THE HOST'S FILE SYSTEM, the few calls `datafs.cpp` needs from it - one
// implementation per platform, each in its own file and compiled only there:
// `datafs_posix.cpp` (std::filesystem and C stdio - the desktops, the 3DS),
// `datafs_vita.cpp` (`sceIo*`) and `datafs_classic.cpp` (the File Manager).
// Everything else in `datafs.cpp` - the case-insensitive resolver, the write
// guard, the whole-file and range reads - is the same code everywhere.
//
// Internal: not part of `datafs.h`'s interface. The public ones a platform
// also supplies (`hostPath`, `makeDirectories`, `fileSize`) are declared
// there.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omk::hostfs {

bool isDirectory(const std::string& p);
bool pathExists(const std::string& p);
// The names in a directory, without "." and "..".
std::vector<std::string> listNames(const std::string& dir);
// The path as the write guard compares it: canonical where the host has such a
// form, as given where it does not (the Vita's device paths are absolute).
std::string comparablePath(const std::string& path);

// A file opened READ-ONLY: `handle` (a descriptor or a file reference) and
// `file` (a C stdio stream) - whichever the platform uses, the other unset.
bool openRead(const std::string& path, long& handle, void*& file);
void closeFile(long handle, void* file);
// Up to `n` bytes at `offset`, read in as many calls as the host needs: the
// bytes read, or -1 when the seek itself failed.
long long readAt(long handle, void* file, std::uint64_t offset, std::byte* dst, std::size_t n);

}  // namespace omk::hostfs
