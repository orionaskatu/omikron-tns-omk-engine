// SPDX-License-Identifier: GPL-3.0-or-later
// THE TEXTURE PACK - the remaster's first feature (`todo/remaster.md` B4).
//
// NOT IN THE ORIGINAL: it replaces authored pixels, so it lives under
// `[Remaster]` (`texturepack = DIR`, or `--texture-pack DIR`), OFF unless named
// - and, in the FORK's layout (below), under `[Enhancements]`. With no pack nothing here runs and
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
// THE FORK'S LAYOUT (`[Enhancements] texturepack = DIR`, `--texturepack DIR`,
// `tools/texpack.py`): the same class holds a pack laid out
// `DIR/<group>/<model>/<MATERIAL>.png` - one PNG per material of a model, at
// an integer multiple (1..16) of the page, pure black the colour key. `openFork`
// INDEXES it at start (the model's `.3DT` decoded, each material's file name by
// `packFileNames`, the content key by `textureKey` - the one key both layouts
// share) and registers key -> PNG in `files_`, so the lazy decode, the preload,
// the F10 switch and every renderer's upload are the ones a `[Remaster]` pack
// has; nothing downstream knows the layout.
//
// Drawn by the Vulkan and GLES backends only. The software reference, the
// GL1 and the 3DS backends never consult it: the reference is what the
// original drew, and the two small consoles' textures are the original's size
// rule (`texscale.h`).
#pragma once

#include "formats/ktx2.h"
#include "formats/tex3dt.h"
#include "platform/sync.h"   // Mutex: none on the classic Mac, whose libstdc++ has no thread model

#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omk {

std::uint64_t textureKey(const Texture& t);
std::string textureKeyHex(std::uint64_t key);

// The file stem a fork-layout pack uses for each material of a model, from the
// material records' +0 names (`tools/texpack.py` `pack_file_names` is the same
// rule): any character outside `A-Za-z0-9 _+.~-` (and a trailing space or
// dot) as '_', an empty name as `mat<i>`, a name repeated (case aside) as
// `<name>~<i>` on the later material.
std::vector<std::string> packFileNames(const std::vector<std::string>& names);

// THE BLOCK FORMATS (`todo/remaster.md`, the pack's KTX2 files): beside each
// `<key>.png` the pack may hold `<key>.bc7.ktx2` (BC7, desktop GPUs) and
// `<key>.astc.ktx2` (ASTC, Android and the Quest) - and `_e` / `_n` the same,
// a light map BC4 or ASTC - each with its whole mip chain
// (`tools/tex_encode.cpp`, `omktexpack.py compress`). A backend says which
// it can sample (`setCodecs`); a texture that has one is then READ, not
// decoded - no PNG inflate on the frame it arrives - and uploaded as blocks,
// a quarter of RGBA's memory or less. The PNG, where there is one, stays the
// fallback and the source a person edits; a pack shipped for play may DROP
// its colour and normal PNGs (half its size): the KTX2 then indexes the key,
// and where pixels are wanted (a GL without BPTC, a grey alias) the BC is
// decoded on the CPU (`formats/blockdecode.h`). An ASTC-only pack keeps them.
enum PackCodec : unsigned { kCodecBc = 1, kCodecAstc = 2 };
struct PackBlocks {
    std::uint32_t vkFormat = 0;   // `formats/ktx2.h`'s numbers
    int w = 0, h = 0;
    std::vector<std::uint8_t> bytes;                          // the whole file
    std::vector<std::pair<std::size_t, std::size_t>> levels;  // offset, length; level 0 first
    bool empty() const { return bytes.empty(); }
};

// One texture's replacement, decoded: RGBA8 of w x h; `ok` false when the
// pack has none for it, or its file was unusable. With the block formats on,
// `colourZ` / `emissiveZ` / `normalZ` hold the blocks INSTEAD of `rgba` /
// `emissive` / `normal` (w, h and ew, eh, nw, nh as before).
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
    // THE NORMAL MAP (`<key>_n.png`, `todo/remaster.md` B3, `o3de/normalmap.h`):
    // RGBA8 `nw x nh` - the normal in OpenGL's convention, the height in A
    // (kept at 1 or more: the shader reads alpha 0 as "none"). Decoded only
    // when the session asked (`setNormals`) and with the colour; nothing asks
    // since the relief was removed (2026-10-29) and no backend draws it - the
    // pack format and `texpack_tool normals` keep it.
    int nw = 0, nh = 0;
    std::vector<std::uint8_t> normal;
    PackBlocks colourZ, emissiveZ, normalZ;
};

class TexturePack {
public:
    // Index DIR's `<16 hex>.png` files (nothing is decoded yet). False, and
    // said on stderr, when DIR is not a directory. An empty directory opens.
    bool open(const std::string& dir);
    // Index a pack of the FORK's layout (see the head of this file) against
    // the game data at `dataRoot`. What is found is in `forkIndexed`; false,
    // said on stderr, when DIR is not a directory.
    struct ForkStats {
        int models = 0;        // directories that name a model of MESHES/
        int indexed = 0;       // materials registered (distinct contents)
        int dups = 0;          // ...whose content an earlier one already had
        int rejected = 0;      // PNGs refused (size, format), said once each
        int strays = 0;        // directories naming no model
        int missing = 0;       // materials of those models with no PNG
        std::map<int, int> scales;   // k -> how many
    };
    bool openFork(const std::string& dir, const std::string& dataRoot, ForkStats* stats = nullptr);
    bool active() const { return opened_; }
    // THE ORIGINAL/REMASTER SWITCH (`todo/graphics-menu.md` B): a pack open but
    // hidden is not drawn - the backends' uploads ask `shown`, and draw the
    // shipped texels - while the surface lights' light maps (`active`) are
    // still read, so a set loaded in the original mode has them for the switch
    void setShown(bool on) { shown_ = on; }
    bool shown() const { return opened_ && shown_; }
    std::size_t size() const { return files_.size(); }
    std::size_t lightMaps() const { return lights_.size(); }
    std::size_t normalMaps() const { return normals_.size(); }
    // the replacements `exclude.txt` left out (the curation, B2)
    std::size_t excluded() const { return excluded_; }
    // decode the NORMAL MAPS too (nothing asks now; each another PNG the size of
    // its colour) - off unless asked
    void setNormals(bool on) { wantNormals_ = on; }
    bool normals() const { return wantNormals_; }
    // decode the LIGHT MAPS with the colours (`colour` true): the bloom's -
    // on unless the session says no bloom will read them. The light maps
    // alone (`colour` false) are decoded either way.
    void setLights(bool on) { wantLights_ = on; }
    // the block formats the backend samples (`PackCodec` bits, 0 none - the
    // default: PNGs only)
    void setCodecs(unsigned c) { codecs_ = c; }
    unsigned codecs() const { return codecs_; }
    // colour textures with a KTX2 of `codec`
    std::size_t blockFiles(PackCodec codec) const { return ktx_[codec == kCodecAstc ? 1 : 0][0].size(); }
    const std::string& dir() const { return dir_; }
    // Each texture's replacement, decoded: RGBA8 of w x h, not `ok` when the
    // pack has none or the file is unusable (not a PNG this reads, not a
    // power-of-two multiple of the texture) - said once per key, and the
    // texture is then drawn as shipped. For a whole POOL at once (the single
    // `replacement` had no caller left - removed 2026-10-26), one result per
    // texture in order: the
    // files read and decoded on the shared thread pool (`platform/threads.h`),
    // the lookups and the refusals on the caller's thread. A null entry is
    // skipped. What a backend calls when a pool reaches it: decoding 31
    // replacements one after another took 634 ms on the street (M3).
    // `colour` false: the LIGHT MAPS alone - the colour PNGs, a texture's
    // 1024x1024, are not decoded.
    // `normals` false: none of the normal maps, whatever `setNormals` said.
    // THREAD-SAFE against itself and `aliasGrey`: a set loads on a
    // BackgroundJob while the renderer's pool is decoded on the main thread.
    // `pooled` false: decoded on the CALLER's thread alone - `prepare`'s, from
    // a load's background job, which must not take the shared pool's workers
    // from the frame (the first pools of a street decoded 5x slower beside
    // six preload jobs, 2026-10-24)
    std::vector<PackImage> replacements(std::span<const Texture* const> ts, bool colour = true,
                                        bool normals = true, bool pooled = true) const;
    // THE GREYSCALE BANK (ops 150/151, `o3de/greybank.h`): the viewer greys a
    // pool's PALETTES, which makes each texture another key. Told which colour
    // texture a grey one came from, the pack answers the grey key with the
    // colour replacement taken to the engine's luma - and keyed (alpha 0)
    // wherever that luma is 0, as `toRgbaKeyed` keys the grey palette's
    // black - so a black-and-white cutscene draws the pack too.
    void aliasGrey(const Texture& colour, const Texture& grey);

    // THE PREPARED RESULTS (the remaster audit's performance item 1,
    // 2026-10-23): `prepare` decodes on the CALLER's thread - a set's load,
    // a `BackgroundJob` - what a backend's pool would decode on the frame
    // the set arrives, and PARKS each result that holds anything by its
    // texture's key; `replacements` (the colours asked, with the normal maps
    // as the parking had them) hands a parked one over, once, instead of
    // reading and decoding it again. `forget` drops what a released set
    // parked - COUNTED, a key as many times as it was prepared: two sets
    // sharing a texture park it once, and one set's release must not drop it
    // from the other (2026-10-26). At most `kParkedBytes` parked, the OLDEST
    // let go to make room (a set released while still preparing parks after
    // its `forget`: it ages out rather than holding the cap).
    static constexpr std::size_t kParkedBytes = std::size_t{256} << 20;
    void prepare(std::span<const Texture* const> ts);
    void forget(std::span<const Texture* const> ts);
    std::size_t parkedBytes() const;
    std::size_t parkedTaken() const;   // results handed over from `prepare` so far


private:
    void clear();
    unsigned codecs_ = 0;
    // [bc, astc][colour, light, normal]: key -> its KTX2
    std::unordered_map<std::uint64_t, std::string> ktx_[2][3];
    bool wantNormals_ = false;
    bool wantLights_ = true;
    std::size_t excluded_ = 0;
    // guards `refused_`, `greyOf_` and `parked_`, what the callers' threads touch
    mutable Mutex m_;
    std::unordered_map<std::uint64_t, std::string> normals_;  // `<key>_n.png`
    std::string dir_;
    bool opened_ = false;
    bool shown_ = true;
    std::unordered_map<std::uint64_t, std::string> files_;
    // the keys `openFork` registered: their PNGs are RGB with black as the key
    // and any whole k, not RGBA with alpha as the key and a power of two
    std::unordered_set<std::uint64_t> forkKeyed_;
    std::unordered_map<std::uint64_t, std::string> lights_;   // `<key>_e.png`
    std::unordered_map<std::uint64_t, std::uint64_t> greyOf_;   // grey key -> colour key
    mutable std::unordered_set<std::uint64_t> refused_;
    // `prepare`'s results (under `m_`), and their bytes
    mutable std::unordered_map<std::uint64_t, PackImage> parked_;
    mutable std::unordered_map<std::uint64_t, int> parkedRefs_;   // `prepare`s a parked key, less `forget`s
    mutable std::size_t parkedBytes_ = 0;
    mutable std::size_t parkedTaken_ = 0;
    std::deque<std::uint64_t> parkedOrder_;   // the keys as parked, oldest first (some since taken)
};

// The process's one pack, empty unless the session opened it.
TexturePack& texturePack();

// The viewer's way in: open DIR as the process's pack and SAY so (a prose
// line and `@evt texpack.open dir= entries=`). False, said, when it is not a
// directory.
bool openTexturePack(const std::string& dir);

// ...and the fork's layout (`[Enhancements] texturepack`, `--texturepack`):
// `openFork` and the same two lines. False, said, when it is not a directory or
// nothing in it matched a model's material.
bool openForkTexturePack(const std::string& dir, const std::string& dataRoot);

}  // namespace omk
