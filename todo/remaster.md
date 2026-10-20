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
   B2) - the same contract as every enhancement row.
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
  falling back to the shipped texture. UVs are already divided by the
  material's width (ASSETS 4), so a 4x texture drops in with no mesh change.
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
2. **A1 + A2** - tone map, grade, bloom on the game's own emitters.
3. **B3 then A3 + A5** - material maps, reflections, characters in the
   painted light.
4. **D1, then D2 on the player character** as the pilot for the glTF route.

Steps 1-3 are about one to two months of engine work and make a convincing
remaster; step 4 is where the small remake begins, bounded by artist time,
not code.
