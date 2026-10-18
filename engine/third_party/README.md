# Vendored third-party code

One file, and it is here rather than found by the build for the reason
[`../../docs/PORTING.md`](../../docs/PORTING.md) A8 gives: **a vendored
dependency is checked in and `make` works without it being installed, so a
missing one is a broken checkout; a system dependency's absence merely disables
the frontend and never the suite.**

| file | what | upstream | licence |
|---|---|---|---|
| `pl_mpeg.h` | MPEG-1 video decoder, MP2 audio decoder and MPEG-PS demuxer, in one header | [phoboslab/pl_mpeg](https://github.com/phoboslab/pl_mpeg) | **MIT** (`SPDX-License-Identifier: MIT`, Dominic Szablewski) |

Fetched 2026-09-01 from `raw.githubusercontent.com/phoboslab/pl_mpeg/master`,
4439 lines, and **kept verbatim** — it is not edited here, so it can be diffed
against upstream. `docs/PORTING.md` A8 called it public domain; it is MIT, and
that is corrected there.

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
