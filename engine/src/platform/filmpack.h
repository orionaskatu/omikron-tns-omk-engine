// SPDX-License-Identifier: GPL-3.0-or-later
// THE FILM PACK - the remaster's Tier E (`todo/remaster.md`): the three FLIS
// films (`platform/movie.h`) played from upscaled copies. NOT IN THE
// ORIGINAL: it handed its 320x240 MPEG-1 files to the system.
//
// A directory of `<key>.mpg`, each an MPEG-1 program stream like the
// original - the picture larger, the original's MP2 sound copied untouched -
// so the same decoder plays it, at the same clock. The key is `filmKey`:
// FNV-1a 64 over the file's size (eight little-endian bytes) and its first
// MiB, so another language's film, or another version's, is never taken for
// this one. `tools/omktexpack.py films` makes them.
//
// Off unless a pack is open: `find` returns "" at once and the original
// plays.
#pragma once

#include <cstdint>
#include <set>
#include <string>

namespace omk {

// `path` a real path (`DataFs::resolve`); 0 when it cannot be read
std::uint64_t filmKey(const std::string& path);

class FilmPack {
public:
    bool open(const std::string& dir);   // false (and inactive) when absent or empty
    bool active() const { return !keys_.empty(); }
    std::size_t files() const { return keys_.size(); }
    // the replacement for the original film at `path`, or ""
    std::string find(const std::string& path) const;
private:
    std::string dir_;
    std::set<std::uint64_t> keys_;
};

FilmPack& filmPack();

}  // namespace omk
