# The remaster mode - a plan, nothing built (2026-10-20)

The reader asked (2026-10-20) for a mode that improves the graphics as far as
they go - lighting, reflections, AI-improved textures and models, "a big
remaster or a small remake" - with small artistic changes allowed as long as
the global vision holds, and the core of the game unchanged. This file is the
feasibility estimate and the plan. Every size below is a guess the first hour
of the actual work is allowed to overturn.

**The verdict**: feasible, and a third of it exists. The hard part is not
technical; it is the one `todo/enhancements.md` row 8 already wrote down - the
baked vertex colour IS the artists' lighting, so the remaster must build on
that look, never replace it.

## What it starts from

`todo/enhancements.md` rows 0-13 are done: MSAA, supersampling, trilinear and
anisotropic filtering, per-pixel lighting, mapped shadows (characters only),
60 fps with smoothed bodies, the lifted clip, filtered interface and text.
`--enhance-all` turns them on. The remaster is a tier ON TOP of that, not a
new renderer.

## Two rules, decided before any code

1. **A section of its own: `[Remaster]` / `--remaster`, NOT part of
   `all = max`.** `[Enhancements]` is held to "the data backs it"; row 8 is
   held back precisely because it overrides authored art, and a remaster does.
   So it says so and lives apart. Still OFF by default, still byte-identical
   at rest, every feature with a check shown to fail (CLAUDE.md §4, PORTING
   B2) - the same contract as every enhancement row. **Every feature is its
   OWN option** (the reader, 2026-10-20): `--remaster` is only a preset over
   them, any key beats it, and the current render must stay available
   after every step - the default run is never changed by this plan.
2. **Generated assets never enter the repo.** An upscaled texture or a remodel
   is a derivative of the copyrighted data and the repo is PUBLIC. Ship the
   PIPELINE (GPL scripts); each player generates the pack from their own
   `gamedata/` into a directory of its own, through `omk::safeOutputPath` /
   the tools' equivalent - never into `gamedata/` (CLAUDE.md §1).

## Tier A - the renderer only, no new assets (~2-4 weeks, Vulkan first)

| # | feature | why it respects the art | open question |
|---|---|---|---|
| A1 | HDR target, tone mapping, a colour-grade LUT per city | a grade, not new light; tuned against the shipped frame | the LUTs are a choice of this port - say so |
| A2 | bloom on what already glows: additive-blend meshes, the neon | the GAME chose which meshes emit - that answers row 8's "bloom has no data behind it" | enumerate the emitters from the mesh flags / blend modes (ASSETS 4), not by name |
| A3 | reflections: generalise the planar mirror pass (`AP_mirror`, `AB_mirror`, ...) and the envmap flag `0x4000000`; screen-space reflections on floors a material tag marks wet or polished | the mirrors and envmaps are the game's; the tags come from B3 | which floors - by eye, per city |
| A4 | SSAO at LOW strength; height or volumetric fog in the game's own fog colour and range | the vertex colours already hold painted occlusion, so AO only adds contact detail | strength by eye, never doubling the painted dark |
| A5 | **characters lit by their surroundings**: light probes sampled from the set's vertex colours near a body | READS the artists' lighting instead of adding lights - the most defensible lighting step | the probe spacing; how it composes with row 7 |
| A6 | FSR (open) / MetalFX upscaling at 4K | cheaper than row 9 (`todo/optimization.md` "The 4K benchmark") | the 4K benchmark on the M3 is the gate (30+ fps) |

## Tier B - the HD texture pack (~1 week engine, then curation). The best ratio.

* **B1, the pipeline.** `tools/tex3dt.py` already decodes all 2534 model
  textures (palettised, at most 256x256, ASSETS 3). Upscale 4x with an AI
  upscaler run locally - chaiNNer or Upscayl, Real-ESRGAN-family models
  (4x-UltraSharp, Nomos-type), both run on Apple GPUs. Set textures go the
  same way.
* **B2, the traps the pipeline must handle:**
  * TILING textures: tile 3x3 before upscaling and crop the centre, or every
    repeat grows a seam.
  * The colour KEY (mesh flag `0x800`, solid black is cut out): upscale the
    mask separately and threshold it, or the cutout edge blurs into a fringe
    (the alpha premultiply of row 1 handles the filtered edge after that).
  * Signs and lettering: AI garbles text - an exclusion list, or by hand.
  * 16-colour textures come out over-smoothed; tune per model.
* **B3, material maps.** Normal / roughness / height generated from the
  upscaled textures (DeepBump - a free Blender add-on; Materialize; Substance
  3D Sampler, paid). They feed row 7's per-pixel law and A3. A small sidecar
  per texture also carries the wet/polished tag.
* **B4, the engine side.** A replacement loader keyed by a HASH of the
  decoded original indices (survives renames, two models sharing a texture),
  falling back to the shipped texture. (Corrected when built: the UVs reach
  the GPU in the original's TEXELS and the shader divides by the bound
  texture's size, so a 4x texture needs the factor passed - see below.)
  Memory: 2534 x 1024² is fine on desktop, not on the Vita or the 3DS - the
  pack is desktop/Quest only.
* **B5, optional and more creative**: ComfyUI + a Stable Diffusion tile
  ControlNet re-draws detail rather than sharpening it. Better-looking, and it
  DRIFTS in style: sign-off per texture on the main ones only.

## Tier C - a lighting re-bake: NOT recommended

A full offline re-bake (Blender Cycles, the sets' authored lights calibrated
to match the painted colours) is months of work and replaces the art
direction instead of honouring it - row 8's objection at full size. A1, A4
and A5 get most of the effect without that. Kept here so nobody has to
rediscover why.

## Tier D - models: the weakest part for AI

| # | route | cost | verdict |
|---|---|---|---|
| D1 | automatic smoothing: PN-triangles / Phong tessellation on characters, plus B3's normal maps | ~1-2 weeks | cheap rounded silhouettes; it softens edges the artists meant hard, so a per-model opt-out list |
| D2 | hand-remodel the main cast (~10-20 characters) in Blender, KEEPING each `.3DO` skeleton and weights so every `.ani`, cutscene and `.3DM` line still drives them | ~1-3 months of ARTIST time | the only route to genuinely better characters; needs a `.3DO`->glTF export (`tools/mesh3do.py` already reads the format) and a glTF loader in the engine (cgltf, vendored under `engine/third_party/`) |
| D3 | AI 3D generators: Hunyuan3D-2, TRELLIS (local), Meshy, Tripo (cloud) | - | static, unrigged, messy topology and UVs, style drift: a starting point for PROPS an artist cleans up; never characters (wrong skeleton), never the four city sets (that is remake scope) |

Uploading game assets to a cloud generator is the reader's call; the local
tools avoid the question.

## Tier E - the rest

* The 640x480 interface bitmaps AI-upscaled (B1's pipeline); row 3's integer
  mode is still open beside it.
* The MPEG-1 films (`todo/classic-mac-port-1999.md`) upscaled: Real-ESRGAN in
  video mode (free) or Topaz Video AI (paid).
* Widescreen: the authored camera editings were framed for 4:3 - read what
  `docs/CUTSCENES.md` says about their framing BEFORE widening anything.
  Unchecked.

## The order

1. **B4 + B1 on ONE model** - the fastest visible result, and it settles the
   hash and the colour-key questions before 2534 textures depend on them.
   **DONE 2026-10-20** - see "Step 1, as built" below.
2. **A1 + A2** - tone map, grade, bloom on the game's own emitters.
3. **B3 then A3 + A5** - material maps, reflections, characters in the
   painted light.
4. **D1, then D2 on the player character** as the pilot for the glTF route.

Steps 1-3 are about one to two months of engine work and make a convincing
remaster; step 4 is where the small remake begins, bounded by artist time,
not code.

## Step 1, as built (2026-10-20) - the texture pack

**What it is.** `[Remaster] texturepack = DIR` or `--texture-pack DIR` on
`omk-play` / `omk-play-gles` (the scene viewer takes the flag too). OFF unless
named; `all = max` never touches it. Vulkan and GLES draw it; the software
reference, GL1 and the 3DS never consult it. A pack is a directory of
`<key>.png` (RGBA, the original's size times a power of two each way), so
any one texture can be opened and fixed in an image editor.

**The pieces.**

| file | what |
|---|---|
| `engine/src/o3de/texpack.{h,cpp}` | the key (FNV-1a 64 over size, indices, 768 palette bytes), the pack index, `openTexturePack` |
| `engine/src/platform/pngread.{h,cpp}` | PNG + inflate, written here (no dependency, PORTING A8) |
| `backends/vulkan` | `Tex::su/sv/packed`; push fields `texScaled`/`texScaleU`/`texScaleV` at 104..112 (the block 104 -> 116 bytes); `scene.frag` divides by the original's size only when `texScaled` |
| `backends/gles` | a replacement uploaded at its size, `texSize` kept the original's |
| `engine/tools/texpack_tool.cpp` | `keys` (the engine's keys) and `png` (the engine's reader, for the Python tool) |
| `tools/omktexpack.py` | `export` / `upscale` / `build` - see its docstring |
| `verify.py: engine: texture pack` | the check, `--slow`, shown to fail five ways |

**How to make and use a pack** (the M3; ~1 h 10 for everything):

```bash
P=~/Documents/omk/omk-pack                       # never inside the repo - refused
python3 tools/omktexpack.py export  $P/full      # 1255 unique textures, ~2 min
python3 tools/omktexpack.py upscale $P/full --bin $P/realesrgan-dl/x/realesrgan-ncnn-vulkan
python3 tools/omktexpack.py build   $P/full      # anchored (the default), ~25 min
engine/build/omk-play-gles ../fr ../tables --save ../traces/save-appart.bin --area 0 \
    --stand 1804,0,-6890,336 --texture-pack $P/full/pack
```

`--match DECORS/Aapkayl.3DO` limits any step to some models. Real-ESRGAN is
the official `realesrgan-ncnn-vulkan-20220424-macos` release (universal,
arm64; sha256 `e0ad0558...cc333a`), ~2 s an image on the M3 GPU.

**What the pilot showed** (Kay'l's apartment `Aapkayl` + the street set
`Anekbah`, 41 textures, all 256x256 -> 1024x1024; judged by eye on
`run_vulkan` frames at 1200x800, trilinear + 16x):

* **Raw Real-ESRGAN DENOISES the art.** `realesrgan-x4plus` sharpens edges
  (the pillars, the sofa's folds, the sign lettering, the cutout triangles)
  but wipes the painted GRAIN of floors and ceilings flat - a plastic look,
  away from the original. `realesrgan-x4plus-anime` is flatter still and
  posterises. Mixing a bilinear enlargement back in (`--grain F`) only
  muddies it.
* **ANCHORING fixes it, and is the default.** Back-projection: the upscaled
  image is corrected so its 4x4 box average is the SHIPPED texel again
  (`omktexpack.anchor`). The grain and the colours are the artists' at their
  own scale; the upscaler contributes only what lies between texels - the
  edges. In the comparison it kept x4plus's sharper sofa and panel edges and
  the original's floor and ceiling. `--no-anchor` keeps the raw picture.
* **The game's own lettering survives** (the hospital sign, the vertical
  banners): crisper, not garbled - at this distance. Close up is not yet
  judged.
* Sizes: 1.75 MB a texture on disk (the pilot 72 MB; a full pack ~2.2 GB);
  in VRAM 4 MB a texture + mips, ~200 MB for the street's pool of ~40.

**Measured, the guarantees.** With no pack, every frame tried is
byte-identical to the build before the feature: Vulkan `run_vulkan` on
Aapkayl and ABar06 under nearest / bilinear / trilinear + 16x / 4x MSAA, the
Vulkan scene viewer on Anekbah camera 3, and the 45-frame street dump on
GLES. Under the nearest sampler a pack of every texture at 4x nearest draws
byte-identical to none - on the street, all 1255 textures, 31 then 39 of the
pool replaced.

**The full pack** (2026-10-20, M3): all 1255 textures exported, upscaled
(x4plus, ~2 s an image, 1916 images with the masks) and built anchored in
about 1 h 10; 2.1 GB at `~/Documents/omk/omk-pack/full/pack`. On the street
from the apartment save every texture of both pools is replaced (31 and 39),
none refused, and at 800x600 the picture is the original's - same colours,
same mood - a little crisper on the ground and the clothes; the gain is for
higher resolutions, where the 256x256 originals are magnified.

**On Vulkan in the game session** (`--world-vulkan`, the offscreen path a
headless run can take - a dummy-driver `omk-play` otherwise falls back to the
software reference): the same street, both pools replaced (31 and 39), the 4x
nearest pack byte-identical to none; the full pack's frame against GLES's
differs in 246 of 480000 pixels by more than ~16 levels, and with NO pack the
two backends already differ in 245 (the fire's additive glow among them) - the
backends' own difference, not the pack's. The windowed Vulkan present was not
run headless.

**Open, in the order I would take them:**

1. **A person's look in play** - the street and an interior at 1080p+ with
   the full pack, and the close-up of lettering. The pilot was judged on
   static frames only.
2. ~~**Decode cost at a pool change**~~ - **DONE 2026-10-20**: a pool's
   replacements are decoded together on the shared thread pool
   (`TexturePack::replacements`); the street's uploads with the full pack
   (GLES, M3) went 633.7 -> **145.7 ms** and 129.6 -> **34.0 ms** (2.4 / 0.7
   without a pack), the frame byte-identical. What is left is the inflate
   itself, ~20 ms a 1024x1024 texture on one core; keeping decoded copies
   would trade 4 MB a texture of memory for it.
3. ~~**The greyscale bank**~~ - **DONE 2026-10-20**: the viewer greys the
   pool's PALETTES (`playframe_world_scene.cpp`), which made every key
   another, so a black-and-white cutscene drew the shipped textures. Now the
   viewer tells the pack where each grey texture came from (`aliasGrey`) and
   the pack greys the colour replacement with the engine's luma, keyed where
   the grey is 0 as the grey palette keys it. Area 237 with `--op-at 30:150`:
   the grey pool replaced 34 of 36 on Vulkan and GLES (1 without the alias),
   the nearest pack byte-identical to none, the full pack 0 colourful pixels;
   `engine: texture pack` part 6.
4. ~~**Textures not under MESHES**~~ - **DONE 2026-10-20**: `export` also
   walks the 26 effect sprites stored whole inside the .SCX files
   (`sprite_fx.library()`, `SCX:<name>` in the index, `--match SCX:`) - 6
   unique atlases, 2 of them also under MESHES. Area 237: 36 of 36 replaced
   (34 before), the nearest pack still byte-identical to none. Added to the
   full pack (1259 files). Judged on the atlases only: shapes and colours
   kept; the sprites draw ADDITIVELY, so the near-black rims a cutout view
   shows add nothing - to confirm in play on a fire or an explosion.
5. Curation: an exclusion list for anything the upscaler hurts, and B3's
   material maps from the same images.

The full-sweep red seen on the way, NOT this change's: `engine: release
build`'s second field (the development build's `OMKSNAP1` marker) is 0 in the
build from before this feature too.
