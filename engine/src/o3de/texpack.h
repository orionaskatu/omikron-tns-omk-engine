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
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omk {

std::uint64_t textureKey(const Texture& t);
std::string textureKeyHex(std::uint64_t key);

// One texture's replacement, decoded: RGBA8 of w x h; `ok` false when the
// pack has none for it, or its file was unusable.
struct PackImage {
    bool ok = false;
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    // THE LIGHT MAP (`<key>_e.png`, `todo/remaster.md` step 2): how much of
    // each texel is light, 0..255, `ew x eh` (the original's size times a
    // power of two). Present with or without a colour replacement; empty
    // when the pack has none. Only the BLOOM reads it.
    int ew = 0, eh = 0;
    std::vector<std::uint8_t> emissive;
};

class TexturePack {
public:
    // Index DIR's `<16 hex>.png` files (nothing is decoded yet). False, and
    // said on stderr, when DIR is not a directory. An empty directory opens.
    bool open(const std::string& dir);
    bool active() const { return opened_; }
    std::size_t size() const { return files_.size(); }
    std::size_t lightMaps() const { return lights_.size(); }
    const std::string& dir() const { return dir_; }
    // t's replacement, decoded: RGBA8 of w x h. False when the pack has none,
    // or when the file is unusable (not a PNG this reads, not a power-of-two
    // multiple of t) - said once per key, and t is then drawn as shipped.
    bool replacement(const Texture& t, std::vector<std::uint8_t>& rgba, int& w, int& h) const;
    // The same for a whole POOL at once, one result per texture in order: the
    // files read and decoded on the shared thread pool (`platform/threads.h`),
    // the lookups and the refusals on the caller's thread. A null entry is
    // skipped. What a backend calls when a pool reaches it: decoding 31
    // replacements one after another took 634 ms on the street (M3).
    // `colour` false: the LIGHT MAPS alone (and the hook) - the colour PNGs, a
    // texture's 1024x1024, are not decoded. The surface lights' way
    // (`o3de/emitterlight.h`): a set's load had cost ~110 ms more for them.
    std::vector<PackImage> replacements(std::span<const Texture* const> ts, bool colour = true) const;
    // THE GREYSCALE BANK (ops 150/151, `o3de/greybank.h`): the viewer greys a
    // pool's PALETTES, which makes each texture another key. Told which colour
    // texture a grey one came from, the pack answers the grey key with the
    // colour replacement taken to the engine's luma - and keyed (alpha 0)
    // wherever that luma is 0, as `toRgbaKeyed` keys the grey palette's
    // black - so a black-and-white cutscene draws the pack too.
    void aliasGrey(const Texture& colour, const Texture& grey);

    // THE LIGHT MAPS AMENDED after the decode (`o3de/emitterlight.h`
    // curateBloom, the remaster's curated light sources): called once a
    // texture, on the pool - it must be thread-safe - with the result so far.
    using LightHook = void (*)(const Texture&, PackImage&);
    void setLightHook(LightHook h) { lightHook_ = h; }

private:
    LightHook lightHook_ = nullptr;
    std::string dir_;
    bool opened_ = false;
    std::unordered_map<std::uint64_t, std::string> files_;
    std::unordered_map<std::uint64_t, std::string> lights_;   // `<key>_e.png`
    std::unordered_map<std::uint64_t, std::uint64_t> greyOf_;   // grey key -> colour key
    mutable std::unordered_set<std::uint64_t> refused_;
};

// The process's one pack, empty unless the session opened it.
TexturePack& texturePack();

// The viewer's way in: open DIR as the process's pack and SAY so (a prose
// line and `@evt texpack.open dir= entries=`). False, said, when it is not a
// directory.
bool openTexturePack(const std::string& dir);

}  // namespace omk
