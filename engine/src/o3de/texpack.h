// SPDX-License-Identifier: GPL-3.0-or-later
// THE TEXTURE PACK - the remaster's first feature (`todo/remaster.md` B4).
//
// NOT IN THE ORIGINAL, and not an `[Enhancements]` row either: it replaces
// authored pixels, so it lives under `[Remaster]` (`texturepack = DIR`, or
// `--texture-pack DIR`), OFF unless named. With no pack nothing here runs and
// every backend uploads exactly what it did before.
//
// A pack is a DIRECTORY of `<key>.png` files, `key` the sixteen hex digits of
// `textureKey` below - one file per texture, so a person can open any one of
// them in an image editor and fix it (the curation `todo/remaster.md` B2 and
// B5 need). `tools/omktexpack.py` makes them from the shipped data; nothing a
// pack holds is ever committed (the repo is public, the pixels are not ours).
//
// The KEY is the texture's own content - FNV-1a 64 over its width and height
// (little-endian 32 bits each), its width*height palette indices and its 768
// palette bytes - not its name: 182 names ship with different pixels in
// different files (CLAUDE.md §6). The same bytes in two models are one file.
//
// A replacement must be the original's size times a POWER OF TWO each way
// (1, 2, 4 ... 16), because the shipped UVs are in the ORIGINAL's texels: the
// GLES backend keeps dividing by the original size, the Vulkan shader is told
// the factor. Its ALPHA is the colour key, as `Texture::toRgbaKeyed` makes it
// (0 where the original is cut out, and the colour there 0 too), so a cutout
// mesh (flag 0x800) keeps its silhouette rule.
//
// Drawn by the Vulkan and GLES backends only. The software reference, the
// GL1 and the 3DS backends never consult it: the reference is what the
// original drew, and the two small consoles' textures are the original's size
// rule (`texscale.h`).
#pragma once

#include "formats/tex3dt.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omk {

std::uint64_t textureKey(const Texture& t);
std::string textureKeyHex(std::uint64_t key);

class TexturePack {
public:
    // Index DIR's `<16 hex>.png` files (nothing is decoded yet). False, and
    // said on stderr, when DIR is not a directory. An empty directory opens.
    bool open(const std::string& dir);
    bool active() const { return opened_; }
    std::size_t size() const { return files_.size(); }
    const std::string& dir() const { return dir_; }
    // t's replacement, decoded: RGBA8 of w x h. False when the pack has none,
    // or when the file is unusable (not a PNG this reads, not a power-of-two
    // multiple of t) - said once per key, and t is then drawn as shipped.
    bool replacement(const Texture& t, std::vector<std::uint8_t>& rgba, int& w, int& h) const;

private:
    std::string dir_;
    bool opened_ = false;
    std::unordered_map<std::uint64_t, std::string> files_;
    mutable std::unordered_set<std::uint64_t> refused_;
};

// The process's one pack, empty unless the session opened it.
TexturePack& texturePack();

// The viewer's way in: open DIR as the process's pack and SAY so (a prose
// line and `@evt texpack.open dir= entries=`). False, said, when it is not a
// directory.
bool openTexturePack(const std::string& dir);

}  // namespace omk
