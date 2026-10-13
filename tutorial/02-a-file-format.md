# Lesson 2 — A file format

**Goal:** read one of the game's file formats through the port's own reader,
write a small tool of your own on it, and see how a format reader in this
repository is proved: by the WHOLE corpus, not by one file that looks right.

## Read

* `engine/src/formats/tex3dt.h`, top to bottom. It is the format and the
  reader's interface in one page: what a `.3DT` holds, the `Texture` struct,
  and the LZ scheme `decodeImage` expands, written out bit by bit.
* `engine/src/formats/tex3dt.cpp`, the functions `textures` and
  `decodeImage`. About a hundred lines between them.
* `engine/tools/dump_textures.cpp` — the existing tool on this reader, and
  the model for yours.
* `docs/ASSETS.md`, section 3, "`.3DT` — textures" — how the format was
  established. Skim it: what matters is how the evidence is shaped.

## The format in one paragraph

Every `.3DO` model has a `.3DT` beside it holding all its textures, in the
order of the model's materials. Each texture is a palette (16 colours at 4
bits a pixel, else 256) followed by its pixels, which are either raw
(exactly 65536 bytes: a 256x256 page) or compressed with a small LZ scheme.
The `.3DO`'s material records say each one's name, size, depth and byte
count. Black is the colour key: a black texel is transparent.

## Run

The game's own test of a format reader is that **the walk lands exactly on
the end of the file**: palette plus data, material after material, must
consume every byte of the `.3DT` and not one more. It does for all 635
model/texture pairs. That is a test the data can fail - a wrong run length
or a wrong token size desynchronises everything after it - which is why it
was trusted.

## Change: your own tool

Create `engine/tools/tex_info.cpp`. The Makefile builds every file in
`engine/tools/` as a tool of the same name; you will not touch it.

```cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Every texture of a model: name, size, bit depth, whether the decode landed
// exactly, and how much of it is the colour key (black = transparent).
//
//     tex_info <model.3DO> [first.ppm]
//
// With a second argument, writes the first texture as a PPM image.
#include "formats/tex3dt.h"
#include "platform/datafs.h"

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tex_info <model.3DO> [first.ppm]\n");
        return 2;
    }
    if (argc > 2 && !omk::safeOutputPath(argv[2])) return 1;   // never into gamedata/

    const std::filesystem::path model = argv[1];
    const auto dir = omk::DataFs(model.parent_path().string());
    const auto tpath = dir.resolve(model.stem().string() + ".3dt");   // any case
    if (!tpath) {
        std::fprintf(stderr, "no .3dt beside %s\n", argv[1]);
        return 1;
    }
    const auto d = omk::DataFs::readPath(model.string());
    const auto t = omk::DataFs::readPath(*tpath);

    const auto txs = omk::textures(d, t);
    for (std::size_t i = 0; i < txs.size(); ++i) {
        const omk::Texture& tx = txs[i];
        long keyed = 0;
        const std::size_t n = static_cast<std::size_t>(tx.width) * tx.height;
        for (std::size_t k = 0; tx.hasPixels() && k < n; ++k) {
            int r, g, b;
            tx.texel(k, r, g, b);
            keyed += (r | g | b) == 0;
        }
        std::printf("%2zu %-20s %3dx%-3d %d bpp  %s  keyed %4.1f%%\n", i, tx.name.c_str(),
                    tx.width, tx.height, tx.bpp, tx.exact ? "exact" : "INEXACT",
                    n ? 100.0 * static_cast<double>(keyed) / static_cast<double>(n) : 0.0);
    }
    if (argc > 2 && !txs.empty() && txs[0].hasPixels()) {
        std::FILE* f = std::fopen(argv[2], "wb");
        if (!f) return 1;
        std::fprintf(f, "P6\n%d %d\n255\n", txs[0].width, txs[0].height);
        const auto rgb = txs[0].rgbCopy();
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
        std::printf("wrote %s\n", argv[2]);
    }
    return 0;
}
```

Three things in it are this repository's habits, not C++'s:

* **`DataFs::resolve` finds a file in any case.** The game ran on Windows,
  whose file system ignores case, and its data was never spelled
  consistently - of the 635 texture files, 397 end `.3dt` and 238 `.3DT`,
  and the data's own references to files are spelled no more consistently.
  Guessing the spelling is a bug waiting for the next one. Every data access in the engine goes through
  `DataFs`.
* **`safeOutputPath` before any write.** It refuses a path inside the
  shipped tree, or one ending in a shipped extension. On 2026-09-02 a tool
  whose second argument was its OUTPUT truncated `grid.3DO`, a file from the
  1999 disc with no other copy. The check that noticed ran after the write,
  which is the wrong side of the event - so the guard sits at the write.
* **The texture is kept as the file has it**: palette indices and a palette
  (`idx`, `pal`), expanded to RGB only on demand (`rgbCopy`, `texel`).

Build it and run it on Kay'l:

```
cd engine
make build/tex_info
build/tex_info ../gamedata/MESHES/PERSOS/HO1_FNM.3DO /tmp/kayl.ppm
```

```
 0 UZAL1                256x256 8 bpp  exact  keyed  0.0%
 1 UZAL2                256x256 8 bpp  exact  keyed  4.4%
 2 TETE                 256x256 8 bpp  exact  keyed  0.0%
wrote /tmp/kayl.ppm
```

Three 256x256 pages: two of his uniform (`UZAL1` is the one you just wrote -
open it in Preview: the blue suit's panels, laid flat) and his head. 4.4% of
the second page's texels are the colour key, and will not be drawn.

Now give it a path it must refuse - twice, for the two different reasons:

```
build/tex_info ../gamedata/MESHES/PERSOS/HO1_FNM.3DO ../gamedata/MESHES/PERSOS/x.ppm
build/tex_info ../gamedata/MESHES/PERSOS/HO1_FNM.3DO /tmp/HO1_FNM.3DT
```

```
refusing to write ../gamedata/MESHES/PERSOS/x.ppm: it is inside the shipped data tree
refusing to write /tmp/HO1_FNM.3DT: .3DT is a shipped game-data extension, so this is almost certainly an argument in the wrong position
```

The second is the 2026-09-02 accident's shape: the arguments swapped, the
model where the output goes.

## Prove

`engine: 3DT` runs the port's reader over **every** texture the game ships
and compares the decoded pixels, byte for byte, with `tools/tex3dt.py` - an
independent Python reader of the same format:

```
cd ..
python3 tools/verify.py --only "engine: 3DT"
```

`ok` in about a minute. Now break the codec. In `decodeImage`, the run
length of a token is `(token & 0xFC) / 4 + 3`; make it `+ 2`. Run the check
again:

```
engine: 3DT          FAIL ('built', 2534, 36) != ('built', 2534, 2534)
```

**36 of 2534** textures still match. Before reading on, guess which. They
are the ones stored raw - `dataSize == 65536` - which never reach the LZ
path. Every compressed texture is wrong, because one wrong run length
shifts every pixel after it.

And run your own tool again: the check's `make` rebuilt it against the
broken codec, so it now says what you broke:

```
 0 UZAL1                256x256 8 bpp  INEXACT  keyed  0.0%
```

Restore the `+ 3`, `touch src/formats/tex3dt.cpp`, rebuild, and see `exact`
again.

The tool is yours to keep or delete; if you keep it, `verify.py: licence
headers` counts the source files and will ask you to raise its number.

## Traps

* **"It decodes without crashing" is not a check.** Random bytes decode. A
  parse is only trusted when the data can make it FAIL: a walk landing
  exactly on the file size, a shared pool consumed in order with no gaps,
  every cross-reference resolving, every quaternion of unit length.
* **A differential test cannot see an error both sides share.** The C++
  reader was written from the Python one's description, so `engine: 3DT`
  proves the PORT, not the format. The format itself was proved by the exact
  landing - on 635 of 635 pairs.
* **The data proposes, the code decides.** Several layouts in this repository
  fitted every shipped byte and were wrong (a count read as 32 bits where the
  loader reads 16; a section read as an archive that the loader opens as a
  plain file). Lesson 3 is about reading the loader.
