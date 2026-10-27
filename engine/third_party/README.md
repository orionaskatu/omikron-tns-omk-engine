# Vendored third-party code

Four files and one folder, and they are here rather than found by the build for the reason
[`../../docs/PORTING.md`](../../docs/PORTING.md) A8 gives: **a vendored
dependency is checked in and `make` works without it being installed, so a
missing one is a broken checkout; a system dependency's absence merely disables
the frontend and never the suite.**

| file | what | upstream | licence |
|---|---|---|---|
| `pl_mpeg.h` | MPEG-1 video decoder, MP2 audio decoder and MPEG-PS demuxer, in one header | [phoboslab/pl_mpeg](https://github.com/phoboslab/pl_mpeg) | **MIT** (`SPDX-License-Identifier: MIT`, Dominic Szablewski) |
| `ffx_a.h`, `ffx_fsr1.h` | AMD FidelityFX Super Resolution 1 (EASU, RCAS) and its portability header, GLSL side | [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) `ffx-fsr/` | **MIT** (Advanced Micro Devices, 2021) |
| `stb_truetype.h` | TrueType parser and rasteriser, in one header | [nothings/stb](https://github.com/nothings/stb) | **MIT or public domain**, either (Sean Barrett) |
| `bc7enc/` | BC7 encoder (`bc7enc.c/.h`), BC7 decoder (`bc7decomp.cpp/.h`), BC1-5 encoders (`rgbcx.h`, `rgbcx_table4.h`), with their `LICENSE` | [richgel999/bc7enc](https://github.com/richgel999/bc7enc) | **MIT or public domain**, either (Richard Geldreich, Jr.) |

Fetched 2026-09-01 from `raw.githubusercontent.com/phoboslab/pl_mpeg/master`,
4439 lines, and **kept verbatim** — it is not edited here, so it can be diffed
against upstream. `docs/PORTING.md` A8 called it public domain; it is MIT, and
that is corrected there.

The two FSR headers were fetched 2026-10-22 from
`raw.githubusercontent.com/GPUOpen-Effects/FidelityFX-FSR/master/ffx-fsr`
(upstream `a21ffb8f6c13`; 2656 and 1199 lines, sha256 `f60e2722...` and
`93c39223...`), **kept verbatim** too. Only the shaders include them
(`backends/vulkan/shaders/easu.frag` and `present.frag`, through `glslc
-I third_party`): the remaster's upscale (`todo/remaster.md` A6), which the
original never had - so A8 rule 3 is not in question, there being nothing of
the original's for it to stand in for. The few constants its CPU side computes
are transcribed in `src/o3de/fsrcon.h` and compared with the header's own by
`verify.py: engine: upscale`.

`stb_truetype.h` was fetched 2026-10-22 from
`raw.githubusercontent.com/nothings/stb/6e9f34d5429c`, v1.26, 5079 lines, and
is **kept verbatim**. One TOOL includes it, `tools/font_hd.cpp` (the
remaster's font pack, `src/ui/fontpack.h`), to draw the disc's own
`FONTS/OMIKRON.TTF` at eight times its glyphs' size; the engine never reads
a TTF, and the original never drew one (its faces are the `.FNT` bitmaps), so
A8 rule 3 is not in question.

`bc7enc/` was fetched 2026-10-22 from
`raw.githubusercontent.com/richgel999/bc7enc/f66c2e489b07`, six sources and
the LICENSE, **kept verbatim**. One TOOL includes them, `tools/tex_encode.cpp`,
which writes the texture pack's KTX2 files in BC7 (colours, normal maps) and
BC4 (light maps), and decodes its own output back to say how close it came.
The engine reads those files with its own `formats/ktx2.cpp` and hands the
blocks to the GPU as they are - it never encodes or decodes a block. ASTC
(Android, the Quest) is made by ARM's `astcenc` binary, which is run, not
vendored.

## Why a decoder is allowed at all

A8 rule 3 says no dependency may perform work a reference implementation is
supposed to be a port of. A decoder looks like it might be — and it is not,
which was checked rather than assumed. `gamedata/FLIS/`'s three files are MPEG-1
**program streams** (`00 00 01 BA` pack headers), and the engine's own import
table names **`CoCreateInstance`** (ole32 — a DirectShow filter graph) and
**`mciSendCommandA`** (winmm — MCI). It contains no MPEG decoder: it handed the
file to the operating system. So vendoring one is the equivalent of what the
original did, not a substitute for code that could have been ported.

`verify.py: engine movies` asserts the three streams' own headers and what the
decoder gets out of them, so the claim is testable rather than a note.

## Runtime libraries NOT in the tree: the upscalers' SDKs

The Direct3D 12 backend's upscalers (`backends/d3d12/upscale*`,
`todo/enhancements.md` 18) load signed binaries at RUN time that are **not**
vendored: AMD's FidelityFX loader and upscaler DLLs (FSR) and NVIDIA's
Streamline interposer, common and DLSS plugins and `nvngx_dlss.dll`. The
`Makefile` reads the SDKs' headers from `deps/` and copies only those DLLs
next to the viewer when the SDKs are there; nothing is committed. Their terms,
which allow binary redistribution with the notices, are kept verbatim in
`licenses/`: `AMD-FidelityFX-SDK-license.md`, `NVIDIA-Streamline-license.txt`
and `NVIDIA-DLSS-license.txt` - text, never stamped with OMK's SPDX line.
