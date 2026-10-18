// SPDX-License-Identifier: GPL-3.0-or-later
// TEXTURE OVERRIDE PACKS - `todo/enhancements.md` 25, an ENHANCEMENT, off
// unless `[Enhancements] texturepack = <directory>` (or `--texturepack DIR`)
// names one.
//
// The game's textures are small palettised pages (most 256x256, 8 bits a
// texel) in the `.3DT` beside each `.3DO`. A pack holds replacements -
// `<pack>/<group>/<model>/<MATERIAL>.png`, laid out as `tools/texpack.py
// extract` writes them - at an INTEGER multiple of the original's size, and
// the Vulkan and Direct3D 12 renderers upload a replacement in its place:
//
//   * THE MATCH. At start the pack is INDEXED: every `<group>/<model>/`
//     directory names a model of the game's `MESHES/`, whose `.3DT` is decoded
//     and each material's file name made by `packFileName` (the rule
//     `tools/texpack.py` uses); a PNG of that name is the replacement of THAT
//     material's pixels, keyed by a hash of its indices and palette
//     (`contentKey`). What the frame's pool holds is then matched by the same
//     hash - so whichever list a texture came through (the set, a character,
//     the player, a scene clip), and whichever model the game's slot cache
//     handed it from (`o3de/texcache.h`), it takes the replacement of the
//     pixels it actually draws.
//   * THE SIZE. A replacement must be k x the original in both directions
//     (k = 1..16); anything else is refused and the original stays. The UVs
//     stay in the ORIGINAL's texel units, as the data stores them: the
//     renderers divide by the original size (`scene.frag`, the scale riding in
//     the cutout word; `rt.glsl`, in the record's texture word), so the wrap
//     (REPEAT) and the tiling are unchanged.
//   * THE COLOUR KEY. A pure-black texel (and an RGBA file's alpha < 128) is
//     the key - (0,0,0,0) - exactly as `Texture::toRgbaKeyed` keys the
//     original, so the cutout test, the premultiplied filtering and the mip
//     chain treat a replacement as they treat a page.
//   * THE GREY BANK (ops 150/151, `o3de/greybank.h`): a replacement has no
//     palette, so it is greyed by LUMA texel by texel (`greyRgba`), the same
//     luma the palette arm takes.
//   * THE LOAD is lazy: a PNG is decoded the first time its texture enters the
//     pool and kept while it is used. A missing or invalid file falls back to
//     the original, with a line naming it.
//   * Not drawn by the software reference, GLES, GL1 or the 3DS: they keep
//     the indices and the palette, which a replacement never changes.
#pragma once

#include "formats/tex3dt.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace omk {

// The file stem a pack uses for each material of a model, from the material
// records' +0 names: any character outside `A-Za-z0-9 _+.~-` (and a trailing
// space or dot) as '_', an empty name as `mat<i>`, and a name repeated (case
// aside) as `<name>~<i>` on the later material. `tools/texpack.py`
// `pack_file_names` is the same rule.
std::vector<std::string> packFileNames(const std::vector<std::string>& names);

// FNV-1a 64 over the size, the indices and the palette: what a replacement is
// matched by. 0 for a texture with no pixels.
std::uint64_t contentKey(const Texture& t);

class TexturePack {
public:
    // Index the pack at `dir` against the game data at `dataRoot`. Prints one
    // line (and `@evt texpack.load`) with what it found. -> false when there
    // is nothing to use (no such directory, nothing in it matched).
    bool open(const std::string& dir, const std::string& dataRoot);
    bool active() const { return !index_.empty(); }

    // Give every texture of `pool` whose pixels the pack replaces its
    // replacement (`Texture::packRgba`, `packScale`); the first use of one
    // decodes its PNG. -> how many slots now carry one. A line (and `@evt
    // texpack.pool`) when that number changes.
    std::size_t apply(std::vector<Texture>& pool);

    std::size_t indexed() const { return index_.size(); }

private:
    struct Entry {
        std::string path;        // the PNG
        int w = 0, h = 0;        // the ORIGINAL's size
        bool tried = false, failed = false;
        PixelBuffer rgba;        // k*w x k*h RGBA, keyed; empty until decoded
        int scale = 0;
        unsigned age = 0;        // the `apply` that last used it
    };
    bool decode(Entry& e);

    std::map<std::uint64_t, Entry> index_;
    // the content key of a pool texture, by its storage - which the entry
    // holds, so the address is not reused while cached
    struct Seen { PixelBuffer idx, pal; std::uint64_t key = 0; };
    std::map<std::pair<const std::uint8_t*, const std::uint8_t*>, Seen> seen_;
    std::size_t decodedBytes_ = 0;
    unsigned applies_ = 0;
    long lastReplaced_ = -1;
    int failedTold_ = 0;
};

}  // namespace omk
